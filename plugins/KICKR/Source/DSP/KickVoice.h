#pragma once

#include <juce_dsp/juce_dsp.h>

namespace kickr
{
    /**
        Stage 1 stub. Implemented in Stage 2 (plan.md Phase 2.1 -> 2.6).

        One triggered kick: aggregates BodyOscillator + PitchEnvelope + AmplitudeEnvelope
        + SubOscillator + ClickGenerator + NoiseGenerator + TailGenerator + TransientShaper.
        Monophonic; noteOff() is ignored (kick runs to completion). Phases reset to 0 on
        noteOn for layering phase-stability.
    */
    class KickVoice
    {
    public:
        void prepare (const juce::dsp::ProcessSpec& spec) noexcept { fsOversampled = spec.sampleRate; }
        void reset() noexcept {}

        void noteOn (float /*freqHz*/, float /*velocity01*/, int /*sampleOffset*/) noexcept {}
        void noteOff() noexcept {}

        bool isActive() const noexcept { return false; }

    private:
        double fsOversampled { 44100.0 };
    };
}
