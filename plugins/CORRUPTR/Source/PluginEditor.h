#pragma once

#include <juce_gui_extra/juce_gui_extra.h>
#include "PluginProcessor.h"

/**
 * CORRUPTR WebView-based Plugin Editor
 *
 * UI Mockup: v5 (1200x1650 total scrollable content, 1200x800 visible viewport,
 * fixed/non-resizable, internal vertical scroll - see v5-ui.yaml `window` block)
 * Full contract: plugins/CORRUPTR/.ideas/parameter-spec.md (locked, 94 total parameters)
 *
 * ============================================================================
 * STAGE 3 PHASED GUI ROLLOUT (plan.md "Stage 3: GUI Phases", 5.1-5.6)
 * ============================================================================
 * This editor loads the FULL v5-ui.html production mockup (all 12
 * modules/strips render visually, per plan.md's phasing intent that later
 * phases only need to WIRE bindings, not redesign markup), but Phase 5.1
 * binds ONLY the Distortion / Filter & EQ / Center Master (Output) / Top Bar
 * (qualityMode) parameter set below (14 total) - the "core chain" per
 * plan.md's Phase 5.1 goal.
 *
 * All other HTML controls (Bitcrush, Glitch, Feedback+Delay, Macros,
 * Performance Triggers, XY Pad, Sequencer, Mod Matrix) remain VISUALLY
 * present and locally interactive (the mockup's generic per-control-type
 * bindXxx() JS helpers call Juce.getSliderState()/getComboBoxState()/
 * getToggleState() for EVERY element carrying a data-param attribute,
 * regardless of phase) but are INERT with respect to the real APVTS/audio
 * engine this phase, because no C++ relay is registered for their parameter
 * IDs below - JUCE's WebView bridge silently falls back to local-only state
 * for any parameter name with no matching relay (console may log
 * "unknown to the backend" warnings for those IDs; this is expected and
 * matches v5-integration-checklist.md's own documented troubleshooting
 * note for this exact partial-binding scenario). Do NOT read this as a bug -
 * later phases (5.2-5.6) add their own relay/attachment pairs to progressively
 * wire the remaining controls; the markup itself is never modified again.
 *
 * CRITICAL: Member declaration order prevents release build crashes.
 * Order: Relays -> WebView -> Attachments
 *
 * Destruction order (reverse of declaration):
 * 1. Attachments destroyed FIRST (stop using relays and WebView)
 * 2. WebView destroyed SECOND (safe, attachments are gone)
 * 3. Relays destroyed LAST (safe, nothing using them)
 *
 * NOTE ON DUAL-BINDING PAIR (filterCutoff): filterCutoff is bound to TWO
 * controls in v5-ui.html (base 'Cutoff' knob + ADV-revealed 'Cutoff Fine'
 * mirror knob) but gets exactly ONE relay + ONE attachment below - the JUCE
 * frontend library's getSliderState() caches state objects per parameter
 * name, so both DOM controls share the same underlying relay automatically.
 *
 * NOT BOUND PHASE 5.1 (documented ambiguity, resolved per task instructions):
 * - tone, bias, distortionMix: parameter-spec.md itself notes these have
 *   "Not yet bound to a mockup control" - no HTML element exists to bind,
 *   so no relay is created for them this phase either. Deferred to a future
 *   mockup iteration per parameter-spec.md's own Reconciliation Notes.
 * - "global bypass": the task brief mentions this, but no such parameter
 *   exists in the locked parameter-spec.md - the Center Master module's
 *   power icon in v5-ui.html is explicitly documented as decorative/
 *   non-bound (v5-ui.yaml line ~1031: "Center Master and Feedback+Delay
 *   show the same row with a decorative, non-bound power icon"). Resolved
 *   as: no global bypass control exists to wire; skipped.
 *
 * ============================================================================
 * PHASE 5.2 ADDITIONS ("Glitch, Sequencer, and Rhythm Sections")
 * ============================================================================
 * Adds 9 more APVTS relay/attachment pairs (Bitcrush's graphBypassBitcrush/
 * bitDepth - explicitly deferred from 5.1 to this phase per parameter-spec.md's
 * module grouping; Glitch's graphBypassGlitch/glitchMode/glitchBufferLength/
 * glitchProbability; Rhythmic Sequencer's sequencerEnabled/sequencerRate/
 * sequencerSteps), bringing the bound total to 23 of 94. NOT bound this phase
 * either: sampleRateReduction and chaos - both are explicitly "Not yet bound
 * to a mockup control" per parameter-spec.md (same "no HTML element exists"
 * reasoning as tone/bias/distortionMix above), so the task's "if
 * mockup-bound" qualifier for chaos resolves to skip.
 *
 * Sequencer PATTERN data (10 lanes x up to 32 steps, dsp/RhythmicSequencer.h)
 * is NOT an APVTS parameter - it is custom message-thread-owned state with
 * its own double-buffered audio-thread snapshot. It cannot use the
 * Relay/Attachment pattern above at all, so this phase adds a SEPARATE
 * WebView<->C++ bridge built from three juce::WebBrowserComponent::Options
 * native functions (JS->C++ commands) plus one emitEventIfBrowserIsVisible()
 * push (C++->JS refresh), registered alongside (not instead of) the ordinary
 * relay/attachment pairs above:
 *   - "sequencerRequestPattern" (JS->C++, no params): the JS step-grid module
 *     calls this exactly once during its own init IIFE (after its event
 *     listener is already registered, so the reply below can never race/be
 *     missed) to fetch the full pattern for the initial render.
 *   - "sequencerSetStep" (JS->C++, params [lane, step, value]): a single-cell
 *     edit from a step-grid click/drag. Calls RhythmicSequencer::setStep()
 *     THEN publishSnapshot() on every call (see .cpp for why this
 *     deliberately deviates from RhythmicSequencer.h's own "batch many
 *     setStep() calls, publish once" doc-comment suggestion) - does NOT emit
 *     a refresh event back (the JS side already knows the value it just set
 *     locally, per the same "local render() first" convention every other
 *     bindXxx() helper in index.html already uses - an echo-back would be
 *     redundant network/serialization traffic for no benefit).
 *   - "sequencerPatternOp" (JS->C++, params [opName string]): invokes one of
 *     RhythmicSequencer's 10 opXxx() pattern operations (each of which
 *     already calls publishSnapshot() internally) using the CURRENT
 *     sequencerSteps parameter's resolved step count, then DOES emit a
 *     "sequencerPatternChanged" refresh event - unlike setStep, the JS side
 *     has no way to predict a pattern operation's (e.g. Random, Mutate)
 *     resulting values, so it must be told.
 * All three completions resolve `true` (a plain ack) - the actual data
 * transfer for the two cases that need one (initial load, post-op refresh)
 * travels via the SEPARATE "sequencerPatternChanged" event
 * (emitEventIfBrowserIsVisible()), not via the native function's own
 * completion value. This is a deliberate, FLAGGED resolution of the task
 * brief's "native functions... AND emitEventIfBrowserIsVisible... (C++->JS:
 * pattern refresh after an operation, initial pattern on load)" instruction:
 * both documented mechanisms are used, cleanly split by direction/purpose
 * (native functions = JS-initiated commands; the event = C++-initiated data
 * pushes), rather than also stuffing pattern data into the native functions'
 * own promise-resolution channel (which JUCE 8 does support, but would blur
 * the command/push distinction and duplicate the payload-serialization code
 * path for no benefit, since nothing here needs a synchronous
 * request/response - the event fires quickly enough after either triggering
 * call that a promise-based round trip buys nothing).
 *
 * The step-grid UI itself (Source/ui/public/index.html) is upgraded from
 * v5-ui.html's decorative 16-step single-row/fake-playhead visual into a
 * real editable grid: a JS-created lane-selector row of 10 chips (one per
 * RhythmicSequencer::Lane, JS-local UI state only, matches this file's
 * established "LFO panels/mod-matrix grid are JS-created" convention) above
 * a step-bar row honoring `sequencerSteps`' current step count (16 or 32).
 * Click/drag on the bars edits that lane's currently-displayed step's value
 * (see index.html's own top-of-section doc comment for the full
 * interaction/value-mapping design). The old mockup's fake `.playhead`
 * outline (static, always step 0, no relationship to real transport
 * position) is REMOVED rather than left in place, now that the grid
 * genuinely reflects live pattern data - real-time playhead-position
 * wiring is explicitly plan.md Phase 5.6 scope (Meters/Waveform/Modulation
 * Range Indicators), not this phase's; a static (no playhead) step-edit-only
 * grid is this phase's deliberate, task-sanctioned stopping point.
 *
 * ============================================================================
 * PHASE 5.3 ADDITIONS ("Modulation Matrix, LFOs, and Macros")
 * ============================================================================
 * Adds 53 more ordinary APVTS relay/attachment pairs, bringing the bound
 * total to 76 of 94:
 *   - 8 Macro knobs (macroDamage..macroMix) - markup already existed in
 *     v5-ui.html (Macros module, always-visible base row + ADV-revealed 2nd
 *     row) with correct `data-param` attributes and was ALREADY being bound
 *     by index.html's generic `bindKnobs(document)` call since Phase 5.1
 *     (that call iterates every `.knob[data-param]` in the document
 *     regardless of phase, same "fixed generic helper, zero JS changes"
 *     precedent Phase 5.2's own state notes already established for
 *     Bitcrush/Glitch/Sequencer) - this phase's work for Macros is 100% C++
 *     relay/attachment wiring, ZERO JS/HTML changes.
 *   - 12 LFO params (lfo1Rate/lfo1Shape/lfo1Sync .. lfo4Rate/lfo4Shape/
 *     lfo4Sync) + `modMatrixEnabled` - the LFO mini-panel JS generator
 *     (index.html's "4 LFO mini-panels" IIFE) ALREADY emitted all three
 *     controls per LFO (Rate knob, Shape combo, Sync toggle) with correct
 *     `data-param` attributes and already called `bindKnobs(row)`/
 *     `bindToggles(row)`/`bindCombos(row)` on itself at the end of that
 *     IIFE - contrary to this task's own brief, which anticipated possibly
 *     having to "extend the generator with the missing controls," NO JS
 *     changes were needed here either (verified by reading the file before
 *     touching it - documented as a FLAGGED resolution of that anticipated-
 *     but-not-actually-needed work). `modMatrixEnabled`'s power-btn markup
 *     also pre-existed (shares the sequencerEnabled/graphBypass* power-btn
 *     pattern, bound by the existing `bindPowerButtons`/`bindEnableButtons`
 *     calls). Choice-index alignment verified against dsp/Lfo.h's `Shape`
 *     enum (Sine=0..Random Walk=8) - the mockup's 9 `<option>` elements are
 *     in that exact order.
 *   - 32 Mod Matrix slot params (modSlot1Source/Destination/Amount/Enable
 *     .. modSlot8Source/Destination/Amount/Enable) - v5-ui.html's
 *     `#matrixTable` was PURELY DECORATIVE (a static source x destination
 *     dot-grid with a hardcoded `activeCells` set, no `data-param`
 *     anywhere - parameter-spec.md itself documents all 32 of these
 *     params as "Not yet bound to a mockup control"). FLAGGED LAYOUT
 *     INTERPRETATION (task-sanctioned - the brief explicitly names this as
 *     an expected gap and directs "extend via JS generation consistent
 *     with the design"): a new JS generator (index.html's "8-slot Mod
 *     Matrix editor" IIFE) builds 8 real slot-editor rows into a new
 *     `#modSlotList` container placed directly below the existing
 *     decorative `#matrixTable` (which is left in place unchanged, exactly
 *     as Phase 5.2 left the sequencer's orphaned `.step-bar.playhead` CSS
 *     rule in place rather than removing still-harmless decoration) - each
 *     row is a `Source` combo (10 choices), `Destination` combo (12
 *     choices), a compact `Amount` knob (-100..+100%), and an `Enable`
 *     toggle, all reusing this file's existing generic `bindCombos`/
 *     `bindKnobs`/`bindToggles` helpers verbatim (same "fixed helpers, zero
 *     bug-reintroduction risk" precedent as every prior phase). Choice-list
 *     ORDER for both combos is copied character-for-character from
 *     parameter-spec.md's `modSlot1Source`/`modSlot1Destination` Choices
 *     lists and cross-checked against dsp/ModMatrix.h's `Source`/
 *     `Destination` enums (`sourceLfo1=0..sourceMacro=9`,
 *     `destDrive=0..destWidth=11`) - both enums' comments explicitly state
 *     they match parameter-spec.md's order, confirmed by inspection; a
 *     mismatch here would silently route the wrong source/destination with
 *     no compiler error, so this was verified by direct side-by-side
 *     reading, not assumed.
 *
 * NOT bound this phase: the 6 draft-only Float params (tone, bias,
 * distortionMix, sampleRateReduction, chaos, xyPadSmoothing) - same "Not yet
 * bound to a mockup control" status as every prior phase's identical
 * skip-reasoning for this exact set, unrelated to Mod Matrix/LFO/Macro scope
 * anyway. xyPadX/xyPadY and the 7 performanceXxx triggers are explicitly
 * Phase 5.4 scope (plan.md), not touched here despite their markup already
 * existing (same "markup exists everywhere, phases only add relays"
 * intentional-inertness convention this whole rollout uses).
 *
 * ============================================================================
 * PHASE 5.4 ADDITIONS ("XY Pad and Performance Mode")
 * ============================================================================
 * Adds 9 more ordinary APVTS relay/attachment pairs, bringing the bound
 * total to 85 of 94:
 *   - xyPadX / xyPadY (WebSliderRelay, Float 0-100%) - the XY Performance Pad
 *     handle already existed and already streamed `setNormalisedValue()` on
 *     every mousemove, but with NO relay registered it was pure local-only
 *     JS state (silently inert, same "unknown to the backend" fallback every
 *     prior phase's unbound controls hit) - this phase's C++ work is purely
 *     the relay/attachment wiring; the JS-side pad logic itself is extended
 *     (not rewritten) in index.html, see that file's own updated XY PAD
 *     doc comment for the pointer-event/gesture-bracketing details satisfying
 *     plan.md's hard requirement that the pad stream continuously during the
 *     drag (not only on release).
 *   - performanceKill/Glitch/Destroy/Freeze/Reverse/Stutter/Chaos (7x
 *     WebToggleButtonRelay, Bool) - the 7 `.perf-btn` markup elements already
 *     existed with correct `data-param` attributes and were already being
 *     iterated by `bindPerfButtons(document)` (called unconditionally at
 *     index.html's bottom since Phase 5.1, same "fixed generic helper wires
 *     itself to whatever relays exist" precedent as every prior phase), but
 *     that helper previously toggled the parameter to a LATCHING on/off state
 *     on mousedown - dsp/PluginProcessor's Phase 3.9 performance-trigger
 *     accumulator override is MOMENTARY-semantic (true while held, false on
 *     release; DSP owns the "predefined processing-state combination" effect
 *     for as long as the bool stays true), so this phase also fixes
 *     `bindPerfButtons` itself to press-and-hold (pointerdown->true,
 *     pointerup/pointercancel->false) to match. A `valueChangedEvent`
 *     listener re-renders each button's `.on` class from `state.getValue()`
 *     (Pattern #15: no callback arguments) so a MIDI note (36-42) held into
 *     the DSP's momentary accumulator override visually lights the matching
 *     button even though no pointer ever touched it.
 *
 * xyPadSmoothing remains unbound - parameter-spec.md still documents it as
 * "Not yet bound to a mockup control" (no HTML element exists for it; same
 * reasoning as every prior phase's identical skip-list). It does not need
 * one for the glide/smoothing feel to work correctly: per plan.md's own
 * Phase 5.4 test criteria, the glide the DSP already implements (dsp-side
 * SmoothedValue chasing whatever raw xyPadX/xyPadY value this phase's UI now
 * streams) is what produces "smooth glide (not snap)" - the UI is
 * deliberately NOT adding any UI-side smoothing of its own, it streams the
 * pointer's raw normalised position every pointermove exactly as before.
 */

