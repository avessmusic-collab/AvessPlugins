#include "PluginEditor.h"

PRISMAudioProcessorEditor::PRISMAudioProcessorEditor(PRISMAudioProcessor& p)
    : AudioProcessorEditor(&p), processorRef(p)
{
    constrainer.setMinimumSize(770, 504);
    constrainer.setFixedAspectRatio(1100.0 / 720.0);
    setConstrainer(&constrainer);
    setResizable(true, true);

    auto& st = processorRef.apvts.state;
    const int w = juce::jmax(770, (int) st.getProperty(prism::ids::uiWidth, 1100));
    const int h = juce::jmax(504, (int) st.getProperty(prism::ids::uiHeight, 720));
    setSize(w, h);
}

PRISMAudioProcessorEditor::~PRISMAudioProcessorEditor() = default;

void PRISMAudioProcessorEditor::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff101215));

    g.setColour(juce::Colours::white);
    g.setFont(juce::FontOptions(28.0f));
    g.drawFittedText("PRISM - Stage 1", getLocalBounds(), juce::Justification::centred, 1);

    g.setColour(juce::Colours::white.withAlpha(0.6f));
    g.setFont(juce::FontOptions(14.0f));
    g.drawFittedText(juce::String(prism::ids::kParameterCount) + " parameters implemented",
                     getLocalBounds().reduced(20).removeFromBottom(30),
                     juce::Justification::centred, 1);
}

void PRISMAudioProcessorEditor::resized()
{
}
