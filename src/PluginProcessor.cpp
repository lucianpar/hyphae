#include "PluginProcessor.h"
#include "PluginEditor.h"

#include <algorithm>
#include <cmath>

namespace
{
constexpr auto stateType = "HyphaeState";
constexpr auto stateVersionProperty = "stateVersion";
constexpr float minimumSpawnDensity = 0.1f;
constexpr float minimumGrainDelayMs = 35.0f;
constexpr float maximumGrainDelayMs = 1200.0f;
constexpr float minimumRateDrift = 0.97f;
constexpr float maximumRateDrift = 1.03f;
constexpr float voiceBaseGain = 0.18f;
constexpr float clusterSmoothingPerSecond = 6.0f;
constexpr float minimumClusterEnergy = 0.2f;
constexpr float maximumClusterEnergy = 1.0f;
constexpr std::array<float, 4> conductionTapOffsets { 0.0f, 0.21f, 0.47f, 0.79f };
constexpr float dcBlockerCoefficient = 0.995f;
constexpr float softClipDrive = 1.4f;
constexpr float wetHeadroomTarget = 0.85f;

namespace ParameterIds
{
constexpr auto dryWet = "dryWet";
constexpr auto outputTrimDb = "outputTrimDb";
constexpr auto density = "density";
constexpr auto sizeMs = "sizeMs";
constexpr auto scatter = "scatter";
constexpr auto spread = "spread";
constexpr auto growth = "growth";
constexpr auto nutrients = "nutrients";
constexpr auto seed = "seed";
constexpr auto conduction = "conduction";
constexpr auto damping = "damping";
constexpr auto sporeBurst = "sporeBurst";
constexpr auto freeze = "freeze";
constexpr auto resetClear = "resetClear";
} // namespace ParameterIds

juce::NormalisableRange<float> percentRange()
{
    return { 0.0f, 1.0f, 0.001f };
}

juce::AudioParameterFloatAttributes makePercentAttributes (const juce::String& label)
{
    return juce::AudioParameterFloatAttributes().withLabel (label);
}

float sanitizeSample (float sample) noexcept
{
    return std::isfinite (sample) ? sample : 0.0f;
}
} // namespace

//==============================================================================
AudioPluginAudioProcessor::AudioPluginAudioProcessor()
     : AudioProcessor (BusesProperties()
                     #if ! JucePlugin_IsMidiEffect
                      #if ! JucePlugin_IsSynth
                       .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                      #endif
                       .withOutput ("Output", juce::AudioChannelSet::stereo(), true)
                     #endif
                       ),
       parameters (*this, nullptr, stateType, createParameterLayout())
{
    parameters.state.setProperty (stateVersionProperty, currentStateVersion, nullptr);
    dryWetParameter = parameters.getRawParameterValue (ParameterIds::dryWet);
    outputTrimDbParameter = parameters.getRawParameterValue (ParameterIds::outputTrimDb);
    densityParameter = parameters.getRawParameterValue (ParameterIds::density);
    sizeMsParameter = parameters.getRawParameterValue (ParameterIds::sizeMs);
    scatterParameter = parameters.getRawParameterValue (ParameterIds::scatter);
    spreadParameter = parameters.getRawParameterValue (ParameterIds::spread);
    growthParameter = parameters.getRawParameterValue (ParameterIds::growth);
    nutrientsParameter = parameters.getRawParameterValue (ParameterIds::nutrients);
    seedParameter = parameters.getRawParameterValue (ParameterIds::seed);
    conductionParameter = parameters.getRawParameterValue (ParameterIds::conduction);
    dampingParameter = parameters.getRawParameterValue (ParameterIds::damping);
    sporeBurstParameter = parameters.getRawParameterValue (ParameterIds::sporeBurst);
    freezeParameter = parameters.getRawParameterValue (ParameterIds::freeze);
    randomGenerator.seed (static_cast<std::minstd_rand::result_type> (lastSeedValue));
    initializeClusters (true);
    updateConductionBedModel();
    scheduleNextSpawnInterval();
}

AudioPluginAudioProcessor::~AudioPluginAudioProcessor()
{
}

void AudioPluginAudioProcessor::DelayBuffer::prepare (double sampleRate, int maximumDelaySamples)
{
    juce::ignoreUnused (sampleRate);

    samples.assign (static_cast<size_t> (juce::jmax (1, maximumDelaySamples)), 0.0f);
    clear();
}

void AudioPluginAudioProcessor::DelayBuffer::clear() noexcept
{
    std::fill (samples.begin(), samples.end(), 0.0f);
    writeIndex = 0;
    validSampleCount = 0;
}

void AudioPluginAudioProcessor::DelayBuffer::write (float sample) noexcept
{
    if (samples.empty())
        return;

    samples[static_cast<size_t> (writeIndex)] = sanitizeSample (sample);
    writeIndex = (writeIndex + 1) % static_cast<int> (samples.size());
    validSampleCount = juce::jmin (validSampleCount + 1, static_cast<int> (samples.size()));
}

float AudioPluginAudioProcessor::DelayBuffer::readDelaySamples (float delaySamples) const noexcept
{
    if (samples.empty() || validSampleCount <= 0)
        return 0.0f;

    const auto maxReadableDelay = static_cast<float> (juce::jmax (0, validSampleCount - 1));
    const auto clampedDelay = juce::jlimit (0.0f, maxReadableDelay, sanitizeSample (delaySamples));
    const auto readPosition = static_cast<float> (writeIndex - 1) - clampedDelay;
    const auto bufferSize = static_cast<float> (samples.size());

    auto wrappedReadPosition = readPosition;
    while (wrappedReadPosition < 0.0f)
        wrappedReadPosition += bufferSize;

    while (wrappedReadPosition >= bufferSize)
        wrappedReadPosition -= bufferSize;

    const auto indexA = static_cast<int> (wrappedReadPosition);
    const auto indexB = (indexA + 1) % static_cast<int> (samples.size());
    const auto fraction = wrappedReadPosition - static_cast<float> (indexA);
    const auto sampleA = samples[static_cast<size_t> (indexA)];
    const auto sampleB = samples[static_cast<size_t> (indexB)];

    return sanitizeSample (sampleA + fraction * (sampleB - sampleA));
}

