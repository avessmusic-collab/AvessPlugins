#pragma once

#include <atomic>
#include <memory>
#include <vector>

#include <juce_audio_processors/juce_audio_processors.h>

#include "Parameters/ParameterLayout.h"
#include "DSP/KickEngine.h"
#include "DSP/Analyzer.h"
#include "Sampling/SampleLibrary.h"
#include "Sampling/SampleBuffer.h"
#include "Presets/PresetManager.h"

/**
    KICKR — dedicated kick-design instrument: algorithmic synthesis +
    a sample-playback layer (parameter-spec v2).

    Stage 1 (Foundation + Shell) + v2 SAMPLE amendment:
      - Full 59-parameter APVTS (see Parameters/ParameterLayout.h): 51 Float, 3 Choice, 5 Bool.
      - Output-only stereo bus, configured in the constructor (Pattern #4 / #22).
      - juce::UndoManager attached to the APVTS (Randomize / Mutate / Save use it in Stage 3).
      - State round-trip: `stateVersion` = 2, preset name/path + `currentSampleName` properties;
        v1 (pre-sample) states load unchanged.
      - Empty (silent) engine — processBlock clears the buffer. DSP arrives in Stage 2
        (synth Phases 2.1–2.6/2.7, sample player + library Phase 2.7b).
*/
class KICKRAudioProcessor final : public juce::AudioProcessor,
                                  private juce::AudioProcessorValueTreeState::Listener
{
public:
    KICKRAudioProcessor();
    ~KICKRAudioProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "KICKR"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 2.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    // Accessors for the editor / preset manager.
    juce::AudioProcessorValueTreeState& getValueTreeState() noexcept { return apvts; }
    juce::UndoManager& getUndoManager() noexcept { return undoManager; }

    // v2: bare file name of the loaded sample from the managed bank ("" = none).
    const juce::String& getCurrentSampleName() const noexcept { return currentSampleName; }

    /** PHASE 2.7b — message thread: decode `bareName` from the managed bank and publish
        the buffer to the audio thread (also called from `setStateInformation` for preset /
        session recall, and later from the Stage-3 UI). Missing file -> layer silent, name
        retained, no crash. "" -> no sample. */
    void loadSampleByName (const juce::String& bareName);

    /** PHASE 2.7b — managed sample bank (message thread only). Stage-3 SampleBrowser +
        drag-drop import go through this. */
    kickr::SampleLibrary& getSampleLibrary() noexcept { return sampleLibrary; }

    /** PHASE 3.2 — real-time waveform + spectrum analyzer taps. The editor's
        `WaveformDisplay` / `SpectrumDisplay` own the message-thread `Timer`s that
        read from this; the audio thread only does a bounded `memcpy` + atomic store
        + `AbstractFifo` write per block (see `processBlock`). */
    kickr::Analyzer& getAnalyzer() noexcept { return analyzer; }

    /** PHASE 3.3 — factory + user presets, Randomize / Mutate (message thread only). */
    kickr::PresetManager& getPresetManager() noexcept { return presetManager; }

    static constexpr int kStateVersion = 2;   // v2: SAMPLE group + currentSampleName

    /** 2026-09-02 — the Color Limiter's look-ahead delay at the BASE rate (0 when the
        limiter is off). Read from the raw parameter atomics — safe on either thread.
        Total reported latency = oversampling latency + this. */
    int limiterLatencySamples() const noexcept;

private:
    void retireUnreferenced();   // PHASE 2.7b — free unreferenced sample buffers (message thread)

    /** PHASE 2.10 — message thread: on an `oversampling` change, report the pending
        factor's latency to the host. Never called from processBlock. */
    void parameterChanged (const juce::String& parameterID, float newValue) override;

    juce::UndoManager                    undoManager;
    juce::AudioProcessorValueTreeState   apvts;

    // Stage 2 Phase 2.1: OS-region shell + MIDI-triggered sine body + amp env.
    kickr::KickEngine                    engine { apvts };

    // Stage 3 Phase 3.2: lock-free waveform + spectrum taps (message-thread FFT).
    kickr::Analyzer                      analyzer;

    juce::String currentSampleName;           // v2 — bare file name ("" = none)

    // PHASE 2.7b — lock-free sample-buffer hand-off (architecture.md AD-11):
    //   message thread owns the objects (double slot + retired list); audio thread only
    //   ever does two atomic loads + one store per block, and one atomic load per noteOn.
    kickr::SampleLibrary                              sampleLibrary;
    std::unique_ptr<kickr::SampleBuffer>             ownedSampleA, ownedSampleB;
    int                                              liveSampleSlot { -1 };   // -1 / 0 / 1
    std::atomic<const kickr::SampleBuffer*>          currentSample { nullptr };
    std::atomic<const kickr::SampleBuffer*>          pendingSample { nullptr };
    std::vector<std::unique_ptr<kickr::SampleBuffer>> retiredSamples;

    // 2026-09-02 (code-review): the two limiter atomics behind limiterLatencySamples() are
    // cached once here — it is called from processBlock on every note-on, and a parameter
    // lookup by ID there was a map search on the audio thread.
    std::atomic<float>* pLimiterOn        { nullptr };
    std::atomic<float>* pLimiterLookahead { nullptr };

    double currentSampleRate { 44100.0 };
    int    currentBlockSize  { 512 };

    // Stage 3 Phase 3.3: presets + Randomize / Mutate. Reloads the sample layer through
    // this processor's own `loadSampleByName` when a preset carries a `currentSampleName`.
    kickr::PresetManager presetManager {
        apvts, undoManager,
        [this] (const juce::String& n) { loadSampleByName (n); },
        [this] { return getCurrentSampleName(); }
    };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (KICKRAudioProcessor)
};
