#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include <array>
#include <atomic>

/**
 * Transitionist - v2 DSP (full redesign, 2026-08-26)
 *
 * Pure audio effect (stereo in -> stereo out), no MIDI, no file I/O.
 * Signal chain: Input Gain -> [dry tap] -> Ping-Pong Delay (tempo-synced,
 * transition+delay driven) -> Reverb (transition+reverb driven, with
 * freeze/hold) -> Output Glue (fixed soft-clip+limiter) -> Dry/Wet Mix
 * (against the pre-effects dry tap) -> Output Gain -> [level meter].
 *
 * Replaces v1's 3-macro/bipolar-filter design (throw/space/sweep) with 7
 * explicit parameters (transition, reverb, delay, delaySync, dryWet,
 * inputGain, outputGain) plus 2 UI-only level meters. See
 * .ideas/creative-brief.md and .ideas/parameter-spec.md (v2) for full
 * rationale.
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

    // Public access to parameters for editor.
    juce::AudioProcessorValueTreeState parameters;

    // Live level meters (UI-only, NOT parameters - no automation, no state
    // save). Peak dB, updated every processBlock(), read by the editor's
    // Timer at 30Hz. See parameter-spec.md's "UI-Only Level Meters" section.
    std::atomic<float> inputLevelDb { -100.0f };
    std::atomic<float> outputLevelDb { -100.0f };

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    double currentSampleRate = 44100.0;
    int maxDelaySamples = 0;

    // ------------------------------------------------------------------
    // Ping-Pong Delay: two delay lines (A/B). Fresh input is injected only
    // into A; A's tap feeds B (no fresh input), B's tap feeds back into A -
    // this cross-feed (not two independently-synced delays) is what
    // produces genuine alternating L/R bounces. Feedback path through each
    // line includes a tanh saturator + one-pole lowpass damping filter
    // (carried forward from v1's delay character).
    // ------------------------------------------------------------------
    juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Lagrange3rd> delayLineA;
    juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Lagrange3rd> delayLineB;
    juce::SmoothedValue<float> smoothedDelaySamples;
    float lastTargetDelaySamples = -1.0f;
    std::array<juce::dsp::FirstOrderTPTFilter<float>, 2> feedbackFilter; // [0]=A->B path, [1]=B->A path

    // ------------------------------------------------------------------
    // Reverb, with Phase 4.2-style smoothstep freeze/hold (carried forward
    // from v1, now gated by transition+reverb instead of throw+space).
    // ------------------------------------------------------------------
    juce::dsp::Reverb reverb;

    // ------------------------------------------------------------------
    // Output Glue - fixed internal safety net (soft-clip + limiter), not a
    // user parameter. Carried forward unchanged from v1.
    // ------------------------------------------------------------------
    juce::dsp::WaveShaper<float> softClip;
    juce::dsp::Limiter<float> limiter;

    // Preallocated scratch buffers (real-time safety - no allocation in
    // processBlock()).
    juce::AudioBuffer<float> dryBuffer;       // true dry tap (post input gain, pre-effects) - for the final dryWet blend
    juce::AudioBuffer<float> reverbWetBuffer; // reverb's own processing copy (real-time safe - no allocation in processBlock())

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TransitionistAudioProcessor)
};