int AudioPluginAudioProcessor::DelayBuffer::getCapacitySamples() const noexcept
{
    return static_cast<int> (samples.size());
}

juce::AudioProcessorValueTreeState::ParameterLayout AudioPluginAudioProcessor::createParameterLayout()
{
    using FloatParameter = juce::AudioParameterFloat;
    using BoolParameter = juce::AudioParameterBool;
    using IntParameter = juce::AudioParameterInt;

    std::vector<std::unique_ptr<juce::RangedAudioParameter>> layout;
    layout.reserve (14);

    layout.push_back (std::make_unique<FloatParameter> (juce::ParameterID { ParameterIds::dryWet, 1 },
                                                        "Dry/Wet",
                                                        percentRange(),
                                                        0.5f,
                                                        makePercentAttributes ("%")));

    layout.push_back (std::make_unique<FloatParameter> (juce::ParameterID { ParameterIds::outputTrimDb, 1 },
                                                        "Output Trim",
                                                        juce::NormalisableRange<float> { -18.0f, 6.0f, 0.1f },
                                                        0.0f,
                                                        juce::AudioParameterFloatAttributes().withLabel ("dB")));

    layout.push_back (std::make_unique<FloatParameter> (juce::ParameterID { ParameterIds::density, 1 },
                                                        "Density",
                                                        juce::NormalisableRange<float> { 0.1f, 24.0f, 0.01f, 0.4f },
                                                        4.0f,
                                                        juce::AudioParameterFloatAttributes().withLabel ("grains/s")));

    layout.push_back (std::make_unique<FloatParameter> (juce::ParameterID { ParameterIds::sizeMs, 1 },
                                                        "Size",
                                                        juce::NormalisableRange<float> { 20.0f, 180.0f, 1.0f, 0.5f },
                                                        80.0f,
                                                        juce::AudioParameterFloatAttributes().withLabel ("ms")));

    layout.push_back (std::make_unique<FloatParameter> (juce::ParameterID { ParameterIds::scatter, 1 },
                                                        "Scatter",
                                                        percentRange(),
                                                        0.35f,
                                                        makePercentAttributes ("%")));

    layout.push_back (std::make_unique<FloatParameter> (juce::ParameterID { ParameterIds::spread, 1 },
                                                        "Spread",
                                                        percentRange(),
                                                        0.6f,
                                                        makePercentAttributes ("%")));

    layout.push_back (std::make_unique<FloatParameter> (juce::ParameterID { ParameterIds::growth, 1 },
                                                        "Growth",
                                                        percentRange(),
                                                        0.5f,
                                                        makePercentAttributes ("%")));

    layout.push_back (std::make_unique<FloatParameter> (juce::ParameterID { ParameterIds::nutrients, 1 },
                                                        "Nutrients",
                                                        percentRange(),
                                                        0.5f,
                                                        makePercentAttributes ("%")));

    layout.push_back (std::make_unique<IntParameter> (juce::ParameterID { ParameterIds::seed, 1 },
                                                      "Seed",
                                                      0,
                                                      65535,
                                                      12345));

    layout.push_back (std::make_unique<FloatParameter> (juce::ParameterID { ParameterIds::conduction, 1 },
                                                        "Conduction",
                                                        percentRange(),
                                                        0.4f,
                                                        makePercentAttributes ("%")));

    layout.push_back (std::make_unique<FloatParameter> (juce::ParameterID { ParameterIds::damping, 1 },
                                                        "Damping",
                                                        percentRange(),
                                                        0.45f,
                                                        makePercentAttributes ("%")));

    layout.push_back (std::make_unique<BoolParameter> (juce::ParameterID { ParameterIds::sporeBurst, 1 },
                                                       "Spore Burst",
                                                       false));

    layout.push_back (std::make_unique<BoolParameter> (juce::ParameterID { ParameterIds::freeze, 1 },
                                                       "Freeze",
                                                       false));

    layout.push_back (std::make_unique<BoolParameter> (juce::ParameterID { ParameterIds::resetClear, 1 },
                                                       "Reset/Clear",
                                                       false));

    return { layout.begin(), layout.end() };
}

//==============================================================================
const juce::String AudioPluginAudioProcessor::getName() const
{
    return JucePlugin_Name;
}

bool AudioPluginAudioProcessor::acceptsMidi() const
{
   #if JucePlugin_WantsMidiInput
    return true;
   #else
    return false;
   #endif
}

bool AudioPluginAudioProcessor::producesMidi() const
{
   #if JucePlugin_ProducesMidiOutput
    return true;
   #else
    return false;
   #endif
}

bool AudioPluginAudioProcessor::isMidiEffect() const
{
   #if JucePlugin_IsMidiEffect
    return true;
   #else
    return false;
   #endif
}

double AudioPluginAudioProcessor::getTailLengthSeconds() const
{
    return 0.0;
}

int AudioPluginAudioProcessor::getNumPrograms()
{
    return 1;   // NB: some hosts don't cope very well if you tell them there are 0 programs,
                // so this should be at least 1, even if you're not really implementing programs.
}

int AudioPluginAudioProcessor::getCurrentProgram()
{
    return 0;
}

void AudioPluginAudioProcessor::setCurrentProgram (int index)
{
    juce::ignoreUnused (index);
}

const juce::String AudioPluginAudioProcessor::getProgramName (int index)
{
    juce::ignoreUnused (index);
    return {};
}

void AudioPluginAudioProcessor::changeProgramName (int index, const juce::String& newName)
{
    juce::ignoreUnused (index, newName);
}

//==============================================================================
void AudioPluginAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    juce::ignoreUnused (sampleRate, samplesPerBlock);

    currentSampleRate = juce::jlimit (1.0, maxSupportedSampleRate, sampleRate);
    preparedBlockSize = juce::jmax (1, samplesPerBlock);

    const auto delayCapacitySamples = juce::jlimit (1,
                                                    hardCapDelaySamples,
                                                    static_cast<int> (std::ceil (currentSampleRate * maxDelaySeconds)));

    writeGain.reset (currentSampleRate, 0.03);
    dryWetMix.reset (currentSampleRate, 0.03);
    outputGain.reset (currentSampleRate, 0.03);
    sporeSideMix.reset (currentSampleRate, 0.03);
    bedMidMix.reset (currentSampleRate, 0.03);
    wetNormalizationGain.reset (currentSampleRate, 0.03);
    delayBuffer.prepare (currentSampleRate, delayCapacitySamples);
    wetBuffer.setSize (2, preparedBlockSize, false, false, true);
    sporeBuffer.setSize (2, preparedBlockSize, false, false, true);
    bedBuffer.setSize (2, preparedBlockSize, false, false, true);
    resetDelayState();
    resetGrainState();
    resetMyceliumState();
    reseedRandomIfNeeded();
    updateConductionBedModel();
    scheduleNextSpawnInterval();
    wetNormalizationGain.setCurrentAndTargetValue (1.0f);
}

