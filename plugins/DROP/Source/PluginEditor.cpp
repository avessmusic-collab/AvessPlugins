#include "PluginEditor.h"

DROPAudioProcessorEditor::DROPAudioProcessorEditor(DROPAudioProcessor& p)
    : AudioProcessorEditor(&p), processorRef(p)
{
    setResizable(true, true);
    setResizeLimits(800, 450, 1400, 790);
    setSize(1000, 560);
}

DROPAudioProcessorEditor::~DROPAudioProcessorEditor()
{
}

void DROPAudioProcessorEditor::paint(juce::Graphics& g)
{
    g.fillAll(getLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId));

    g.setColour(juce::Colours::white);
    g.setFont(24.0f);
    g.drawFittedText("DROP - Stage 1", getLocalBounds(), juce::Justification::centred, 1);

    g.setFont(14.0f);
    g.drawFittedText("19 parameters implemented",
                      getLocalBounds().reduced(20).removeFromBottom(30),
                      juce::Justification::centred, 1);
}

void DROPAudioProcessorEditor::resized()
{
    // Layout will be replaced by the WebView UI (finalized mockup v10) in
    // Stage 3 GUI Phase 4.1.
}
