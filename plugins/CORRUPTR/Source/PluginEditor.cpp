#include "PluginEditor.h"

CORRUPTRAudioProcessorEditor::CORRUPTRAudioProcessorEditor(CORRUPTRAudioProcessor& p)
    : AudioProcessorEditor(&p), processorRef(p)
{
    setSize(1200, 800);
}

CORRUPTRAudioProcessorEditor::~CORRUPTRAudioProcessorEditor()
{
}

void CORRUPTRAudioProcessorEditor::paint(juce::Graphics& g)
{
    g.fillAll(getLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId));

    g.setColour(juce::Colours::white);
    g.setFont(24.0f);
    g.drawFittedText("CORRUPTR - Stage 1", getLocalBounds(), juce::Justification::centred, 1);

    g.setFont(14.0f);
    g.drawFittedText("94 parameters implemented",
                     getLocalBounds().reduced(20).removeFromBottom(30),
                     juce::Justification::centred, 1);
}

void CORRUPTRAudioProcessorEditor::resized()
{
    // Layout will be added in Stage 3 (GUI)
}
