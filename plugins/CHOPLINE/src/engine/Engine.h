// CHOPLINE — Engine (spec §3)
// Owns the whole DSP graph: SampleStore, SliceMap, PatternScheduler, voice
// pool, per-chop FX, Beat Repeat and Master FX (Phases 1–4). This is the
// processBlock entry point; it must build and run with no GUI module linked.
//
// Phase 0: stub. Owns nothing yet; process() renders silence and ignores
// MIDI. All real members are allocated in prepare() in later phases — never
// on the audio thread (spec §8).

#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

namespace chopline::engine
{

class Engine final
{
public:
    Engine() = default;

    /// Called from prepareToPlay on the message thread. All allocation
    /// happens here (or on the background thread) in later phases.
    void prepare (double sampleRate, int maxBlockSize, int numOutputChannels);

    /// Called from releaseResources.
    void release();

    /// Audio-thread entry point. Real-time safe: no allocation, locks,
    /// logging or file I/O (spec §8). Phase 0: clears the buffer.
    void process (juce::AudioBuffer<float>& buffer, const juce::MidiBuffer& midi) noexcept;

    double getSampleRate() const noexcept { return currentSampleRate; }

private:
    double currentSampleRate = 0.0;
    int currentMaxBlockSize = 0;
    int currentNumChannels = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Engine)
};

} // namespace chopline::engine
