#include "PluginProcessor.h"
#include "PluginEditor.h"

#include <cmath>

namespace
{
constexpr auto stateType = "HyphaeState";
constexpr auto stateVersionProperty = "stateVersion";

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
    freezeParameter = parameters.getRawParameterValue (ParameterIds::freeze);
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

    const auto delayCapacitySamples = juce::jlimit (1,
                                                    hardCapDelaySamples,
                                                    static_cast<int> (std::ceil (currentSampleRate * maxDelaySeconds)));

    writeGain.reset (currentSampleRate, 0.03);
    delayBuffer.prepare (currentSampleRate, delayCapacitySamples);
    resetDelayState();
}

void AudioPluginAudioProcessor::releaseResources()
{
    resetDelayState();
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

    writeGain.setTargetValue (getFreezeTargetWriteGain());

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

void AudioPluginAudioProcessor::resetDelayState() noexcept
{
    delayBuffer.clear();
    writeGain.setCurrentAndTargetValue (1.0f);
    lastWriteGain.store (1.0f, std::memory_order_relaxed);
    lastDelayPreviewSample.store (0.0f, std::memory_order_relaxed);
}

float AudioPluginAudioProcessor::getFreezeTargetWriteGain() const noexcept
{
    if (freezeParameter == nullptr)
        return 1.0f;

    return *freezeParameter >= 0.5f ? 0.0f : 1.0f;
}

//==============================================================================
// This creates new instances of the plugin..
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new AudioPluginAudioProcessor();
}
