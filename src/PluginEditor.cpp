#include "PluginProcessor.h"
#include "PluginEditor.h"

//==============================================================================
AudioPluginAudioProcessorEditor::AudioPluginAudioProcessorEditor (AudioPluginAudioProcessor& p)
    : AudioProcessorEditor (&p), processorRef (p)
{
    setSize (420, 240);
}

AudioPluginAudioProcessorEditor::~AudioPluginAudioProcessorEditor()
{
}

//==============================================================================
void AudioPluginAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour::fromRGB (21, 24, 26));

    auto bounds = getLocalBounds().reduced (20);

    g.setColour (juce::Colours::white);
    g.setFont (juce::FontOptions (26.0f, juce::Font::bold));
    g.drawText ("Hyphae", bounds.removeFromTop (36), juce::Justification::centredLeft);

    g.setColour (juce::Colours::lightgrey);
    g.setFont (juce::FontOptions (14.0f));
    g.drawFittedText ("M1 state skeleton: APVTS + versioned save/restore", bounds.removeFromTop (26),
                      juce::Justification::centredLeft, 1);

    bounds.removeFromTop (12);

    const auto parameterCount = processorRef.getValueTreeState().state.getNumChildren()
                                + processorRef.getParameters().size();
    const auto stateVersion = processorRef.getValueTreeState().state.getProperty ("stateVersion");
    const auto bufferSamples = processorRef.getDelayBufferCapacitySamples();
    const auto previewSample = processorRef.getLastDelayPreviewSample();
    const auto currentWriteGain = processorRef.getLastWriteGain();

    g.drawFittedText ("Parameters: " + juce::String (parameterCount), bounds.removeFromTop (22),
                      juce::Justification::centredLeft, 1);
    g.drawFittedText ("State version: " + stateVersion.toString(), bounds.removeFromTop (22),
                      juce::Justification::centredLeft, 1);
    g.drawFittedText ("Delay buffer: " + juce::String (bufferSamples) + " samples", bounds.removeFromTop (22),
                      juce::Justification::centredLeft, 1);
    g.drawFittedText ("Freeze write gain: " + juce::String (currentWriteGain, 2), bounds.removeFromTop (22),
                      juce::Justification::centredLeft, 1);
    g.drawFittedText ("250 ms preview sample: " + juce::String (previewSample, 3), bounds.removeFromTop (22),
                      juce::Justification::centredLeft, 1);
    g.drawFittedText ("Next up: grain pool, conduction bed, and real controls.", bounds.removeFromTop (22),
                      juce::Justification::centredLeft, 1);
}

void AudioPluginAudioProcessorEditor::resized()
{
    // This is generally where you'll want to lay out the positions of any
    // subcomponents in your editor..
}
