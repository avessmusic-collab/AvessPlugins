// CHOPLINE tests — master chain (spec §7/§12): distortion stability +
// aliasing at 2× oversampling, limiter ceiling/transparency/GR, width &
// bass-mono, chain order, send routing through the Engine, block sizes.

#include <catch2/catch_test_macros.hpp>

#include "TestHelpers.h"

#include "engine/MasterFX.h"

#include <juce_dsp/juce_dsp.h>

#include <cmath>

using namespace chopline;
using namespace chopline::test;

namespace
{

constexpr double rate = 48000.0;

struct FxHarness
{
    engine::MasterFX fx;
    juce::AudioBuffer<float> silentSend { 2, 512 };

    FxHarness()
    {
        fx.prepare (rate, 512);
        silentSend.clear();
    }

    void run (juce::AudioBuffer<float>& io, const engine::MasterParams& p,
              double bpm = 120.0, int blockSize = 512)
    {
        const auto total = io.getNumSamples();
        for (int pos = 0; pos < total; pos += blockSize)
        {
            const auto n = juce::jmin (blockSize, total - pos);
            juce::AudioBuffer<float> view (io.getArrayOfWritePointers(),
                                           io.getNumChannels(), pos, n);
            fx.process (view, silentSend, silentSend, p, bpm);
        }
    }
};

juce::AudioBuffer<float> stereoSine (double freq, int n, float amp)
{
    juce::AudioBuffer<float> b (2, n);
    for (int i = 0; i < n; ++i)
    {
        const auto v = amp * static_cast<float> (
            std::sin (juce::MathConstants<double>::twoPi * freq * i / rate));
        b.setSample (0, i, v);
        b.setSample (1, i, v);
    }
    return b;
}

/// Peak level (dB) around `freq` in a Hann-windowed FFT of channel 0.
float levelAtDb (const juce::AudioBuffer<float>& b, int from, double freq)
{
    constexpr int order = 13, size = 1 << order; // 8192
    juce::dsp::FFT fft (order);
    std::vector<float> data (static_cast<size_t> (2 * size), 0.0f);
    for (int i = 0; i < size; ++i)
    {
        const auto w = 0.5f * (1.0f - std::cos (juce::MathConstants<float>::twoPi
                                                * static_cast<float> (i) / (size - 1)));
        data[static_cast<size_t> (i)] = b.getSample (0, from + i) * w;
    }
    fft.performFrequencyOnlyForwardTransform (data.data());
    const auto centre = static_cast<int> (freq / rate * size);
    auto peak = 0.0f;
    for (int k = juce::jmax (0, centre - 3); k <= juce::jmin (size / 2, centre + 3); ++k)
        peak = juce::jmax (peak, data[static_cast<size_t> (k)]);
    return juce::Decibels::gainToDecibels (peak, -200.0f);
}

} // namespace

TEST_CASE ("Master distortion: all 6 types stay finite", "[master][distortion]")
{
    for (int type = 0; type < 6; ++type)
    {
        for (const auto drivePct : { 0.0f, 50.0f, 100.0f })
        {
            FxHarness h;
            auto io = stereoSine (997.0, 8192, 0.95f);
            engine::MasterParams p;
            p.distOn = true;
            p.distType = type;
            p.drivePct = drivePct;
            p.autoGain = drivePct > 50.0f; // exercise both paths
            h.run (io, p);
            for (int i = 0; i < io.getNumSamples(); ++i)
            {
                REQUIRE (std::isfinite (io.getSample (0, i)));
                REQUIRE (std::isfinite (io.getSample (1, i)));
            }
        }
    }
}

