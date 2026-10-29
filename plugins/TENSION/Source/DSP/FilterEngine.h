#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>

// FilterEngine - the Pre-Filter stage (Phase 3.2).
//
//   in -> HP (2-pole TPT SVF, Butterworth Q, rising cutoff) = hp
//   out = hp + blend * ( BP(hp) * (1/Q) )        <- parallel resonant band-pass sweep, 0 dB peak
//
// * juce::dsp::StateVariableTPTFilter<float> (JUCE 8.0.15): setType / setCutoffFrequency /
//   setResonance / processSample(channel, x). Coefficients are recomputed by every
//   setCutoffFrequency / setResonance call, so they are updated ONLY from setTargets(), which the
//   engine calls once per 32-sample control period (never per sample).
// * Resonance is hard-capped (Q <= 4.0); cutoffs are clamped to [20 Hz, 0.45 fs] (also AFTER the
//   LFO octave modulation). The TPT SVF is unconditionally stable for Q > 0.
// * Cutoffs / Q are smoothed multiplicatively (HP 50 ms, BP centre 60 ms, Q 80 ms); the blend and
//   the LFO depth use linear smoothers. The lfoA modulation is applied AFTER smoothing, as
//   cutoff * 2^(depthOct * lfo): the LFO is already a smooth signal and a 50 ms smoother would
//   low-pass away the faster LFO rates.
// * Both channels are processed with identical coefficients; per-channel state.
class FilterEngine
{
public:
    static constexpr float  kMaxQ     = 4.0f;
    static constexpr float  kMinQ     = 0.1f;
    static constexpr float  kMinHz    = 20.0f;
    static constexpr float  kMaxHzRel = 0.45f;     // x fs
    static constexpr double kHpSmoothSec = 0.050;
    static constexpr double kBpSmoothSec = 0.060;
    static constexpr double kQSmoothSec  = 0.080;

    struct Targets
    {
        float hpHz          = 20.0f;
        float bpBlend       = 0.0f;
        float bpCenterHz    = 300.0f;
        float bpQ           = 0.9f;
        float lfoDepthOct   = 0.0f;
    };

    void prepare (double sampleRate);
    void reset();

    // Control-rate update (call once per control period). `lfo` is the bipolar lfoA value at the
    // start of the period. snap == true jumps the smoothers to the targets (first block / at rest).
    void setTargets (const Targets& t, float lfo, bool snap, int controlSamples);

    // Constant coefficients over the call; n is arbitrary (the engine passes <= 32).
    void process (float* l, float* r, int n);

    // Introspection (tests): values actually applied to the SVFs at the last setTargets().
    float getAppliedHpHz() const noexcept   { return appliedHp; }
    float getAppliedBpHz() const noexcept   { return appliedBp; }
    float getAppliedQ() const noexcept      { return appliedQ; }

private:
    using MulSm = juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative>;
    using LinSm = juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear>;

    double sampleRate = 44100.0;
    float  maxHz = 19845.0f;

    juce::dsp::StateVariableTPTFilter<float> hpFilter, bpFilter;

    MulSm hpSm, bpSm, qSm;
    LinSm blendSm, depthSm;

    float blendNow = 0.0f;
    float bpNorm   = 1.0f;     // 1/Q: turns the SVF band-pass (peak gain Q) into a 0 dB peak
    float appliedHp = 20.0f, appliedBp = 300.0f, appliedQ = 0.9f;
};
