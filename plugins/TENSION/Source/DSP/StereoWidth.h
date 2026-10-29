#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>

// StereoWidth - progressive, mono-safe stereo expansion (Phase 3.2).
//
//   LR4 split at 150 Hz (juce::dsp::LinkwitzRileyFilter<float>::processSample(ch, x, low, high);
//   low + high is an all-pass of the input = flat magnitude).
//   Lows : bypass the widener. Side is progressively collapsed: S_low *= (1 - 0.8 * k).
//   Highs: M/S, S' = S * w, M' = M * midComp(w), with
//            midComp = clamp( 1 / sqrt((1 + w^2) / 2), 10^(-1/20), 1 )   (mid never attenuated > 1 dB)
//          lfoB sway: w_eff = max(0, w + moveDepth * lfoB);
//          lfoB rotation of the (M, S) plane by a = moveDepth * lfoB * 0.35 rad (energy preserving),
//          evaluated with 5th-order polynomials (|a| <= 0.35).
//   NO inter-channel delays / Haas anywhere.
//   Width, collapse and moveDepth are smoothed over 100 ms (per sample).
//   At w = 1, k = 0, moveDepth = 0 the module is a flat (all-pass) identity.
class StereoWidth
{
public:
    static constexpr float  kCrossoverHz = 150.0f;
    static constexpr float  kMidFloor    = 0.89125094f;     // -1 dB
    static constexpr float  kRotation    = 0.35f;           // rad per unit moveDepth * lfoB
    static constexpr double kSmoothSec   = 0.100;

    void prepare (double sampleRate);
    void reset();

    // Control-rate update. collapse = k in [0,1]; moveDepth in [0,1].
    void setTargets (float width, float collapse, float moveDepth, bool snap);

    // lfoB: per-sample bipolar lfoB values for these n samples (may be nullptr -> treated as 0).
    void process (float* l, float* r, int n, const float* lfoB);

    static float midCompFor (float width) noexcept;

private:
    using Sm = juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear>;

    juce::dsp::LinkwitzRileyFilter<float> crossover;
    Sm widthSm, collapseSm, depthSm;
};
