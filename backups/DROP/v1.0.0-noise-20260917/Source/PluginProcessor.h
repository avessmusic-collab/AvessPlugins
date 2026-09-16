#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include "DSP/MusicalClock.h"
#include "DSP/TransitionEngine.h"
#include "DSP/DropStateMachine.h"
#include "DSP/FilterModule.h"
#include "DSP/WidthModule.h"
#include "DSP/GateModule.h"
#include "DSP/DelayModule.h"
#include "DSP/SaturationModule.h"
#include "DSP/ReverbModule.h"
#include "DSP/IntensityBudget.h"
#include "DSP/PitchModule.h"
#include "DSP/StutterModule.h"
#include "DSP/DigitalModule.h"
#include "DSP/VacuumModule.h"
#include "DSP/NoiseModule.h"
#include "DSP/ImpactEngine.h"
#include "DSP/AudioAnalyzer.h"
#include "Core/DropRandomizer.h"
#include "Core/DropStyle.h"
#include "Core/DropPreset.h"
#include <array>
#include <atomic>
#include <vector>

/**
 * DROP - Musical Transition Engine.
 *
 * "Give me a boring loop, tell me where the drop happens" - DROP turns a
 * normal musical passage into BUILD -> TENSION -> VACUUM -> IMPACT ->
 * RECOVERY. See plugins/DROP/.ideas/architecture.md (authoritative DSP
 * spec) and plugins/DROP/.ideas/parameter-spec.md (LOCKED, 19 APVTS
 * parameters + `tail`, added 2026-09-16 as a POST-1.0 amendment - 20 total)
 * for full detail.
 *
 * Stage 1 (Foundation + Shell) scope: build system, full 19-parameter
 * APVTS, and non-parameter ValueTree state scaffolding (trigger mode, MIDI
 * trigger note, Chaos seed, custom tension curve points, UI expanded flag).
 *
 * Stage 3 DSP Phase 3.1 ("Timing & Control Skeleton") scope: MusicalClock +
 * TransitionEngine + DropStateMachine + sample-accurate block segmentation
 * in processBlock(), wired to the `trigger` parameter and the configurable
 * MIDI trigger note. Audio remains a real-time-safe PASS-THROUGH - no
 * audible DSP modules exist yet (those arrive in Stage 3 DSP Phases 3.2-3.5).
 *
 * Stage 3 DSP Phase 3.2 ("Core DSP Modules") scope: FilterModule,
 * WidthModule, GateModule, DelayModule, SaturationModule, ReverbModule, and
 * a placeholder IntensityBudget wired into the segmentation loop in the
 * fixed series order Filter -> Width -> Gate -> Delay -> Saturation ->
 * Reverb (Pitch/Stutter series slot deferred to Phase 3.3), followed by the
 * global Dry/Wet mix + Output gain stage (architecture.md Processing Order
 * steps 9-10). Every module always processes (architecture.md
 * "Always-Process, Weight-Mix Architecture") - IntensityBudget-computed
 * weights (0-1) blend each module's contribution, never bypass it.
 *
 * Stage 3 DSP Phase 3.3 ("Creative DSP Modules") scope: PitchModule,
 * StutterModule, DigitalModule, NoiseModule, and VacuumModule added,
 * plus Source/Core/DropRandomizer.h (Chaos seed management). Wired into
 * the fixed series order Filter -> Width -> Gate -> Pitch -> Stutter ->
 * Delay -> Saturation -> Digital, with Noise as a parallel add and
 * VacuumModule state-gated to the DropStateMachine Peak state (per
 * architecture.md's Processing Chain / this phase's explicit wiring
 * instructions - full ImpactEngine coordination lands in Phase 3.4).
 *
 * Stage 3 DSP Phase 3.4 ("ImpactEngine") scope: Peak's ENTIRE latched
 * duration now IS the musically-derived Vacuum window (1/32 note, floor-
 * clamped to >= 25ms - see kVacuumMinFloorSeconds), and a new Impact-state
 * window (architecture.md: "typically 50-300ms") is state-gated to
 * Source/DSP/ImpactEngine.* - a coordinator adding a band-limited 40-80Hz
 * sub-hit + noise burst on top of the bus, driving WidthModule's
 * stereo-expansion and a light SaturationModule overlay for the same
 * segment, and owning a dedicated short-tail ReverbModule instance.
 * computeIntensityProgress() ducks the 10 shared-budget modules to 0
 * starting at PEAK ENTRY (not Impact entry - a post-3.4-review bugfix: each
 * module's own ~20ms internal weightSmoothed ramp needs real elapsed time
 * to actually settle to 0, not just start targeting it, so the duck must
 * begin early enough that it has GENUINELY finished by Impact's first
 * sample) and holds at 0 through the entire Impact state (TRANSIENT
 * PRESERVATION: "duck the effect chain, not the source") so the (near-)dry
 * source transient survives the drop. Recovery's musically-derived duration
 * (half a bar) also replaces its Phase 3.1-era fixed-seconds placeholder.
 *
 * Stage 3 DSP Phase 3.5 ("IntensityBudget, DropStyle, AudioAnalyzer, Preset
 * Wiring") scope - this is the FINAL DSP phase: the Phase 3.2-3.4 placeholder
 * IntensityBudget (fixed/static weight curves) is replaced by the real,
 * DropStyle-authored, source-aware, Chaos-jittered
 * drop::IntensityBudget::computeWeights(); the `style` APVTS parameter now
 * selects one of 16 factory drop::DropStyleData entries (Source/Core/
 * DropStyle.*), latched at Arming into activeStyleIndexForBuild (mirroring
 * every other per-build-latched setting) - the Style also now drives
 * FilterModule/SaturationModule/NoiseModule/PitchModule's mode selection and
 * TransitionEngine's curve type, plus the Vacuum/Impact/Recovery timing
 * previously hard-coded in computeDurationsForNewBuild(). A new
 * drop::AudioAnalyzer (Source/DSP/AudioAnalyzer.*) taps the pre-chain dry
 * signal every block (RT-safe, read-only) and feeds its SourceProfile
 * classification into computeWeights() for source-aware weight
 * redistribution. Source/Core/DropPreset.* exposes the 16 presets (= the 16
 * Styles at recommended macro defaults) via the standard
 * getNumPrograms()/setCurrentProgram() JUCE program API. A lock-free
 * single-producer/single-consumer visualization ring buffer
 * (VisualizationFrame, popVisualizationFrame()) is populated once per
 * processBlock() call and drained by PluginEditor's 30Hz juce::Timer (Stage
 * 3 GUI Phase 4.3, complete - the finalized v10 design's progress-strip/
 * phase-chip/module-macro-arc-glow treatment consumes this feed in place of
 * the original spec's separate DropGraph/MeterDisplay components) - see
 * VisualizationFrame's doc comment for the PINNED activeModuleWeights[10]
 * index order.
 *
 * POST-1.0 IMPROVEMENT (2026-09-16, user-directed, see plugins/DROP/CHANGELOG.md
 * - stays at version 1.0.0 per this repo's factory convention until public
 * release). Fixes three user-reported real-DAW-testing bugs plus one
 * approved feature, all interdependent:
 *
 *   BUG A ("instant cut"): ImpactEngine::process() was state-gated to
 *   Impact ONLY - its sub-hit decay (~120ms) and dedicated short-tail
 *   reverb's own algorithmic decay were amputated at the exact
 *   Impact->Recovery boundary whenever that boundary landed before those
 *   envelopes/that reverb had genuinely finished ringing.
 *
 *   BUG B ("post-impact artifact"): computeIntensityProgress() returned
 *   `1.0f - t` (ramping 1->0) for Recovery, so the 10 shared-budget modules
 *   (freshly settled to exactly 0 through Peak/Impact) SLAMMED back up
 *   toward full weight at the instant Recovery began, then ramped back
 *   down - an audible post-impact effect blast. Fixed: Recovery now also
 *   returns 0.0f (matches implementation-spec.md's IMPACT PHILOSOPHY:
 *   "BUILD -> TENSION -> VACUUM -> ORIGINAL/CLEAN IMPACT -> SHORT TAIL ->
 *   RECOVERY" - Recovery stays clean, tails only).
 *
 *   BUG C ("stale tails"): DelayModule/ReverbModule's wet mix followed the
 *   same (now-zero) shared weight, so their already-circulating
 *   echoes/reverb tails were muted within one ~20ms smoother ramp instead
 *   of ringing out - mid-ring, and stale for the NEXT build since nothing
 *   ever reset their internal state either.
 *
 *   FEATURE: new 20th APVTS parameter `tail` (0-100%, default 35%),
 *   exponentially mapped to a Recovery-state duration of ~0.15s (0%) to
 *   ~4.0s (100%) - see computeDurationsForNewBuild()'s doc comment for the
 *   exact formula. Latched at Arming (like buildLength), OVERRIDES the
 *   active DropStyle's own `recoveryDivision` timing preference (Style
 *   authoring still governs Vacuum/Impact timing, only Recovery's length is
 *   now user-controlled).
 *
 * Fix design (see processSegment()'s "tail envelope" doc comment and
 * DelayModule.h/ReverbModule.h's own class doc comments for the DSP-level
 * detail): DelayModule/ReverbModule gained an additive send/return-weight
 * API (existing single-weight process() unchanged/still used during
 * Building). During Recovery ONLY, INPUT-SEND is forced to 0 (no fresh
 * ducked-bus material re-excites the tail) while WET-RETURN is driven by a
 * raised-cosine "tail envelope" (1.0 at Recovery's first sample, decaying
 * 1->0 exactly across Recovery's tail-parameter-derived duration) multiplied
 * against the LAST weight value Building actually used (so the tail starts
 * exactly where the source content left off, not at an arbitrary reset
 * value). ImpactEngine continues processing through Recovery too, with
 * elapsed time tracked continuously across the boundary
 * (impactCarryoverElapsedSamples) and its own effective strength scaled by
 * the SAME tail envelope. At the click-free instant the tail envelope
 * reaches exactly 0 (Recovery->Idle), every owned DSP module is reset() so
 * nothing stale replays into the next build (advanceStateMachineTicks()) -
 * deliberately NOT done at Idle->Arming, since audio is still live at that
 * transition.
 *
 * POST-1.0 SECOND-PASS FIX (2026-09-16, same session, coordinator-reported
 * build/test feedback after the first pass): two bugs found and fixed via
 * source inspection of DropStyle.cpp/IntensityBudget.cpp -
 *   1. Peak AND Impact are now BYTE-IDENTICAL to the pre-existing (already-
 *      passing) legacy behavior for DelayModule/ReverbModule send/return -
 *      the tail envelope/frozen-weight treatment applies ONLY during
 *      Recovery. The first pass incorrectly held the tail envelope at 1.0
 *      through Peak+Impact too, substituting the frozen (Building-end)
 *      weight for the live (small, ~0.05-0.15-ish) Peak/Impact weight -
 *      this DILUTED the Impact-state transient (crossfading it toward a
 *      near-silent "wet" signal at a much higher blend ratio than before),
 *      regressing the transient-preservation floor. DropStyle.cpp's
 *      budgetEnvelope/moduleCurves are authored with nonzero startWeight
 *      (e.g. 0.05) - IntensityBudget weights at progress==0 were NEVER
 *      exactly 0.0f even before any of this session's changes; "duck the
 *      chain" only ever meant "small", not "exactly zero" - restoring the
 *      untouched legacy path for Peak/Impact fixes the regression.
 *   2. The buffer-hygiene reset (advanceStateMachineTicks()) now covers
 *      EVERY owned DSP module (Filter/Width/Gate/Saturation/Pitch/Digital/
 *      Vacuum/Noise, in addition to the original Delay/Reverb/Impact/
 *      Stutter four) - since every module carries a genuinely nonzero live
 *      weight even while "ducked" (point 1 above), every module with
 *      persistent internal filter/oscillator state was ALREADY live-
 *      processing real signal throughout the previous build and needs
 *      resetting, not just the four originally-scoped tail-capable ones.
 *
 * POST-1.0 SECOND IMPROVEMENT SESSION (2026-09-16, same day, three NEW
 * real-DAW-testing bug reports plus one approved feature - independent of,
 * and layered on top of, the tail-envelope session above. Still 1.0.0.):
 *
 *   BUG D ("not locking to the grid... as if it's using its own clock"):
 *   computeDurationsForNewBuild() quantized Arming to the next bar boundary
 *   and set `buildingSamples` to the FULL requested bar-length duration,
 *   THEN let Peak's Vacuum-collapse window run AFTER that - so Impact (the
 *   actual "drop") always landed `peakSamples` samples LATE relative to the
 *   downbeat the user was building toward (>= 25ms Style-authored floor,
 *   more at slow tempi/long vacuum divisions). Fixed: the Vacuum window is
 *   now carved OUT of the requested bar-length duration (computed FIRST,
 *   then `buildingSamples = barsToSamples(bars) - peakSamples`, floor-
 *   clamped to >= 1 sample) so `armingToBuildingSamples + buildingSamples +
 *   peakSamples` sums to EXACTLY the quantized-start-point + the requested
 *   bar count, landing Impact's first sample precisely on the downbeat. See
 *   computeDurationsForNewBuild()'s doc comment for the exact math/clamp
 *   rationale.
 *
 *   BUG E ("artifacts then the sound stops"): nothing in the codebase ever
 *   read AudioPlayHead::PositionInfo::isPlaying - a host transport STOP
 *   mid-transition (e.g. the user hits stop in their DAW mid-build) left
 *   the ENTIRE state machine (noise riser still rising, GateModule still
 *   chopping, delay/reverb tails still ringing) running against silence (no
 *   more host audio arriving, or audio that's no longer musically
 *   continuous with what came before) - audible as chattering artifacts
 *   that eventually trail off once Recovery's own tail envelope finally
 *   finishes, which could be seconds later. Fixed: MusicalClock now exposes
 *   isHostPlaying() (see its own doc comment); processBlock() detects
 *   "transition active (state != Idle) AND a valid host playhead reports
 *   isPlaying == false" and gracefully ABORTS - a short (~5ms), click-free
 *   raised-cosine fade of whatever wet/generated content was already
 *   playing, converging to the SAME pre-chain dry capture the final Dry/Wet
 *   stage already uses (so the abort is inert to the user's Dry/Wet knob
 *   setting - post-abort output is pure dry regardless), followed by
 *   forcing the state machine to Idle and running the identical module-
 *   hygiene reset() sweep advanceStateMachineTicks() uses at the natural
 *   Recovery->Idle transition (factored into the new private
 *   resetAllOwnedDspModules() helper so the two call sites can never drift
 *   out of sync). Deliberately gated on a VALID host playhead
 *   (!isUsingFallbackTransport()) - the standalone app / bare-AudioProcessor
 *   fallback path has no concept of "stopped" and must remain fully usable
 *   (this is also why every existing test that never calls setPlayHead()/
 *   uses the fallback transport is completely unaffected by this feature).
 *   fireTriggerIfIdle() additionally refuses to ARM a brand-new build while
 *   a valid host playhead reports stopped (see its own doc comment for the
 *   rationale) - a build immediately aborted again next block would be
 *   pointless, and bar-quantized Arming is only musically meaningful against
 *   a moving transport.
 *
 *   BUG F ("give me a choice to continue the tails, or... clean cut" - user
 *   chose: TAIL knob at 0% = clean cut): the existing `tail` parameter's
 *   0-100% -> 0.15s-4.0s exponential ring-out mapping (see the tail-envelope
 *   session above) had no way to represent "no tail at all" - even at its
 *   minimum, Recovery still rang out for 150ms. Fixed: tail == 0% (within a
 *   small epsilon - see computeDurationsForNewBuild()'s kTailCutEpsilon) now
 *   maps Recovery to a very short (~5ms) duration instead of the 150ms
 *   floor - the EXISTING raised-cosine tail-envelope mechanism in
 *   processSegment() (unchanged) then produces a fast, click-free fade-to-
 *   silence over that short window, audibly an instant clean stop. No new
 *   envelope-shape code was needed - only the DURATION mapping changed.
 *   Every tail value above the epsilon is COMPLETELY UNCHANGED (same
 *   exponential formula, same 0.15s-4.0s range) - "no discontinuity in
 *   feel" going from just-above-0% (still ~0.15s) is therefore automatic,
 *   not something that needed separate blending logic.
 *
 *   FEATURE: VisualizationFrame gained two additive fields (`bpm`,
 *   `hostPlayheadValid` - see that struct's doc comment) so the GUI can show
 *   a "SYNC" readout confirming lock-to-host at a glance (Source/ui/public/
 *   index.html's #syncChip, Advanced page).
 *
 * POST-1.0 THIRD IMPROVEMENT SESSION (2026-09-17, user-directed, stays at
 * 1.0.0). Four related changes, all from one user request:
 *
 *   1. GRADUAL NOISE: NoiseModule's riser used `progress * progress`
 *      (quadratic) - inaudible until very late in the build, then arriving
 *      abruptly. Replaced with the new `drop::NoiseModule::
 *      riseAmountForProgress()` (smoothstep, monotonic, C1-continuous at
 *      both ends) - a pure `static` function so it is provably the SAME
 *      math the Advanced-page TENSION CURVE display's new second curve
 *      plots (see #3). See NoiseModule.h/.cpp for the shape justification.
 *   2. MIDI NOTE SELECTION REMOVED: the "MIDI NOTE" mini-screen chip (added
 *      2026-09-15, Stage 3 GUI Phase 4.2) and its "setMidiTriggerNote"
 *      native-function bridge are removed from Source/PluginEditor.{h,cpp}
 *      and Source/ui/public/index.html (+ the byte-identical
 *      .ideas/mockups/v10-ui.html). DESIGN DECISION: with no UI element left
 *      to pick a note, MIDI trigger mode now responds to ANY note-on
 *      message (see processBlock()'s MIDI-trigger doc comment) - most
 *      useful for triggering from a MIDI clip or a pad. The underlying
 *      `midiTriggerNote` atomic + its persisted `<DropCustomState
 *      midiNote="...">` field are KEPT (unused by trigger detection now) so
 *      a session saved before this change still loads cleanly - see
 *      getMidiTriggerNote()/setStateInformation() below, both unchanged.
 *   3. SECOND CURVE: the Advanced page's TENSION CURVE display now draws a
 *      second curve (GRAY) alongside the existing tension curve (now
 *      PURPLE) showing the noise riser's own rise shape from #1, using the
 *      SAME curved-progress basis the tension curve's x-axis already uses.
 *   4. COLORS: tension curve -> purple (#A78BFA), noise curve -> gray
 *      (#8A8F98, reused from the tension curve's own prior color).
 *
 * No APVTS parameter IDs changed; no DSP processing-chain reordering. See
 * plugins/DROP/CHANGELOG.md for the user-facing changelog entry and
 * .ideas/mockups/v10-ui.yaml for the UI-side amendment notes.
 *
 * See plugins/DROP/CHANGELOG.md for the user-facing changelog entries.
 */
