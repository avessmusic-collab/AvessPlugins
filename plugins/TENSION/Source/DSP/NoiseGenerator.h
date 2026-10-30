#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>

#include <cstdint>

// NoiseGenerator - independent parallel riser-noise source (Phase 3.3).
//
//  * Deterministic xorshift32 per channel: seedL = S, seedR = S ^ 0x9E3779B9. setSeed() + reset()
//    restore a bit-identical stream. The RNG advances EVERY sample (also at gain 0, and via skip()
//    while the engine is at rest), so the stream is independent of automation history.
//  * Colour: white and pink (Paul Kellett cascade, RMS-normalised to the white RMS) blended by a
//    brightness curve (pink -> white); then a rising HPF and a resonant-free LPF top shaper
//    (juce::dsp::StateVariableTPTFilter); plus an optional resonant band-pass of the white stream
//    (centre = top frequency, "filtered" colour) mixed in by filteredAmt.
//  * Level: linear gain delivered by the mapping (dB-linear curve * onset gate, EXACTLY 0 at t = 0).
//    A target of 0 reaches an exact 0.0f (40 ms linear smoother), so the output is exactly 0.
//  * Own tremolo from lfoA: g = 1 - depth * 0.5 * (1 - lfo).
//  * Smoothing: gain 40 ms, brightness / cutoffs 60 ms. Cutoffs are only changed from setTargets()
//    (once per 32-sample control period).
class NoiseGenerator
{
public:
    static constexpr double        kGainSmoothSec = 0.040;
    static constexpr double        kToneSmoothSec = 0.060;
    static constexpr std::uint32_t kSeedXor       = 0x9E3779B9u;
    static constexpr float         kBpQ           = 2.5f;
    static constexpr float         kWhiteRms      = 0.57735027f;   // RMS of uniform [-1, 1)
    static constexpr float         kRmsNorm       = 1.7320508f;    // 1 / kWhiteRms: gain ~ RMS level

    struct Targets
    {
        float gain        = 0.0f;      // linear (RMS-ish), exactly 0 at rest
        float brightness  = 0.0f;      // 0 = pink, 1 = white
        float hpfHz       = 80.0f;
        float topHz       = 2500.0f;   // LPF top shaper + band-pass centre
        float filteredAmt = 0.0f;      // resonant band-pass of the white stream
        float tremDepth   = 0.0f;      // lfoA tremolo depth
    };

    struct PinkState { float b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0; };

    static std::uint32_t xorshift32 (std::uint32_t& s) noexcept
    {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return s;
    }

    // Uniform [-1, 1) from the top 24 bits of the state.
    static float whiteFromState (std::uint32_t s) noexcept { return (float) (s >> 8) / 8388608.0f - 1.0f; }

    void prepare (double sampleRate);
    void reset();                                   // restores the seeds, clears filters / pink state, gain -> 0
    void resetFilters();                            // clears filter / pink states only (RNG untouched)
    void setSeed (std::uint32_t masterSeed) noexcept;

    void setTargets (const Targets& t, bool snap, int controlSamples);

    // Writes (overwrites) n noise samples. lfo (bipolar, may be nullptr == 1.0) drives the tremolo.
    void process (float* outL, float* outR, int n, const float* lfo) noexcept;

    // Advances only the RNG (used while the engine is at rest).
    void skip (int n) noexcept;

    // TEST HOOK: raw white streams (same RNG, no filtering / gain).
    void renderRawWhiteForTesting (float* l, float* r, int n) noexcept;

    bool  isSilent() const noexcept
    {
        return gainSm.getCurrentValue() == 0.0f && gainSm.getTargetValue() == 0.0f && ! gainSm.isSmoothing();
    }

    float getLevel() const noexcept { return gainSm.getCurrentValue(); }

private:
    using LinSm = juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear>;
    using MulSm = juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative>;

    static float pinkNormalisation() noexcept;
    static float pinkRaw (float w, PinkState& s) noexcept;

    double sampleRate = 44100.0;
    float  maxHz = 19845.0f;
    float  pinkNorm = 1.0f;

    // Phase 3.6 engine fix (sample-rate matrix, T22): the white source has a constant per-SAMPLE RMS, so
    // its spectral density fell with 1/fs and the band-limited noise (LPF top 2.5..16 kHz) got up to ~6 dB
    // quieter at 192 kHz than at 48 kHz. rateNorm = sqrt(fs / 48 kHz) holds the density (and therefore the
    // in-band level) constant across host rates. Exactly 1.0f at 48 kHz => 48 kHz output is bit-unchanged.
    float  rateNorm = 1.0f;

    std::uint32_t seedL = 1u, seedR = 1u ^ kSeedXor;
    std::uint32_t stateL = 1u, stateR = 1u ^ kSeedXor;
    PinkState pinkL, pinkR;

    juce::dsp::StateVariableTPTFilter<float> hpf, lpf, bpf;

    LinSm gainSm, brightSm, filtSm, tremSm;
    MulSm hpfSm, topSm;
};
