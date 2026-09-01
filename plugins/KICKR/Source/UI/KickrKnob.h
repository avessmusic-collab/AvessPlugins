#pragma once

#include <memory>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

namespace kickr
{
    /**
        2026-09-01 (user request): hold Shift while dragging a knob for finer, slower
        control — e.g. for micro-adjustments where the normal full-range-in-250-px feel
        is too coarse. `juce::Slider` has no such mode built in — that's the only reason
        this subclass exists.

        Fix (same day, user report — "the knob snaps back to where it started before you
        press shift"): `Slider::mouseDrag`'s rotary drag is relative-to-mouseDown — it
        remembers the mouse position AND value from the original `mouseDown`, and computes
        `value = valueAtMouseDown + (mouseDelta / sensitivity) * range` on every move.
        Changing `setMouseDragSensitivity()` mid-drag (the first version of this fix) keeps
        that SAME `mouseDelta` but reinterprets it under the new sensitivity — a sudden
        jump toward `valueAtMouseDown`, i.e. back toward wherever the drag started. The
        fix: when Shift is pressed or released mid-drag, synthesize a `mouseUp` + fresh
        `mouseDown` right at the current mouse position (with the CURRENT value already
        applied) before changing sensitivity — that resets Slider's internal "mouseDown"
        reference to right now, so the new sensitivity applies smoothly from the current
        position with no jump. (Briefly closes + reopens the automation gesture — harmless;
        the value itself never jumps, which is what actually matters.)
    */
    class KickrSlider : public juce::Slider
    {
    public:
        void mouseDown (const juce::MouseEvent& e) override
        {
            shiftWasDown = e.mods.isShiftDown();
            applySensitivity (shiftWasDown);
            juce::Slider::mouseDown (e);
        }

        void mouseDrag (const juce::MouseEvent& e) override
        {
            const bool shiftIsDown = e.mods.isShiftDown();
            if (shiftIsDown != shiftWasDown)
            {
                juce::Slider::mouseUp (e);     // close the drag AT THE CURRENT VALUE...
                applySensitivity (shiftIsDown);
                juce::Slider::mouseDown (e);   // ...and reopen it from right here, right now
                shiftWasDown = shiftIsDown;
            }
            juce::Slider::mouseDrag (e);
        }

    private:
        void applySensitivity (bool fine)
        {
            if (baseSensitivity <= 0)
                baseSensitivity = getMouseDragSensitivity();
            setMouseDragSensitivity (fine ? baseSensitivity * 5 : baseSensitivity);
        }

        int  baseSensitivity { 0 };   // captured from JUCE's own default, first use
        bool shiftWasDown    { false };
    };

    /**
        Stage 3 Phase 3.1 — compound rotary control.

        A `KickrSlider` (RotaryHorizontalVerticalDrag, no text box) + a caption label
        + a value readout drawn in paint(). Mouse drag (hold Shift for a 5x finer/slower
        micro-adjust — 2026-09-01) + wheel + double-click-to-default + tooltip. Bound to
        an APVTS float parameter through
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
        void enablementChanged() override;

    private:
        void refreshReadout();

        KickrSlider slider;
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