void AudioPluginAudioProcessor::releaseResources()
{
    resetDelayState();
    resetGrainState();
    resetMyceliumState();
}

bool AudioPluginAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
  #if JucePlugin_IsMidiEffect
    juce::ignoreUnused (layouts);
    return true;
  #else
    // This is the place where you check if the layout is supported.
    // In this template code we only support mono or stereo.
    // Some plugin hosts, such as certain GarageBand versions, will only
    // load plugins that support stereo bus layouts.
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::mono()
     && layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;

    // This checks if the input layout matches the output layout
   #if ! JucePlugin_IsSynth
    if (layouts.getMainOutputChannelSet() != layouts.getMainInputChannelSet())
        return false;
   #endif

    return true;
  #endif
}

void AudioPluginAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer,
                                              juce::MidiBuffer& midiMessages)
{
    juce::ignoreUnused (midiMessages);

    juce::ScopedNoDenormals noDenormals;
    auto totalNumInputChannels  = getTotalNumInputChannels();
    auto totalNumOutputChannels = getTotalNumOutputChannels();

    // In case we have more outputs than inputs, this code clears any output
    // channels that didn't contain input data, (because these aren't
    // guaranteed to be empty - they may contain garbage).
    // This is here to avoid people getting screaming feedback
    // when they first compile a plugin, but obviously you don't need to keep
    // this code if your algorithm always overwrites all the output channels.
    for (auto i = totalNumInputChannels; i < totalNumOutputChannels; ++i)
        buffer.clear (i, 0, buffer.getNumSamples());

    const auto numSamples = buffer.getNumSamples();
    if (numSamples <= 0)
        return;

    if (numSamples > preparedBlockSize || wetBuffer.getNumSamples() < numSamples)
    {
        jassertfalse;
        return;
    }

    reseedRandomIfNeeded();
    handleSporeBurstTrigger();
    updateMyceliumModel (numSamples);
    updateConductionBedModel();

    writeGain.setTargetValue (getFreezeTargetWriteGain());
    dryWetMix.setTargetValue (getFloatParameterValue (dryWetParameter, 0.5f));

    const auto outputTrimDb = getFloatParameterValue (outputTrimDbParameter, 0.0f);
    outputGain.setTargetValue (juce::Decibels::decibelsToGain (outputTrimDb));
    const auto spread = juce::jlimit (0.0f, 1.0f, getFloatParameterValue (spreadParameter, 0.6f));
    const auto conduction = juce::jlimit (0.0f, 1.0f, getFloatParameterValue (conductionParameter, 0.4f));
    sporeSideMix.setTargetValue (juce::jlimit (0.0f, 1.0f, 0.08f + (spread * 0.32f)));
    bedMidMix.setTargetValue (juce::jlimit (0.0f, 1.0f, 0.10f + (conduction * 0.28f)));

    spawnGrainsForBlock (numSamples);
    wetBuffer.clear();
    sporeBuffer.clear();
    bedBuffer.clear();

    for (int sampleIndex = 0; sampleIndex < numSamples; ++sampleIndex)
    {
        float monoSum = 0.0f;

        for (int channel = 0; channel < totalNumInputChannels; ++channel)
            monoSum += sanitizeSample (buffer.getReadPointer (channel)[sampleIndex]);

        if (totalNumInputChannels > 0)
            monoSum /= static_cast<float> (totalNumInputChannels);

        const auto smoothedWriteGain = writeGain.getNextValue();
        delayBuffer.write (monoSum * smoothedWriteGain);
        lastWriteGain.store (smoothedWriteGain, std::memory_order_relaxed);
    }

    renderWetGrains (wetBuffer, sporeBuffer, numSamples);
    renderConductionBed (wetBuffer, bedBuffer, numSamples);
    applyStereoShaping (wetBuffer, sporeBuffer, bedBuffer, numSamples);
    applyOutputSafety (wetBuffer, buffer, totalNumInputChannels, totalNumOutputChannels, numSamples);

    if (const auto capacitySamples = delayBuffer.getCapacitySamples(); capacitySamples > 1)
    {
        const auto previewDelaySamples = static_cast<float> (juce::jlimit (1.0,
                                                                           static_cast<double> (capacitySamples - 1),
                                                                           currentSampleRate * 0.25));
        lastDelayPreviewSample.store (delayBuffer.readDelaySamples (previewDelaySamples), std::memory_order_relaxed);
    }
    else
    {
        lastDelayPreviewSample.store (0.0f, std::memory_order_relaxed);
    }
}

//==============================================================================
bool AudioPluginAudioProcessor::hasEditor() const
{
    return true; // (change this to false if you choose to not supply an editor)
}

juce::AudioProcessorEditor* AudioPluginAudioProcessor::createEditor()
{
    return new AudioPluginAudioProcessorEditor (*this);
}

//==============================================================================
void AudioPluginAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = parameters.copyState();
    state.setProperty (stateVersionProperty, currentStateVersion, nullptr);

    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void AudioPluginAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    const std::unique_ptr<juce::XmlElement> xmlState (getXmlFromBinary (data, sizeInBytes));

    if (xmlState == nullptr)
        return;

    const auto state = juce::ValueTree::fromXml (*xmlState);
    if (! state.isValid() || ! state.hasType (stateType))
        return;

    auto restoredState = state;
    if (! restoredState.hasProperty (stateVersionProperty))
        restoredState.setProperty (stateVersionProperty, currentStateVersion, nullptr);

    parameters.replaceState (restoredState);
}

juce::AudioProcessorValueTreeState& AudioPluginAudioProcessor::getValueTreeState() noexcept
{
    return parameters;
}

