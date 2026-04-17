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
    seedParameter = parameters.getRawParameterValue (ParameterIds::seed);
    freezeParameter = parameters.getRawParameterValue (ParameterIds::freeze);
    randomGenerator.seed (static_cast<std::minstd_rand::result_type> (lastSeedValue));
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
    delayBuffer.prepare (currentSampleRate, delayCapacitySamples);
    wetBuffer.setSize (2, preparedBlockSize, false, false, true);
    resetDelayState();
    resetGrainState();
    reseedRandomIfNeeded();
    scheduleNextSpawnInterval();
}

void AudioPluginAudioProcessor::releaseResources()
{
    resetDelayState();
    resetGrainState();
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

    writeGain.setTargetValue (getFreezeTargetWriteGain());
    dryWetMix.setTargetValue (getFloatParameterValue (dryWetParameter, 0.5f));

    const auto outputTrimDb = getFloatParameterValue (outputTrimDbParameter, 0.0f);
    outputGain.setTargetValue (juce::Decibels::decibelsToGain (outputTrimDb));

    spawnGrainsForBlock (numSamples);
    wetBuffer.clear();

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

    renderWetGrains (wetBuffer, numSamples);

    auto* wetLeft = wetBuffer.getWritePointer (0);
    auto* wetRight = wetBuffer.getWritePointer (1);
    auto* outputLeft = buffer.getWritePointer (0);
    auto* outputRight = buffer.getNumChannels() > 1 ? buffer.getWritePointer (1) : nullptr;

    for (int sampleIndex = 0; sampleIndex < numSamples; ++sampleIndex)
    {
        const auto dryMix = 1.0f - dryWetMix.getNextValue();
        const auto wetMix = 1.0f - dryMix;
        const auto smoothedOutputGain = outputGain.getNextValue();

        const auto dryLeft = sanitizeSample (outputLeft[sampleIndex]);
        const auto dryRight = outputRight != nullptr ? sanitizeSample (outputRight[sampleIndex]) : dryLeft;
        const auto mixedLeft = ((dryLeft * dryMix) + (wetLeft[sampleIndex] * wetMix)) * smoothedOutputGain;
        const auto mixedRight = ((dryRight * dryMix) + (wetRight[sampleIndex] * wetMix)) * smoothedOutputGain;

        outputLeft[sampleIndex] = sanitizeSample (mixedLeft);
        if (outputRight != nullptr)
            outputRight[sampleIndex] = sanitizeSample (mixedRight);
    }

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
}

void AudioPluginAudioProcessor::resetGrainState() noexcept
{
    for (auto& voice : grainVoices)
        voice = {};

    dryWetMix.setCurrentAndTargetValue (getFloatParameterValue (dryWetParameter, 0.5f));
    outputGain.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (getFloatParameterValue (outputTrimDbParameter, 0.0f)));
    samplesUntilNextSpawn = 0.0;
    lastActiveGrainCount.store (0, std::memory_order_relaxed);
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
    const auto delayMs = juce::jlimit (minimumGrainDelayMs,
                                       maximumGrainDelayMs,
                                       120.0f + (scatter * 680.0f) + (nextRandomFloat() * 320.0f));

    const auto pan = juce::jlimit (-1.0f, 1.0f, nextRandomBipolar() * spread);
    const auto angle = (pan + 1.0f) * 0.25f * juce::MathConstants<float>::pi;
    const auto drift = juce::jmap (nextRandomFloat(), minimumRateDrift, maximumRateDrift);

    freeVoice->active = true;
    freeVoice->delaySamples = delayMs * 0.001f * static_cast<float> (currentSampleRate);
    freeVoice->readOffsetSamples = 0.0f;
    freeVoice->readIncrement = drift;
    freeVoice->gain = voiceBaseGain * juce::jmap (scatter, 1.0f, 0.75f);
    freeVoice->leftGain = std::cos (angle);
    freeVoice->rightGain = std::sin (angle);
    freeVoice->samplesRemaining = durationSamples;
    freeVoice->totalSamples = durationSamples;

    return true;
}

void AudioPluginAudioProcessor::renderWetGrains (juce::AudioBuffer<float>& destinationBuffer, int numSamples) noexcept
{
    auto* wetLeft = destinationBuffer.getWritePointer (0);
    auto* wetRight = destinationBuffer.getWritePointer (1);

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

            wetLeft[sampleIndex] = sanitizeSample (wetLeft[sampleIndex] + (sample * voice.leftGain));
            wetRight[sampleIndex] = sanitizeSample (wetRight[sampleIndex] + (sample * voice.rightGain));

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
    }
}

//==============================================================================
// This creates new instances of the plugin..
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new AudioPluginAudioProcessor();
}
