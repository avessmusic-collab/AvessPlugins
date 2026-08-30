#pragma once

#include <memory>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

namespace kickr
{
    /**
        Stage 3 Phase 3.1 — compound rotary control.

        A `juce::Slider` (RotaryHorizontalVerticalDrag, no text box) + a caption label
        + a value readout drawn in paint(). Mouse drag + wheel + double-click-to-default
        + tooltip. Bound to an APVTS float parameter through
        `AudioProcessorValueTreeState::SliderAttachment`, which brackets
        begin/endChangeGesture automatically (Pattern #23 — native equivalent).

        The value readout is routed through the parameter's own text function, so
        `fundamental` shows "A1", dB params show "+3.0 dB", etc.

        `Size` only picks font sizes / arc weight — the knob diameter always adapts to
        the bounds it is given, so the whole UI stays scale-driven.
    */
    class KickrKnob : public juce::Component
    {
    public:
        enum class Size { Small, Medium, Large };

        KickrKnob (juce::AudioProcessorValueTreeState& apvts,
                   juce::StringRef paramID,
                   const juce::String& caption,
                   juce::Colour accent,
                   bool bipolar = false,
                   Size size = Size::Medium);

        void resized() override;
        void paint (juce::Graphics&) override;

    private:
        void refreshReadout();

        juce::Slider slider;
        juce::Label  captionLabel;
        juce::String readoutText;
        juce::RangedAudioParameter* param { nullptr };
        Size knobSize;

        std::unique_ptr<juce::SliderParameterAttachment> attachment;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (KickrKnob)
    };

    /**
        Bool parameter -> a pill / segment toggle (ButtonAttachment). Thin wrapper so the
        editor can treat it like any other laid-out control.
    */
    class KickrToggle : public juce::Component
    {
    public:
        KickrToggle (juce::AudioProcessorValueTreeState& apvts,
                     juce::StringRef paramID,
                     const juce::String& text,
                     juce::Colour accent);

        void resized() override;

    private:
        juce::ToggleButton button;
        std::unique_ptr<juce::ButtonParameterAttachment> attachment;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (KickrToggle)
    };
}