const juce::AudioProcessorValueTreeState& AudioPluginAudioProcessor::getValueTreeState() const noexcept
{
    return parameters;
}

float AudioPluginAudioProcessor::getLastDelayPreviewSample() const noexcept
{
    return lastDelayPreviewSample.load (std::memory_order_relaxed);
}

float AudioPluginAudioProcessor::getLastWriteGain() const noexcept
{
    return lastWriteGain.load (std::memory_order_relaxed);
}

int AudioPluginAudioProcessor::getDelayBufferCapacitySamples() const noexcept
{
    return delayBuffer.getCapacitySamples();
}

int AudioPluginAudioProcessor::getActiveGrainCount() const noexcept
{
    return lastActiveGrainCount.load (std::memory_order_relaxed);
}

void AudioPluginAudioProcessor::resetDelayState() noexcept
{
    delayBuffer.clear();
    writeGain.setCurrentAndTargetValue (1.0f);
    lastWriteGain.store (1.0f, std::memory_order_relaxed);
    lastDelayPreviewSample.store (0.0f, std::memory_order_relaxed);
    conductionLowpassLeft = 0.0f;
    conductionLowpassRight = 0.0f;
    dcBlockerX1 = { 0.0f, 0.0f };
    dcBlockerY1 = { 0.0f, 0.0f };
}

void AudioPluginAudioProcessor::resetGrainState() noexcept
{
    for (auto& voice : grainVoices)
        voice = {};

    dryWetMix.setCurrentAndTargetValue (getFloatParameterValue (dryWetParameter, 0.5f));
    outputGain.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (getFloatParameterValue (outputTrimDbParameter, 0.0f)));
    sporeSideMix.setCurrentAndTargetValue (0.08f + (juce::jlimit (0.0f, 1.0f, getFloatParameterValue (spreadParameter, 0.6f)) * 0.32f));
    bedMidMix.setCurrentAndTargetValue (0.10f + (juce::jlimit (0.0f, 1.0f, getFloatParameterValue (conductionParameter, 0.4f)) * 0.28f));
    wetNormalizationGain.setCurrentAndTargetValue (1.0f);
    samplesUntilNextSpawn = 0.0;
    lastActiveGrainCount.store (0, std::memory_order_relaxed);
}

void AudioPluginAudioProcessor::resetMyceliumState() noexcept
{
    myceliumEventTimerSamples = 0.0;
    lastSporeBurstState = false;
    initializeClusters (true);
    updateConductionBedModel();
}

float AudioPluginAudioProcessor::getFreezeTargetWriteGain() const noexcept
{
    if (freezeParameter == nullptr)
        return 1.0f;

    return *freezeParameter >= 0.5f ? 0.0f : 1.0f;
}

float AudioPluginAudioProcessor::getFloatParameterValue (std::atomic<float>* parameter, float fallbackValue) const noexcept
{
    return parameter != nullptr ? parameter->load (std::memory_order_relaxed) : fallbackValue;
}

void AudioPluginAudioProcessor::reseedRandomIfNeeded() noexcept
{
    const auto requestedSeed = static_cast<int> (getFloatParameterValue (seedParameter, static_cast<float> (lastSeedValue)));
    if (requestedSeed == lastSeedValue)
        return;

    lastSeedValue = requestedSeed;
    randomGenerator.seed (static_cast<std::minstd_rand::result_type> (juce::jmax (0, requestedSeed)));
    initializeClusters (true);
    scheduleNextSpawnInterval();
}

float AudioPluginAudioProcessor::nextRandomFloat() noexcept
{
    return static_cast<float> (randomGenerator()) / static_cast<float> (std::minstd_rand::max());
}

float AudioPluginAudioProcessor::nextRandomBipolar() noexcept
{
    return (nextRandomFloat() * 2.0f) - 1.0f;
}

int AudioPluginAudioProcessor::getActiveGrainCountInternal() const noexcept
{
    return static_cast<int> (std::count_if (grainVoices.begin(), grainVoices.end(),
                                            [] (const GrainVoice& voice) { return voice.active; }));
}

void AudioPluginAudioProcessor::scheduleNextSpawnInterval() noexcept
{
    const auto density = juce::jmax (minimumSpawnDensity, getFloatParameterValue (densityParameter, 4.0f));
    const auto randomValue = juce::jmax (0.0001f, nextRandomFloat());
    const auto intervalSeconds = -std::log (randomValue) / density;
    samplesUntilNextSpawn = juce::jmax (1.0, intervalSeconds * currentSampleRate);
}

void AudioPluginAudioProcessor::spawnGrainsForBlock (int numSamples) noexcept
{
    samplesUntilNextSpawn -= static_cast<double> (numSamples);

    auto spawnedThisBlock = 0;
    while (samplesUntilNextSpawn <= 0.0 && spawnedThisBlock < 2)
    {
        if (spawnGrainVoice())
            ++spawnedThisBlock;

        scheduleNextSpawnInterval();
    }

    if (samplesUntilNextSpawn <= 0.0)
        samplesUntilNextSpawn = 1.0;
}

