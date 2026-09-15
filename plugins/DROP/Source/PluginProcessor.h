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
#include "Core/DropRandomizer.h"
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

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

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
    // loop, once per segment.
    void processSegment(juce::AudioBuffer<float>& buffer, int startSample, int numSamplesInSegment) noexcept;

    // Derives the 0-1 "budget progress" input fed to IntensityBudget::compute()
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
    drop::IntensityBudget intensityBudget;

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

    // Pre-chain dry capture (architecture.md Processing Order step 1) +
    // final global Dry/Wet mix / Output gain smoothing (steps 9-10) -
    // preallocated in prepareToPlay(), RT-safe.
    juce::AudioBuffer<float> dryCaptureBuffer;
    juce::SmoothedValue<float> dryWetSmoothed;
    juce::SmoothedValue<float> outputGainSmoothed;

    double currentSampleRate = 44100.0;

    // Edge-detection state for the `trigger` APVTS parameter (see
    // processBlock()'s trigger-event-detection comment for why this is
    // block-granular rather than true sub-sample-accurate).
    bool previousTriggerParamValue = false;

    // Stage 3 DSP Phase 3.4: musically-derived Peak(Vacuum)/Impact/Recovery
    // window lengths, replacing the Phase 3.1-era fixed-seconds placeholders.
    // DropStyle-authored per-Style values (architecture.md: Vacuum
    // duration/floor, Impact tail length, Recovery length are all
    // Style-configurable) land in Phase 3.5 - these are deterministic,
    // documented architecture defaults, not yet Style-driven.
    //
    // kVacuumMinFloorSeconds: architecture.md Implementation Risks >
    // VacuumModule Click-Free Collapse fallback: "clamp the minimum Vacuum
    // duration to a fixed millisecond floor (e.g. 8-10ms)... a
    // barely-longer Vacuum is a smaller compromise than an audible click."
    // Stage 3 DSP Phase 3.4 bugfix: raised from the originally-documented
    // 8ms to 25ms - this floor now does DOUBLE duty. Since
    // computeIntensityProgress() ducks the 10 shared-budget modules to 0
    // starting at PEAK entry (not Impact entry - see that method's doc
    // comment), Peak's ACTUAL duration must exceed each module's internal
    // weightSmoothed ramp time (~20ms, see e.g. WidthModule.cpp's
    // kWeightSmoothingSeconds) with margin, so the duck has GENUINELY
    // settled (not merely started ramping) by the time Impact's first
    // sample arrives, at every tempo (jmax() below guarantees Peak's
    // duration is never shorter than this floor, regardless of how short a
    // literal 1/32 note becomes at extreme tempos).
    // kImpactWindowSeconds: architecture.md ImpactEngine config: "typically
    // 50-300ms" - a fixed, non-tempo-synced duration (unlike Peak/Vacuum,
    // this window is characterized by the sub-hit's own AR-envelope timing,
    // not a musical division).
    static constexpr double kVacuumMinFloorSeconds = 0.025;
    static constexpr double kImpactWindowSeconds = 0.22;

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
