// CHOPLINE — PluginProcessor
// Phase 1: owns the APVTS and the Engine (sampler, slicer, voice pool, MIDI
// triggering). State persists the parameters plus the sample reference
// (path + hash + optional embedded FLAC) and the slice map (spec §10).

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
    ~ChoplineAudioProcessor() override;

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
    engine::Engine& getEngine() noexcept { return engine; }

private:
    juce::AudioProcessorValueTreeState state;
    engine::Engine engine;
    std::atomic<float>* baseNoteParam = nullptr;

    // Guards the message-thread hop in setStateInformation against destruction.
    std::shared_ptr<bool> aliveFlag = std::make_shared<bool> (true);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChoplineAudioProcessor)
};

} // namespace chopline
