#pragma once

#include <array>
#include <atomic>
#include <random>
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
    int getActiveGrainCount() const noexcept;

private:
    static constexpr int maxGrainVoices = 12;

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

    struct GrainVoice
    {
        bool active = false;
        float delaySamples = 0.0f;
        float readOffsetSamples = 0.0f;
        float readIncrement = 1.0f;
        float gain = 0.0f;
        float leftGain = 0.0f;
        float rightGain = 0.0f;
        int samplesRemaining = 0;
        int totalSamples = 0;
    };

    void resetDelayState() noexcept;
    void resetGrainState() noexcept;
    float getFreezeTargetWriteGain() const noexcept;
    float getFloatParameterValue (std::atomic<float>* parameter, float fallbackValue) const noexcept;
    void reseedRandomIfNeeded() noexcept;
    float nextRandomFloat() noexcept;
    float nextRandomBipolar() noexcept;
    int getActiveGrainCountInternal() const noexcept;
    void scheduleNextSpawnInterval() noexcept;
    void spawnGrainsForBlock (int numSamples) noexcept;
    bool spawnGrainVoice() noexcept;
    void renderWetGrains (juce::AudioBuffer<float>& wetBuffer, int numSamples) noexcept;

    juce::AudioProcessorValueTreeState parameters;
    std::atomic<float>* dryWetParameter = nullptr;
    std::atomic<float>* outputTrimDbParameter = nullptr;
    std::atomic<float>* densityParameter = nullptr;
    std::atomic<float>* sizeMsParameter = nullptr;
    std::atomic<float>* scatterParameter = nullptr;
    std::atomic<float>* spreadParameter = nullptr;
    std::atomic<float>* seedParameter = nullptr;
    std::atomic<float>* freezeParameter = nullptr;
    DelayBuffer delayBuffer;
    std::array<GrainVoice, maxGrainVoices> grainVoices {};
    juce::AudioBuffer<float> wetBuffer;
    juce::LinearSmoothedValue<float> writeGain;
    juce::LinearSmoothedValue<float> dryWetMix;
    juce::LinearSmoothedValue<float> outputGain;
    std::minstd_rand randomGenerator;
    double samplesUntilNextSpawn = 0.0;
    double currentSampleRate = 44100.0;
    int preparedBlockSize = 0;
    int lastSeedValue = 12345;
    std::atomic<float> lastDelayPreviewSample { 0.0f };
    std::atomic<float> lastWriteGain { 1.0f };
    std::atomic<int> lastActiveGrainCount { 0 };

    //==============================================================================
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioPluginAudioProcessor)
};