class CORRUPTRAudioProcessorEditor : public juce::AudioProcessorEditor
{
public:
    explicit CORRUPTRAudioProcessorEditor(CORRUPTRAudioProcessor&);
    ~CORRUPTRAudioProcessorEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    /**
     * Resource provider (JUCE 8 required pattern)
     * Maps URLs to embedded binary data from Source/ui/public/
     */
    std::optional<juce::WebBrowserComponent::Resource> getResource(const juce::String& url);

    // ------------------------------------------------------------------------
    // Phase 5.2: Rhythmic Sequencer pattern bridge helpers (see this header's
    // top doc comment, "PHASE 5.2 ADDITIONS", for the full design). These are
    // plain helper METHODS (not stored Relay/WebView/Attachment objects), so
    // their declaration position does not participate in the destruction-
    // order contract the member list below documents.
    // ------------------------------------------------------------------------

    // Builds the { lanes: [ [32 floats] x10 ] } payload consumed by index.html's
    // "sequencerPatternChanged" event listener - reads directly from
    // RhythmicSequencer::getStep() (message-thread API; safe to call here,
    // this class only ever runs on the message thread).
    juce::var buildSequencerPatternVar() const;

    // Pushes the current pattern to the WebView via emitEventIfBrowserIsVisible.
    // Safe to call even before webView finishes construction is never an issue
    // in practice (native-function callbacks that call this can only fire
    // AFTER the page has loaded, i.e. long after the constructor returns and
    // webView is non-null) but the null check is kept defensively anyway.
    void emitSequencerPatternChanged();

