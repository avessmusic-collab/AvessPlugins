#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include "DSP/MusicalClock.h"
#include "DSP/TransitionEngine.h"
#include "DSP/DropStateMachine.h"
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

    // Stage 3 DSP Phase 3.1 components (declared before non-DSP private
    // state below, matching this repo's DSP-members-first convention).
    drop::MusicalClock musicalClock;
    drop::TransitionEngine transitionEngine;
    drop::DropStateMachine dropStateMachine;

    double currentSampleRate = 44100.0;

    // Edge-detection state for the `trigger` APVTS parameter (see
    // processBlock()'s trigger-event-detection comment for why this is
    // block-granular rather than true sub-sample-accurate).
    bool previousTriggerParamValue = false;

    // Phase 3.1 placeholder Peak/Impact/Recovery window lengths - VacuumModule/
    // ImpactEngine/DropStyle (which will supply real, Style-authored values)
    // land in Stage 3 DSP Phases 3.3-3.5. These are deterministic,
    // sample-accurate placeholders, not final musical values.
    static constexpr double kPlaceholderPeakSeconds = 0.01;
    static constexpr double kPlaceholderImpactSeconds = 0.2;
    static constexpr double kPlaceholderRecoverySeconds = 0.5;

    // Default seed is a fixed constant so a freshly-loaded plugin is still
    // deterministic (architecture.md "State Persistence" restore behavior).
    static constexpr uint32_t kDefaultChaosSeed = 1;
    static constexpr int kDefaultMidiTriggerNote = 60; // C3 - arbitrary, configurable; no default UI element yet (flagged gap, v10-integration-checklist.md #5)

    std::atomic<int> triggerMode { static_cast<int>(TriggerMode::Manual) };
    std::atomic<int> midiTriggerNote { kDefaultMidiTriggerNote };
    std::atomic<uint32_t> chaosSeed { kDefaultChaosSeed };
    std::atomic<bool> uiAdvancedRowExpanded { false };
    std::vector<CurvePoint> tensionCurvePoints; // empty = Linear/no custom points

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DROPAudioProcessor)
};
