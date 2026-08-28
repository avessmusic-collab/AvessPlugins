#pragma once

#include <juce_dsp/juce_dsp.h>

namespace kickr
{
    /**
        Stage 1 stub. Implemented in Stage 2 (plan.md Phase 2.7).

        Optional white/pink/filtered noise layer (default off, `noiseLevel` = 0).
        `noiseType`: White = raw Random; Pink = Paul Kellet 7-pole; Filtered = white
        -> band-pass with centre from `noiseTone`. AD env exp decay = `noiseDecay`.
        Generated in-region (AD-10).
    */
    class NoiseGenerator
    {
    public:
        void prepare (const juce::dsp::ProcessSpec& spec) noexcept { fsOversampled = spec.sampleRate; }
        void reset() noexcept {}

    private:
        double fsOversampled { 44100.0 };
        juce::Random rng;
    };
}