    // Resolves the CURRENT sequencerSteps choice parameter to an actual step
    // count (16 or 32) - same formula/comment as
    // CORRUPTRAudioProcessor::updateSequencerStepAndContributions()'s own
    // `sequencerNumSteps` resolution in PluginProcessor.cpp (AudioParameterChoice
    // raw values are stored as their integer index, 0="16"/1="32", NOT a
    // normalised 0-1 value).
    int getCurrentSequencerNumSteps() const;

    CORRUPTRAudioProcessor& processorRef;

    // ========================================================================
    // ⚠️ CRITICAL MEMBER DECLARATION ORDER ⚠️
    // Order: Relays -> WebView -> Attachments
    // Members are destroyed in REVERSE order of declaration.
    // ========================================================================

    // ------------------------------------------------------------------------
    // 1️⃣ RELAYS FIRST — Phase 5.1 scope only (14 of 94 total parameters)
    // ------------------------------------------------------------------------

    // --- Top Bar / Global ---
    std::unique_ptr<juce::WebComboBoxRelay> qualityModeRelay;

    // --- Distortion ---
    std::unique_ptr<juce::WebToggleButtonRelay> graphBypassSaturationRelay;
    std::unique_ptr<juce::WebComboBoxRelay> distortionAlgorithmRelay;
    std::unique_ptr<juce::WebSliderRelay> driveRelay;
    std::unique_ptr<juce::WebSliderRelay> foldRelay;
    std::unique_ptr<juce::WebToggleButtonRelay> graphBypassWaveshaperRelay;

