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

    //=========================================================================
    // Stage 2 Phase 3.3: Core Linear Chain
    //
    // Working, non-modulated core signal path: Input Gain -> Distortion
    // Engine (12 algorithms) -> Bitcrusher/SRR -> Filter Stage (7
    // topologies) -> Master Mix (0-200%) -> Output Gain -> Output Limiter
    // (dual-mode). See architecture.md components #1, #2, #3, #6, #13, #14
    // and the "Sequential DSP chain (per block, REQUIRED order)" section
    // (steps 3, 5, 6, 9, 11, 12, 13). Oversampling, Glitch, Sequencer, Mod
    // Matrix, Macros, and Feedback-loop integration are explicitly OUT of
    // scope for this phase — native sample rate only, no oversampling
    // wrapper yet. Phase 3.1's feedback-loop code and Phase 3.2's
    // modulation-accumulator observation code above are untouched and NOT
    // wired into this chain — that integration is Phase 3.4 (feedback) and
    // Phase 3.7-3.9 (modulation generalization).
    //
    // Real-time-safety note: the tone-tilt filter, DC blocker, and Notch
    // filter below are all HAND-ROLLED one-pole/biquad implementations
    // (NOT juce::dsp::IIR::Filter) specifically because
    // IIR::Coefficients<float>::make*() factory functions allocate a new
    // heap-backed Coefficients object on every call — recomputing them
    // every block (needed since filterCutoff/filterResonance/tone/bias are
    // all automatable) would violate "no allocation in processBlock()".
    // Manual coefficient math (plain floats, no JUCE Coefficients object)
    // sidesteps this entirely. StateVariableTPTFilter and DelayLine are
    // used as-is for the other topologies since their setCutoffFrequency/
    // setResonance/setDelay calls are documented as allocation-free.
    //=========================================================================

    // --- Component #1: Input Gain ---
    juce::dsp::Gain<float> inputGainDsp;

    // --- Component #2: Distortion Engine (12-algorithm waveshaper) ---
    std::array<float, 2> distortionToneLpState { 0.0f, 0.0f };  // one-pole LP state for tone-tilt filter (per channel)
    std::array<float, 2> distortionDcBlockerX1 { 0.0f, 0.0f };  // DC blocker x[n-1] (per channel)
    std::array<float, 2> distortionDcBlockerY1 { 0.0f, 0.0f };  // DC blocker y[n-1] (per channel)
    std::array<juce::dsp::Oscillator<float>, 2> ringModOsc;     // Ring-Mod carrier (per channel, sine, lookup-table mode)
    float distortionToneLpCoeff = 0.0f; // computed in prepareToPlay from a fixed 1kHz tilt-filter pole
    float distortionDcBlockerR = 0.0f;  // computed in prepareToPlay from a fixed ~5Hz DC-blocker pole
    juce::SmoothedValue<float> driveGainSmoothed;     // smoothed linear gain from `drive` (dB) — avoids zipper on automation
    juce::SmoothedValue<float> distortionMixSmoothed; // smoothed 0-1 stage-local dry/wet (`distortionMix`)

    // Per-block parameter cache (recomputed every block from APVTS, not
    // persistent DSP state — plain scalars, no allocation).
    int distortionAlgorithmIndex = 0;
    bool distortionEngineBypassed = false; // graphBypassSaturation (whole-module power-bypass)
    bool waveshaperCurveBypassed = false;  // graphBypassWaveshaper ("Fold Bypass" — waveshaper-curve-only bypass)
    float distortionBiasOffset = 0.0f;
    float distortionToneNorm = 0.0f;
    float distortionFoldPct = 0.0f;

    float processDistortionEngine(float xIn, int channel, float driveGain, float distortionMixAmt);

    // --- Component #3: Bitcrusher / Sample Rate Reducer (always-on) ---
    std::array<float, 2> bitcrushHeldSample { 0.0f, 0.0f };
    std::array<int, 2> bitcrushHoldCounter { 0, 0 };
    bool bitcrushBypassed = false;
    float bitcrushLevels = 65535.0f;
    int bitcrushHoldSamples = 1;

    float processBitcrusher(float xIn, int channel);

    // --- Component #6: Filter Stage (7 topologies) ---
    std::array<juce::dsp::StateVariableTPTFilter<float>, 2> svfFilter; // LP/HP/BP/Resonant LP/Resonant HP
    std::array<float, 2> notchX1 { 0.0f, 0.0f }, notchX2 { 0.0f, 0.0f };
    std::array<float, 2> notchY1 { 0.0f, 0.0f }, notchY2 { 0.0f, 0.0f };
    float notchB0 = 1.0f, notchB1 = 0.0f, notchB2 = 0.0f, notchA1 = 0.0f, notchA2 = 0.0f; // manual biquad coeffs (see rationale above)
    std::array<juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Linear>, 2> combDelay;
    int combMaxDelaySamples = 0;
    float combFeedbackGain = 0.0f;
    int lastFilterTypeIndex = -1; // triggers a one-time state reset when filterType changes (architecture.md: "must reset state on filterType change")
    bool filterBypassed = false;
    int filterTypeIndex = 0;

    void updateFilterParameters(float cutoffHz, float resonancePct, double sampleRate);
    float processFilterStage(float xIn, int channel);

    // --- Component #14: Master Mix ---
    juce::AudioBuffer<float> dryBuffer; // captured at the very start of processBlock(), before Input Gain
    juce::SmoothedValue<float> masterDryGainSmoothed;
    juce::SmoothedValue<float> masterWetGainSmoothed;

    // --- Component #13: Output Gain + Output Limiter (dual-mode) ---
    // CONTRADICTION FLAGGED & RESOLVED (see JSON report): architecture.md's
    // component #13 prose says "outputGain applied after the limiter", but
    // its own "Sequential DSP chain (REQUIRED order)" section lists step 12
    // Output Gain BEFORE step 13 Output Limiter. This implementation follows
    // the Sequential DSP chain's explicit numbered order (authoritative per
    // its own text, "REQUIRED order") — Output Gain, THEN Output Limiter.
    juce::dsp::Gain<float> outputGainDsp;
    juce::dsp::WaveShaper<float> coloredLimiterSaturation; // fixed, gentle, always-on-in-Colored-mode saturation before the limiter (architecture.md #13's recommended MVP approximation for GR-proportional coloring)
    juce::dsp::Limiter<float> outputLimiter;

    juce::AudioProcessorValueTreeState parameters;

    // Parameter layout creation
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CORRUPTRAudioProcessor)
};
