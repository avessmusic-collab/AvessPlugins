#pragma once

#include <juce_dsp/juce_dsp.h>

namespace kickr
{
    /**
        Stage 1 stub. Implemented in Stage 2 (plan.md Phase 2.9).

        Tone (3-band IIR, fixed PRE-distortion — low shelf 130 Hz, mid bell 750 Hz Q0.7,
        high shelf 5 kHz), stereo width + Linkwitz-Riley mono crossover fixed at 130 Hz
        (low band forced mono, high band M/S width from `outputWidth`), output gain
        (-24..+12 dB, after Mix before limiter), equal-power Mix (processed <-> silence,
        AD-7), zero-latency soft-clip `tanh` safety limiter at -0.5 dBFS (AD-4).
        Everything except the final base-rate DC blocker runs inside the OS region (AD-10).
        Coefficients computed against fsOversampled.
    */
    class OutputStage
    {
    public:
        static constexpr float kMonoCrossoverHz  = 130.0f;
        static constexpr float kLimiterCeilingDb = -0.5f;

        void prepare (const juce::dsp::ProcessSpec& spec) noexcept { fsOversampled = spec.sampleRate; }
        void reset() noexcept {}

    private:
        double fsOversampled { 44100.0 };
    };
}