    // --- Filter & EQ ---
    std::unique_ptr<juce::WebToggleButtonRelay> graphBypassFilterRelay;
    std::unique_ptr<juce::WebComboBoxRelay> filterTypeRelay;
    std::unique_ptr<juce::WebSliderRelay> filterCutoffRelay;
    std::unique_ptr<juce::WebSliderRelay> filterResonanceRelay;

    // --- Center Master (Output/Global) ---
    std::unique_ptr<juce::WebSliderRelay> mixRelay;
    std::unique_ptr<juce::WebComboBoxRelay> outputLimiterStyleRelay;
    std::unique_ptr<juce::WebSliderRelay> inputGainRelay;
    std::unique_ptr<juce::WebSliderRelay> outputGainRelay;

    // --- Phase 5.2: Bitcrush ---
    std::unique_ptr<juce::WebToggleButtonRelay> graphBypassBitcrushRelay;
    std::unique_ptr<juce::WebSliderRelay> bitDepthRelay;

    // --- Phase 5.2: Glitch ---
    std::unique_ptr<juce::WebToggleButtonRelay> graphBypassGlitchRelay;
    std::unique_ptr<juce::WebComboBoxRelay> glitchModeRelay;
    std::unique_ptr<juce::WebComboBoxRelay> glitchBufferLengthRelay; // hslider bound via getComboBoxState() - see index.html bindHSliders()
    std::unique_ptr<juce::WebSliderRelay> glitchProbabilityRelay; // dual-bound (base knob + ADV mirror), ONE relay per Phase 5.1's dual-binding precedent

