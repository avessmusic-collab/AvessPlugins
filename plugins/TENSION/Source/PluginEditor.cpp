#include "PluginEditor.h"

TENSIONAudioProcessorEditor::TENSIONAudioProcessorEditor (TENSIONAudioProcessor& p)
    : AudioProcessorEditor (&p), processorRef (p)
{
    setSize (600, 400);
}

TENSIONAudioProcessorEditor::~TENSIONAudioProcessorEditor() = default;

void TENSIONAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff0b0b0d));

    auto bounds = getLocalBounds();

    g.setColour (juce::Colour (0xffe8541f));
    g.setFont (juce::FontOptions (32.0f).withStyle ("Bold"));
    g.drawFittedText ("TENSION", bounds, juce::Justification::centred, 1);

    g.setColour (juce::Colours::white.withAlpha (0.45f));
    g.setFont (juce::FontOptions (13.0f));
    g.drawFittedText ("Stage 1 - 5 parameters implemented",
                      bounds.reduced (20).removeFromBottom (30),
                      juce::Justification::centred, 1);
}

void TENSIONAudioProcessorEditor::resized()
{
}