TEST_CASE ("Master distortion: aliasing stays low at 2× oversampling (10 kHz sine)",
           "[master][distortion][aliasing]")
{
    // 3rd harmonic of 10 kHz = 30 kHz → would alias to 18 kHz at 48 kHz;
    // 4th = 40 kHz → 8 kHz. Both must sit far below the fundamental.
    // (Downsample is intentionally NOT oversampled — aliasing is the point —
    // and Bitcrush is broadband quantisation noise by design; both skipped.)
    for (int type = 0; type < 4; ++type) // Soft, Hard, Tape, Fold
    {
        FxHarness h;
        auto io = stereoSine (10000.0, 20480, 0.8f);
        engine::MasterParams p;
        p.distOn = true;
        p.distType = type;
        p.drivePct = 60.0f;
        p.autoGain = false;
        h.run (io, p);

        const auto fundamental = levelAtDb (io, 8192, 10000.0);
        const auto alias18k = levelAtDb (io, 8192, 18000.0);
        const auto alias8k = levelAtDb (io, 8192, 8000.0);
        INFO ("type " << type << " fundamental " << fundamental
              << " alias18k " << alias18k << " alias8k " << alias8k);
        REQUIRE (alias18k < fundamental - 40.0f);
        REQUIRE (alias8k < fundamental - 40.0f);
    }
}

TEST_CASE ("Safety limiter: output never exceeds the ceiling", "[master][limiter]")
{
    FxHarness h;
    auto io = stereoSine (220.0, 24000, 1.0f);
    engine::MasterParams p;
    p.outOn = true;
    p.outDb = 6.0f;       // push well past the ceiling
    p.ceilingDb = -0.3f;
    h.run (io, p);

    const auto ceilingLin = juce::Decibels::decibelsToGain (-0.3f);
    for (int i = 0; i < io.getNumSamples(); ++i)
    {
        REQUIRE (std::abs (io.getSample (0, i)) <= ceilingLin + 1.0e-4f);
        REQUIRE (std::abs (io.getSample (1, i)) <= ceilingLin + 1.0e-4f);
    }

    // GR is reported (blue meter feed) while limiting.
    REQUIRE (h.fx.getGainReductionDb() < -0.5f);
}

TEST_CASE ("Safety limiter: bit-transparent below the ceiling", "[master][limiter]")
{
    // House rule (juce::dsp::Limiter hides a 4:1 stage at −10 dBFS — not used
    // here): a signal below the ceiling must pass bit-identical.
    FxHarness h;
    const auto n = 12000;
    juce::AudioBuffer<float> io (2, n);
    juce::Random rnd (4242);
    for (int i = 0; i < n; ++i)
    {
        io.setSample (0, i, (rnd.nextFloat() * 2.0f - 1.0f) * 0.5f);
        io.setSample (1, i, (rnd.nextFloat() * 2.0f - 1.0f) * 0.5f);
    }
    juce::AudioBuffer<float> ref (io);

    engine::MasterParams p;
    p.outOn = true;
    p.outDb = 0.0f;
    p.ceilingDb = -0.3f; // 0.966 — the 0.5 noise stays below it
    h.run (io, p);

    for (int i = 0; i < n; ++i)
    {
        REQUIRE (juce::exactlyEqual (io.getSample (0, i), ref.getSample (0, i)));
        REQUIRE (juce::exactlyEqual (io.getSample (1, i), ref.getSample (1, i)));
    }
    REQUIRE (juce::exactlyEqual (h.fx.getGainReductionDb(), 0.0f));
}

