#pragma once

#include <juce_core/juce_core.h>

//==============================================================================
// Stage 2 Phase 3.8: Macro System (8 Macros) — architecture.md component #9.
//
// Custom `MacroEngine` with a FIXED internal routing table (code-defined,
// NOT user-editable for MVP, per architecture.md: "brief doesn't request
// macro-destination customization"), mapping each of the 8 macros to a
// hand-tuned combination of destination-contribution weights. This REPLACES
// Phase 3.2's placeholder `macroDamage -> drive` curve
// (`juce::jlimit(0,100,macroDamagePct) * 0.08f`, a 0-100% -> 0..+8dB
// hardcoded line inline in PluginProcessor.cpp) with this class's real,
// documented `driveDb` routing.
//
// architecture.md names each macro's TARGETS (e.g. "macroDamage -> Drive
// (+weight), Fold-bias-toward-clip (+weight), Feedback Amount (+weight)")
// but never pins down exact numeric weights/curves ("e.g." phrasing
// throughout component #9) — every constant below is this implementation's
// own resolution, flagged here per this codebase's established precedent
// for such numeric choices (see e.g. RhythmicSequencer's
// kSequencerModDepthFraction, the Glitch Engine's kGlitchStutterFraction/
// kGlitchBufferRepeatFraction, etc.).
//
// Every macro's own default value (0% for all except macroWidth at 50% and
// macroMix at 100%) is designed to produce a ZERO net contribution from
// this class — matching every other modulation source's own "default =
// no effect" convention already established by RhythmicSequencer's lane
// defaults and Mod Matrix's Amount=0% default.
//
// Two of the eight macros' documented targets have NO live destination yet
// this phase and are intentionally left unrouted to any real DSP parameter,
// matching the established "computed but not yet applied" precedent
// (RhythmicSequencer's Pan/Pitch lanes, Phase 3.2's original drive
// observation before Phase 3.7 wired it live):
//   - macroGlitch's "Buffer repeat intensity" and "Pitch variation" targets:
//     no continuously-modulatable APVTS parameter exists for either concept
//     (glitchBufferLength is a discrete Choice, not continuous; Pitch FX is
//     explicitly post-MVP per architecture.md's Scope Reconciliation Note).
//     macroGlitch's OTHER two documented targets (Glitch Probability, and
//     Sample Rate Reduction shared with macroCrush) ARE routed below.
//   - macroWidth's "stereo width" target: architecture.md's own flagged
//     scope gap — "reduced/no-op effect until Stereo ships" — routed to a
//     diagnostic-only observation (`widthObservation`), consumed the same
//     way RhythmicSequencer's sequencerPanObservation/
//     sequencerPitchObservation already are.
//==============================================================================
class MacroEngine
{
public:
    // Physical-units-ready per-destination contributions, consumed directly
    // at each destination's ModulationAccumulator::accumulate() call site in
    // PluginProcessor.cpp (no further scaling needed by the caller, unlike
    // ModMatrix::resolve()'s raw -1..1-ish totals which DO still need a
    // caller-side range multiply — MacroEngine already knows each target's
    // physical units, since its routing table is fixed/hand-written rather
    // than generic/free-form).
    struct Contributions
    {
        float driveDb = 0.0f;                     // macroDamage -> Drive
        float foldPct = 0.0f;                      // macroDamage -> "Fold-bias-toward-clip"
        float feedbackAmountPct = 0.0f;             // macroDamage -> Feedback Amount
        float bitDepthBits = 0.0f;                  // macroCrush -> Bit Depth (negative weight, per architecture.md)
        float sampleRateFactor = 0.0f;              // macroCrush + macroGlitch (shared, per architecture.md) -> Sample Rate Reduction
        float glitchProbabilityPct = 0.0f;          // macroGlitch + macroRhythm -> Glitch Probability
        float gateGainOffset = 0.0f;                // macroRhythm -> "Sequencer/Glitch gate intensity" (routed to the Sequencer's existing Gate lane accumulate() call)
        float mixPct = 0.0f;                        // macroMix -> mirrors master mix (offset from macroMix's own 100% default, see resolve())
        float chaosPct = 0.0f;                      // macroChaos -> global `chaos` parameter scaling
        float lfoModMatrixDepthMultiplier = 1.0f;   // macroMovement -> "LFO depth multiplier across all 4 LFOs' Mod Matrix contributions" (consumed by ModMatrix::resolve())
        float randomSourceDepthMultiplier = 1.0f;   // macroChaos -> "Mod Matrix Random source depth" (consumed by ModMatrix::resolve())
        float widthObservation = 0.0f;               // macroWidth -> stereo width (deferred, diagnostic-only -- see class doc comment)
    };

