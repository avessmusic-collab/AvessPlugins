#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include <array>
#include <atomic>
#include "dsp/ModulationAccumulator.h"

class CORRUPTRAudioProcessor : public juce::AudioProcessor
{
public:
    CORRUPTRAudioProcessor();
    ~CORRUPTRAudioProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "CORRUPTR"; }
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

    juce::AudioProcessorValueTreeState& getAPVTS() { return parameters; }

private:
    //=========================================================================
    // Stage 2 Phase 3.1: Feedback Routing Safety Validation (Isolated)
    //
    // See architecture.md component #7 ("Feedback Routing Path") and
    // plan.md's "Phase 3.1: Feedback Routing Safety Validation (Isolated)".
    // This is the single highest-risk component in CORRUPTR (potential for
    // runaway/audible-harm if the safety math is wrong) and is deliberately
    // built + proven in isolation BEFORE the Distortion Engine (#2) or
    // Filter Stage (#6) exist to be spliced into the loop. Those two
    // components' splice points are marked with TODO comments in
    // processBlock() below and will be wired in during Phase 3.3/3.4.
    //
    // Safety design (non-negotiable per architecture.md):
    //   1. Soft-clamp gain: internalGain = kFeedbackMaxSafeGain * tanh(feedbackAmount/100)
    //      -> tanh() is bounded in [-1,1] for ALL finite inputs, so the
    //      *parameter itself* can never command unity/runaway gain.
    //   2. In-loop one-pole lowpass damping (feedbackDamping -> cutoff Hz)
    //      attenuates HF energy every pass around the loop.
    //   3. Per-sample std::isfinite() circuit breaker - hard-resets the
    //      delay line + filter + RMS state for that channel if a NaN/Inf
    //      is ever produced (checked every sample, not just once per block,
    //      since a bad sample must never be allowed to recirculate even for
    //      the remainder of the current block - unlike a feedforward chain,
    //      a feedback loop can sustain a bad sample indefinitely).
    //   4. Recommended belt-and-suspenders addition (architecture.md):
    //      continuous RMS-envelope limiter on the feedback path itself,
    //      independent of the tanh gain clamp and of the (not-yet-built)
    //      main Output Limiter (component #13).
    //=========================================================================
    static constexpr float kFeedbackMaxSafeGain = 0.85f;      // architecture.md "Feedback Safety Soft-Clamp" — constant well below 1.0
    static constexpr float kFeedbackDampingMinHz = 200.0f;    // feedbackDamping = 100% -> heaviest HF cut
    static constexpr float kFeedbackDampingMaxHz = 18000.0f;  // feedbackDamping = 0%   -> near-transparent
    static constexpr float kFeedbackRmsLimitThreshold = 0.95f;
    static constexpr float kFeedbackRmsTimeConstantMs = 50.0f;
    static constexpr double kFeedbackDelayHeadroomSeconds = 0.060; // microDelayTime max is 50ms; allocate to 60ms for automation-sweep headroom

    juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Linear> feedbackDelayLine { 1 };
    std::array<juce::dsp::FirstOrderTPTFilter<float>, 2> feedbackDampingFilter;
    std::array<float, 2> feedbackRmsEnvelope { 0.0f, 0.0f };
    float feedbackRmsOnePoleCoeff = 0.0f;
    int feedbackMaxDelaySamples = 0;
    juce::SmoothedValue<float> feedbackInternalGainSmoothed;
    juce::SmoothedValue<float> feedbackDampingCutoffSmoothed;
    std::atomic<bool> feedbackCircuitBreakerTripped { false }; // diagnostic only (not read for control flow on the audio thread)

    // Real-time-safe: bounded loop over the already-allocated delay buffer
    // (feedbackMaxDelaySamples iterations, no allocation) - the circuit
    // breaker's "hard reset to zero for that channel" mechanism.
    void resetFeedbackLoopChannel(int channel);

    //=========================================================================
    // Stage 2 Phase 3.2: Unified Modulation Accumulator (Isolated)
    //
    // See dsp/ModulationAccumulator.h for the generic, reusable combine-math
    // class (Performance Trigger hard-override, else additive Sequencer +
    // Mod Matrix + Macro combine, always clamped to the destination's valid
    // range - architecture.md's recommended default rule). This section
    // wires that class up to ONE destination only (`drive`, 0-40dB) per
    // plan.md's recommended build order, using SYNTHETIC stand-ins for the
    // Sequencer and Mod Matrix contributions (neither subsystem exists yet -
    // built in Phase 3.7/3.8) and the REAL `macroDamage` APVTS parameter for
    // the Macro contribution (macros are genuine Stage 1 parameters already).
    //
    // Deliberately NOT wired into the live audio signal path yet - this
    // phase proves the accumulator's math in isolation, matching Phase
    // 3.1's "isolated" pattern. The Distortion Engine (architecture.md
    // component #2, which would actually READ a modulated `drive` value)
    // doesn't exist yet either (built in Phase 3.3), so there is nothing
    // for this observation point to feed even if it wanted to. Computed
    // once per block (diagnostic/future-use only, not consumed anywhere
    // yet) and stored in `phase32DriveModulationObservation` below.
    //=========================================================================
    std::atomic<bool> modulationAccumulatorSelfTestPassed { false }; // set once in the constructor (see .cpp)
    std::atomic<float> phase32DriveModulationObservation { 0.0f };   // diagnostic only - last computed modulated `drive` value, not yet applied to any DSP
    double phase32SyntheticSequencerPhase = 0.0;  // free-running phase for the synthetic Sequencer-lane stand-in (advances once per block)
    double phase32SyntheticModMatrixPhase = 0.0;  // free-running phase for the synthetic Mod-Matrix-slot stand-in (advances once per block, different rate than the above so the two are distinguishable)

    juce::AudioProcessorValueTreeState parameters;

    // Parameter layout creation
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CORRUPTRAudioProcessor)
};