    // --- Phase 5.2: Rhythmic Sequencer (APVTS params only - pattern data bridge is separate, see native functions below) ---
    std::unique_ptr<juce::WebToggleButtonRelay> sequencerEnabledRelay;
    std::unique_ptr<juce::WebComboBoxRelay> sequencerRateRelay;
    std::unique_ptr<juce::WebComboBoxRelay> sequencerStepsRelay;

    // --- Phase 5.3: Macros (8) ---
    std::unique_ptr<juce::WebSliderRelay> macroDamageRelay;
    std::unique_ptr<juce::WebSliderRelay> macroCrushRelay;
    std::unique_ptr<juce::WebSliderRelay> macroGlitchRelay;
    std::unique_ptr<juce::WebSliderRelay> macroChaosRelay;
    std::unique_ptr<juce::WebSliderRelay> macroRhythmRelay;
    std::unique_ptr<juce::WebSliderRelay> macroMovementRelay;
    std::unique_ptr<juce::WebSliderRelay> macroWidthRelay;
    std::unique_ptr<juce::WebSliderRelay> macroMixRelay;

    // --- Phase 5.3: Modulation Matrix module enable + 4 LFOs (rate/shape/sync) ---
    std::unique_ptr<juce::WebToggleButtonRelay> modMatrixEnabledRelay;
    std::unique_ptr<juce::WebSliderRelay> lfo1RateRelay;
    std::unique_ptr<juce::WebComboBoxRelay> lfo1ShapeRelay;
    std::unique_ptr<juce::WebToggleButtonRelay> lfo1SyncRelay;
    std::unique_ptr<juce::WebSliderRelay> lfo2RateRelay;
    std::unique_ptr<juce::WebComboBoxRelay> lfo2ShapeRelay;
    std::unique_ptr<juce::WebToggleButtonRelay> lfo2SyncRelay;
    std::unique_ptr<juce::WebSliderRelay> lfo3RateRelay;
    std::unique_ptr<juce::WebComboBoxRelay> lfo3ShapeRelay;
    std::unique_ptr<juce::WebToggleButtonRelay> lfo3SyncRelay;
    std::unique_ptr<juce::WebSliderRelay> lfo4RateRelay;
    std::unique_ptr<juce::WebComboBoxRelay> lfo4ShapeRelay;
    std::unique_ptr<juce::WebToggleButtonRelay> lfo4SyncRelay;

