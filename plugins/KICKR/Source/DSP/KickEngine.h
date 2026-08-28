#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>

namespace kickr
{
    /**
        Stage 1 stub. Implemented in Stage 2 (plan.md Phase 2.1 -> 2.11).

        Owns the full synthesis lifecycle: prepareToPlay fan-out, per-block parameter
        smoothing + macro-offset resolution, MIDI dispatch with sample-accurate sub-block
        splitting, monophonic click-free retrigger crossfade (std::array<KickVoice, 2>),
        the OversamplingProcessor region (AD-10 — whole voice + master chain), the
        OutputStage, and the lock-free analyzer taps.
    */
    class KickEngine
    {
    public:
        KickEngine() = default;

        void prepare (double /*sampleRate*/, int /*maximumBlockSize*/) {}
        void reset() {}

        void process (juce::AudioBuffer<float>& /*buffer*/,
                      juce::MidiBuffer& /*midi*/,
                      juce::AudioProcessorValueTreeState& /*apvts*/) {}

        int getLatencySamples() const noexcept { return 0; }
    };
}
