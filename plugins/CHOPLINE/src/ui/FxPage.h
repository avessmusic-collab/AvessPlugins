// CHOPLINE — FxPage (locked v17 `fx` layer, Phase 3: CHOP column only)
// The FX page: transparent CHOP column (360 px) holding the PLAYBACK card
// (mode / reverse / warp — mirrors of the overlay controls, same model fields
// through separate attachments) and the compact ENVELOPE card (draggable
// 4-handle AHDSR display + ATTACK/HOLD/DECAY/SUSTAIN/RELEASE knobs). The
// MASTER column stays a quiet placeholder until the Phase 4 master chain.
// Chop target: always the selected chop (same selection as MAIN) — prev/next
// arrows here and keys 1–8 change it (v17 chop_fx_target).

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "../engine/Engine.h"
#include "CompactKnob.h"
#include "SegControl.h"

namespace chopline::ui
{

class FxPage final : public juce::Component,
                     private juce::ChangeListener
{
public:
    explicit FxPage (engine::Engine& engineToUse);
    ~FxPage() override;

    /// Selection shared with MAIN (wired by the editor).
    std::function<int()> getChop;
    std::function<void (int)> setChop;

    void refresh(); // sync all controls from the engine (quiet)

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    // ---- draggable AHDSR display (v17 chop_env, 332x176) --------------------------
    class EnvDisplay final : public juce::Component
    {
    public:
        explicit EnvDisplay (FxPage& o) : owner (o) {}

        enum class Handle { none, attack, hold, decaySustain, release };

        void paint (juce::Graphics&) override;
        void mouseMove (const juce::MouseEvent&) override;
        void mouseExit (const juce::MouseEvent&) override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseDrag (const juce::MouseEvent&) override;
        void mouseUp (const juce::MouseEvent&) override;

    private:
        struct Nodes
        {
            juce::Point<float> a, h, d, s, r; // polyline nodes (a/h/d/r draggable)
            juce::Rectangle<float> area;
        };
        Nodes computeNodes (const engine::ChopSettings&) const;
        Handle handleAt (juce::Point<float>, const Nodes&) const;
        void applyDrag (const juce::MouseEvent&);

        FxPage& owner;
        Handle drag = Handle::none, hover = Handle::none;
    };

    void changeListenerCallback (juce::ChangeBroadcaster*) override;

    int chopIndex() const { return getChop != nullptr ? getChop() : 0; }
    engine::ChopSettings settings() const { return eng.getChopSettings (chopIndex()); }
    void setSettings (const engine::ChopSettings& s) { eng.setChopSettings (chopIndex(), s); }

    engine::Engine& eng;
    bool refreshing = false;

    juce::ComboBox modeBox;
    juce::ToggleButton reverseToggle { "Reverse" };
    SegControl warpSeg;
    juce::TextButton prevButton { juce::CharPointer_UTF8 ("\xe2\x80\xb9") };
    juce::TextButton nextButton { juce::CharPointer_UTF8 ("\xe2\x80\xba") };
    EnvDisplay envDisplay { *this };
    CompactKnob attackKnob { "ATTACK", true };
    CompactKnob holdKnob { "HOLD", true };
    CompactKnob decayKnob { "DECAY", true };
    CompactKnob sustainKnob { "SUSTAIN", true };
    CompactKnob releaseKnob { "RELEASE", true };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FxPage)
};

} // namespace chopline::ui
