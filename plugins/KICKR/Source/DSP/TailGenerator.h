#pragma once

#include <juce_dsp/juce_dsp.h>

namespace kickr
{
    /**
        Stage 1 stub. Implemented in Stage 2 (plan.md Phase 2.6).

        Sustained tail / rumble. Source = dedicated LF sine locked to fundamentalEff
        (AD-8 — NOT a body-bus tap), phase reset on trigger, ~10 ms attack so it sits
        behind the transient, exp decay = `tailLength`. `tailTone` LP 120 Hz -> 4 kHz.
        `tailDrive` tanh saturation (nonlinear — oversampled by construction, AD-10).
    */
    class TailGenerator
    {
    public:
        void prepare (const juce::dsp::ProcessSpec& spec) noexcept { fsOversampled = spec.sampleRate; }
        void reset() noexcept { phase = 0.0; }

    private:
        double fsOversampled { 44100.0 };
        double phase { 0.0 };
    };
}
