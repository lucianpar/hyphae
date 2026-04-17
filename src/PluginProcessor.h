#pragma once

#include <atomic>
#include <vector>

#include <juce_audio_processors/juce_audio_processors.h>

//==============================================================================
class AudioPluginAudioProcessor final : public juce::AudioProcessor
{
public:
    static constexpr int currentStateVersion = 1;
    static constexpr double maxDelaySeconds = 8.0;
    static constexpr double maxSupportedSampleRate = 192000.0;
    static constexpr int hardCapDelaySamples = static_cast<int> (maxDelaySeconds * maxSupportedSampleRate);

    //==============================================================================
    AudioPluginAudioProcessor();
    ~AudioPluginAudioProcessor() override;

    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    //==============================================================================
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;

    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;

    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    //==============================================================================
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;

    //==============================================================================
    const juce::String getName() const override;

    bool acceptsMidi() const override;
    bool producesMidi() const override;
    bool isMidiEffect() const override;
    double getTailLengthSeconds() const override;

    //==============================================================================
    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int index, const juce::String& newName) override;

    //==============================================================================
    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState& getValueTreeState() noexcept;
    const juce::AudioProcessorValueTreeState& getValueTreeState() const noexcept;
    float getLastDelayPreviewSample() const noexcept;
    float getLastWriteGain() const noexcept;
    int getDelayBufferCapacitySamples() const noexcept;

private:
    class DelayBuffer
    {
    public:
        void prepare (double sampleRate, int maximumDelaySamples);
        void clear() noexcept;
        void write (float sample) noexcept;
        float readDelaySamples (float delaySamples) const noexcept;
        int getCapacitySamples() const noexcept;

    private:
        std::vector<float> samples;
        int writeIndex = 0;
        int validSampleCount = 0;
    };

    void resetDelayState() noexcept;
    float getFreezeTargetWriteGain() const noexcept;

    juce::AudioProcessorValueTreeState parameters;
    std::atomic<float>* freezeParameter = nullptr;
    DelayBuffer delayBuffer;
    juce::LinearSmoothedValue<float> writeGain;
    double currentSampleRate = 44100.0;
    std::atomic<float> lastDelayPreviewSample { 0.0f };
    std::atomic<float> lastWriteGain { 1.0f };

    //==============================================================================
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioPluginAudioProcessor)
};