TEST_CASE ("Width: 0 % sums to mono, 100 % is identity, mono-below folds bass",
           "[master][width]")
{
    // width 0 → L == R == mid.
    {
        FxHarness h;
        const auto n = 9600;
        juce::AudioBuffer<float> io (2, n);
        for (int i = 0; i < n; ++i)
        {
            io.setSample (0, i, 0.5f * static_cast<float> (
                std::sin (juce::MathConstants<double>::twoPi * 500.0 * i / rate)));
            io.setSample (1, i, 0.5f * static_cast<float> (
                std::cos (juce::MathConstants<double>::twoPi * 500.0 * i / rate)));
        }
        juce::AudioBuffer<float> ref (io);
        engine::MasterParams p;
        p.widthOn = true;
        p.widthPct = 0.0f;
        p.monoHz = 20.0f; // bass-mono filter off
        h.run (io, p);
        for (int i = 2000; i < n; i += 97)
        {
            const auto mid = 0.5f * (ref.getSample (0, i) + ref.getSample (1, i));
            REQUIRE (std::abs (io.getSample (0, i) - mid) < 1.0e-4f);
            REQUIRE (std::abs (io.getSample (0, i) - io.getSample (1, i)) < 1.0e-6f);
        }
    }

    // width 100 %, mono ≤ 20 Hz → identity.
    {
        FxHarness h;
        auto io = stereoSine (500.0, 9600, 0.5f);
        io.applyGain (1, 0, 9600, -0.7f); // make it properly stereo
        juce::AudioBuffer<float> ref (io);
        engine::MasterParams p;
        p.widthOn = true;
        p.widthPct = 100.0f;
        p.monoHz = 20.0f;
        h.run (io, p);
        for (int i = 0; i < 9600; ++i)
        {
            REQUIRE (std::abs (io.getSample (0, i) - ref.getSample (0, i)) < 1.0e-6f);
            REQUIRE (std::abs (io.getSample (1, i) - ref.getSample (1, i)) < 1.0e-6f);
        }
    }

    // mono-below: pure-side 60 Hz content collapses, pure-side 2 kHz survives.
    {
        FxHarness h;
        const auto n = 48000;
        juce::AudioBuffer<float> io (2, n);
        for (int i = 0; i < n; ++i)
        {
            const auto bass = 0.5f * static_cast<float> (
                std::sin (juce::MathConstants<double>::twoPi * 60.0 * i / rate));
            io.setSample (0, i, bass);
            io.setSample (1, i, -bass); // pure side
        }
        engine::MasterParams p;
        p.widthOn = true;
        p.widthPct = 100.0f;
        p.monoHz = 300.0f;
        h.run (io, p);
        REQUIRE (io.getMagnitude (0, n / 2, n / 2) < 0.12f); // ≥ ~12 dB down

        FxHarness h2;
        juce::AudioBuffer<float> hi (2, n);
        for (int i = 0; i < n; ++i)
        {
            const auto v = 0.5f * static_cast<float> (
                std::sin (juce::MathConstants<double>::twoPi * 2000.0 * i / rate));
            hi.setSample (0, i, v);
            hi.setSample (1, i, -v);
        }
        h2.run (hi, p);
        REQUIRE (hi.getMagnitude (0, n / 2, n / 2) > 0.4f); // passes
    }
}

TEST_CASE ("Chain order: distortion runs before width", "[master][chain]")
{
    // Hard-clipped L-only DC, then width 0 (mono): dist-then-mono gives
    // (clip(g·0.8) + clip(0)) / 2 = 0.5; mono-then-dist would clip to 1.0.
    FxHarness h;
    const auto n = 4800;
    juce::AudioBuffer<float> io (2, n);
    for (int i = 0; i < n; ++i)
    {
        io.setSample (0, i, 0.8f);
        io.setSample (1, i, 0.0f);
    }
    engine::MasterParams p;
    p.distOn = true;
    p.distType = 1; // Hard clip
    p.drivePct = 100.0f;
    p.autoGain = false;
    p.widthOn = true;
    p.widthPct = 0.0f;
    p.monoHz = 20.0f;
    h.run (io, p);
    REQUIRE (std::abs (io.getSample (0, n - 100) - 0.5f) < 0.03f);
    REQUIRE (std::abs (io.getSample (1, n - 100) - 0.5f) < 0.03f);
}

