#pragma once

#include <juce_dsp/juce_dsp.h>

namespace kickr
{
    /**
        Stage 1 stub. Implemented in Stage 2 (plan.md Phase 2.5).

        Independent clean sub sine (phase accumulator + std::sin), fixed `subFreq`
        (25-80 Hz, pitch-envelope independent), phase reset to 0 on every noteOn,
        own exponential AD envelope, summed identically to L and R (mono).
    */
    class SubOscillator
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