bool AudioPluginAudioProcessor::spawnGrainVoice() noexcept
{
    auto freeVoice = std::find_if (grainVoices.begin(), grainVoices.end(),
                                   [] (const GrainVoice& voice) { return ! voice.active; });

    if (freeVoice == grainVoices.end())
        return false;

    const auto sizeMs = juce::jlimit (20.0f, 180.0f, getFloatParameterValue (sizeMsParameter, 80.0f));
    const auto durationSamples = juce::jmax (8, static_cast<int> (std::round ((sizeMs * 0.001f) * static_cast<float> (currentSampleRate))));
    const auto scatter = juce::jlimit (0.0f, 1.0f, getFloatParameterValue (scatterParameter, 0.35f));
    const auto spread = juce::jlimit (0.0f, 1.0f, getFloatParameterValue (spreadParameter, 0.6f));
    const auto clusterIndex = chooseClusterIndex();
    const auto& cluster = clusters[static_cast<size_t> (clusterIndex)];
    const auto clusterDelaySpread = cluster.currentDelaySpreadMs + (scatter * 220.0f);
    const auto delayMs = juce::jlimit (minimumGrainDelayMs,
                                       maximumGrainDelayMs,
                                       cluster.currentDelayMs + (nextRandomBipolar() * clusterDelaySpread));

    const auto clusterPanSpread = juce::jlimit (0.02f, 1.0f, cluster.currentPanSpread + (scatter * 0.18f));
    const auto pan = juce::jlimit (-1.0f, 1.0f, cluster.currentPanCenter + (nextRandomBipolar() * clusterPanSpread * spread));
    const auto angle = (pan + 1.0f) * 0.25f * juce::MathConstants<float>::pi;
    const auto drift = juce::jmap (nextRandomFloat(), minimumRateDrift, maximumRateDrift);

    freeVoice->active = true;
    freeVoice->delaySamples = delayMs * 0.001f * static_cast<float> (currentSampleRate);
    freeVoice->readOffsetSamples = 0.0f;
    freeVoice->readIncrement = drift;
    freeVoice->gain = voiceBaseGain * juce::jmap (scatter, 1.0f, 0.75f) * juce::jmap (cluster.currentEnergy, 0.85f, 1.2f);
    freeVoice->leftGain = std::cos (angle);
    freeVoice->rightGain = std::sin (angle);
    freeVoice->samplesRemaining = durationSamples;
    freeVoice->totalSamples = durationSamples;

    return true;
}

void AudioPluginAudioProcessor::renderWetGrains (juce::AudioBuffer<float>& destinationBuffer,
                                                 juce::AudioBuffer<float>& sporeDestinationBuffer,
                                                 int numSamples) noexcept
{
    auto* wetLeft = destinationBuffer.getWritePointer (0);
    auto* wetRight = destinationBuffer.getWritePointer (1);
    auto* sporeLeft = sporeDestinationBuffer.getWritePointer (0);
    auto* sporeRight = sporeDestinationBuffer.getWritePointer (1);

    for (auto& voice : grainVoices)
    {
        if (! voice.active)
            continue;

        for (int sampleIndex = 0; sampleIndex < numSamples; ++sampleIndex)
        {
            if (! voice.active || voice.samplesRemaining <= 0)
            {
                voice = {};
                break;
            }

            const auto progress = 1.0f - (static_cast<float> (voice.samplesRemaining) / static_cast<float> (voice.totalSamples));
            const auto window = 0.5f - (0.5f * std::cos (juce::MathConstants<float>::twoPi * progress));
            const auto sourceSample = delayBuffer.readDelaySamples (voice.delaySamples + voice.readOffsetSamples);
            const auto sample = sanitizeSample (sourceSample * window * voice.gain);

            const auto leftSample = sample * voice.leftGain;
            const auto rightSample = sample * voice.rightGain;
            wetLeft[sampleIndex] = sanitizeSample (wetLeft[sampleIndex] + leftSample);
            wetRight[sampleIndex] = sanitizeSample (wetRight[sampleIndex] + rightSample);
            sporeLeft[sampleIndex] = sanitizeSample (sporeLeft[sampleIndex] + leftSample);
            sporeRight[sampleIndex] = sanitizeSample (sporeRight[sampleIndex] + rightSample);

            voice.readOffsetSamples += voice.readIncrement;
            --voice.samplesRemaining;
        }
    }

    const auto activeVoiceCount = getActiveGrainCountInternal();
    lastActiveGrainCount.store (activeVoiceCount, std::memory_order_relaxed);

    const auto normalization = 1.0f / std::sqrt (static_cast<float> (juce::jmax (1, activeVoiceCount)));
    for (int sampleIndex = 0; sampleIndex < numSamples; ++sampleIndex)
    {
        wetLeft[sampleIndex] = sanitizeSample (wetLeft[sampleIndex] * normalization);
        wetRight[sampleIndex] = sanitizeSample (wetRight[sampleIndex] * normalization);
        sporeLeft[sampleIndex] = sanitizeSample (sporeLeft[sampleIndex] * normalization);
        sporeRight[sampleIndex] = sanitizeSample (sporeRight[sampleIndex] * normalization);
    }
}

void AudioPluginAudioProcessor::updateMyceliumModel (int numSamples) noexcept
{
    const auto deltaSeconds = static_cast<float> (numSamples / currentSampleRate);
    const auto spread = juce::jlimit (0.0f, 1.0f, getFloatParameterValue (spreadParameter, 0.6f));
    const auto growth = juce::jlimit (0.0f, 1.0f, getFloatParameterValue (growthParameter, 0.5f));
    const auto nutrients = juce::jlimit (0.0f, 1.0f, getFloatParameterValue (nutrientsParameter, 0.5f));
    const auto scatter = juce::jlimit (0.0f, 1.0f, getFloatParameterValue (scatterParameter, 0.35f));
    const auto smoothing = juce::jlimit (0.0f, 1.0f, deltaSeconds * clusterSmoothingPerSecond);

    myceliumEventTimerSamples -= static_cast<double> (numSamples);
    if (myceliumEventTimerSamples <= 0.0)
    {
        const auto eventIntervalSeconds = juce::jmap (growth, 1.8f, 0.35f);
        myceliumEventTimerSamples = eventIntervalSeconds * currentSampleRate;

        const auto branchChance = 0.2f + (growth * 0.55f);
        const auto mergeChance = 0.12f + ((1.0f - nutrients) * 0.45f);

        if (nextRandomFloat() < branchChance && getActiveClusterCount() < maxClusters)
            branchCluster();
        else if (nextRandomFloat() < mergeChance && getActiveClusterCount() > 2)
            mergeClusters();
        else
            reseedClusterTargets();
    }

    targetClusterCount = juce::jlimit (2, maxClusters, 2 + static_cast<int> (std::round (growth * 2.0f)));

    for (auto& cluster : clusters)
    {
        if (! cluster.active)
            continue;

        cluster.ageSeconds += deltaSeconds;
        cluster.targetPanSpread = juce::jlimit (0.06f, 0.95f, 0.12f + (spread * 0.35f) + (scatter * 0.18f));
        cluster.targetDelaySpreadMs = juce::jlimit (25.0f, 360.0f, 55.0f + (scatter * 180.0f) + (spread * 45.0f));
        cluster.targetEnergy = juce::jlimit (minimumClusterEnergy,
                                             maximumClusterEnergy,
                                             0.35f + (nutrients * 0.45f) + (nextRandomBipolar() * 0.08f));

        cluster.targetPanCenter = juce::jlimit (-1.0f, 1.0f, cluster.targetPanCenter + (cluster.driftVelocity * deltaSeconds));
        cluster.targetDelayMs = juce::jlimit (minimumGrainDelayMs,
                                              maximumGrainDelayMs,
                                              cluster.targetDelayMs + (nextRandomBipolar() * (18.0f + (growth * 22.0f))));

        cluster.currentPanCenter += (cluster.targetPanCenter - cluster.currentPanCenter) * smoothing;
        cluster.currentPanSpread += (cluster.targetPanSpread - cluster.currentPanSpread) * smoothing;
        cluster.currentDelayMs += (cluster.targetDelayMs - cluster.currentDelayMs) * smoothing;
        cluster.currentDelaySpreadMs += (cluster.targetDelaySpreadMs - cluster.currentDelaySpreadMs) * smoothing;
        cluster.currentEnergy += (cluster.targetEnergy - cluster.currentEnergy) * smoothing;
    }
}

