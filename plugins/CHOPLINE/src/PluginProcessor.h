// CHOPLINE — PluginProcessor
// Phase 0: silent passthrough shell. Owns the APVTS (empty layout for now)
// and the Engine stub. MIDI is accepted (synth) but ignored until Phase 1.

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "engine/Engine.h"
#include "state/Parameters.h"

namespace chopline
{

class ChoplineAudioProcessor final : public juce::AudioProcessor
{
public:
    ChoplineAudioProcessor();
    ~ChoplineAudioProcessor() override = default;

    // AudioProcessor
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState& getState() noexcept { return state; }

private:
    juce::AudioProcessorValueTreeState state;
    engine::Engine engine;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChoplineAudioProcessor)
};

} // namespace chopline
