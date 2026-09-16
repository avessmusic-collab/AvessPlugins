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
    void fireTriggerIfIdle();
    drop::DropStateMachine::Durations computeDurationsForNewBuild();

    // POST-1.0 bugfix helper: advances dropStateMachine.tick() in a loop
    // (same cascade-through-zero-length-states contract as every other
    // `while (dropStateMachine.tick()) {}` call site this class already
    // used - both call sites in processBlock() now route through this
    // method instead), additionally detecting the exact Recovery->Idle
    // transition (the tail envelope's click-free, guaranteed-reached-zero
    // instant - see processSegment()'s "tail envelope" doc comment) and
    // resetting DelayModule/ReverbModule/ImpactEngine/StutterModule at that
    // moment so no stale tail/capture content replays into the next build
    // (Bug C "stale tails" fix). Deliberately NOT triggered at Idle->Arming
    // (audio is still live then).
    void advanceStateMachineTicks() noexcept;

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

    // Derives the 0-1 "budget progress" input fed to IntensityBudget::computeWeights()
    // from the DropStateMachine's CURRENT state - 0 during Idle/Arming,
    // TransitionEngine::evaluate(t) during Building (t = elapsed/total
    // within Building), 0.0 across BOTH Peak and Impact (Stage 3 DSP Phase
    // 3.4 bugfix, TRANSIENT PRESERVATION: "duck the effect chain, not the
    // source" - ducking starts at Peak/Vacuum entry, not Impact entry, so
    // each module's internal weightSmoothed ramp has genuinely settled to 0
    // by Impact's first sample; see this method's .cpp doc comment),
    // ramping 1.0->0.0 across Recovery (architecture.md: "all module
    // weights ramp back to 0" during Recovery).
    float computeIntensityProgress() noexcept;

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
    float lastComputedProgress = 0.0f;

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
    // floor / 0.22s window / ThirtySecond / Half) so the pre-existing Phase
    // 3.4 transient-preservation unit test's precomputed sample offsets
    // (block 760, local offset 184) remain correct unmodified - see
    // Source/Core/DropStyle.cpp style index 0's doc comment.

    // Default seed is a fixed constant so a freshly-loaded plugin is still
    // deterministic (architecture.md "State Persistence" restore behavior).
    static constexpr uint32_t kDefaultChaosSeed = 1;
    static constexpr int kDefaultMidiTriggerNote = 60; // C3 - arbitrary, configurable; no default UI element yet (flagged gap, v10-integration-checklist.md #5)

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