void AudioPluginAudioProcessor::updateConductionBedModel() noexcept
{
    const auto conduction = juce::jlimit (0.0f, 1.0f, getFloatParameterValue (conductionParameter, 0.4f));
    const auto damping = juce::jlimit (0.0f, 1.0f, getFloatParameterValue (dampingParameter, 0.45f));
    const auto activeClusters = juce::jmax (1, getActiveClusterCount());

    float averageDelayMs = 220.0f;
    float averageEnergy = 0.5f;
    float averagePan = 0.0f;

    if (activeClusters > 0)
    {
        averageDelayMs = 0.0f;
        averageEnergy = 0.0f;
        averagePan = 0.0f;

        for (const auto& cluster : clusters)
        {
            if (! cluster.active)
                continue;

            averageDelayMs += cluster.currentDelayMs;
            averageEnergy += cluster.currentEnergy;
            averagePan += cluster.currentPanCenter;
        }

        averageDelayMs /= static_cast<float> (activeClusters);
        averageEnergy /= static_cast<float> (activeClusters);
        averagePan /= static_cast<float> (activeClusters);
    }

    const auto coherence = juce::jlimit (0.0f, 1.0f, 0.35f + (getFloatParameterValue (growthParameter, 0.5f) * 0.4f)
                                                       + (getFloatParameterValue (nutrientsParameter, 0.5f) * 0.15f));
    const auto bedCenterMs = juce::jlimit (40.0f, 320.0f, (averageDelayMs * 0.55f) + 30.0f);
    const auto bedSpreadMs = juce::jlimit (18.0f, 140.0f, (1.0f - coherence) * 110.0f + 24.0f);

    for (int index = 0; index < conductionTapCount; ++index)
    {
        auto& tap = conductionTaps[static_cast<size_t> (index)];
        const auto signedOffset = (conductionTapOffsets[static_cast<size_t> (index)] - 0.4f) * bedSpreadMs * 1.8f;
        tap.delayMs = juce::jlimit (30.0f, 350.0f, bedCenterMs + signedOffset);
        tap.gain = conduction * (0.16f - (0.024f * static_cast<float> (index))) * juce::jmap (averageEnergy, 0.8f, 1.15f);
        tap.pan = juce::jlimit (-0.45f, 0.45f, (averagePan * 0.35f) + ((static_cast<float> (index) - 1.5f) * 0.12f * (1.0f - coherence)));
    }

    const auto dampingBlend = juce::jlimit (0.05f, 0.6f, 0.55f - (damping * 0.45f));
    conductionLowpassLeft *= (1.0f - dampingBlend);
    conductionLowpassRight *= (1.0f - dampingBlend);
}

void AudioPluginAudioProcessor::initializeClusters (bool randomizeCurrentState) noexcept
{
    targetClusterCount = 3;

    for (int index = 0; index < maxClusters; ++index)
    {
        auto& cluster = clusters[static_cast<size_t> (index)];
        cluster = {};
        cluster.active = index < targetClusterCount;
        cluster.targetPanCenter = juce::jlimit (-1.0f, 1.0f, nextRandomBipolar() * 0.8f);
        cluster.targetPanSpread = 0.14f + (nextRandomFloat() * 0.18f);
        cluster.targetDelayMs = 150.0f + (nextRandomFloat() * 500.0f);
        cluster.targetDelaySpreadMs = 55.0f + (nextRandomFloat() * 120.0f);
        cluster.targetEnergy = 0.45f + (nextRandomFloat() * 0.4f);
        cluster.driftVelocity = nextRandomBipolar() * 0.32f;
        cluster.ageSeconds = nextRandomFloat() * 3.0f;

        if (randomizeCurrentState)
        {
            cluster.currentPanCenter = cluster.targetPanCenter;
            cluster.currentPanSpread = cluster.targetPanSpread;
            cluster.currentDelayMs = cluster.targetDelayMs;
            cluster.currentDelaySpreadMs = cluster.targetDelaySpreadMs;
            cluster.currentEnergy = cluster.targetEnergy;
        }
        else
        {
            cluster.currentPanCenter += (cluster.targetPanCenter - cluster.currentPanCenter) * 0.35f;
            cluster.currentPanSpread += (cluster.targetPanSpread - cluster.currentPanSpread) * 0.35f;
            cluster.currentDelayMs += (cluster.targetDelayMs - cluster.currentDelayMs) * 0.35f;
            cluster.currentDelaySpreadMs += (cluster.targetDelaySpreadMs - cluster.currentDelaySpreadMs) * 0.35f;
            cluster.currentEnergy += (cluster.targetEnergy - cluster.currentEnergy) * 0.35f;
        }
    }
}

void AudioPluginAudioProcessor::handleSporeBurstTrigger() noexcept
{
    const auto sporeBurstState = getFloatParameterValue (sporeBurstParameter, 0.0f) >= 0.5f;
    const auto risingEdge = sporeBurstState && ! lastSporeBurstState;
    lastSporeBurstState = sporeBurstState;

    if (! risingEdge)
        return;

    reseedClusterTargets();
}

