// CHOPLINE tests — shared helpers (synthetic buffers + engine setup).

#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_events/juce_events.h>

#include "engine/Engine.h"

#include <cmath>
#include <vector>

namespace chopline::test
{

/// Click train: short decaying broadband bursts at the given onset times.
inline juce::AudioBuffer<float> makeClickTrain (const std::vector<double>& onsetsSec,
                                                double lengthSec, double rate)
{
    juce::AudioBuffer<float> buf (1, static_cast<int> (lengthSec * rate));
    buf.clear();
    auto* d = buf.getWritePointer (0);
    juce::Random rng (42);

    for (auto onset : onsetsSec)
    {
        const auto start = static_cast<int> (onset * rate);
        const auto burst = static_cast<int> (0.015 * rate); // 15 ms decaying noise burst
        for (int i = 0; i < burst && start + i < buf.getNumSamples(); ++i)
        {
            const auto env = std::exp (-6.0f * static_cast<float> (i) / static_cast<float> (burst));
            d[start + i] += (rng.nextFloat() * 2.0f - 1.0f) * env;
        }
    }
    return buf;
}

/// Constant (DC) buffer — makes declick fades directly measurable.
inline juce::AudioBuffer<float> makeConstant (float value, double lengthSec, double rate)
{
    juce::AudioBuffer<float> buf (1, static_cast<int> (lengthSec * rate));
    for (int i = 0; i < buf.getNumSamples(); ++i)
        buf.setSample (0, i, value);
    return buf;
}

/// Sine buffer.
inline juce::AudioBuffer<float> makeSine (double freq, double lengthSec, double rate,
                                          float amplitude = 0.8f)
{
    juce::AudioBuffer<float> buf (1, static_cast<int> (lengthSec * rate));
    for (int i = 0; i < buf.getNumSamples(); ++i)
        buf.setSample (0, i, amplitude * static_cast<float> (
            std::sin (juce::MathConstants<double>::twoPi * freq * i / rate)));
    return buf;
}

/// "Staircase" buffer: N segments of distinct constant values ((seg+1) / 10).
inline juce::AudioBuffer<float> makeStaircase (int numSegments, double lengthSec, double rate)
{
    juce::AudioBuffer<float> buf (1, static_cast<int> (lengthSec * rate));
    const auto segLen = buf.getNumSamples() / numSegments;
    for (int s = 0; s < numSegments; ++s)
    {
        const auto v = static_cast<float> (s + 1) / 10.0f;
        for (int i = s * segLen; i < juce::jmin ((s + 1) * segLen, buf.getNumSamples()); ++i)
            buf.setSample (0, i, v);
    }
    return buf;
}

/// Largest |x[n] − x[n−1]| in a channel over [start, end).
inline float maxSampleJump (const juce::AudioBuffer<float>& buf, int channel = 0,
                            int start = 1, int end = -1)
{
    const auto* d = buf.getReadPointer (channel);
    const auto n = end < 0 ? buf.getNumSamples() : end;
    float maxJump = 0.0f;
    for (int i = juce::jmax (1, start); i < n; ++i)
        maxJump = juce::jmax (maxJump, std::abs (d[i] - d[i - 1]));
    return maxJump;
}

/// Prepares an engine at the given rate and synchronously loads a buffer.
inline void prepareEngineWithBuffer (engine::Engine& eng, const juce::AudioBuffer<float>& source,
                                     double rate, int blockSize = 512)
{
    eng.prepare (rate, blockSize, 2);
    const auto error = eng.loadBufferSync (source, rate);
    jassert (error.isEmpty());
    juce::ignoreUnused (error);
}

/// Renders n silent-input blocks through the engine (no MIDI).
inline void renderBlocks (engine::Engine& eng, juce::AudioBuffer<float>& block, int count,
                          int baseNote = 36)
{
    const juce::MidiBuffer noMidi;
    const engine::Engine::BlockContext ctx { baseNote };
    for (int i = 0; i < count; ++i)
        eng.process (block, noMidi, ctx);
}

} // namespace chopline::test
