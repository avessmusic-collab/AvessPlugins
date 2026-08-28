#pragma once

#include <juce_dsp/juce_dsp.h>

namespace kd2
{
    /**
        Stage 1 stub. Implemented in Stage 2 (plan.md Phase 2.1 / 2.2).

        Phase-accumulated std::sin body fundamental (AD-1 — not juce::dsp::Oscillator;
        needs per-sample frequency from PitchEnvelope with guaranteed phase continuity)
        + `bodyHarmonics` tanh timbral saturation. Runs inside the OS region (AD-10),
        phase increment uses fsOversampled.
    */
    class BodyOscillator
    {
    public:
        void prepare (const juce::dsp::ProcessSpec& spec) noexcept { fsOversampled = spec.sampleRate; }
        void reset() noexcept { phase = 0.0; }

        void setPhaseToZero() noexcept { phase = 0.0; }

    private:
        double fsOversampled { 44100.0 };
        double phase { 0.0 };
    };
}
