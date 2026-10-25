// CHOPLINE — MasterPanel (locked v17 `master` card, MAIN bottom bar)
// FILTER / DIST / WIDTH / OUT compact knobs (shared APVTS values with the FX
// page through separate attachments), the small hairline BPM readout (host
// BPM, or the free-run BPM in standalone — v17: BPM lives in MASTER, not the
// header), the '…' button opening the MASTER–MORE flyout (CEILING, DLY RTN,
// VERB RTN — v17 fl_mastermore) and the limiter lamp (blue while limiting).

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "../engine/Engine.h"
#include "CompactKnob.h"
#include "LookAndFeel.h"

namespace chopline::ui
{

class MasterPanel final : public juce::Component
{
public:
    MasterPanel (juce::AudioProcessorValueTreeState& apvts, engine::Engine& engineToUse);

    void animationTick(); // BPM readout + limiter lamp

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    class LimiterLamp final : public juce::Component
    {
    public:
        float grDb = 0.0f; // ≤ 0, set by the panel tick
        void paint (juce::Graphics&) override;
    };

    void openMoreFlyout();

    juce::AudioProcessorValueTreeState& state;
    engine::Engine& eng;

    CompactKnob filterKnob { "FILTER", true };
    CompactKnob distKnob { "DIST", true };
    CompactKnob widthKnob { "WIDTH", true };
    CompactKnob outKnob { "OUT", true };
    juce::TextButton moreButton { juce::CharPointer_UTF8 ("\xe2\x80\xa6") }; // …
    LimiterLamp lamp;

    std::unique_ptr<juce::SliderParameterAttachment> filterAttachment, distAttachment,
        widthAttachment, outAttachment;

    double shownBpm = 0.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MasterPanel)
};

} // namespace chopline::ui
