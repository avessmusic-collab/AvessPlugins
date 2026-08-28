#include "PluginProcessor.h"
#include "PluginEditor.h"

KickDesigner2AudioProcessorEditor::KickDesigner2AudioProcessorEditor (KickDesigner2AudioProcessor& p)
    : AudioProcessorEditor (&p), processorRef (p)
{
    setResizable (true, true);
    setResizeLimits (640, 400, 1920, 1200);

    if (auto* constrainer = getConstrainer())
        constrainer->setFixedAspectRatio (640.0 / 400.0);

    setSize (960, 600);
}

KickDesigner2AudioProcessorEditor::~KickDesigner2AudioProcessorEditor() = default;

void KickDesigner2AudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff17171b));

    g.setColour (juce::Colours::white);
    g.setFont (juce::FontOptions (22.0f, juce::Font::bold));
    g.drawFittedText (juce::String::fromUTF8 ("Kick Designer 2  \xe2\x80\x94  Stage 1"),
                      getLocalBounds().reduced (24), juce::Justification::centred, 1);

    g.setColour (juce::Colours::grey);
    g.setFont (juce::FontOptions (13.0f));
    g.drawFittedText (juce::String::fromUTF8 ("Foundation + Shell  \xc2\xb7  59 parameters  \xc2\xb7  native UI arrives in Stage 3"),
                      getLocalBounds().reduced (24).removeFromBottom (44),
                      juce::Justification::centred, 1);
}

void KickDesigner2AudioProcessorEditor::resized()
{
    juce::ignoreUnused (processorRef);
}
