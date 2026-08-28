#pragma once

#include <juce_dsp/juce_dsp.h>

namespace kd2
{
    /**
        Stage 1 stub. Implemented in Stage 2 (plan.md Phase 2.4).

        Fully synthesised transient/click (no samples). Blend (Open Q 7 RESOLVED):
            0.50 * filtered-noise burst (StateVariableTPTFilter BP @ clickTone)
          + 0.35 * transient oscillator (sine @ clickPitch, own fast pitch drop)
          + 0.15 * windowed raised-cosine impulse (>= 8 samples at fsOversampled)
        Generated in-region (AD-10) AND band-limited by construction. L/R decorrelation
        scaled by `clickWidth`.
    */
    class ClickGenerator
    {
    public:
        void prepare (const juce::dsp::ProcessSpec& spec) noexcept { fsOversampled = spec.sampleRate; }
        void reset() noexcept {}

    private:
        double fsOversampled { 44100.0 };
        juce::Random rngL, rngR;
    };
}