TEST_CASE ("Engine: chop delay sends reach the tempo-synced return; output gain "
           "sits after the returns", "[engine][master][sends]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    const auto renderEcho = [] (float outDb, float sendLevel)
    {
        engine::Engine eng;
        eng.prepare (rate, 512, 2);
        REQUIRE (eng.loadBufferSync (makeStaircase (8, 0.4, rate), rate).isEmpty());
        eng.setZeroCrossSnap (false);
        eng.setSliceMode (engine::SliceMode::equal);
        eng.setSensitivity (6.0f / 62.0f);
        disableChoke (eng);
        auto s = eng.getChopSettings (0);
        s.delaySend = sendLevel * 100.0f; // model stores 0–100 %
        eng.setChopSettings (0, s);

        engine::Engine::BlockContext ctx { 36 };
        ctx.master.delayOn = true;
        ctx.master.delayTimeIndex = 0; // 1/16 at 120 BPM = 6000 samples
        ctx.master.delayFbPct = 0.0f;
        ctx.master.delayRtnPct = 100.0f;
        ctx.master.outOn = true;
        ctx.master.outDb = outDb;
        ctx.master.ceilingDb = 0.0f;
        ctx.hasBpm = true;
        ctx.bpm = 120.0;

        juce::AudioBuffer<float> out (2, 512);
        juce::AudioBuffer<float> full (2, 12000);
        full.clear();
        for (int pos = 0; pos < 12000; pos += 512)
        {
            juce::MidiBuffer midi;
            if (pos == 0)
                midi.addEvent (juce::MidiMessage::noteOn (1, 36, static_cast<juce::uint8> (127)), 0);
            const auto n = juce::jmin (512, 12000 - pos);
            out.setSize (2, n, false, false, true);
            eng.process (out, midi, ctx);
            for (int ch = 0; ch < 2; ++ch)
                full.copyFrom (ch, pos, out, ch, 0, n);
        }
        // Chop 0 ≈ 2400 samples; the echo lands at +6000 where dry is silent.
        return full.getMagnitude (0, 6100, 2000);
    };

    const auto echoFull = renderEcho (0.0f, 1.0f);
    REQUIRE (echoFull > 0.05f); // the send reached the return

    const auto echoNone = renderEcho (0.0f, 0.0f);
    REQUIRE (echoNone < 1.0e-4f); // send level 0 → no echo

    // −6 dB output gain scales the RETURNED echo too → gain is after returns.
    const auto echoQuiet = renderEcho (-6.0f, 1.0f);
    const auto ratio = echoQuiet / echoFull;
    INFO ("ratio " << ratio);
    REQUIRE (std::abs (ratio - 0.501f) < 0.05f);
}

TEST_CASE ("Engine: chop verb sends ring into the plate return after the voice ends",
           "[engine][master][sends]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    const auto tailEnergy = [] (float sendLevel)
    {
        engine::Engine eng;
        eng.prepare (rate, 512, 2);
        REQUIRE (eng.loadBufferSync (makeStaircase (8, 0.4, rate), rate).isEmpty());
        eng.setZeroCrossSnap (false);
        eng.setSliceMode (engine::SliceMode::equal);
        eng.setSensitivity (6.0f / 62.0f);
        disableChoke (eng);
        auto s = eng.getChopSettings (0);
        s.verbSend = sendLevel * 100.0f; // model stores 0–100 %
        eng.setChopSettings (0, s);

        engine::Engine::BlockContext ctx { 36 };
        ctx.master.verbOn = true;
        ctx.master.verbRtnPct = 100.0f;
        ctx.master.verbDecayPct = 60.0f;

        juce::AudioBuffer<float> out (2, 512);
        juce::AudioBuffer<float> full (2, 24000);
        full.clear();
        for (int pos = 0; pos < 24000; pos += 512)
        {
            juce::MidiBuffer midi;
            if (pos == 0)
                midi.addEvent (juce::MidiMessage::noteOn (1, 36, static_cast<juce::uint8> (127)), 0);
            eng.process (out, midi, ctx);
            for (int ch = 0; ch < 2; ++ch)
                full.copyFrom (ch, pos, out, ch, 0, 512);
        }
        // The chop (≈2400) + release is long gone by 12000: only reverb rings.
        return full.getMagnitude (0, 12000, 10000);
    };

    REQUIRE (tailEnergy (1.0f) > 0.01f);
    REQUIRE (tailEnergy (0.0f) < 1.0e-5f);
}