void AudioPluginAudioProcessor::reseedClusterTargets() noexcept
{
    const auto growth = juce::jlimit (0.0f, 1.0f, getFloatParameterValue (growthParameter, 0.5f));
    targetClusterCount = juce::jlimit (2, maxClusters, 2 + static_cast<int> (std::round (growth * 2.0f)));

    for (int index = 0; index < maxClusters; ++index)
    {
        auto& cluster = clusters[static_cast<size_t> (index)];
        cluster.active = index < targetClusterCount;
        cluster.targetPanCenter = juce::jlimit (-1.0f, 1.0f, nextRandomBipolar() * 0.92f);
        cluster.targetPanSpread = 0.08f + (nextRandomFloat() * 0.32f);
        cluster.targetDelayMs = 110.0f + (nextRandomFloat() * 760.0f);
        cluster.targetDelaySpreadMs = 40.0f + (nextRandomFloat() * 180.0f);
        cluster.targetEnergy = 0.35f + (nextRandomFloat() * 0.55f);
        cluster.driftVelocity = nextRandomBipolar() * 0.48f;
        cluster.ageSeconds = 0.0f;

        if (! cluster.active)
            cluster.currentEnergy *= 0.7f;
    }
}

int AudioPluginAudioProcessor::getActiveClusterCount() const noexcept
{
    return static_cast<int> (std::count_if (clusters.begin(), clusters.end(),
                                            [] (const ClusterState& cluster) { return cluster.active; }));
}

int AudioPluginAudioProcessor::chooseClusterIndex() noexcept
{
    const auto activeClusterCount = getActiveClusterCount();
    if (activeClusterCount <= 0)
        return 0;

    float totalEnergy = 0.0f;
    for (const auto& cluster : clusters)
        if (cluster.active)
            totalEnergy += juce::jmax (0.01f, cluster.currentEnergy);

    auto selection = nextRandomFloat() * totalEnergy;
    for (int index = 0; index < maxClusters; ++index)
    {
        const auto& cluster = clusters[static_cast<size_t> (index)];
        if (! cluster.active)
            continue;

        selection -= juce::jmax (0.01f, cluster.currentEnergy);
        if (selection <= 0.0f)
            return index;
    }

    for (int index = 0; index < maxClusters; ++index)
        if (clusters[static_cast<size_t> (index)].active)
            return index;

    return 0;
}

void AudioPluginAudioProcessor::branchCluster() noexcept
{
    auto sourceIndex = chooseClusterIndex();
    auto freeIt = std::find_if (clusters.begin(), clusters.end(),
                                [] (const ClusterState& cluster) { return ! cluster.active; });

    if (freeIt == clusters.end())
        return;

    const auto& source = clusters[static_cast<size_t> (sourceIndex)];
    auto& newCluster = *freeIt;
    newCluster = source;
    newCluster.active = true;
    newCluster.targetPanCenter = juce::jlimit (-1.0f, 1.0f, source.targetPanCenter + (nextRandomBipolar() * 0.35f));
    newCluster.targetDelayMs = juce::jlimit (minimumGrainDelayMs, maximumGrainDelayMs, source.targetDelayMs + (nextRandomBipolar() * 140.0f));
    newCluster.targetEnergy = juce::jlimit (minimumClusterEnergy, maximumClusterEnergy, source.targetEnergy * 0.82f);
    newCluster.driftVelocity = nextRandomBipolar() * 0.44f;
    newCluster.ageSeconds = 0.0f;
}

void AudioPluginAudioProcessor::mergeClusters() noexcept
{
    int firstIndex = -1;
    int secondIndex = -1;

    for (int index = 0; index < maxClusters; ++index)
    {
        if (! clusters[static_cast<size_t> (index)].active)
            continue;

        if (firstIndex < 0)
            firstIndex = index;
        else
        {
            secondIndex = index;
            break;
        }
    }

    if (firstIndex < 0 || secondIndex < 0)
        return;

    auto& a = clusters[static_cast<size_t> (firstIndex)];
    auto& b = clusters[static_cast<size_t> (secondIndex)];
    a.targetPanCenter = 0.5f * (a.targetPanCenter + b.targetPanCenter);
    a.targetDelayMs = 0.5f * (a.targetDelayMs + b.targetDelayMs);
    a.targetPanSpread = juce::jmax (a.targetPanSpread, b.targetPanSpread) * 0.85f;
    a.targetDelaySpreadMs = juce::jmax (a.targetDelaySpreadMs, b.targetDelaySpreadMs) * 0.85f;
    a.targetEnergy = juce::jlimit (minimumClusterEnergy, maximumClusterEnergy, a.targetEnergy + (b.targetEnergy * 0.45f));
    b.active = false;
    b.targetEnergy = 0.0f;
    b.currentEnergy *= 0.5f;
}

void AudioPluginAudioProcessor::renderConductionBed (juce::AudioBuffer<float>& destinationBuffer,
                                                     juce::AudioBuffer<float>& bedDestinationBuffer,
                                                     int numSamples) noexcept
{
    auto* wetLeft = destinationBuffer.getWritePointer (0);
    auto* wetRight = destinationBuffer.getWritePointer (1);
    auto* bedLeftOut = bedDestinationBuffer.getWritePointer (0);
    auto* bedRightOut = bedDestinationBuffer.getWritePointer (1);
    const auto damping = juce::jlimit (0.0f, 1.0f, getFloatParameterValue (dampingParameter, 0.45f));
    const auto lowpassCoeff = juce::jlimit (0.05f, 0.6f, 0.55f - (damping * 0.45f));

    for (int sampleIndex = 0; sampleIndex < numSamples; ++sampleIndex)
    {
        float bedLeft = 0.0f;
        float bedRight = 0.0f;

        for (const auto& tap : conductionTaps)
        {
            const auto delaySamples = tap.delayMs * 0.001f * static_cast<float> (currentSampleRate);
            const auto sourceSample = delayBuffer.readDelaySamples (delaySamples);
            const auto angle = (tap.pan + 1.0f) * 0.25f * juce::MathConstants<float>::pi;
            const auto tapSample = sanitizeSample (sourceSample * tap.gain);
            bedLeft += tapSample * std::cos (angle);
            bedRight += tapSample * std::sin (angle);
        }

        conductionLowpassLeft += lowpassCoeff * (bedLeft - conductionLowpassLeft);
        conductionLowpassRight += lowpassCoeff * (bedRight - conductionLowpassRight);

        wetLeft[sampleIndex] = sanitizeSample (wetLeft[sampleIndex] + conductionLowpassLeft);
        wetRight[sampleIndex] = sanitizeSample (wetRight[sampleIndex] + conductionLowpassRight);
        bedLeftOut[sampleIndex] = sanitizeSample (bedLeftOut[sampleIndex] + conductionLowpassLeft);
        bedRightOut[sampleIndex] = sanitizeSample (bedRightOut[sampleIndex] + conductionLowpassRight);
    }
}

