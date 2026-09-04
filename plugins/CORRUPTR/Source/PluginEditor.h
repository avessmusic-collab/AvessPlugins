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

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CORRUPTRAudioProcessorEditor)
};
