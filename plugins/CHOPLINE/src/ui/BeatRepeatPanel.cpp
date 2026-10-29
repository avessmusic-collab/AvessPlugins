// CHOPLINE — BeatRepeatPanel implementation. See header.

#include "BeatRepeatPanel.h"

#include "../state/Parameters.h"

namespace chopline::ui
{

using LnF = ChoplineLookAndFeel;

BeatRepeatPanel::BeatRepeatPanel (juce::AudioProcessorValueTreeState& apvts,
                                  engine::Engine& engineToUse)
    : eng (engineToUse)
{
    namespace pid = params::id;

    // HOLD: latching toggle, fills BR red while on (v17 beat_repeat_hold).
    holdButton.setClickingTogglesState (true);
    holdButton.setColour (juce::TextButton::buttonOnColourId, LnF::repeat);
    holdButton.setTooltip ("Latch a manual repeat (H key is momentary)");
    addAndMakeVisible (holdButton);
    if (auto* p = apvts.getParameter (pid::brHold))
        holdAttachment = std::make_unique<juce::ButtonParameterAttachment> (*p, holdButton);

    // AUTO: the v17 toggle drives br_mode Auto ↔ Manual (Off is an advanced /
    // automation value; it also shows as AUTO-off here).
    autoToggle.setTooltip ("Auto: roll CHANCE at every EVERY boundary");
    addAndMakeVisible (autoToggle);
    modeParam = apvts.getParameter (pid::brMode);
    if (modeParam != nullptr)
    {
        modeAttachment = std::make_unique<juce::ParameterAttachment> (
            *modeParam,
            [this] (float denormalised)
            {
                autoToggle.setToggleState (static_cast<int> (denormalised) == 0,
                                           juce::dontSendNotification);
            });
        modeAttachment->sendInitialUpdate();
        autoToggle.onClick = [this]
        {
            if (modeAttachment != nullptr)
                modeAttachment->setValueAsCompleteGesture (
                    autoToggle.getToggleState() ? 0.0f : 1.0f); // Auto : Manual
        };
    }

    intervalBox.setName ("EVERY");
    intervalBox.addItemList ({ "1/4", "1/2", "1 bar", "2 bars", "4 bars" }, 1);
    intervalBox.setTooltip ("Repeat decision interval");
    addAndMakeVisible (intervalBox);
    if (auto* p = dynamic_cast<juce::RangedAudioParameter*> (apvts.getParameter (pid::brInterval)))
        intervalAttachment = std::make_unique<juce::ComboBoxParameterAttachment> (*p, intervalBox);

    gridBox.setName ("GRID");
    gridBox.addItemList ({ "1/2", "1/4", "1/8", "1/16", "1/32", "1/64",
                           "1/8T", "1/16T", "1/32T" }, 1);
    gridBox.setTooltip ("Repeat slice size");
    addAndMakeVisible (gridBox);
    if (auto* p = dynamic_cast<juce::RangedAudioParameter*> (apvts.getParameter (pid::brGrid)))
        gridAttachment = std::make_unique<juce::ComboBoxParameterAttachment> (*p, gridBox);

    const auto setupKnob = [&apvts] (CompactKnob& knob, const char* paramId,
                                     std::unique_ptr<juce::SliderParameterAttachment>& attachment)
    {
        knob.getSlider().setColour (juce::Slider::rotarySliderFillColourId, LnF::repeat);
        knob.valueSuffix = "%";
        if (auto* p = dynamic_cast<juce::RangedAudioParameter*> (apvts.getParameter (paramId)))
            attachment = std::make_unique<juce::SliderParameterAttachment> (*p, knob.getSlider());
    };
    setupKnob (chanceKnob, pid::brChance, chanceAttachment);
    chanceKnob.setTooltip ("Probability of a repeat at every EVERY boundary (Auto)");
    addAndMakeVisible (chanceKnob);
    setupKnob (decayKnob, pid::brDecay, decayAttachment);
    decayKnob.setTooltip ("Volume drop per repeat");
    addAndMakeVisible (decayKnob);
}

void BeatRepeatPanel::animationTick()
{
    const auto lit = eng.isBeatRepeatActive();
    if (lit != ringLit)
    {
        ringLit = lit;
        repaint();
    }
}

void BeatRepeatPanel::resized()
{
    // v17 absolute positions, local to the card at (15, 593).
    holdButton.setBounds (12, 33, 120, 28);
    autoToggle.setBounds (12, 71, 120, 28);
    intervalBox.setBounds (146, 33, 130, 28);
    gridBox.setBounds (146, 71, 130, 28);
    chanceKnob.setBounds (332, 44, 50, 54);
    decayKnob.setBounds (438, 44, 50, 54);
}

void BeatRepeatPanel::paint (juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat();
    g.setColour (LnF::paper);
    g.fillRoundedRectangle (b, 16.0f);
    g.setColour (LnF::hairline());
    g.drawRoundedRectangle (b.reduced (0.5f), 16.0f, 1.0f);

    g.setColour (LnF::label);
    g.setFont (LnF::labelFont (8.5f));
    g.drawText ("BEAT REPEAT", 12, 8, 160, 12, juce::Justification::centredLeft, false);

    if (ringLit) // 1 px red ring while repeating / holding (v17)
    {
        g.setColour (LnF::repeat);
        g.drawRoundedRectangle (b.reduced (0.5f), 16.0f, 1.0f);
    }
}

} // namespace chopline::ui
