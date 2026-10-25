// CHOPLINE tests — Beat Repeat (spec §7/§12): PPQ-aligned repeats at every
// grid (incl. triplets), click-free crossfaded boundaries, decay and pitch-
// decay progression, Insert dry-muting, freeze, seeded chance determinism.

#include <catch2/catch_test_macros.hpp>

#include "TestHelpers.h"

#include "engine/BeatRepeat.h"

#include <cmath>

using namespace chopline;

namespace
{

constexpr double rate = 48000.0;

struct BrHarness
{
    engine::BeatRepeat br;
    double bpm = 120.0;
    double phase = 0.0; // bars

    BrHarness() { br.prepare (rate, 512); }

    double samplesPerBar() const { return rate * 60.0 / bpm * 4.0; }

    /// Processes `io` in place, advancing the bar phase per block.
    void run (juce::AudioBuffer<float>& io, const engine::BeatRepeat::Params& p,
              int blockSize = 256)
    {
        const auto total = io.getNumSamples();
        for (int pos = 0; pos < total; pos += blockSize)
        {
            const auto n = juce::jmin (blockSize, total - pos);
            juce::AudioBuffer<float> view (io.getArrayOfWritePointers(),
                                           io.getNumChannels(), pos, n);
            br.process (view, p, phase, samplesPerBar());
            phase += static_cast<double> (n) / samplesPerBar();
        }
    }

    /// Variant with per-block parameter control (hold/freeze automation).
    void run (juce::AudioBuffer<float>& io,
              const std::function<engine::BeatRepeat::Params (int samplePos)>& paramsAt,
              int blockSize = 256)
    {
        const auto total = io.getNumSamples();
        for (int pos = 0; pos < total; pos += blockSize)
        {
            const auto n = juce::jmin (blockSize, total - pos);
            juce::AudioBuffer<float> view (io.getArrayOfWritePointers(),
                                           io.getNumChannels(), pos, n);
            br.process (view, paramsAt (pos), phase, samplesPerBar());
            phase += static_cast<double> (n) / samplesPerBar();
        }
    }
};

engine::BeatRepeat::Params manualHold (int gridIndex, int mixMode = 2 /* Gate */)
{
    engine::BeatRepeat::Params p;
    p.mode = 1; // Manual
    p.hold = true;
    p.gridIndex = gridIndex;
    p.mixMode = mixMode;
    p.decayPct = 0.0f;
    p.pitchDecaySt = 0.0f;
    p.chancePct = 0.0f;
    return p;
}

juce::AudioBuffer<float> stereoSilence (int n)
{
    juce::AudioBuffer<float> b (2, n);
    b.clear();
    return b;
}

int argmaxAbs (const juce::AudioBuffer<float>& b, int from, int to)
{
    const auto* d = b.getReadPointer (0);
    int best = from;
    for (int i = from; i < juce::jmin (to, b.getNumSamples()); ++i)
        if (std::abs (d[i]) > std::abs (d[best]))
            best = i;
    return best;
}

} // namespace

TEST_CASE ("Beat Repeat: repeats land PPQ-aligned at each grid incl. triplets",
           "[beatrepeat][timing]")
{
    // 120 BPM, 4/4 → one bar = 96000 samples. Impulse at offset 300 of the
    // captured slice; each repeat must reproduce it at exactly k·grid + 300.
    struct GridCase { int index; int gridSamples; };
    const GridCase cases[] = {
        { 3, 6000 },  // 1/16
        { 2, 12000 }, // 1/8
        { 7, 4000 },  // 1/16T (triplet)
        { 5, 1500 },  // 1/64
    };

    for (const auto blockSize : { 256, 333 })
    {
        for (const auto& c : cases)
        {
            BrHarness h;
            const auto repeats = 6;
            auto io = stereoSilence (c.gridSamples * repeats);
            io.setSample (0, 300, 1.0f);
            io.setSample (1, 300, 1.0f);

            h.run (io, manualHold (c.index), blockSize);

            for (int k = 0; k < repeats; ++k)
            {
                const auto expected = k * c.gridSamples + 300;
                const auto found = argmaxAbs (io, k * c.gridSamples + 150,
                                              k * c.gridSamples + c.gridSamples - 50);
                INFO ("grid idx " << c.index << " block " << blockSize << " repeat " << k);
                REQUIRE (found == expected);
                REQUIRE (std::abs (io.getSample (0, expected)) > 0.5f);
            }
        }
    }
}