class DROPAudioProcessor : public juce::AudioProcessor,
                            private juce::AudioProcessorValueTreeState::Listener
{
public:
    DROPAudioProcessor();
    ~DROPAudioProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "DROP"; }
    // MIDI input required for the configurable MIDI trigger note
    // (architecture.md "MIDI Routing") - DROP still processes audio
    // (input+output bus below), it is NOT an instrument/synth.
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    // Stage 3 DSP Phase 3.5: the 16 factory presets (Source/Core/
    // DropPreset.*) = the 16 DropStyles at recommended macro defaults
    // (architecture.md Data-Driven Style System > Presets). Message-thread
    // API per JUCE convention (setCurrentProgram() writes APVTS parameters
    // via setValueNotifyingHost(), same as any host-driven parameter change).
    int getNumPrograms() override { return drop::kNumPresets; }
    int getCurrentProgram() override { return currentProgramIndex; }
    void setCurrentProgram(int index) override;
    const juce::String getProgramName(int index) override { return drop::getPreset(index).name; }
    void changeProgramName(int, const juce::String&) override {} // factory presets are read-only, matches this repo's convention for fixed preset lists

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    // Public access to parameters for the editor (matches this repo's
    // established convention, e.g. plugins/Transitionist/Source/PluginProcessor.h).
    juce::AudioProcessorValueTreeState parameters;

    // ------------------------------------------------------------------
    // Non-parameter state (architecture.md "State Persistence" - NOT
    // APVTS parameters, persisted via a child <DropCustomState> ValueTree
    // in getStateInformation()/setStateInformation()). Trigger mode and
    // MIDI note are discrete user preferences (not host-automatable);
    // Chaos seed must be deterministic/reproducible; custom tension curve
    // points only apply when a Custom curve type is selected (out of
    // scope for the `tension` APVTS float itself, which only shapes the
    // authored curve types' steepness).
    // ------------------------------------------------------------------
    enum class TriggerMode
    {
        Manual = 0,
        Midi = 1,
        AutoSync = 2
    };

    struct CurvePoint
    {
        float x = 0.0f;
        float y = 0.0f;
    };

    TriggerMode getTriggerMode() const noexcept { return static_cast<TriggerMode>(triggerMode.load()); }
    void setTriggerMode(TriggerMode mode) noexcept { triggerMode.store(static_cast<int>(mode)); }

    int getMidiTriggerNote() const noexcept { return midiTriggerNote.load(); }
    void setMidiTriggerNote(int noteNumber) noexcept { midiTriggerNote.store(noteNumber); }

    uint32_t getChaosSeed() const noexcept { return chaosSeed.load(); }
    void setChaosSeed(uint32_t seed) noexcept { chaosSeed.store(seed); }

    bool getUiAdvancedRowExpanded() const noexcept { return uiAdvancedRowExpanded.load(); }
    void setUiAdvancedRowExpanded(bool expanded) noexcept { uiAdvancedRowExpanded.store(expanded); }

    // Message-thread-only (UI/state-load access; not read from the audio
    // thread in Stage 1 - only the Custom tension curve type, not yet
    // implemented, will consume this from DSP in a later stage).
    const std::vector<CurvePoint>& getTensionCurvePoints() const noexcept { return tensionCurvePoints; }
    void setTensionCurvePoints(std::vector<CurvePoint> points) { tensionCurvePoints = std::move(points); }

    // ------------------------------------------------------------------
    // Stage 3 DSP Phase 3.1 (Timing & Control Skeleton) - read-only access
    // to the DropStateMachine's current state. Audio-thread-owned data;
    // reading it from elsewhere (e.g. a future GUI Timer poll) is safe
    // because DropState is a small enum (effectively atomic-word-sized
    // reads/writes on every supported platform), but this accessor exists
    // primarily for the Phase 3.1 unit test harness
    // (Source/Tests/RunTests.cpp) - real GUI visualization will use the
    // dedicated lock-free ring buffer described in architecture.md
    // "Thread Boundaries" once DropGraph exists (Stage 3 GUI Phase 4.3).
    // ------------------------------------------------------------------
    drop::DropState getCurrentDropState() const noexcept { return dropStateMachine.getState(); }

    // ------------------------------------------------------------------
    // Stage 3 DSP Phase 3.5 - visualization ring buffer (architecture.md
    // Thread Boundaries: "Audio thread -> UI (visualization): a small
    // lock-free single-producer/single-consumer ring buffer... pushed once
    // per processed block... drained by a juce::Timer (~30-60Hz) on the
    // message thread"). Pushed once per processBlock() call by the audio
    // thread (pushVisualizationFrame(), private); drained by
    // PluginEditor's 30Hz juce::Timer (Stage 3 GUI Phase 4.3, complete) and
    // also once synchronously from the "getInitialUIState" native function
    // at editor load so a reopened editor immediately reflects the current
    // state instead of waiting for the first Timer tick.
    // ------------------------------------------------------------------
    struct VisualizationFrame
    {
        float progress = 0.0f;
        float tensionLevel = 0.0f;
        int currentState = 0; // drop::DropState, stored as int for POD-simplicity across the FIFO
        float inputRMS = 0.0f;

        // PINNED ORDER (do not reorder without updating every GUI JS/native
        // consumer that indexes into this array): Filter, Width, Pitch,
        // Gate, Stutter, Delay, Reverb, Distortion, Digital, Noise - matches
        // drop::BudgetModule's enum declaration order exactly (see
        // IntensityBudget.h). Flagged as previously-unpinned in
        // v10-integration-checklist.md #5's "Open items" note - now fixed
        // here as the single source of truth.
        float activeModuleWeights[10] = {};

        // POST-1.0 additive fields (2026-09-16, BPM diagnostic feature - see
        // this class's top doc comment "POST-1.0 SECOND IMPROVEMENT
        // SESSION"). Additive/appended, not interleaved - matches this
        // struct's own established convention for growing without breaking
        // any existing consumer that only reads the original fields.
        float bpm = 0.0f;              // MusicalClock::getBpm() as of this block - 120.0 while hostPlayheadValid is false (the fixed fallback transport's own BPM), so ALWAYS check hostPlayheadValid before presenting this as "the detected host tempo".
        bool hostPlayheadValid = false; // MusicalClock::isUsingFallbackTransport() == false as of this block - true only when a genuine host bpm/position was observed; drives the Advanced-page "SYNC" readout's "128.0 BPM" (locked) vs. "INT 120" (internal fallback) wording.
    };

    /** Message-thread-only. Returns false when the FIFO is empty. */
    bool popVisualizationFrame(VisualizationFrame& outFrame) noexcept;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    // ------------------------------------------------------------------
    // Stage 3 GUI Phase 4.2 fix: juce::AudioProcessorValueTreeState::Listener
    // override, registered on the "trigger" parameter only
    // (parameters.addParameterListener("trigger", this) in the constructor).
    // Replaces the Phase 3.1-era block-granular `triggerParam->load() > 0.5f`
    // polling edge-detector (see processBlock()'s trigger-event-detection
    // comment for why): that approach snapshots the parameter's value only
    // ONCE per processBlock() call, so at LARGE block sizes (e.g. an
    // 8192-sample block at 44.1kHz is called only every ~186ms) the DROP
    // button's short (~30ms) true->false click pulse (v10-ui.html) could in
    // principle fall entirely BETWEEN two consecutive processBlock() calls
    // and never be observed as `true` by either one - a genuine, block-size-
    // dependent race, not merely a theoretical concern. parameterChanged()
    // instead fires SYNCHRONOUSLY at the exact moment the parameter's value
    // changes (from setValueNotifyingHost()/host automation, on whichever
    // thread performed the write - JUCE's own documented behavior for this
    // listener type), so it can never miss an edge regardless of block size
    // or pulse duration. The callback itself only performs a single atomic
    // store (triggerEventPendingFlag below) - real-time-safe on every
    // thread.
    // ------------------------------------------------------------------
    void parameterChanged(const juce::String& parameterID, float newValue) override;

    // Sample-accurate trigger/state-machine segmentation helpers
    // (architecture.md "Sample-Accurate Events / Block Segmentation").
    // Fires Idle->Arming (re-trigger safety enforced by DropStateMachine
    // itself) using durations computed fresh for this build.
    //
    // POST-1.0 TRANSPORT-STOP SAFETY addition (see this class's top doc
    // comment "POST-1.0 SECOND IMPROVEMENT SESSION", Bug E): also refuses to
    // arm a NEW build while a VALID host playhead (musicalClock's
    // !isUsingFallbackTransport()) reports the transport stopped
    // (!isHostPlaying()) - arming/quantizing bar positions is only
    // musically meaningful against a moving transport, and a build armed
    // against a stopped one would simply be aborted again by processBlock()'s
    // own transport-stop-abort handling the very next block anyway. The
    // standalone/no-playhead fallback transport is NEVER blocked here (it
    // has no concept of "stopped" and must remain fully usable - matches
    // processBlock()'s own abort gating exactly).
    void fireTriggerIfIdle();
    drop::DropStateMachine::Durations computeDurationsForNewBuild();

    // POST-1.0 bugfix helper: advances dropStateMachine.tick() in a loop
    // (same cascade-through-zero-length-states contract as every other
    // `while (dropStateMachine.tick()) {}` call site this class already
    // used - both call sites in processBlock() now route through this
    // method instead), additionally detecting the exact Recovery->Idle
    // transition (the tail envelope's click-free, guaranteed-reached-zero
    // instant - see processSegment()'s "tail envelope" doc comment) and
    // calling resetAllOwnedDspModules() at that moment so no stale tail/
    // capture content replays into the next build (Bug C "stale tails"
    // fix). Deliberately NOT triggered at Idle->Arming (audio is still live
    // then).
    void advanceStateMachineTicks() noexcept;

    // POST-1.0 addition (see this class's top doc comment, Bug E): the
    // "nothing must ring into whatever comes next" module-hygiene reset()
    // sweep, factored out of advanceStateMachineTicks() so it can ALSO be
    // called from processBlock()'s new transport-stop-abort finalize path -
    // both are the same kind of instant (a guaranteed-reached, click-free
    // "this build is genuinely over" moment), and factoring guarantees the
    // two call sites can never drift out of sync.
    void resetAllOwnedDspModules() noexcept;

    // Stage 3 DSP Phase 3.2: processes one sample-accurate segment
    // [startSample, startSample+numSamplesInSegment) of `buffer` through the
    // fixed series DSP chain (Filter -> Width -> Gate -> Delay -> Saturation
    // -> Reverb), using the CURRENT (pre-advance) DropStateMachine state/
    // progress for this segment. Called from processBlock()'s segmentation
    // loop, once per segment. Stage 3 DSP Phase 3.5: weights now come from
    // drop::IntensityBudget::computeWeights() (Style-driven, source-aware,
    // Chaos-jittered) instead of the Phase 3.2-3.4 fixed placeholder (the
    // active Style's filterMode/saturationMode/noiseColor/pitchMode/
    // transitionCurveType are applied once at Arming, in fireTriggerIfIdle() -
    // not re-applied per segment, since they only ever change per-build).
    void processSegment(juce::AudioBuffer<float>& buffer, int startSample, int numSamplesInSegment) noexcept;

    // Derives the 0-1 CURVED "tension" progress from the DropStateMachine's
    // CURRENT state - 0 during Idle/Arming, TransitionEngine::evaluate(t)
    // during Building (t = elapsed/total within Building), 0.0 across BOTH
    // Peak and Impact (Stage 3 DSP Phase 3.4 bugfix, TRANSIENT PRESERVATION:
    // "duck the effect chain, not the source" - ducking starts at
    // Peak/Vacuum entry, not Impact entry, so each module's internal
    // weightSmoothed ramp has genuinely settled to 0 by Impact's first
    // sample; see this method's .cpp doc comment), and 0.0 across Recovery
    // (Bug B fix, see this class's top doc comment).
    //
    // POST-1.0 BUGFIX #2 (2026-09-16, "curve progression" - user-reported
    // real-DAW bug: "I chose 8 bars and the effect is active only at the end
    // of the last bar, while the visual dot on the curve moves"):
    //
    // DECISION MATRIX - this CURVED value (TransitionEngine's own
    // tension-shaped output, itself potentially end-weighted - SCurve/
    // Exponential/LateAcceleration all bias "most tension near the end" by
    // design) is now consumed ONLY by:
    //   1. Each fixed-series DSP module's own `progress` argument
    //      (FilterModule's log-cutoff sweep position, WidthModule's narrow
    //      target, GateModule/StutterModule's step-length interpolation,
    //      PitchModule's semitone-ramp shape, DelayModule's feedback-amount
    //      ramp, SaturationModule's drive ramp, DigitalModule's severity,
    //      NoiseModule's rise/brightness, ReverbModule's roomSize) - every
    //      one of these is a SIMPLE linear lerp/map inside its own module
    //      (verified by source inspection, Source/DSP/*.cpp), so feeding
    //      this curved value applies exactly ONE curve to "how the
    //      character of each effect sweeps over time" - musically
    //      appropriate, since shaping that sweep's steepness/late-
    //      acceleration bias is literally `tension`'s documented job
    //      (parameter-spec.md: "TransitionEngine progress-curve steepness /
    //      late-acceleration bias"). UNCHANGED from pre-fix behavior.
    //   2. The visualization tensionLevel field (lastComputedTensionLevel,
    //      see processSegment()) - "what the user feels" right now, ramping
    //      with the shaped curve and ducking at Peak/Impact/Recovery exactly
    //      like the audio's own module-internal sweeps do.
    //
    // It is DELIBERATELY NO LONGER passed to
    // drop::IntensityBudget::computeWeights() (that call site now uses
    // computeLinearBuildProgress() instead, see below) - computeWeights()
    // internally evaluates EACH module's Style-authored ModuleCurve (itself
    // often ALSO an end-weighted shape, e.g. EaseInCubic - see
    // Source/Core/DropStyle.cpp) plus budgetEnvelope at whatever progress
    // value it's given. Feeding this ALREADY-curved value into a SECOND
    // end-weighted curve composed two end-weighted shapes together
    // (verified: at t=0.25 elapsed through an 8-bar Building with Clean
    // EDM's SCurve/tension=50%, the old stacked path produced ~7% of the
    // final drop's total weight-sum vs. the fixed path's ~36% - see
    // RunTests.cpp's "[POST-1.0] weight progression" test block, which
    // reproduces both the old and new math live and asserts the old path
    // fails the audible-early-progression floor) - exactly the reported
    // "inert for 7 of 8 bars, everything at the end" symptom. THE INVARIANT:
    // no module's effective WET WEIGHT may be the composition of two
    // end-weighted curves; each curve (TransitionEngine's OR a Style's
    // ModuleCurve/budgetEnvelope) is now applied exactly once, in
    // orthogonal roles (WEIGHT vs. internal-sweep TIMBRE/tensionLevel).
    float computeIntensityProgress() noexcept;

    // POST-1.0 addition ("curve progression" fix, see computeIntensityProgress()'s
    // doc comment above for the full decision matrix). Returns the LINEAR
    // elapsed-fraction t (0-1) within the current Building state (0 during
    // every other DropState, matching computeIntensityProgress()'s own
    // state-gating exactly - only the SHAPE of the Building-state value
    // differs between the two methods, not which states duck to 0).
    // Deliberately independent of transitionEngine.evaluate() - this is the
    // SINGLE-CURVE-APPLICATION input fed to
    // drop::IntensityBudget::computeWeights() in processSegment(), so that
    // Style's own authored ModuleCurve/budgetEnvelope shapes are each
    // applied exactly once, not composed on top of TransitionEngine's own
    // curve. Kept as a small, independent, deliberately-duplicated
    // elapsed/total calculation (rather than refactoring
    // computeIntensityProgress() to share a helper) to avoid any risk of
    // perturbing that already-extensively-tested method's exact behavior.
    float computeLinearBuildProgress() noexcept;

    // Stage 3 DSP Phase 3.1 components (declared before non-DSP private
    // state below, matching this repo's DSP-members-first convention).
    drop::MusicalClock musicalClock;
    drop::TransitionEngine transitionEngine;
    drop::DropStateMachine dropStateMachine;

    // Stage 3 DSP Phase 3.2 components (see class-level doc comment for the
    // fixed series processing order).
    drop::FilterModule filterModule;
    drop::WidthModule widthModule;
    drop::GateModule gateModule;
    drop::DelayModule delayModule;
    drop::SaturationModule saturationModule;
    drop::ReverbModule reverbModule;
    // Stage 3 DSP Phase 3.5: IntensityBudget::computeWeights() is now a pure
    // `static` function (no instance state) - see IntensityBudget.h. No
    // member instance needed any more.

    // Stage 3 DSP Phase 3.3 components (Creative DSP Modules). Wired into
    // processSegment() in the architecture.md-mandated fixed series order:
    // ... Gate -> Pitch -> Stutter -> Delay -> Saturation -> Digital -> [+
    // Noise parallel add] -> Reverb. VacuumModule is State-gated (not
    // IntensityBudget weight-blended) - applied only during the
    // DropStateMachine Peak state (this phase's placeholder pre-Impact
    // window; full ImpactEngine coordination lands in Phase 3.4).
    // DropRandomizer is control-rate only (Chaos seed management +
    // musically-constrained randomization, all draws latched at Arming).
    drop::PitchModule pitchModule;
    drop::StutterModule stutterModule;
    drop::DigitalModule digitalModule;
    drop::VacuumModule vacuumModule;
    drop::NoiseModule noiseModule;
    drop::DropRandomizer dropRandomizer;

    // Stage 3 DSP Phase 3.4 component. State-gated to the DropStateMachine
    // Impact state (like VacuumModule is state-gated to Peak) - see
    // processSegment()'s ImpactEngine doc comment for the Vacuum->Impact
    // handoff design.
    drop::ImpactEngine impactEngine;

    // Stage 3 DSP Phase 3.5 component: parallel, read-only tap of the
    // pre-chain dry signal (architecture.md Processing Order step 2), fed
    // once per processBlock() (see processBlock()'s doc comment at the
    // analyze() call site). Its SourceProfile + metrics feed
    // IntensityBudget::computeWeights() via processSegment().
    drop::AudioAnalyzer audioAnalyzer;

    // Pre-chain dry capture (architecture.md Processing Order step 1) +
    // final global Dry/Wet mix / Output gain smoothing (steps 9-10) -
    // preallocated in prepareToPlay(), RT-safe.
    juce::AudioBuffer<float> dryCaptureBuffer;
    juce::SmoothedValue<float> dryWetSmoothed;
    juce::SmoothedValue<float> outputGainSmoothed;

    double currentSampleRate = 44100.0;

    // POST-1.0 bugfix state (see this class's top doc comment "POST-1.0
    // IMPROVEMENT" and processSegment()'s "tail envelope" doc comment).
    // Audio-thread-owned, no atomics needed (read/written only from
    // processBlock()/processSegment()/advanceStateMachineTicks(), all
    // audio-thread-only per this class's existing convention).
    //
    // previousObservedState: the "previous state" half of the Recovery->Idle
    // edge test in advanceStateMachineTicks() - updated on every actual
    // dropStateMachine.tick() transition (NOT polled once per block), so a
    // Recovery state shorter than one segment can never be missed (mirrors
    // this class's existing trigger-edge-detection rationale, just for a
    // DIFFERENT edge).
    drop::DropState previousObservedState = drop::DropState::Idle;

    // frozenDelayReturnWeight / frozenReverbReturnWeight: the LAST
    // Building-state weight actually used for DelayModule/ReverbModule,
    // continuously re-captured every segment while state == Building (so by
    // the time state advances past Building, each holds exactly its final
    // Building-segment value) - the tail envelope's WET-RETURN multiplier
    // during Peak/Impact/Recovery starts from THIS value (not an arbitrary
    // reset), which is what makes the tail envelope C0-continuous at the
    // Building->Peak boundary too, not just Impact->Recovery.
    float frozenDelayReturnWeight = 0.0f;
    float frozenReverbReturnWeight = 0.0f;

    // impactCarryoverElapsedSamples: elapsed-sample counter for
    // ImpactEngine, tracked CONTINUOUSLY across the Impact->Recovery
    // boundary (unlike dropStateMachine's own per-state samplesIntoState,
    // which resets to 0 at every transition) so ImpactEngine's
    // self-terminating envelopes/shortTailReverb never see a discontinuity
    // when PluginProcessor keeps calling it through Recovery (Bug A fix).
    // -1 == "Impact has not been entered yet this build" (or already
    // reset after Recovery ended) - guards against ever calling
    // ImpactEngine during Recovery for a build that never actually reached
    // Impact (e.g. a hypothetical zero-length-Impact edge case).
    int64_t impactCarryoverElapsedSamples = -1;

    // POST-1.0 TRANSPORT-STOP SAFETY state (see this class's top doc comment
    // "POST-1.0 SECOND IMPROVEMENT SESSION", Bug E, and processBlock()'s
    // "transport-stop" doc comment). Audio-thread-owned; no atomics needed
    // (same convention as every other audio-thread-only POST-1.0 bugfix
    // member above - read/written only from processBlock()).
    //
    // transportStopAbortActive: true from the block a host transport-stop is
    // first detected (state != Idle, valid host playhead, isPlaying==false)
    // until the fade below completes and the state machine/modules have been
    // force-reset. While true, processBlock() skips the normal trigger-
    // detection/segmentation/processSegment() pipeline entirely (the abort
    // has already decided "this build is over" - no further wet content
    // should be generated) and instead applies only the fade-to-dry loop.
    bool transportStopAbortActive = false;
    // transportStopFadeElapsedSamples / transportStopFadeTotalSamples: a
    // simple elapsed/total sample counter (same raised-cosine convention as
    // processSegment()'s Recovery tail envelope) driving the fade's per-
    // sample gain from 1.0 (whatever was already playing) down to exactly
    // 0.0 (fully converged to the pre-chain dry capture) over
    // kTransportStopFadeSeconds. Persists across processBlock() calls (not
    // reset per-block) so an abort that needs more samples than a single
    // (possibly very small) host block remains click-free and exactly
    // continuous across the block boundary.
    int64_t transportStopFadeElapsedSamples = 0;
    int64_t transportStopFadeTotalSamples = 0;
    // ~5ms: short enough to read as "instant" (matches the tail=0% "CUT"
    // fade's own duration, Bug F below - both are "stop everything cleanly,
    // fast" instants and deliberately share the same order-of-magnitude
    // fade time for a consistent click-free feel), long enough for a
    // genuinely smooth raised-cosine ramp at every supported sample rate.
    static constexpr double kTransportStopFadeSeconds = 0.005;

    // Stage 3 DSP Phase 3.5: the `style` APVTS parameter's value, LATCHED at
    // Arming (mirroring buildLength/tempo's own latching - architecture.md
    // DropStyle: "Switching style while a build is in-flight... is deferred
    // until the state machine returns to Idle"). Read fresh from
    // styleParam->load() inside fireTriggerIfIdle() every time a trigger is
    // actually accepted; used by processSegment()/computeDurationsForNewBuild()
    // for the ENTIRE resulting build (including while Idle, so a live style
    // change is reflected immediately for a not-yet-triggered preview -
    // harmless since Idle's weights are always 0 regardless of Style).
    // Audio-thread-owned, no atomic needed.
    int activeStyleIndexForBuild = 0;

    // Stage 3 DSP Phase 3.5: JUCE program-management state (see
    // setCurrentProgram()/getCurrentProgram()). Message-thread-only.
    int currentProgramIndex = 0;

    // Stage 3 DSP Phase 3.5: visualization ring buffer (see
    // VisualizationFrame's public doc comment). juce::AbstractFifo is
    // lock-free for exactly one writer (audio thread, pushVisualizationFrame())
    // and one reader (message thread, popVisualizationFrame()) - matches
    // this class's single-writer/single-reader usage exactly.
    void pushVisualizationFrame(const VisualizationFrame& frame) noexcept;
    static constexpr int kVisualizationFifoSize = 256; // generous headroom against a ~30-60Hz GUI drain vs. a per-block (up to several kHz at tiny block sizes) audio-thread push rate
    juce::AbstractFifo visualizationFifo { kVisualizationFifoSize };
    std::array<VisualizationFrame, (size_t) kVisualizationFifoSize> visualizationBuffer {};

    // Stage 3 DSP Phase 3.5: the most recently computed IntensityBudget
    // weights + progress (cached by processSegment(), consumed once per
    // processBlock() by the visualization-frame push at the end of the
    // segmentation loop - see processBlock()'s doc comment there).
    drop::IntensityBudget::Weights lastComputedWeights {};
    // POST-1.0 ("curve progression" fix): now the LINEAR elapsed-build
    // fraction (computeLinearBuildProgress()'s output - the SAME basis fed
    // to IntensityBudget::computeWeights()), not the curved
    // computeIntensityProgress() value it held pre-fix - see
    // computeIntensityProgress()'s doc comment. This is what
    // VisualizationFrame::progress publishes to the UI (drives the tension-
    // curve display dot's X-position and the progress-strip fill), so the
    // dot's motion along the timeline now shares the EXACT same progress
    // basis as the audio weights it's meant to represent (the user
    // explicitly compared the two - "the visual dot on the curve moves"
    // while the audio stayed inert).
    float lastComputedProgress = 0.0f;
    // POST-1.0 addition ("curve progression" fix): the CURVED
    // computeIntensityProgress() value, cached once per processSegment()
    // call for the visualization-frame push at the end of processBlock().
    // Published as VisualizationFrame::tensionLevel - "the curved tension
    // the user feels" building over the course of Building and ducking at
    // Peak/Impact/Recovery exactly like the audio's own module-internal
    // sweeps (computeIntensityProgress()'s consumers) do. DOCUMENTED
    // SUPERSESSION of the pre-fix `tensionParam->load() / 100.0f` (a flat,
    // time-invariant readout of the Tension KNOB's raw setting, not an
    // actual curve-evaluation output, despite PluginEditor.cpp's own
    // pre-existing doc comment already describing it as one).
    float lastComputedTensionLevel = 0.0f;

    // Stage 3 GUI Phase 4.2 DOCUMENTED SUPERSESSION: the Phase 3.1-era
    // `bool previousTriggerParamValue` block-granular POLLING flag is
    // replaced by the pair below, both now driven from parameterChanged()
    // (see its doc comment above) instead of processBlock() sampling
    // triggerParam->load() once per call:
    //   - triggerEventEdgeSeenHigh: the "previous value" half of the rising-
    //     edge test, but evaluated per ACTUAL parameter-change EVENT rather
    //     than per audio block - preserves the exact original low->high-only
    //     semantics (a host re-sending the SAME already-high value, which
    //     some DAWs' automation engines do during static/held automation,
    //     must NOT be treated as a fresh edge; whether or not JUCE's own
    //     AudioProcessorParameter::setValue() internally dedupes
    //     unchanged-value writes before invoking listeners is not something
    //     this code relies on either way - this flag makes the guarantee
    //     explicit and independent of that internal JUCE detail).
    //   - triggerEventPendingFlag: set true only on a genuine low->high
    //     transition; consumed (test-and-clear) once per processBlock() call.
    // Together these reproduce the OLD polling logic's exact rising-edge
    // semantics, but event-driven - the edge itself can no longer be missed
    // regardless of block size or pulse duration (unlike the old approach,
    // which snapshotted the parameter's value only once per processBlock()
    // call and could miss a short UI pulse entirely at large block sizes -
    // see parameterChanged()'s doc comment for the full race analysis). The
    // resulting DropStateMachine transition is still applied at this block's
    // sample offset 0 either way (APVTS exposes no sub-block automation
    // resolution for this parameter).
    std::atomic<bool> triggerEventEdgeSeenHigh { false };
    std::atomic<bool> triggerEventPendingFlag { false };

    // Stage 3 DSP Phase 3.5 DOCUMENTED SUPERSESSION: the Phase 3.4-era fixed
    // `kVacuumMinFloorSeconds`/`kImpactWindowSeconds` constants (25ms Vacuum
    // floor, 220ms Impact window) are REMOVED - computeDurationsForNewBuild()
    // now reads drop::getStyle(activeStyleIndexForBuild)'s vacuumDivision/
    // vacuumFloorSeconds/impactWindowSeconds/recoveryDivision fields instead
    // (architecture.md: "Vacuum duration/floor, Impact tail length, Recovery
    // length are all Style-configurable"). The DEFAULT style (index 0,
    // "Clean EDM") deliberately reproduces the exact prior values (0.025s
    // floor / 0.22s window / ThirtySecond / Half) - see Source/Core/
    // DropStyle.cpp style index 0's doc comment.
    //
    // POST-1.0 DOCUMENTED SUPERSESSION (2026-09-16, GRID-LOCK FIX, Bug D):
    // the Phase 3.4 transient-preservation unit test's precomputed sample
    // offsets (originally "block 760, local offset 184") are SUPERSEDED by
    // the grid-lock fix's re-derivation of `buildingSamples` (see
    // computeDurationsForNewBuild()'s own doc comment) - the Vacuum window
    // is now carved OUT of the requested bar length instead of appended
    // after it, so the same 1-bar-at-120bpm@48kHz/256-sample-block scenario
    // now lands Impact's first sample at global sample 191744 = EXACTLY
    // block 749, local offset 0 (was 194744 = block 760, offset 184) - see
    // RunTests.cpp's "[POST-1.0] Grid lock" test block for the updated,
    // re-derived arithmetic and the corresponding transient-preservation
    // test's updated constants.

    // Default seed is a fixed constant so a freshly-loaded plugin is still
    // deterministic (architecture.md "State Persistence" restore behavior).
    static constexpr uint32_t kDefaultChaosSeed = 1;
    // C3 - arbitrary. POST-1.0 (2026-09-17): the MIDI trigger note is no
    // longer consulted by processBlock()'s MIDI-trigger detection (MIDI
    // trigger mode now responds to ANY note-on - see processBlock()'s doc
    // comment); this default and the getMidiTriggerNote()/
    // setMidiTriggerNote() accessors below are kept only so a session saved
    // before this change (which may have persisted a non-default note in
    // <DropCustomState midiNote="...">) still loads without error.
    static constexpr int kDefaultMidiTriggerNote = 60;

    std::atomic<int> triggerMode { static_cast<int>(TriggerMode::Manual) };
    std::atomic<int> midiTriggerNote { kDefaultMidiTriggerNote };
    std::atomic<uint32_t> chaosSeed { kDefaultChaosSeed };
    std::atomic<bool> uiAdvancedRowExpanded { false };
    std::vector<CurvePoint> tensionCurvePoints; // empty = Linear/no custom points

    // ------------------------------------------------------------------
    // Cached raw parameter pointers (Stage 3 DSP Phase 3.2 validation
    // carry-over from Phase 3.1): all 19 APVTS parameters looked up ONCE in
    // the constructor via parameters.getRawParameterValue(), replacing
    // every per-block string-keyed lookup (real-time-safety/performance -
    // getRawParameterValue(String) does a linear search + String
    // comparisons; the returned std::atomic<float>* itself is what's
    // actually safe/cheap to read every block).
    // ------------------------------------------------------------------
    // Stage 3 GUI Phase 4.2: no longer read for edge-detection (superseded
    // by the parameterChanged()/triggerEventPendingFlag latch above) - kept
    // cached for consistency with "all 19 parameters cached once" and in
    // case any future debug/UI readout wants the raw current value.
    std::atomic<float>* triggerParam = nullptr;
    std::atomic<float>* buildLengthParam = nullptr;
    std::atomic<float>* styleParam = nullptr;
    std::atomic<float>* intensityParam = nullptr;
    std::atomic<float>* tensionParam = nullptr;
    std::atomic<float>* chaosParam = nullptr;
    std::atomic<float>* impactParam = nullptr;
    std::atomic<float>* filterParam = nullptr;
    std::atomic<float>* widthParam = nullptr;
    std::atomic<float>* pitchParam = nullptr;
    std::atomic<float>* gateParam = nullptr;
    std::atomic<float>* stutterParam = nullptr;
    std::atomic<float>* delayParam = nullptr;
    std::atomic<float>* reverbParam = nullptr;
    std::atomic<float>* distortionParam = nullptr;
    std::atomic<float>* digitalParam = nullptr;
    std::atomic<float>* noiseParam = nullptr;
    std::atomic<float>* dryWetParam = nullptr;
    std::atomic<float>* outputParam = nullptr;
    // POST-1.0 addition: the 20th parameter (see this class's top doc
    // comment "POST-1.0 IMPROVEMENT"). Cached alongside the original 19 for
    // consistency, but added AFTER them (not interleaved) so parameter IDs
    // 1-19 remain byte-for-byte untouched, and a 1.0.0 session saved before
    // this parameter existed loads cleanly with the default (APVTS's
    // standard behavior for a ValueTree missing a since-added parameter
    // node - see createParameterLayout()'s doc comment at the `tail` entry).
    std::atomic<float>* tailParam = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DROPAudioProcessor)
};
