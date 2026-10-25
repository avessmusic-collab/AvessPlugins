// CHOPLINE — FxPage (locked v17 `fx` layer)
// The FX page: transparent CHOP column (360 px) holding the PLAYBACK card
// (mode / reverse / warp — mirrors of the overlay controls, same model fields
// through separate attachments) and the compact ENVELOPE card (draggable
// 4-handle AHDSR display + ATTACK/HOLD/DECAY/SUSTAIN/RELEASE knobs).
//
// Phase 4 — MASTER column (560 px): six hairline cards, each with its title
// top-left and its bypass mini-toggle top-right INSIDE the card; a bypassed
// card dims to 45 % (v17). FILTER (type/cutoff/reso + response curve),
// DISTORTION (6 types, drive/mix/auto-gain + transfer curve), WIDTH (width /
// mono-below + stereo field), OUTPUT (gain/ceiling + out/GR meter), DELAY
// (time/feedback/return + taps), REVERB (decay/damp/return + tail). CUTOFF /
// DRIVE / WIDTH / OUT / CEILING / RETURNs mirror the MAIN bottom bar through
// separate attachments to the same APVTS parameters. The small hairline BPM
// readout mirrors the MAIN master card (v17 fx_bpm).
//
// Chop target: always the selected chop (same selection as MAIN) — prev/next
// arrows here and keys 1–8 change it (v17 chop_fx_target).

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
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
    FxPage (engine::Engine& engineToUse, juce::AudioProcessorValueTreeState& apvts);
    ~FxPage() override;

    /// Selection shared with MAIN (wired by the editor).
    std::function<int()> getChop;
    std::function<void (int)> setChop;

    void refresh();        // sync all chop controls from the engine (quiet)
    void animationTick();  // bypass dimming, displays, meter, BPM readout

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

    // ---- master displays (v17 light hairline-grid screens) --------------------------
    /// Base: paints the display chrome, then `drawContent`; `signature()`
    /// changes trigger a repaint from the page tick.
    class MasterDisplay : public juce::Component
    {
    public:
        void paint (juce::Graphics&) final;
        bool refreshIfChanged(); // true when repainted

    protected:
        virtual void drawContent (juce::Graphics&, juce::Rectangle<float>) = 0;
        virtual float signature() const = 0; // cheap change hash

    private:
        float lastSignature = -1.0e9f;
    };

    class FilterCurve final : public MasterDisplay
    {
    public:
        std::atomic<float>*type = nullptr, *cutoff = nullptr, *reso = nullptr;

    private:
        void drawContent (juce::Graphics&, juce::Rectangle<float>) override;
        float signature() const override;
    };

    class DistCurve final : public MasterDisplay
    {
    public:
        std::atomic<float>*type = nullptr, *drive = nullptr, *mix = nullptr,
            *autoGain = nullptr;

    private:
        void drawContent (juce::Graphics&, juce::Rectangle<float>) override;
        float signature() const override;
    };

    class WidthScope final : public MasterDisplay
    {
    public:
        std::atomic<float>*width = nullptr, *mono = nullptr;

    private:
        void drawContent (juce::Graphics&, juce::Rectangle<float>) override;
        float signature() const override;
    };

    class OutMeter final : public MasterDisplay
    {
    public:
        float peak = 0.0f, grDb = 0.0f; // fed by animationTick

    private:
        void drawContent (juce::Graphics&, juce::Rectangle<float>) override;
        float signature() const override;
    };

    class DelayTaps final : public MasterDisplay
    {
    public:
        std::atomic<float>*time = nullptr, *feedback = nullptr;
        double bpm = 120.0;

    private:
        void drawContent (juce::Graphics&, juce::Rectangle<float>) override;
        float signature() const override;
    };

    class VerbTail final : public MasterDisplay
    {
    public:
        std::atomic<float>*decay = nullptr, *damp = nullptr;

    private:
        void drawContent (juce::Graphics&, juce::Rectangle<float>) override;
        float signature() const override;
    };

    // ---- plumbing ---------------------------------------------------------------------
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void buildMasterColumn();

    int chopIndex() const { return getChop != nullptr ? getChop() : 0; }
    engine::ChopSettings settings() const { return eng.getChopSettings (chopIndex()); }
    void setSettings (const engine::ChopSettings& s) { eng.setChopSettings (chopIndex(), s); }

    engine::Engine& eng;
    juce::AudioProcessorValueTreeState& state;
    bool refreshing = false;

    // CHOP column (Phase 3).
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

    // MASTER column (Phase 4).
    struct Section
    {
        juce::Rectangle<int> card;
        juce::String title;
        juce::ToggleButton bypass;
        std::atomic<float>* onParam = nullptr;
        std::unique_ptr<juce::ButtonParameterAttachment> attachment;
        std::vector<juce::Component*> content;
        bool dimmed = false;
    };
    std::array<Section, 6> sections; // filt, dist, width, out, delay, verb

    SegControl filterTypeSeg;
    CompactKnob fxCutoff { "CUTOFF", true };
    CompactKnob fxReso { "RESO", true };
    FilterCurve filterCurve;

    juce::ComboBox distTypeBox;
    juce::ToggleButton autoGainToggle { "Auto gain" };
    CompactKnob fxDrive { "DRIVE", true };
    CompactKnob fxDistMix { "MIX", true };
    DistCurve distCurve;

    CompactKnob fxWidth { "WIDTH", true };
    CompactKnob fxMono { "MONO <", true };
    WidthScope widthScope;

    CompactKnob fxOut { "OUT", true };
    CompactKnob fxCeiling { "CEILING", true };
    OutMeter outMeter;

    juce::ComboBox delayTimeBox;
    CompactKnob fxDelayFb { "FEEDBACK", true };
    CompactKnob fxDelayRtn { "RETURN", true };
    DelayTaps delayTaps;

    CompactKnob fxVerbDecay { "DECAY", true };
    CompactKnob fxVerbDamp { "DAMP", true };
    CompactKnob fxVerbRtn { "RETURN", true };
    VerbTail verbTail;

    std::unique_ptr<juce::ParameterAttachment> filterTypeAttachment;
    std::unique_ptr<juce::ComboBoxParameterAttachment> distTypeAttachment, delayTimeAttachment;
    std::unique_ptr<juce::ButtonParameterAttachment> autoGainAttachment;
    std::vector<std::unique_ptr<juce::SliderParameterAttachment>> knobAttachments;

    double shownBpm = 0.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FxPage)
};

} // namespace chopline::ui
