// CHOPLINE — PluginEditor implementation (Phase 0 empty editor).

#include "PluginEditor.h"

namespace chopline
{

ChoplineAudioProcessorEditor::ChoplineAudioProcessorEditor (ChoplineAudioProcessor& p)
    : AudioProcessorEditor (p), processorRef (p)
{
    // processorRef is unused until Phase 1 (attachments land then).
    juce::ignoreUnused (processorRef);

    setLookAndFeel (&lookAndFeel);

    phaseLabel.setText ("CHOPLINE — Phase 0", juce::dontSendNotification);
    phaseLabel.setJustificationType (juce::Justification::centred);
    phaseLabel.setColour (juce::Label::textColourId, ui::ChoplineLookAndFeel::ink);
    addAndMakeVisible (phaseLabel);

    // Resizable 75%–200% of 1100x720, fixed aspect ratio (spec §9).
    setResizable (true, true);
    setResizeLimits (defaultWidth * 3 / 4, defaultHeight * 3 / 4,
                     defaultWidth * 2,     defaultHeight * 2);
    if (auto* constrainer = getConstrainer())
        constrainer->setFixedAspectRatio (static_cast<double> (defaultWidth) / defaultHeight);

    setSize (defaultWidth, defaultHeight);
}

ChoplineAudioProcessorEditor::~ChoplineAudioProcessorEditor()
{
    setLookAndFeel (nullptr);
}

void ChoplineAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (ui::ChoplineLookAndFeel::paper);
}

void ChoplineAudioProcessorEditor::resized()
{
    phaseLabel.setBounds (getLocalBounds());
}

} // namespace chopline
