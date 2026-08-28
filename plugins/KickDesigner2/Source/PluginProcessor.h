#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "Parameters/ParameterLayout.h"

/**
    KickDesigner2 — dedicated algorithmic kick-design instrument.

    Stage 1 (Foundation + Shell):
      - Full 45-parameter APVTS (see Parameters/ParameterLayout.h).
      - Output-only stereo bus, configured in the constructor (Pattern #4 / #22).
      - juce::UndoManager attached to the APVTS (Randomize / Mutate / Save use it in Stage 3).
      - State round-trip with a top-level `stateVersion` int + preset-name / -path properties.
      - Empty (silent) engine — processBlock clears the buffer. DSP arrives in Stage 2.
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

    static constexpr int kStateVersion = 1;

private:
    juce::UndoManager                    undoManager;
    juce::AudioProcessorValueTreeState   apvts;

    double currentSampleRate { 44100.0 };
    int    currentBlockSize  { 512 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (KickDesigner2AudioProcessor)
};
