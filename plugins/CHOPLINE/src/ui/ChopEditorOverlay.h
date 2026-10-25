// CHOPLINE — ChopEditorOverlay (locked v17 `overlay` + `chop_sample_editor`)
// The chop detail overlay: an 800x468 panel at (150,120) over a scrim. Top is
// the Simpler-style chop SAMPLE EDITOR (zoomed waveform of the selected chop
// ±30%, clamped to the neighbour slices): draggable START/END flags (blue),
// fade-in/out corner grips with wedge envelopes, loop brace + crossfade
// diamond (Loop mode only), seconds ruler, direction chevron, and
// click-to-audition from that point (playhead sweeps; reverse plays
// backwards; Loop wraps). Below: MODE | REVERSE | SNAP ZERO-X | WARP seg, the
// START/END/LENGTH/FADE/LOOP readouts, and the knob row PITCH RELEASE DELAY
// VERB UNISON XFADE. All controls edit the per-chop model through the engine
// (same fields the FX page mirrors — v17 shared_params).

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "../engine/Engine.h"
#include "CompactKnob.h"
#include "SegControl.h"

namespace chopline::ui
{

class ChopEditorOverlay final : public juce::Component,
                                private juce::ChangeListener
{
public:
    explicit ChopEditorOverlay (engine::Engine& engineToUse);
    ~ChopEditorOverlay() override;

    /// Editor hides the overlay through this (Esc / scrim / close button).
    std::function<void()> onClose;

    void setChop (int chopIndex);
    int getChop() const noexcept { return chop; }

    /// ~60 fps tick from the editor's VBlankAttachment (audition playhead).
    void animationTick();

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override; // scrim click closes
    bool keyPressed (const juce::KeyPress&) override;  // Esc closes
    void visibilityChanged() override;

private:
    // Effective chop view in seconds (replicates the snapshot clamp rules).
    struct ChopView
    {
        bool valid = false;
        double baseStart = 0, baseEnd = 0;   // marker bounds
        double prevBound = 0, nextBound = 0; // trim limits (v17)
        double effStart = 0, effEnd = 0;     // trimmed bounds
        double minLen = 0.02;
    };
    ChopView computeView() const;

    // ---- the sample editor display (v17 chop_editor, 768x260) ------------------
    class EditorDisplay final : public juce::Component
    {
    public:
        explicit EditorDisplay (ChopEditorOverlay& o) : owner (o) {}

        enum class Handle { none, startFlag, endFlag, fadeIn, fadeOut,
                            loopStart, loopEnd, xfade };
        Handle currentDrag() const noexcept { return drag; }

        void refitView(); // chop ±30% clamped to neighbours (on open / chop change)
        void animationTick();

        void paint (juce::Graphics&) override;
        void mouseMove (const juce::MouseEvent&) override;
        void mouseExit (const juce::MouseEvent&) override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseDrag (const juce::MouseEvent&) override;
        void mouseUp (const juce::MouseEvent&) override;
        void mouseDoubleClick (const juce::MouseEvent&) override;

    private:
        float timeToX (double sec) const;
        double xToTime (float x) const;
        Handle handleAt (juce::Point<float>) const;
        void applyDrag (const juce::MouseEvent&);
        void drawWave (juce::Graphics&, juce::Rectangle<float> area, const ChopView&);

        ChopEditorOverlay& owner;
        double viewStart = 0.0, viewLen = 0.0; // seconds
        Handle drag = Handle::none, hover = Handle::none;
        double dragRefMs = 0.0; // xfade drag reference
        float dragRefX = 0.0f;
        bool wasAnimating = false;
    };

    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void refresh(); // sync every widget from the engine (quiet)

    engine::ChopSettings settings() const { return eng.getChopSettings (chop); }
    void setSettings (const engine::ChopSettings& s) { eng.setChopSettings (chop, s); }

    engine::Engine& eng;
    int chop = 0;
    bool refreshing = false;

    static constexpr int panelX = 150, panelY = 120, panelW = 800, panelH = 468;

    // Widgets (positions are v17 canvas-absolute; the overlay fills the canvas).
    class CloseButton final : public juce::Button
    {
    public:
        CloseButton() : juce::Button ("Close") {}
        void paintButton (juce::Graphics&, bool, bool) override;
    };

    EditorDisplay display { *this };
    juce::ComboBox modeBox;
    juce::ToggleButton reverseToggle { "Reverse" };
    juce::ToggleButton snapToggle { "Snap zero-X" };
    SegControl warpSeg;
    CloseButton closeButton;
    CompactKnob pitchKnob { "PITCH", true };
    CompactKnob releaseKnob { "RELEASE", true };
    CompactKnob delayKnob { "DELAY", true };
    CompactKnob verbKnob { "VERB", true };
    CompactKnob unisonKnob { "UNISON", true };
    CompactKnob xfadeKnob { "XFADE", true };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChopEditorOverlay)
};

} // namespace chopline::ui