TEST_CASE ("Beat Repeat: crossfaded boundaries are click-free", "[beatrepeat][declick]")
{
    BrHarness h;
    const auto n = 48000;
    juce::AudioBuffer<float> io (2, n);
    for (int i = 0; i < n; ++i)
    {
        const auto v = 0.5f * static_cast<float> (
            std::sin (juce::MathConstants<double>::twoPi * 220.0 * i / rate));
        io.setSample (0, i, v);
        io.setSample (1, i, v);
    }

    auto p = manualHold (3, 1); // Insert: output is the repeat path
    h.run (io, p);

    // A 220 Hz sine at 0.5 moves at most ~0.0144/sample; crossfaded repeat
    // boundaries must stay in that order of magnitude — never a hard step.
    const auto maxJump = test::maxSampleJump (io, 0, 500, n);
    INFO ("max jump " << maxJump);
    REQUIRE (maxJump < 0.08f);

    // And repeats actually happened: the slice is 6000 samples (not a whole
    // number of 220 Hz periods), so the second slot differs from a plain sine.
    bool differs = false;
    for (int i = 7000; i < 12000 && ! differs; ++i)
        differs = std::abs (io.getSample (0, i)
                            - 0.5f * static_cast<float> (std::sin (
                                  juce::MathConstants<double>::twoPi * 220.0 * i / rate)))
                  > 0.05f;
    REQUIRE (differs);
}

TEST_CASE ("Beat Repeat: decay drops each repeat's level", "[beatrepeat][decay]")
{
    BrHarness h;
    auto io = stereoSilence (6000 * 5);
    io.setSample (0, 300, 1.0f);
    io.setSample (1, 300, 1.0f);

    auto p = manualHold (3);
    p.decayPct = 50.0f;
    h.run (io, p);

    for (int k = 0; k < 4; ++k)
    {
        const auto peak = std::abs (io.getSample (0, k * 6000 + 300));
        const auto expected = std::pow (0.5f, static_cast<float> (k));
        INFO ("repeat " << k << " peak " << peak << " expected " << expected);
        REQUIRE (std::abs (peak - expected) < 0.05f);
    }
}

TEST_CASE ("Beat Repeat: pitch decay reads each repeat slower (lower)",
           "[beatrepeat][pitch]")
{
    BrHarness h;
    const auto n = 6000 * 4;
    juce::AudioBuffer<float> io (2, n);
    for (int i = 0; i < n; ++i) // slow linear ramp — slope measures read rate
    {
        const auto v = static_cast<float> (i) / static_cast<float> (n);
        io.setSample (0, i, v);
        io.setSample (1, i, v);
    }

    auto p = manualHold (3);
    p.pitchDecaySt = 12.0f; // −1 octave per repeat → read rate 0.5^k
    h.run (io, p);

    // Slot 1 (samples 6000..12000) replays the slice at half speed.
    const auto slope = [&io] (int a, int b)
    {
        return (io.getSample (0, b) - io.getSample (0, a)) / static_cast<float> (b - a);
    };
    const auto base = 1.0f / static_cast<float> (n);
    const auto s1 = slope (6500, 8500);
    INFO ("slot1 slope " << s1 << " base " << base);
    REQUIRE (std::abs (s1 - 0.5f * base) < 0.1f * base);

    // Slot 2 reads at a quarter speed.
    const auto s2 = slope (12500, 14500);
    REQUIRE (std::abs (s2 - 0.25f * base) < 0.1f * base);
}

