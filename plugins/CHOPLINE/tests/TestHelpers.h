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

/// Phase 3: all chops default to choke group 1 (spec §6 classic vocal-chop
/// behaviour). Tests that exercise POLYPHONY turn choking off explicitly.
inline void disableChoke (engine::Engine& eng)
{
    for (int i = 0; i < eng.getNumChops(); ++i)
    {
        auto s = eng.getChopSettings (i);
        s.chokeGroup = 0;
        eng.setChopSettings (i, s);
    }
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

/// Simulated host transport for scheduler tests (Phase 2): builds BlockContexts
/// the way the processor does from AudioPlayHead, and advances the PPQ exactly
/// as a well-behaved host would.
struct SimHost
{
    double rate = 48000.0;
    double bpm = 120.0;
    double ppq = 0.0;
    bool playing = true;
    bool hasPosition = true;
    bool hasPpq = true;
    bool hasBpm = true;
    int tsNum = 4, tsDen = 4;
    float swingPct = 0.0f;

    engine::Engine::BlockContext ctx (int baseNote = 36) const
    {
        engine::Engine::BlockContext c;
        c.midiBaseNote = baseNote;
        c.swingPct = swingPct;
        c.hasHostPosition = hasPosition;
        c.hostPlaying = playing;
        c.hasPpq = hasPpq;
        c.ppqPosition = ppq;
        c.hasBpm = hasBpm;
        c.bpm = bpm;
        c.timeSigNumerator = tsNum;
        c.timeSigDenominator = tsDen;
        return c;
    }

    void advance (int numSamples)
    {
        ppq += static_cast<double> (numSamples) / rate * bpm / 60.0;
    }

    double samplesPerBar() const
    {
        return rate * 60.0 / bpm * (static_cast<double> (tsNum) * 4.0 / tsDen);
    }
};

/// Renders `totalSamples` through the engine in `blockSize` chunks while
/// advancing the simulated host; returns the concatenated output (channel 0+1).
inline juce::AudioBuffer<float> renderWithHost (engine::Engine& eng, SimHost& host,
                                                int totalSamples, int blockSize)
{
    juce::AudioBuffer<float> full (2, totalSamples);
    full.clear();
    juce::AudioBuffer<float> block (2, blockSize);
    const juce::MidiBuffer noMidi;
    for (int pos = 0; pos < totalSamples; pos += blockSize)
    {
        const auto n = juce::jmin (blockSize, totalSamples - pos);
        block.setSize (2, n, false, false, true);
        eng.process (block, noMidi, host.ctx());
        host.advance (n);
        for (int ch = 0; ch < 2; ++ch)
            full.copyFrom (ch, pos, block, ch, 0, n);
    }
    return full;
}

/// First sample index at or after `from` whose magnitude exceeds the threshold,
/// or −1.
inline int firstAudibleSample (const juce::AudioBuffer<float>& buf, int from = 0,
                               float threshold = 1.0e-6f)
{
    const auto* d = buf.getReadPointer (0);
    for (int i = juce::jmax (0, from); i < buf.getNumSamples(); ++i)
        if (std::abs (d[i]) > threshold)
            return i;
    return -1;
}

} // namespace chopline::test
