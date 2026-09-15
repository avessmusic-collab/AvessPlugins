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
 * parameters) for full detail.
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
 * (VisualizationFrame, popVisualizationFrame()) is now populated once per
 * processBlock() call, ready for the Stage 3 GUI Phase 4.3 DropGraph to
 * drain via a juce::Timer - see VisualizationFrame's doc comment for the
 * PINNED activeModuleWeights[10] index order.
 */
class DROPAudioProcessor : public juce::AudioProcessor
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
    // thread (pushVisualizationFrame(), private); drained here by the
    // message thread (Stage 3 GUI Phase 4.3's DropGraph will call this from
    // a juce::Timer - not yet wired to any GUI code this phase).
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

    // Sample-accurate trigger/state-machine segmentation helpers
    // (architecture.md "Sample-Accurate Events / Block Segmentation").
    // Fires Idle->Arming (re-trigger safety enforced by DropStateMachine
    // itself) using durations computed fresh for this build.
    void fireTriggerIfIdle();
    drop::DropStateMachine::Durations computeDurationsForNewBuild();

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

    // Edge-detection state for the `trigger` APVTS parameter (see
    // processBlock()'s trigger-event-detection comment for why this is
    // block-granular rather than true sub-sample-accurate).
    bool previousTriggerParamValue = false;

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

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DROPAudioProcessor)
};