    static Contributions resolve(float macroDamagePct, float macroCrushPct, float macroGlitchPct,
                                  float macroChaosPct, float macroRhythmPct, float macroMovementPct,
                                  float macroWidthPct, float macroMixPct) noexcept
    {
        Contributions c;

        // macroDamage -> Drive (+), Fold-bias-toward-clip (+), Feedback Amount (+)
        const float damage = juce::jlimit(0.0f, 100.0f, macroDamagePct) / 100.0f;
        c.driveDb           = damage * kMacroDamageDriveRangeDb;
        c.foldPct           = damage * kMacroDamageFoldRangePct;
        c.feedbackAmountPct = damage * kMacroDamageFeedbackRangePct;

        // macroCrush -> Bit Depth (-, i.e. reduces depth as macro increases), Sample Rate Reduction (+)
        const float crush = juce::jlimit(0.0f, 100.0f, macroCrushPct) / 100.0f;
        c.bitDepthBits     += -crush * kMacroCrushBitDepthRangeBits;
        c.sampleRateFactor += crush * kMacroCrushSrrRangeFactor;

        // macroGlitch -> Glitch Probability (+), Sample Rate Reduction (+, shared w/ Crush per
        // architecture.md); "Buffer repeat intensity" + "Pitch variation" targets have no live
        // destination yet -- see class doc comment.
        const float glitch = juce::jlimit(0.0f, 100.0f, macroGlitchPct) / 100.0f;
        c.glitchProbabilityPct += glitch * kMacroGlitchProbabilityRangePct;
        c.sampleRateFactor     += glitch * kMacroGlitchSrrRangeFactor;

        // macroChaos -> global `chaos` param scaling + Mod Matrix "Random" source depth
        const float chaosNorm = juce::jlimit(0.0f, 100.0f, macroChaosPct) / 100.0f;
        c.chaosPct = chaosNorm * kMacroChaosRangePct;
        c.randomSourceDepthMultiplier = 1.0f + chaosNorm * kMacroChaosRandomDepthGain;

        // macroRhythm -> Sequencer/Glitch gate intensity, glitchProbability (shared w/ macroGlitch)
        const float rhythm = juce::jlimit(0.0f, 100.0f, macroRhythmPct) / 100.0f;
        c.glitchProbabilityPct += rhythm * kMacroRhythmGlitchProbabilityRangePct;
        c.gateGainOffset = -rhythm * kMacroRhythmGateDepth; // pulls the Gate lane's net contribution toward "more choppy/closed" as macroRhythm increases

        // macroMovement -> LFO depth multiplier across all 4 LFOs' Mod Matrix contributions
        const float movement = juce::jlimit(0.0f, 100.0f, macroMovementPct) / 100.0f;
        c.lfoModMatrixDepthMultiplier = 1.0f + movement * kMacroMovementLfoDepthGain;

        // macroWidth -> stereo width (deferred to post-MVP Stereo module -- see class doc
        // comment). Offset from macroWidth's own 50% default, so the default contributes 0.
        c.widthObservation = juce::jlimit(0.0f, 100.0f, macroWidthPct) - 50.0f;

        // macroMix -> mirrors master mix, supports >100%. Offset from macroMix's own 100%
        // default, so the default contributes 0 (matching every other macro's "default = no
        // effect" convention even though macroMix's own numeric default isn't 0%).
        c.mixPct = juce::jlimit(0.0f, 200.0f, macroMixPct) - 100.0f;

        return c;
    }

private:
    static constexpr float kMacroDamageDriveRangeDb              = 20.0f; // 0-100% -> 0..+20dB
    static constexpr float kMacroDamageFoldRangePct              = 60.0f;
    static constexpr float kMacroDamageFeedbackRangePct          = 50.0f;
    static constexpr float kMacroCrushBitDepthRangeBits          = 10.0f; // pulls bit depth down toward ~6 bits at 100%
    static constexpr float kMacroCrushSrrRangeFactor             = 20.0f;
    static constexpr float kMacroGlitchProbabilityRangePct       = 60.0f;
    static constexpr float kMacroGlitchSrrRangeFactor             = 12.0f;
    static constexpr float kMacroChaosRangePct                   = 100.0f;
    static constexpr float kMacroChaosRandomDepthGain             = 1.0f;  // up to 2x Random-source Mod Matrix depth at macroChaos=100%
    static constexpr float kMacroRhythmGlitchProbabilityRangePct = 40.0f;
    static constexpr float kMacroRhythmGateDepth                  = 0.6f;  // pulls Gate's net 0..1 contribution down by up to 0.6 at macroRhythm=100%
    static constexpr float kMacroMovementLfoDepthGain             = 1.0f;  // up to 2x LFO-sourced Mod Matrix depth at macroMovement=100%
};
