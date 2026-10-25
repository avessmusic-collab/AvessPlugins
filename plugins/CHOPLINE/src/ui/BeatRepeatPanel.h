// CHOPLINE — BeatRepeatPanel (locked v17 `br` card, MAIN bottom bar)
// The ONLY Beat Repeat control surface (v17: no FX-page mirror): HOLD latch
// (fills red while latched), AUTO toggle (drives br_mode Auto↔Manual), EVERY
// interval combo, GRID combo, CHANCE and DECAY compact knobs (red accent).
// While a repeat is sounding or a hold is engaged the card shows a 1 px red
// ring (v17 beat_repeat_hold.panel). Red #ff5a4f is STRICTLY the BR accent.

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "../engine/Engine.h"
#include "CompactKnob.h"
#include "LookAndFeel.h"

namespace chopline::ui
{

class BeatRepeatPanel final : public juce::Component
{
public:
    BeatRepeatPanel (juce::AudioProcessorValueTreeState& apvts, engine::Engine& engineToUse);

    void animationTick(); // editor VBlank: red ring follows the engine state

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    engine::Engine& eng;

    juce::TextButton holdButton { "HOLD" };
    juce::ToggleButton autoToggle { "AUTO" };
    juce::ComboBox intervalBox, gridBox;
    CompactKnob chanceKnob { "CHANCE", true };
    CompactKnob decayKnob { "DECAY", true };

    std::unique_ptr<juce::ButtonParameterAttachment> holdAttachment;
    std::unique_ptr<juce::ParameterAttachment> modeAttachment; // AUTO ↔ br_mode
    std::unique_ptr<juce::ComboBoxParameterAttachment> intervalAttachment, gridAttachment;
    std::unique_ptr<juce::SliderParameterAttachment> chanceAttachment, decayAttachment;

    juce::RangedAudioParameter* modeParam = nullptr;
    bool ringLit = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BeatRepeatPanel)
};

} // namespace chopline::ui
