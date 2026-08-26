#pragma once
#include <juce_audio_processors/juce_audio_processors.h>

/**
 * Transitionist - Stage 1 (Foundation + Shell)
 *
 * Pure audio effect (stereo in -> stereo out), no MIDI, no file I/O.
 * DSP implementation (Delay -> Reverb -> Bipolar DJ Filter -> Output Glue ->
 * Width, per architecture.md) is added in Stage 2. This stage establishes the
 * build system, bus configuration, and the 3-parameter APVTS contract
 * (throw, space, sweep) per parameter-spec.md.
 */
class TransitionistAudioProcessor : public juce::AudioProcessor
{
public:
    TransitionistAudioProcessor();
    ~TransitionistAudioProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Transitionist"; }
    bool acceptsMidi() const override { return false; }
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

    // Public access to parameters for editor (Stage 3 GUI binds via this member,
    // following this codebase's established convention - see AngelGrain/GainKnob).
    juce::AudioProcessorValueTreeState parameters;

private:
    // Parameter layout creation - implements the locked 3-parameter contract
    // from parameter-spec.md: throw, space, sweep (in this exact order).
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TransitionistAudioProcessor)
};
