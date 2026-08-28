#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "Parameters/ParameterLayout.h"

/**
    KickDesigner2 — dedicated kick-design instrument: algorithmic synthesis +
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
class KickDesigner2AudioProcessor final : public juce::AudioProcessor
{
public:
    KickDesigner2AudioProcessor();
    ~KickDesigner2AudioProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Kick Designer 2"; }
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
    // Full SampleLibrary / SamplePlayer wiring is Stage 2 Phase 2.7b.
    const juce::String& getCurrentSampleName() const noexcept { return currentSampleName; }

    static constexpr int kStateVersion = 2;   // v2: SAMPLE group + currentSampleName

private:
    juce::UndoManager                    undoManager;
    juce::AudioProcessorValueTreeState   apvts;

    juce::String currentSampleName;           // v2 — see Phase 2.7b

    double currentSampleRate { 44100.0 };
    int    currentBlockSize  { 512 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (KickDesigner2AudioProcessor)
};