TEST_CASE ("Beat Repeat: Insert mutes the dry while repeating", "[beatrepeat][mix]")
{
    // The captured slice is DC 0.5; from slot 1 on the INPUT is DC 0.9.
    // Insert outputs the slice (0.5), never the live input.
    BrHarness h;
    const auto n = 6000 * 4;
    juce::AudioBuffer<float> io (2, n);
    for (int i = 0; i < n; ++i)
    {
        const auto v = i < 6000 ? 0.5f : 0.9f;
        io.setSample (0, i, v);
        io.setSample (1, i, v);
    }

    h.run (io, manualHold (3, 1)); // Insert

    for (int i = 8000; i < 11500; i += 250)
    {
        INFO ("sample " << i);
        REQUIRE (std::abs (io.getSample (0, i) - 0.5f) < 0.02f);
    }

    // Mix mode reference: dry (0.9) + repeat (0.5).
    BrHarness h2;
    juce::AudioBuffer<float> io2 (2, n);
    for (int i = 0; i < n; ++i)
    {
        const auto v = i < 6000 ? 0.5f : 0.9f;
        io2.setSample (0, i, v);
        io2.setSample (1, i, v);
    }
    h2.run (io2, manualHold (3, 0)); // Mix
    REQUIRE (std::abs (io2.getSample (0, 9000) - 1.4f) < 0.03f);
}

TEST_CASE ("Beat Repeat: freeze pins the current slice and slot", "[beatrepeat][freeze]")
{
    BrHarness h;
    auto io = stereoSilence (6000 * 6);
    io.setSample (0, 300, 1.0f);
    io.setSample (1, 300, 1.0f);

    // Hold throughout, decay 50 %; freeze engages during slot 1 → the 0.5
    // level is pinned instead of decaying to 0.25 / 0.125.
    h.run (io, [] (int pos)
    {
        auto p = manualHold (3);
        p.decayPct = 50.0f;
        p.freeze = pos >= 7000; // mid slot 1
        return p;
    });

    REQUIRE (std::abs (std::abs (io.getSample (0, 6300)) - 0.5f) < 0.05f);  // slot 1
    REQUIRE (std::abs (std::abs (io.getSample (0, 18300)) - 0.5f) < 0.07f); // still 0.5
    REQUIRE (std::abs (std::abs (io.getSample (0, 24300)) - 0.5f) < 0.07f);
}

TEST_CASE ("Beat Repeat: chance 0 % never fires, 100 % always fires (seeded)",
           "[beatrepeat][chance]")
{
    engine::BeatRepeat::Params p;
    p.mode = 0; // Auto
    p.hold = false;
    p.intervalIndex = 1; // 1/2 bar = 48000 samples
    p.gateIndex = 3;     // 1/2 bar gate → repeats the whole interval
    p.gridIndex = 3;
    p.mixMode = 2;       // Gate: output = repeats only → activity is visible
    p.decayPct = 0.0f;

    const auto n = 96000 * 2; // two bars

    {
        BrHarness h;
        h.br.setRandomSeed (7);
        juce::AudioBuffer<float> io (2, n);
        for (int i = 0; i < n; ++i)
        {
            io.setSample (0, i, 0.8f);
            io.setSample (1, i, 0.8f);
        }
        p.chancePct = 0.0f;
        h.run (io, p);
        REQUIRE (juce::exactlyEqual (io.getMagnitude (0, 2000, n - 2000), 0.0f));
    }

    {
        BrHarness h;
        h.br.setRandomSeed (7);
        juce::AudioBuffer<float> io (2, n);
        for (int i = 0; i < n; ++i)
        {
            io.setSample (0, i, 0.8f);
            io.setSample (1, i, 0.8f);
        }
        p.chancePct = 100.0f;
        h.run (io, p);

        // Repeating from the very first interval boundary (phase 0): the
        // repeat path passes the captured DC throughout.
        int loud = 0;
        for (int i = 2000; i < n; ++i)
            if (std::abs (io.getSample (0, i)) > 0.5f)
                ++loud;
        REQUIRE (loud > (n - 2000) * 9 / 10);
    }
}

TEST_CASE ("Beat Repeat: mode Off is bit-transparent", "[beatrepeat][bypass]")
{
    BrHarness h;
    const auto n = 20000;
    juce::AudioBuffer<float> io (2, n);
    juce::Random rnd (99);
    for (int i = 0; i < n; ++i)
    {
        io.setSample (0, i, rnd.nextFloat() * 2.0f - 1.0f);
        io.setSample (1, i, rnd.nextFloat() * 2.0f - 1.0f);
    }
    juce::AudioBuffer<float> ref (io);

    engine::BeatRepeat::Params p; // defaults: mode Off
    REQUIRE (p.mode == 2);
    h.run (io, p);

    for (int i = 0; i < n; ++i)
        REQUIRE (juce::exactlyEqual (io.getSample (0, i), ref.getSample (0, i)));
}