    // --- Phase 5.3: Mod Matrix slots (8 x {Source,Destination,Amount,Enable} = 32) ---
    std::unique_ptr<juce::WebComboBoxRelay> modSlot1SourceRelay;
    std::unique_ptr<juce::WebComboBoxRelay> modSlot1DestinationRelay;
    std::unique_ptr<juce::WebSliderRelay> modSlot1AmountRelay;
    std::unique_ptr<juce::WebToggleButtonRelay> modSlot1EnableRelay;
    std::unique_ptr<juce::WebComboBoxRelay> modSlot2SourceRelay;
    std::unique_ptr<juce::WebComboBoxRelay> modSlot2DestinationRelay;
    std::unique_ptr<juce::WebSliderRelay> modSlot2AmountRelay;
    std::unique_ptr<juce::WebToggleButtonRelay> modSlot2EnableRelay;
    std::unique_ptr<juce::WebComboBoxRelay> modSlot3SourceRelay;
    std::unique_ptr<juce::WebComboBoxRelay> modSlot3DestinationRelay;
    std::unique_ptr<juce::WebSliderRelay> modSlot3AmountRelay;
    std::unique_ptr<juce::WebToggleButtonRelay> modSlot3EnableRelay;
    std::unique_ptr<juce::WebComboBoxRelay> modSlot4SourceRelay;
    std::unique_ptr<juce::WebComboBoxRelay> modSlot4DestinationRelay;
    std::unique_ptr<juce::WebSliderRelay> modSlot4AmountRelay;
    std::unique_ptr<juce::WebToggleButtonRelay> modSlot4EnableRelay;
    std::unique_ptr<juce::WebComboBoxRelay> modSlot5SourceRelay;
    std::unique_ptr<juce::WebComboBoxRelay> modSlot5DestinationRelay;
    std::unique_ptr<juce::WebSliderRelay> modSlot5AmountRelay;
    std::unique_ptr<juce::WebToggleButtonRelay> modSlot5EnableRelay;
    std::unique_ptr<juce::WebComboBoxRelay> modSlot6SourceRelay;
    std::unique_ptr<juce::WebComboBoxRelay> modSlot6DestinationRelay;
    std::unique_ptr<juce::WebSliderRelay> modSlot6AmountRelay;
    std::unique_ptr<juce::WebToggleButtonRelay> modSlot6EnableRelay;
    std::unique_ptr<juce::WebComboBoxRelay> modSlot7SourceRelay;
    std::unique_ptr<juce::WebComboBoxRelay> modSlot7DestinationRelay;
    std::unique_ptr<juce::WebSliderRelay> modSlot7AmountRelay;
    std::unique_ptr<juce::WebToggleButtonRelay> modSlot7EnableRelay;
    std::unique_ptr<juce::WebComboBoxRelay> modSlot8SourceRelay;
    std::unique_ptr<juce::WebComboBoxRelay> modSlot8DestinationRelay;
    std::unique_ptr<juce::WebSliderRelay> modSlot8AmountRelay;
    std::unique_ptr<juce::WebToggleButtonRelay> modSlot8EnableRelay;

    // --- Phase 5.4: XY Performance Pad (2) ---
    std::unique_ptr<juce::WebSliderRelay> xyPadXRelay;
    std::unique_ptr<juce::WebSliderRelay> xyPadYRelay;

    // --- Phase 5.4: Performance Mode triggers (7, momentary) ---
    std::unique_ptr<juce::WebToggleButtonRelay> performanceKillRelay;
    std::unique_ptr<juce::WebToggleButtonRelay> performanceGlitchRelay;
    std::unique_ptr<juce::WebToggleButtonRelay> performanceDestroyRelay;
    std::unique_ptr<juce::WebToggleButtonRelay> performanceFreezeRelay;
    std::unique_ptr<juce::WebToggleButtonRelay> performanceReverseRelay;
    std::unique_ptr<juce::WebToggleButtonRelay> performanceStutterRelay;
    std::unique_ptr<juce::WebToggleButtonRelay> performanceChaosRelay;

    // ------------------------------------------------------------------------
    // 2️⃣ WEBVIEW SECOND
    // ------------------------------------------------------------------------
    std::unique_ptr<juce::WebBrowserComponent> webView;

    // ------------------------------------------------------------------------
    // 3️⃣ PARAMETER ATTACHMENTS LAST — matches relay list 1:1
    // ------------------------------------------------------------------------

    // --- Top Bar / Global ---
    std::unique_ptr<juce::WebComboBoxParameterAttachment> qualityModeAttachment;

