#pragma once

#include <juce_dsp/juce_dsp.h>

namespace kd2
{
    /**
        Stage 1 stub. Implemented in Stage 2 (plan.md Phase 2.8).

        Master morphing distortion — runs inside OversamplingProcessor (AD-10).
        Locked order (parameter-spec.md authoritative):
            0 tanh -> 1 cubic -> 2 asymmetric -> 3 soft-clip (quintic)
            -> 4 hard-clip -> 5 foldback -> 6 bitcrush/decimate
        `drive` -> pre-gain 0..+36 dB. `character` -> 6-segment equal-gain crossfade of
        adjacent curves. Adaptive short-window RMS makeup (seeded by a static prime
        table, AD-5). `driveMix` -> parallel blend clean (pre-drive-gain) <-> shaped.
    */
    class Waveshaper
    {
    public:
        static constexpr int kNumCurves = 7;

        void prepare (const juce::dsp::ProcessSpec& spec) noexcept { fsOversampled = spec.sampleRate; }
        void reset() noexcept { makeup = 1.0f; }

    private:
        double fsOversampled { 44100.0 };
        float  makeup { 1.0f };
    };
}