void AudioPluginAudioProcessor::applyStereoShaping (juce::AudioBuffer<float>& destinationBuffer,
                                                    juce::AudioBuffer<float>& sporeSourceBuffer,
                                                    juce::AudioBuffer<float>& bedSourceBuffer,
                                                    int numSamples) noexcept
{
    auto* wetLeft = destinationBuffer.getWritePointer (0);
    auto* wetRight = destinationBuffer.getWritePointer (1);
    auto* sporeLeft = sporeSourceBuffer.getWritePointer (0);
    auto* sporeRight = sporeSourceBuffer.getWritePointer (1);
    auto* bedLeft = bedSourceBuffer.getWritePointer (0);
    auto* bedRight = bedSourceBuffer.getWritePointer (1);

    for (int sampleIndex = 0; sampleIndex < numSamples; ++sampleIndex)
    {
        const auto sporeSideAmount = sporeSideMix.getNextValue();
        const auto bedMidAmount = bedMidMix.getNextValue();

        const auto sporeMid = 0.5f * (sporeLeft[sampleIndex] + sporeRight[sampleIndex]);
        const auto sporeSide = 0.5f * (sporeLeft[sampleIndex] - sporeRight[sampleIndex]);
        const auto shapedSporeSide = sporeSide * (1.0f + sporeSideAmount);
        const auto shapedSporeLeft = sporeMid + shapedSporeSide;
        const auto shapedSporeRight = sporeMid - shapedSporeSide;

        const auto bedMid = 0.5f * (bedLeft[sampleIndex] + bedRight[sampleIndex]);
        const auto bedSide = 0.5f * (bedLeft[sampleIndex] - bedRight[sampleIndex]);
        const auto shapedBedMid = bedMid * (1.0f + bedMidAmount);
        const auto shapedBedSide = bedSide * (1.0f - (bedMidAmount * 0.65f));
        const auto shapedBedLeft = shapedBedMid + shapedBedSide;
        const auto shapedBedRight = shapedBedMid - shapedBedSide;

        wetLeft[sampleIndex] = sanitizeSample (shapedSporeLeft + shapedBedLeft);
        wetRight[sampleIndex] = sanitizeSample (shapedSporeRight + shapedBedRight);
    }
}

void AudioPluginAudioProcessor::applyOutputSafety (juce::AudioBuffer<float>& wetSourceBuffer,
                                                   juce::AudioBuffer<float>& outputBuffer,
                                                   int totalNumInputChannels,
                                                   int totalNumOutputChannels,
                                                   int numSamples) noexcept
{
    auto* wetLeft = wetSourceBuffer.getWritePointer (0);
    auto* wetRight = wetSourceBuffer.getWritePointer (1);
    auto* outputLeft = outputBuffer.getWritePointer (0);
    auto* outputRight = totalNumOutputChannels > 1 ? outputBuffer.getWritePointer (1) : nullptr;

    const auto activeVoices = juce::jmax (1, getActiveGrainCountInternal());
    const auto conduction = juce::jlimit (0.0f, 1.0f, getFloatParameterValue (conductionParameter, 0.4f));
    const auto wetNormTarget = wetHeadroomTarget
                               * (1.0f / std::sqrt (static_cast<float> (activeVoices)))
                               * (1.0f - (conduction * 0.08f));
    wetNormalizationGain.setTargetValue (juce::jlimit (0.2f, 1.0f, wetNormTarget));

    for (int sampleIndex = 0; sampleIndex < numSamples; ++sampleIndex)
    {
        const auto smoothedWetNorm = wetNormalizationGain.getNextValue();
        const auto wetLeftSample = wetLeft[sampleIndex] * smoothedWetNorm;
        const auto wetRightSample = wetRight[sampleIndex] * smoothedWetNorm;

        const auto dryMix = 1.0f - dryWetMix.getNextValue();
        const auto wetMix = 1.0f - dryMix;
        const auto smoothedOutputGain = outputGain.getNextValue();

        const auto dryLeft = sanitizeSample (outputLeft[sampleIndex]);
        const auto dryRight = outputRight != nullptr
                                  ? sanitizeSample (outputRight[sampleIndex])
                                  : (totalNumInputChannels > 0 ? dryLeft : 0.0f);

        auto mixedLeft = ((dryLeft * dryMix) + (wetLeftSample * wetMix)) * smoothedOutputGain;
        auto mixedRight = ((dryRight * dryMix) + (wetRightSample * wetMix)) * smoothedOutputGain;

        mixedLeft = sanitizeSample (mixedLeft);
        mixedRight = sanitizeSample (mixedRight);

        const auto dcLeft = mixedLeft - dcBlockerX1[0] + (dcBlockerCoefficient * dcBlockerY1[0]);
        const auto dcRight = mixedRight - dcBlockerX1[1] + (dcBlockerCoefficient * dcBlockerY1[1]);
        dcBlockerX1[0] = mixedLeft;
        dcBlockerY1[0] = sanitizeSample (dcLeft);
        dcBlockerX1[1] = mixedRight;
        dcBlockerY1[1] = sanitizeSample (dcRight);

        const auto clippedLeft = std::tanh (softClipDrive * dcBlockerY1[0]) / std::tanh (softClipDrive);
        const auto clippedRight = std::tanh (softClipDrive * dcBlockerY1[1]) / std::tanh (softClipDrive);

        outputLeft[sampleIndex] = sanitizeSample (clippedLeft);
        if (outputRight != nullptr)
            outputRight[sampleIndex] = sanitizeSample (clippedRight);
    }
}

//==============================================================================
// This creates new instances of the plugin..
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new AudioPluginAudioProcessor();
}