    // --- Distortion ---
    std::unique_ptr<juce::WebToggleButtonParameterAttachment> graphBypassSaturationAttachment;
    std::unique_ptr<juce::WebComboBoxParameterAttachment> distortionAlgorithmAttachment;
    std::unique_ptr<juce::WebSliderParameterAttachment> driveAttachment;
    std::unique_ptr<juce::WebSliderParameterAttachment> foldAttachment;
    std::unique_ptr<juce::WebToggleButtonParameterAttachment> graphBypassWaveshaperAttachment;

    // --- Filter & EQ ---
    std::unique_ptr<juce::WebToggleButtonParameterAttachment> graphBypassFilterAttachment;
    std::unique_ptr<juce::WebComboBoxParameterAttachment> filterTypeAttachment;
    std::unique_ptr<juce::WebSliderParameterAttachment> filterCutoffAttachment;
    std::unique_ptr<juce::WebSliderParameterAttachment> filterResonanceAttachment;

    // --- Center Master (Output/Global) ---
    std::unique_ptr<juce::WebSliderParameterAttachment> mixAttachment;
    std::unique_ptr<juce::WebComboBoxParameterAttachment> outputLimiterStyleAttachment;
    std::unique_ptr<juce::WebSliderParameterAttachment> inputGainAttachment;
    std::unique_ptr<juce::WebSliderParameterAttachment> outputGainAttachment;

    // --- Phase 5.2: Bitcrush ---
    std::unique_ptr<juce::WebToggleButtonParameterAttachment> graphBypassBitcrushAttachment;
    std::unique_ptr<juce::WebSliderParameterAttachment> bitDepthAttachment;

    // --- Phase 5.2: Glitch ---
    std::unique_ptr<juce::WebToggleButtonParameterAttachment> graphBypassGlitchAttachment;
    std::unique_ptr<juce::WebComboBoxParameterAttachment> glitchModeAttachment;
    std::unique_ptr<juce::WebComboBoxParameterAttachment> glitchBufferLengthAttachment;
    std::unique_ptr<juce::WebSliderParameterAttachment> glitchProbabilityAttachment;

    // --- Phase 5.2: Rhythmic Sequencer ---
    std::unique_ptr<juce::WebToggleButtonParameterAttachment> sequencerEnabledAttachment;
    std::unique_ptr<juce::WebComboBoxParameterAttachment> sequencerRateAttachment;
    std::unique_ptr<juce::WebComboBoxParameterAttachment> sequencerStepsAttachment;

    // --- Phase 5.3: Macros (8) ---
    std::unique_ptr<juce::WebSliderParameterAttachment> macroDamageAttachment;
    std::unique_ptr<juce::WebSliderParameterAttachment> macroCrushAttachment;
    std::unique_ptr<juce::WebSliderParameterAttachment> macroGlitchAttachment;
    std::unique_ptr<juce::WebSliderParameterAttachment> macroChaosAttachment;
    std::unique_ptr<juce::WebSliderParameterAttachment> macroRhythmAttachment;
    std::unique_ptr<juce::WebSliderParameterAttachment> macroMovementAttachment;
    std::unique_ptr<juce::WebSliderParameterAttachment> macroWidthAttachment;
    std::unique_ptr<juce::WebSliderParameterAttachment> macroMixAttachment;

    // --- Phase 5.3: Modulation Matrix module enable + 4 LFOs (rate/shape/sync) ---
    std::unique_ptr<juce::WebToggleButtonParameterAttachment> modMatrixEnabledAttachment;
    std::unique_ptr<juce::WebSliderParameterAttachment> lfo1RateAttachment;
    std::unique_ptr<juce::WebComboBoxParameterAttachment> lfo1ShapeAttachment;
    std::unique_ptr<juce::WebToggleButtonParameterAttachment> lfo1SyncAttachment;
    std::unique_ptr<juce::WebSliderParameterAttachment> lfo2RateAttachment;
    std::unique_ptr<juce::WebComboBoxParameterAttachment> lfo2ShapeAttachment;
    std::unique_ptr<juce::WebToggleButtonParameterAttachment> lfo2SyncAttachment;
    std::unique_ptr<juce::WebSliderParameterAttachment> lfo3RateAttachment;
    std::unique_ptr<juce::WebComboBoxParameterAttachment> lfo3ShapeAttachment;
    std::unique_ptr<juce::WebToggleButtonParameterAttachment> lfo3SyncAttachment;
    std::unique_ptr<juce::WebSliderParameterAttachment> lfo4RateAttachment;
    std::unique_ptr<juce::WebComboBoxParameterAttachment> lfo4ShapeAttachment;
    std::unique_ptr<juce::WebToggleButtonParameterAttachment> lfo4SyncAttachment;

