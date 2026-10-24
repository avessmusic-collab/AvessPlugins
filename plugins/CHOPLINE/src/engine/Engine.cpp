// CHOPLINE — Engine implementation (Phase 0 stub: silence).

#include "Engine.h"

namespace chopline::engine
{

void Engine::prepare (double sampleRate, int maxBlockSize, int numOutputChannels)
{
    currentSampleRate = sampleRate;
    currentMaxBlockSize = maxBlockSize;
    currentNumChannels = numOutputChannels;
}

void Engine::release()
{
    // Nothing to free yet (Phase 0).
}

void Engine::process (juce::AudioBuffer<float>& buffer, const juce::MidiBuffer& midi) noexcept
{
    juce::ignoreUnused (midi); // MIDI triggers arrive in Phase 1.
    buffer.clear();
}

} // namespace chopline::engine