TEST_CASE ("Engine: full master chain + Beat Repeat over odd block sizes stays finite",
           "[engine][master][realtime]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    engine::Engine eng;
    eng.prepare (rate, 4096, 2);
    REQUIRE (eng.loadBufferSync (makeStaircase (8, 1.0, rate), rate).isEmpty());
    eng.setZeroCrossSnap (false);
    eng.setSliceMode (engine::SliceMode::equal);
    eng.setSensitivity (6.0f / 62.0f);

    engine::Engine::BlockContext ctx { 36 };
    ctx.master.filterOn = true;
    ctx.master.cutoffHz = 2000.0f;
    ctx.master.resoPct = 60.0f;
    ctx.master.distOn = true;
    ctx.master.distType = 3;
    ctx.master.drivePct = 80.0f;
    ctx.master.delayOn = true;
    ctx.master.delayRtnPct = 60.0f;
    ctx.master.verbOn = true;
    ctx.master.verbRtnPct = 60.0f;
    ctx.master.widthOn = true;
    ctx.master.widthPct = 180.0f;
    ctx.master.outOn = true;
    ctx.master.outDb = 6.0f;
    ctx.br.mode = 0; // Auto
    ctx.br.chancePct = 100.0f;
    ctx.br.pitchDecaySt = 5.0f;
    ctx.br.accelPct = 60.0f;
    ctx.br.reverseMode = 1;
    ctx.br.filterMode = 1;
    ctx.br.mixMode = 0;

    for (const auto blockSize : { 1, 3, 16, 333, 512, 4096 })
    {
        juce::AudioBuffer<float> out (2, blockSize);
        juce::MidiBuffer midi;
        midi.addEvent (juce::MidiMessage::noteOn (1, 36, static_cast<juce::uint8> (127)),
                       juce::jmax (0, blockSize - 1));
        eng.process (out, midi, ctx);
        for (int i = 0; i < out.getNumSamples(); ++i)
        {
            REQUIRE (std::isfinite (out.getSample (0, i)));
            REQUIRE (std::isfinite (out.getSample (1, i)));
        }
    }
}

TEST_CASE ("Engine: BR performance notes repeat instead of triggering chops",
           "[engine][beatrepeat][midi]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    engine::Engine eng;
    eng.prepare (rate, 512, 2);
    REQUIRE (eng.loadBufferSync (makeStaircase (8, 2.0, rate), rate).isEmpty());
    eng.setZeroCrossSnap (false);
    eng.setSliceMode (engine::SliceMode::equal);
    eng.setSensitivity (6.0f / 62.0f);
    REQUIRE (eng.getNumChops() == 8);
    disableChoke (eng);

    // C3 = 60 with chop base C1 = 36: note 60 is ALSO chop 24 — out of the
    // 8-chop range here, but note 61 (chop 25) etc. must not fire voices when
    // BR is active. Use base 56 so the perf range overlaps real chops:
    // perf base 60 … chop base 56 → notes 60..63 = chops 4..7.
    engine::Engine::BlockContext ctx { 56 };
    ctx.br.mode = 1;       // Manual
    ctx.brPerfBase = 60;   // C3 (spec §7 default)

    juce::AudioBuffer<float> out (2, 512);
    juce::MidiBuffer midi;
    midi.addEvent (juce::MidiMessage::noteOn (1, 60, static_cast<juce::uint8> (127)), 0);
    eng.process (out, midi, ctx);
    REQUIRE (eng.countActiveVoices() == 0);     // no chop fired
    REQUIRE (eng.isBeatRepeatActive());         // the hold engaged

    juce::MidiBuffer off;
    off.addEvent (juce::MidiMessage::noteOff (1, 60), 0);
    eng.process (out, off, ctx);
    const juce::MidiBuffer none;
    // Note 60 = perf base = 1/4-note grid (spec §7) → the slot is 24000
    // samples at 120 BPM; the release lands at its end — run well past it.
    for (int b = 0; b < 60; ++b)
        eng.process (out, none, ctx);
    REQUIRE_FALSE (eng.isBeatRepeatActive());

    // With BR Off, the same note falls through to the chop range again.
    ctx.br.mode = 2;
    ctx.brPerfBase = -1;
    juce::MidiBuffer chopNote;
    chopNote.addEvent (juce::MidiMessage::noteOn (1, 60, static_cast<juce::uint8> (127)), 0);
    eng.process (out, chopNote, ctx);
    REQUIRE (eng.countActiveVoices() == 1);     // chop 4 fired
}
