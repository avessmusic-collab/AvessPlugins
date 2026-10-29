#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <atomic>

#include "DSP/TensionEngine.h"

namespace TensionParams
{
    // Stable parameter IDs (parameter-spec.md, LOCKED). Never rename.
    inline constexpr const char* tension  = "tension";
    inline constexpr const char* mix      = "mix";
    inline constexpr const char* output   = "output";
    inline constexpr const char* sync     = "sync";
    inline constexpr const char* riseMode = "riseMode";

    inline constexpr int parameterVersion = 1;
    inline constexpr int stateSchemaVersion = 1;
}

class TENSIONAudioProcessor : public juce::AudioProcessor
{
public:
    TENSIONAudioProcessor();
    ~TENSIONAudioProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "TENSION"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 8.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState& getAPVTS() noexcept { return parameters; }

    static const juce::Identifier stateType;

    // Lock-free audio -> UI telemetry (tension, out peak, wet RMS, noise level, LFO phase).
    const TensionTelemetry& getTelemetry() const noexcept { return engine.getTelemetry(); }
    TensionEngine& getEngine() noexcept { return engine; }

private:
    juce::AudioProcessorValueTreeState parameters;

    TensionEngine engine;

    // Cached raw parameter pointers (no string lookups on the audio thread).
    std::atomic<float>* tensionParam  = nullptr;
    std::atomic<float>* mixParam      = nullptr;
    std::atomic<float>* outputParam   = nullptr;
    std::atomic<float>* syncParam     = nullptr;
    std::atomic<float>* riseModeParam = nullptr;

    HostTransport readHostTransport() const;

    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TENSIONAudioProcessor)
};
