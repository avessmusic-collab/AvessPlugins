// CHOPLINE — PluginProcessor
// Phase 1: owns the APVTS and the Engine (sampler, slicer, voice pool, MIDI
// triggering). State persists the parameters plus the sample reference
// (path + hash + optional embedded FLAC) and the slice map (spec §10).

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "engine/Engine.h"
#include "state/Parameters.h"

#include <limits>

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
    // Master delay/reverb tails (Phase 4): at 95 % feedback the delay rings
    // for minutes, so report an infinite tail (JUCE wrappers map this to the
    // host's "infinite tail" flag) rather than a constant a bounce could trim.
    double getTailLengthSeconds() const override
    {
        return std::numeric_limits<double>::infinity();
    }

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
    std::atomic<float>* swingParam = nullptr;

    // Phase 4: cached raw parameter values (spec §8 — no APVTS lookups on the
    // audio thread). Filled once in the constructor.
    struct Phase4Params
    {
        std::atomic<float>* brMode = nullptr;
        std::atomic<float>* brHold = nullptr;
        std::atomic<float>* brInterval = nullptr;
        std::atomic<float>* brOffset = nullptr;
        std::atomic<float>* brChance = nullptr;
        std::atomic<float>* brGrid = nullptr;
        std::atomic<float>* brGate = nullptr;
        std::atomic<float>* brDecay = nullptr;
        std::atomic<float>* brPitchDecay = nullptr;
        std::atomic<float>* brAccel = nullptr;
        std::atomic<float>* brReverse = nullptr;
        std::atomic<float>* brFilterMode = nullptr;
        std::atomic<float>* brSweep = nullptr;
        std::atomic<float>* brMix = nullptr;
        std::atomic<float>* brFreeze = nullptr;
        std::atomic<float>* brRndGrid = nullptr;
        std::atomic<float>* brMidiBase = nullptr;

        std::atomic<float>* filterOn = nullptr;
        std::atomic<float>* filterType = nullptr;
        std::atomic<float>* filterCutoff = nullptr;
        std::atomic<float>* filterReso = nullptr;
        std::atomic<float>* distOn = nullptr;
        std::atomic<float>* distType = nullptr;
        std::atomic<float>* distDrive = nullptr;
        std::atomic<float>* distMix = nullptr;
        std::atomic<float>* distAutoGain = nullptr;
        std::atomic<float>* widthOn = nullptr;
        std::atomic<float>* width = nullptr;
        std::atomic<float>* mono = nullptr;
        std::atomic<float>* limiterOn = nullptr;
        std::atomic<float>* output = nullptr;
        std::atomic<float>* ceiling = nullptr;
        std::atomic<float>* delayOn = nullptr;
        std::atomic<float>* delayTime = nullptr;
        std::atomic<float>* delayFb = nullptr;
        std::atomic<float>* delayRtn = nullptr;
        std::atomic<float>* verbOn = nullptr;
        std::atomic<float>* verbDecay = nullptr;
        std::atomic<float>* verbDamp = nullptr;
        std::atomic<float>* verbRtn = nullptr;
    } p4;

    // Guards the message-thread hop in setStateInformation against destruction.
    std::shared_ptr<bool> aliveFlag = std::make_shared<bool> (true);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChoplineAudioProcessor)
};

} // namespace chopline
