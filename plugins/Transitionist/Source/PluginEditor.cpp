#include "PluginEditor.h"

TransitionistAudioProcessorEditor::TransitionistAudioProcessorEditor(TransitionistAudioProcessor& p)
    : AudioProcessorEditor(&p), processorRef(p)
{
    setSize(600, 400);
}

TransitionistAudioProcessorEditor::~TransitionistAudioProcessorEditor()
{
}

void TransitionistAudioProcessorEditor::paint(juce::Graphics& g)
{
    g.fillAll(getLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId));

    g.setColour(juce::Colours::white);
    g.setFont(24.0f);
    g.drawFittedText("Transitionist - Stage 1", getLocalBounds(), juce::Justification::centred, 1);

    g.setFont(14.0f);
    g.drawFittedText("3 parameters implemented (throw, space, sweep)",
                     getLocalBounds().reduced(20).removeFromBottom(30),
                     juce::Justification::centred, 1);
}

void TransitionistAudioProcessorEditor::resized()
{
    // Layout will be added in Stage 3 (GUI) - WebView integration from
    // the finalized v4 mockup scaffolding.
}