    // --- Phase 5.3: Mod Matrix slots (8 x {Source,Destination,Amount,Enable} = 32) ---
    std::unique_ptr<juce::WebComboBoxParameterAttachment> modSlot1SourceAttachment;
    std::unique_ptr<juce::WebComboBoxParameterAttachment> modSlot1DestinationAttachment;
    std::unique_ptr<juce::WebSliderParameterAttachment> modSlot1AmountAttachment;
    std::unique_ptr<juce::WebToggleButtonParameterAttachment> modSlot1EnableAttachment;
    std::unique_ptr<juce::WebComboBoxParameterAttachment> modSlot2SourceAttachment;
    std::unique_ptr<juce::WebComboBoxParameterAttachment> modSlot2DestinationAttachment;
    std::unique_ptr<juce::WebSliderParameterAttachment> modSlot2AmountAttachment;
    std::unique_ptr<juce::WebToggleButtonParameterAttachment> modSlot2EnableAttachment;
    std::unique_ptr<juce::WebComboBoxParameterAttachment> modSlot3SourceAttachment;
    std::unique_ptr<juce::WebComboBoxParameterAttachment> modSlot3DestinationAttachment;
    std::unique_ptr<juce::WebSliderParameterAttachment> modSlot3AmountAttachment;
    std::unique_ptr<juce::WebToggleButtonParameterAttachment> modSlot3EnableAttachment;
    std::unique_ptr<juce::WebComboBoxParameterAttachment> modSlot4SourceAttachment;
    std::unique_ptr<juce::WebComboBoxParameterAttachment> modSlot4DestinationAttachment;
    std::unique_ptr<juce::WebSliderParameterAttachment> modSlot4AmountAttachment;
    std::unique_ptr<juce::WebToggleButtonParameterAttachment> modSlot4EnableAttachment;
    std::unique_ptr<juce::WebComboBoxParameterAttachment> modSlot5SourceAttachment;
    std::unique_ptr<juce::WebComboBoxParameterAttachment> modSlot5DestinationAttachment;
    std::unique_ptr<juce::WebSliderParameterAttachment> modSlot5AmountAttachment;
    std::unique_ptr<juce::WebToggleButtonParameterAttachment> modSlot5EnableAttachment;
    std::unique_ptr<juce::WebComboBoxParameterAttachment> modSlot6SourceAttachment;
    std::unique_ptr<juce::WebComboBoxParameterAttachment> modSlot6DestinationAttachment;
    std::unique_ptr<juce::WebSliderParameterAttachment> modSlot6AmountAttachment;
    std::unique_ptr<juce::WebToggleButtonParameterAttachment> modSlot6EnableAttachment;
    std::unique_ptr<juce::WebComboBoxParameterAttachment> modSlot7SourceAttachment;
    std::unique_ptr<juce::WebComboBoxParameterAttachment> modSlot7DestinationAttachment;
    std::unique_ptr<juce::WebSliderParameterAttachment> modSlot7AmountAttachment;
    std::unique_ptr<juce::WebToggleButtonParameterAttachment> modSlot7EnableAttachment;
    std::unique_ptr<juce::WebComboBoxParameterAttachment> modSlot8SourceAttachment;
    std::unique_ptr<juce::WebComboBoxParameterAttachment> modSlot8DestinationAttachment;
    std::unique_ptr<juce::WebSliderParameterAttachment> modSlot8AmountAttachment;
    std::unique_ptr<juce::WebToggleButtonParameterAttachment> modSlot8EnableAttachment;

    // --- Phase 5.4: XY Performance Pad (2) ---
    std::unique_ptr<juce::WebSliderParameterAttachment> xyPadXAttachment;
    std::unique_ptr<juce::WebSliderParameterAttachment> xyPadYAttachment;

    // --- Phase 5.4: Performance Mode triggers (7, momentary) ---
    std::unique_ptr<juce::WebToggleButtonParameterAttachment> performanceKillAttachment;
    std::unique_ptr<juce::WebToggleButtonParameterAttachment> performanceGlitchAttachment;
    std::unique_ptr<juce::WebToggleButtonParameterAttachment> performanceDestroyAttachment;
    std::unique_ptr<juce::WebToggleButtonParameterAttachment> performanceFreezeAttachment;
    std::unique_ptr<juce::WebToggleButtonParameterAttachment> performanceReverseAttachment;
    std::unique_ptr<juce::WebToggleButtonParameterAttachment> performanceStutterAttachment;
    std::unique_ptr<juce::WebToggleButtonParameterAttachment> performanceChaosAttachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CORRUPTRAudioProcessorEditor)
};
