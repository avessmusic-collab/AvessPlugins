/*
    WARPR - phase D7 test block (granular engine).  This file holds D7a: the GrainEngine's CLASSIC and LOOP schedulers
    (DSP/GrainEngine.h), their integration into the oscillator / voice (UnisonOsc, WarprVoice, WarprSynth), the half-band
    up-sampler S -> OS, the playMode / grainMode crossfades and the grain viz feed.  D7b (CLOUD, per-grain phase warps)
    will append [D7b] sections below.

    Called from RunTests.cpp main():   failures += runPhaseD7Tests (full);

    Contents (plan.md "Phase D7a/b" test criteria + T7)
      1. windows          ramp tables, Hann at shape 0.5 (analytic + overlap-add sum), mean overlap-add gain = 1 for the
                          cosine-ramp family, every ramp >= 0.5 ms (or half the grain)
      2. up-sampler       half-band interpolation stage: unity gain, image rejection, DC
      3. scan             scan phase vs time exact (loop / no loop / SCAN KEY / off), spawn start positions exact
      4. pool bound       random parameter hammer at OS 1/2/4/8: active grains <= pool, zero allocations, finite
      5. determinism      same seed bit-equal; variation 0 is independent of the seed; variation > 0 depends on it
      6. click metric     size sweeps, position jumps, shape sweep, scheduler switches, playMode crossfade, scan,
                          variation + spread: max |dy| / RMS bounded; no step at grain boundaries
      7. normalisation    output RMS across grainSize 2 ms .. 2 s (Classic and Loop) on a noise source
      8. spawn semantics  reverse probability (Loop only), direction, spread L/R, OS invariance, other loop modes
      9. Classic equiv.   a Classic grain train whose hop is a whole number of cycles reproduces Classic playback
                          (processor level, spectral null)
     10. processor        playMode off is bit-exact Classic, determinism with mid-note edits, scheduler / playMode
                          switches at the voice level, OS 8 path, zero allocations, viz grain feed

    Hand-derived gates carry a comment saying so; measured numbers are always printed next to the assertion.
*/

#include "PluginProcessor.h"
#include "DSP/GrainEngine.h"
#include "DSP/HalfbandIIR.h"
#include "DSP/RegionFold.h"
#include "DSP/SampleData.h"
#include "DSP/UnisonOsc.h"
#include "Parameters/ParamTable.h"
#include "Tests/AllocHook.h"
#include "Tests/OfflineRender.h"
#include "Tests/Scanners.h"
#include "Tests/WarpTestKit.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

// Bit-exact float equality is the point of several assertions in this file (bypass nulls, symmetry, determinism), so the
// warning is suppressed here only.
JUCE_BEGIN_IGNORE_WARNINGS_GCC_LIKE ("-Wfloat-equal")

int runPhaseD7Tests (bool full);

namespace
{
    using namespace warpr::tests::kit;

    int failures = 0;
    int printedDetails = 0;

    constexpr double kFs = 48000.0;

    void check (bool cond, const char* name)
    {
        std::printf ("  %s  %s\n", cond ? "PASS" : "FAIL", name);
        if (! cond) ++failures;
    }

    /** Prints a failure detail line (bounded, so a systematic failure does not flood the log). */
   #define DETAIL(...) do { if (printedDetails++ < 60) std::printf (__VA_ARGS__); } while (false)

    void setParam (PluginProcessor& p, const char* id, float value)
    {
        if (auto* prm = p.apvts.getParameter (id))
            prm->setValueNotifyingHost (prm->convertTo0to1 (value));
        else
        {
            std::printf ("  FAIL  unknown parameter '%s'\n", id);
            ++failures;
        }
    }

    double dbOf (double ratio) { return 20.0 * std::log10 (std::max (ratio, 1.0e-12)); }

    //==========================================================================
    // fixtures
    //==========================================================================
    /** Smooth non-periodic programme (220.5 + 331.2 + 1710.7 Hz): its own steps are small, so a click stands out. */
    warpr::SampleData::Ptr makeMixSample (int frames)
    {
        juce::AudioBuffer<float> b (1, frames);
        for (int i = 0; i < frames; ++i)
        {
            const double t = (double) i / kFs;
            b.setSample (0, i, (float) (0.3 * std::sin (2.0 * kPi * 220.5 * t) + 0.2 * std::sin (2.0 * kPi * 331.2 * t + 0.7)
                                        + 0.1 * std::sin (2.0 * kPi * 1710.7 * t + 1.9)));
        }
        return warpr::SampleData::create (b, kFs, "mix", false, false);
    }

    warpr::SampleData::Ptr makeNoiseSample (int frames)
    {
        juce::Random rng (4242);
        juce::AudioBuffer<float> b (1, frames);
        for (int i = 0; i < frames; ++i)
            b.setSample (0, i, 0.3f * (rng.nextFloat() * 2.0f - 1.0f));
        return warpr::SampleData::create (b, kFs, "noise", false, false);
    }

    /** Two periods of a 100 Hz harmonic wave (960 frames at 48 kHz = 20 ms): a Classic grain of 20 ms has a hop of
        exactly one period, so the overlapping grains are coherent. */
    warpr::SampleData::Ptr makeTwoCycleSample()
    {
        juce::AudioBuffer<float> b (1, 960);
        for (int i = 0; i < 960; ++i)
        {
            const double ph = (double) (i % 480) / 480.0;
            b.setSample (0, i, (float) (0.35 * std::sin (2.0 * kPi * ph) + 0.2 * std::sin (4.0 * kPi * ph + 0.3)
                                        + 0.1 * std::sin (6.0 * kPi * ph + 1.0)));
        }
        return warpr::SampleData::create (b, kFs, "two cycles", true, false);
    }

    std::vector<float> samplePcm (const warpr::SampleData& sd)
    {
        const float* ch = sd.levels[0].channel (0);
        return std::vector<float> (ch, ch + sd.numFrames);
    }

    std::vector<float> chan (const juce::AudioBuffer<float>& b, int ch)
    {
        const float* d = b.getReadPointer (ch);
        return std::vector<float> (d, d + b.getNumSamples());
    }

    //==========================================================================
    // measures
    //==========================================================================
    struct Click
    {
        double step = 0.0, rms = 0.0, ratio = 0.0;
    };

    /** Largest sample-to-sample step relative to the RMS of the same range. */
    Click clickOf (const std::vector<float>& x, size_t from, size_t to)
    {
        Click c;
        to = std::min (to, x.size());
        double s2 = 0.0;

        for (size_t i = from; i < to; ++i)
        {
            s2 += (double) x[i] * (double) x[i];

            if (i > from)
                c.step = std::max (c.step, (double) std::abs (x[i] - x[i - 1]));
        }

        c.rms = std::sqrt (s2 / (double) std::max<size_t> (1, to - from));
        c.ratio = c.step / std::max (c.rms, 1.0e-9);
        return c;
    }

    double rmsOf (const std::vector<float>& x, size_t from, size_t to)
    {
        to = std::min (to, x.size());
        double s2 = 0.0;

        for (size_t i = from; i < to; ++i)
            s2 += (double) x[i] * (double) x[i];

        return std::sqrt (s2 / (double) std::max<size_t> (1, to - from));
    }

    /** Amplitude of the sinusoid at exactly `hz` in the last `n` samples (Hann-windowed single-frequency DFT). */
    double lineAmp (const std::vector<float>& y, double fs, double hz, size_t n)
    {
        n = std::min (n, y.size());
        const size_t off = y.size() - n;
        double re = 0.0, im = 0.0, ws = 0.0;

        for (size_t i = 0; i < n; ++i)
        {
            const double w = 0.5 - 0.5 * std::cos (2.0 * kPi * (double) i / (double) n);
            const double ph = 2.0 * kPi * hz * (double) (off + i) / fs;
            re += w * (double) y[off + i] * std::cos (ph);
            im += w * (double) y[off + i] * std::sin (ph);
            ws += w;
        }

        return 2.0 * std::sqrt (re * re + im * im) / ws;
    }

    //==========================================================================
    // class-level harness: one oscillator driven with an explicit GrainSpan
    //==========================================================================
    struct OscOut
    {
        std::vector<float> l, r;
        int maxActive = 0, dropped = 0, spawned = 0;
    };

    /** Renders `totalBase` host samples (rounded down to 32) of ONE unison oscillator in granular mode at `rho` source frames
        per host sample.  `fn (chunkIndex, span)` edits the span before every 32-sample chunk (the harness presets
        active = true, weight 1, 48 kHz, the half-band coefficients and the scratch). */
    template <class Fn>
    OscOut renderGrainOsc (const warpr::SampleData& sd, int os, int totalBase, double rho, std::uint32_t seed, Fn&& fn,
                           warpr::RegionFold::Mode mode = warpr::RegionFold::Mode::Forward)
    {
        warpr::RegionFold rg;
        rg.configure (sd, 0.0f, 1.0f, mode);

        auto osc = std::make_unique<warpr::UnisonOsc>();
        osc->start (rg, false, 0, 0.0, seed);
        auto scratch = std::make_unique<warpr::GrainScratch>();

        constexpr int chunk = 32;
        const int total = (totalBase / chunk) * chunk;

        OscOut out;
        out.l.assign (ix (total) * ix (os), 0.0f);
        out.r.assign (ix (total) * ix (os), 0.0f);

        int ci = 0;
        for (int i = 0; i < total; i += chunk, ++ci)
        {
            warpr::GrainSpan gs;
            gs.active = true;
            gs.w0 = gs.w1 = 1.0f;
            gs.sampleRate = kFs;
            gs.hb = &warpr::HalfbandCoeffs::get();
            gs.scratch = scratch.get();
            fn (ci, gs);

            osc->render (sd, rg, 0, rho, rho, os, 1.0f, 1.0f, 1.0f, 1.0f,
                         out.l.data() + ix (i) * ix (os), out.r.data() + ix (i) * ix (os), chunk * os,
                         nullptr, nullptr, &gs);

            out.maxActive = std::max (out.maxActive, osc->grain.activeCount());
        }

        out.dropped = osc->grain.droppedSpawns();
        out.spawned = osc->grain.spawnCount();
        return out;
    }

    //==========================================================================
    // processor-level helpers
    //==========================================================================
    struct Blocks
    {
        std::vector<float> l, r;
    };

    /** Plays `note` from block 0 for `numBlocks` blocks; `onBlock (b)` runs before block b (parameter edits). */
    Blocks renderBlocks (PluginProcessor& p, int numBlocks, int blockSize, int note, const std::function<void (int)>& onBlock)
    {
        p.setPlayConfigDetails (0, 2, kFs, blockSize);
        p.prepareToPlay (kFs, blockSize);

        juce::AudioBuffer<float> buf (2, blockSize);
        juce::MidiBuffer on, none;
        on.addEvent (juce::MidiMessage::noteOn (1, note, 0.8f), 0);

        Blocks out;
        for (int b = 0; b < numBlocks; ++b)
        {
            if (onBlock)
                onBlock (b);

            buf.clear();
            p.processBlock (buf, b == 0 ? on : none);
            out.l.insert (out.l.end(), buf.getReadPointer (0), buf.getReadPointer (0) + blockSize);
            out.r.insert (out.r.end(), buf.getReadPointer (1), buf.getReadPointer (1) + blockSize);
        }

        p.releaseResources();
        return out;
    }

    /** Sustained-note patch: the amp envelope is neutral by default (attack 1 ms, sustain 1), nothing to set. */
    void useSample (PluginProcessor& p, const warpr::SampleData::Ptr& s)
    {
        p.getSampleStore().publish (s);
    }

    void setGranular (PluginProcessor& p, int grainMode, float sizeMs, float shape = 0.5f)
    {
        setParam (p, "playMode", 1.0f);
        setParam (p, "grainMode", (float) grainMode);
        setParam (p, "grainSize", sizeMs);
        setParam (p, "grainShape", shape);
    }

    //==========================================================================
    // 1. windows
    //==========================================================================
    void testWindows()
    {
        std::printf ("\n[D7a] grain windows, ramps and level normalisation\n");
        using warpr::GrainEngine;
        const auto& T = warpr::GrainWindowTable::get();

        bool monotone = true, ends = true;
        for (const auto* ramp : { &T.cosine, &T.quarterSine, &T.expo })
        {
            for (size_t i = 1; i + 1 < ramp->size(); ++i)
                monotone = monotone && (*ramp)[i] >= (*ramp)[i - 1];

            ends = ends && std::abs ((*ramp)[0]) < 1.0e-9f && std::abs ((*ramp)[ix (warpr::GrainWindowTable::size)] - 1.0f) < 1.0e-6f;
        }
        check (monotone && ends, "ramp tables (raised cosine, quarter sine, exponential) rise monotonically from 0 to 1");

        // Hann at shape 0.5: analytic, and the hop-spaced overlap-add is exactly 1
        {
            const auto w = GrainEngine::makeWindow (false, 0.5f, 0.08);
            double worst = 0.0, worstSum = 0.0;
            for (int i = 0; i <= 4096; ++i)
            {
                const double u = (double) i / 4096.0;
                worst = std::max (worst, std::abs ((double) w.at (T, (float) u) - (0.5 - 0.5 * std::cos (2.0 * kPi * u))));

                if (i < 2048)
                    worstSum = std::max (worstSum, std::abs ((double) w.at (T, (float) u) + (double) w.at (T, (float) (u + 0.5)) - 1.0));
            }
            std::printf ("        (Hann: worst deviation %.2e, worst overlap-add error %.2e, gain %.6f)\n", worst, worstSum, (double) w.gain);
            check (worst < 3.0e-5 && worstSum < 3.0e-5 && std::abs (w.gain - 1.0f) < 1.0e-6f,
                   "shape 0.5 is a Hann window: matches 0.5 (1 - cos 2 pi u), hop = size / 2 overlap-add sums to 1, gain 1");
        }

        // mean overlap-add gain: 2 * mean(w) * gain == 1 across the shape family (until the gain clamp at 0.5 / 2)
        {
            std::printf ("        (shape: 2 mean(w) gain)");
            bool ok = true;

            for (int k = 0; k <= 10; ++k)
            {
                const float s = (float) k * 0.1f;
                const auto w = GrainEngine::makeWindow (false, s, 0.08);
                double sum = 0.0;
                constexpr int n = 8000;

                for (int i = 0; i < n; ++i)
                    sum += (double) w.at (T, ((float) i + 0.5f) / (float) n);

                const double oa = 2.0 * sum / (double) n * (double) w.gain;
                std::printf (" %.1f:%.4f", (double) s, oa);

                if (k >= 1 && k <= 9)
                    ok = ok && std::abs (oa - 1.0) < 0.01;
            }
            std::printf ("\n");
            check (ok, "level normalisation: mean overlap-add of the window train is 1 for shapes 0.1 .. 0.9 (ends are clamped at gain 0.5 / 2)");
        }

        // minimum ramp
        {
            int bad = 0;
            for (double ms : { 1.0, 2.0, 5.0, 20.0, 100.0, 2000.0 })
                for (float s : { 0.0f, 0.02f, 0.5f, 0.98f, 1.0f })
                    for (bool loop : { false, true })
                    {
                        const auto w = GrainEngine::makeWindow (loop, s, ms * 0.001);
                        const double attack = (double) w.a * ms, release = (double) w.r * ms;     // ms; every size here is >= 1 ms

                        if (attack < 0.5 - 1.0e-4 || release < 0.5 - 1.0e-4)
                        {
                            ++bad;
                            DETAIL ("        ramp too short: loop %d size %.0f ms shape %.2f: attack %.4f ms, release %.4f ms\n",
                                    loop ? 1 : 0, ms, (double) s, attack, release);
                        }

                        if (std::abs (w.at (T, 0.0f)) > 1.0e-6f || std::abs (w.at (T, 1.0f)) > 1.0e-6f)
                            ++bad;
                    }
            check (bad == 0, "every window ramp is at least 0.5 ms long (sizes 1 ms .. 2 s, shapes 0 .. 1, Classic and Loop); windows start and end at 0");
        }

        // schedulers: hop
        check (std::abs (GrainEngine::hopSeconds (0, 80.0f, 0.5f) - 0.040) < 1.0e-12
               && std::abs (GrainEngine::hopSeconds (2, 80.0f, 0.5f) - 0.040) < 1.0e-12
               && std::abs (GrainEngine::hopSeconds (1, 80.0f, 0.5f) - 0.060) < 1.0e-12
               && std::abs (GrainEngine::hopSeconds (1, 80.0f, 1.0f) - 0.040) < 1.0e-12,
               "hop: Classic and (D7b placeholder) Cloud = size / 2; Loop = size (1 - shape / 2)");
    }

    //==========================================================================
    // 2. up-sampler
    //==========================================================================
    void testUpsampler()
    {
        std::printf ("\n[D7a] half-band up-sampler (grain rate -> OS rate)\n");
        const auto& hb = warpr::HalfbandCoeffs::get();
        constexpr int n = 16384;

        warpr::HalfbandStageState st;
        std::vector<float> in (ix (n)), outL (ix (2 * n)), outR (ix (2 * n));
        for (int i = 0; i < n; ++i)
            in[ix (i)] = 0.5f * (float) std::sin (2.0 * kPi * 5000.0 * (double) i / kFs);

        warpr::upsampleStage (hb, st, in.data(), in.data(), outL.data(), outR.data(), n);

        const double amp = lineAmp (outL, 2.0 * kFs, 5000.0, 16384);
        const double image = lineAmp (outL, 2.0 * kFs, kFs - 5000.0, 16384);
        std::printf ("        (5 kHz in, 96 kHz out: line %.5f (expected 0.5), image at 43 kHz %.1f dB re the line)\n", amp, dbOf (image / amp));
        check (std::abs (amp - 0.5) < 0.005, "interpolation stage: unity gain in the passband");
        check (dbOf (image / amp) < -80.0, "interpolation stage: the 43 kHz image is rejected by more than 80 dB (design target 105 dB)");
        check (bitEqual (outL, outR), "interpolation stage: identical channels stay bit-identical");

        warpr::HalfbandStageState st2;
        std::vector<float> dc (2000, 0.5f), dcL (4000), dcR (4000);
        warpr::upsampleStage (hb, st2, dc.data(), dc.data(), dcL.data(), dcR.data(), 2000);
        check (std::abs (dcL.back() - 0.5f) < 1.0e-5f && std::abs (dcL[3998] - 0.5f) < 1.0e-5f, "interpolation stage: DC passes with unity gain");
    }

    //==========================================================================
    // 3. scan
    //==========================================================================
    void testScan()
    {
        std::printf ("\n[D7a] scan: position vs time exact (loop, no loop, SCAN KEY, off)\n");
        using warpr::GrainEngine;

        const auto sd = makeMixSample (96000);
        warpr::RegionFold rg;
        rg.configure (*sd, 0.0f, 1.0f, warpr::RegionFold::Mode::Forward);

        auto zeroRead = [] (double, int, float& l, float& r) { l = 0.0f; r = 0.0f; };

        struct Case
        {
            const char* name;
            float scan, pos, dist;
            bool loop, key;
            double pitch;
            int mode;
        };

        const Case cases[] = {
            { "loop, scan 0.5, pos 0.1, dist +0.5",            0.5f,    0.1f,  0.5f, true,  false, 60.0, 0 },
            { "no loop (holds at the end), scan 0.8",           0.8f,    0.1f,  0.5f, false, false, 60.0, 0 },
            { "SCAN KEY on, note 72 (twice the speed)",          0.5f,    0.1f,  0.5f, true,  true,  72.0, 0 },
            { "SCAN KEY on, note 48 (half the speed), Loop",     0.5f,    0.2f,  0.4f, true,  true,  48.0, 1 },
            { "negative distance, pos 0.8, dist -0.5",           0.6f,    0.8f, -0.5f, true,  false, 60.0, 0 },
            { "distance clamps at the end: pos 0.7, dist +1",    0.5f,    0.7f,  1.0f, true,  false, 60.0, 0 },
            { "scan below 0.001 = off",                          0.0005f, 0.3f,  0.5f, true,  false, 60.0, 0 },
        };

        for (const auto& cs : cases)
        {
            auto eng = std::make_unique<warpr::GrainEngine>();
            auto sc = std::make_unique<warpr::GrainScratch>();
            auto log = std::make_unique<warpr::GrainSpawnLog>();
            eng->start (1u);

            warpr::GrainSpan sp;
            sp.active = true;
            sp.mode = cs.mode;
            sp.position = cs.pos;
            sp.scan = cs.scan;
            sp.scanDist = cs.dist;
            sp.scanLoop = cs.loop;
            sp.scanKey = cs.key;
            sp.pitchSemis = cs.pitch;
            sp.sizeMs = 80.0f;
            sp.sampleRate = kFs;
            sp.hb = &warpr::HalfbandCoeffs::get();
            sp.scratch = sc.get();
            sp.log = log.get();

            constexpr int total = 96000;
            for (int i = 0; i < total; i += 32)
                eng->render (sp, rg, *sd, 1.0, 1, 32, zeroRead);

            const double d = GrainEngine::scanDurationSeconds (cs.scan, cs.key, cs.pitch);
            const double dRef = cs.scan < 0.001f ? 0.0 : 20.0 * std::pow (0.0004, (double) cs.scan) / (cs.key ? std::exp2 ((cs.pitch - 60.0) / 12.0) : 1.0);
            const double inc = d > 0.0 ? 1.0 / (d * kFs) : 0.0;

            auto phaseAt = [&] (double n)
            {
                const double raw = n * inc;
                return cs.loop ? raw - std::floor (raw) : std::min (1.0, raw);
            };

            /* Loop phases live on a circle: the engine integrates 96000 additions and can sit at
               1 - 1e-12 where the one-multiplication analytic lands exactly on the wrap (0.0).
               Those are the same position, so loop-mode phases compare by circular distance.
               The start position is then checked as the exact function of the engine's own logged
               phase (which the circular check has just pinned to the analytic value). */
            const auto phaseDiff = [&] (double a, double b)
            {
                const double lin = std::abs (a - b);
                return cs.loop ? std::min (lin, 1.0 - lin) : lin;
            };

            const double endPhase = eng->scanPhase();
            const double expectEnd = phaseAt ((double) total);

            int checkedSpawns = 0, badPhase = 0, badStart = 0;
            const double posC = std::clamp ((double) cs.pos, 0.0, 1.0);
            const double endC = std::clamp (posC + (double) cs.dist, 0.0, 1.0);

            for (int i = 0; i < log->count; ++i)
            {
                const auto& e = log->entries[ix (i)];
                if (e.preAged)
                    continue;

                ++checkedSpawns;
                const double ph = phaseAt ((double) e.sample);
                const double startExpect = std::clamp (posC + (endC - posC) * e.scanPhase, 0.0, 1.0);

                if (phaseDiff (e.scanPhase, ph) > 1.0e-9)
                    ++badPhase;
                if (std::abs (e.startNorm - startExpect) > 1.0e-9)
                    ++badStart;
            }

            std::printf ("        %-46s D %.5f s, phase after 2 s %.9f (expected %.9f), %d spawns checked\n",
                         cs.name, d, endPhase, expectEnd, checkedSpawns);

            char label[160];
            std::snprintf (label, sizeof (label), "scan '%s': duration formula, phase after 2 s, every spawn's phase and start position exact", cs.name);
            check (std::abs (d - dRef) < 1.0e-12 * std::max (1.0, dRef) && phaseDiff (endPhase, expectEnd) < 1.0e-8
                   && checkedSpawns >= 10 && badPhase == 0 && badStart == 0,
                   label);
        }

        check (std::abs (GrainEngine::scanDurationSeconds (0.5f, true, 72.0) * 2.0 - GrainEngine::scanDurationSeconds (0.5f, false, 72.0)) < 1.0e-12
               && GrainEngine::scanDurationSeconds (0.001f, false, 60.0) > 19.0 && GrainEngine::scanDurationSeconds (1.0f, false, 60.0) < 0.0081,
               "scan duration: 20 s at the slow end, 8 ms at scan 1, SCAN KEY note +12 halves it");
    }

    //==========================================================================
    // 4. pool bound + allocations
    //==========================================================================
    void testPoolAndAllocations()
    {
        std::printf ("\n[D7a] grain pool bound, zero allocations, finite output (random parameter hammer)\n");
        const auto sd = makeMixSample (96000);
        warpr::RegionFold rg;
        rg.configure (*sd, 0.0f, 1.0f, warpr::RegionFold::Mode::Forward);

        (void) warpr::GrainWindowTable::get();
        (void) warpr::HalfbandCoeffs::get();

        for (int os : { 1, 2, 4, 8 })
        {
            auto osc = std::make_unique<warpr::UnisonOsc>();
            auto scratch = std::make_unique<warpr::GrainScratch>();
            osc->start (rg, false, 0, 0.0, 77u);
            juce::Random rnd (1234 + os);

            std::array<float, 256> bl {}, br {};
            std::size_t allocs = 0;
            int maxActive = 0;
            bool finite = true;
            double peak = 0.0;

            {
                warpr::tests::ScopedAllocCount counter;

                for (int ci = 0; ci < 1500; ++ci)
                {
                    warpr::GrainSpan gs;
                    gs.active = true;
                    gs.w0 = gs.w1 = 1.0f;
                    gs.sampleRate = kFs;
                    gs.hb = &warpr::HalfbandCoeffs::get();
                    gs.scratch = scratch.get();
                    gs.mode = rnd.nextInt (3);
                    gs.sizeMs = (float) (2.0 * std::pow (1000.0, (double) rnd.nextFloat()));
                    gs.shape = rnd.nextFloat();
                    gs.variation = rnd.nextFloat();
                    gs.spread = rnd.nextFloat();
                    gs.reverse = rnd.nextFloat();
                    gs.position = rnd.nextFloat();
                    gs.scan = rnd.nextFloat();
                    gs.scanDist = rnd.nextFloat() * 2.0f - 1.0f;
                    gs.scanLoop = rnd.nextBool();
                    gs.scanKey = rnd.nextBool();
                    gs.reverseDir = rnd.nextInt (8) == 0;
                    gs.pitchSemis = 36.0 + 48.0 * (double) rnd.nextFloat();

                    bl.fill (0.0f);
                    br.fill (0.0f);
                    const double rho = std::exp2 ((gs.pitchSemis - 60.0) / 12.0);
                    osc->render (*sd, rg, 0, rho, rho, os, 1.0f, 1.0f, 1.0f, 1.0f, bl.data(), br.data(), 32 * os,
                                 nullptr, nullptr, &gs);

                    maxActive = std::max (maxActive, osc->grain.activeCount());

                    for (int k = 0; k < 32 * os; ++k)
                    {
                        const float a = bl[ix (k)], b = br[ix (k)];
                        finite = finite && std::isfinite (a) && std::isfinite (b);
                        peak = std::max (peak, (double) std::max (std::abs (a), std::abs (b)));
                    }
                }

                allocs = counter.count();
            }

            std::printf ("        OS %dx: max active grains %d (pool %d), %d spawns, %d dropped, peak %.2f, allocations %zu\n",
                         os, maxActive, warpr::GrainEngine::poolSize, osc->grain.spawnCount(), osc->grain.droppedSpawns(), peak, allocs);

            char label[160];
            std::snprintf (label, sizeof (label), "OS %dx: 1500 chunks of random sizes / shapes / modes / scan / variation: pool never exceeded, finite, bounded", os);
            check (maxActive <= warpr::GrainEngine::poolSize && finite && peak < 64.0, label);
            std::snprintf (label, sizeof (label), "OS %dx: the grain engine performs zero allocations", os);
            check (allocs == 0, label);
        }
    }

    //==========================================================================
    // 5. determinism (PRNG)
    //==========================================================================
    void testDeterminism()
    {
        std::printf ("\n[D7a] deterministic PRNG\n");
        const auto sd = makeMixSample (96000);

        auto cfg = [] (float variation, float spread, float reverse)
        {
            return [=] (int ci, warpr::GrainSpan& gs)
            {
                gs.mode = (ci / 100) % 2;
                gs.sizeMs = 30.0f;
                gs.variation = variation;
                gs.spread = spread;
                gs.reverse = reverse;
                gs.position = 0.4f;
                gs.scan = 0.3f;
                gs.scanDist = 0.2f;
            };
        };

        const auto a = renderGrainOsc (*sd, 2, 48000, 1.0, 5u, cfg (0.7f, 0.4f, 0.5f));
        const auto b = renderGrainOsc (*sd, 2, 48000, 1.0, 5u, cfg (0.7f, 0.4f, 0.5f));
        const auto c = renderGrainOsc (*sd, 2, 48000, 1.0, 6u, cfg (0.7f, 0.4f, 0.5f));
        check (bitEqual (a.l, b.l) && bitEqual (a.r, b.r) && peakOf (a.l) > 0.05, "same seed, same parameters: two renders are bit-identical");
        check (! bitEqual (a.l, c.l), "variation > 0: a different seed gives different grains");

        const auto d = renderGrainOsc (*sd, 2, 48000, 1.0, 5u, cfg (0.0f, 0.0f, 0.0f));
        const auto e = renderGrainOsc (*sd, 2, 48000, 1.0, 99u, cfg (0.0f, 0.0f, 0.0f));
        check (bitEqual (d.l, e.l) && bitEqual (d.r, e.r), "variation 0, spread 0, reverse 0: the output does not depend on the seed (no hidden randomness)");
        check (bitEqual (d.l, d.r), "spread 0 on a mono sample: L and R are bit-identical");

        const auto f = renderGrainOsc (*sd, 2, 48000, 1.0, 5u, cfg (0.0f, 0.5f, 0.0f));
        check (! bitEqual (f.l, f.r), "spread 0.5: the left and right grain heads differ (opposite detune + start offset)");
    }

    //==========================================================================
    // 6. click metric
    //==========================================================================
    void testClicks (bool full)
    {
        std::printf ("\n[D7a] click metric: max |dy| / RMS on size sweeps, position jumps, shape sweeps, switches\n");

        const auto sd = makeMixSample (full ? 192000 : 144000);
        const auto src = samplePcm (*sd);
        const Click natural = clickOf (src, 0, src.size());
        std::printf ("        (the source itself: max step %.4f, RMS %.4f, ratio %.4f)\n", natural.step, natural.rms, natural.ratio);

        const int total = full ? 144000 : 96000;
        const int nChunks = total / 32;
        constexpr double kBound = 0.40;           // HAND-DERIVED: ~2.7x the source's own ratio (0.15) plus window slopes (< 0.1); tune from the printed values

        auto run = [&] (const char* name, auto&& fn, int os = 1, warpr::RegionFold::Mode mode = warpr::RegionFold::Mode::Forward)
        {
            const auto out = renderGrainOsc (*sd, os, total, 1.0, 7u, fn, mode);
            const Click c = clickOf (out.l, 256 * ix (os), out.l.size());
            const Click cr = clickOf (out.r, 256 * ix (os), out.r.size());
            std::printf ("        %-44s ratio %.4f (max step %.4f, RMS %.4f), active max %d\n", name, c.ratio, c.step, c.rms, out.maxActive);

            char label[200];
            std::snprintf (label, sizeof (label), "%s: click ratio %.3f <= %.2f, finite", name, c.ratio, kBound);
            check (c.ratio <= kBound && cr.ratio <= kBound && allFinite (out.l) && allFinite (out.r) && c.rms > 0.02, label);
        };

        auto sizeSweep = [=] (int mode, float shape)
        {
            return [=] (int ci, warpr::GrainSpan& gs)
            {
                const double t = (double) ci / (double) nChunks;
                gs.mode = mode;
                gs.shape = shape;
                gs.sizeMs = (float) (400.0 * std::pow (2.0 / 400.0, t));             // 400 ms -> 2 ms, geometric
            };
        };

        run ("size sweep 400 -> 2 ms, Classic", sizeSweep (0, 0.5f));
        run ("size sweep 400 -> 2 ms, Loop", sizeSweep (1, 0.5f));
        run ("size sweep 400 -> 2 ms, Classic shape 0", sizeSweep (0, 0.0f));
        run ("size sweep 2 -> 400 ms (reverse sweep), Classic", [=] (int ci, warpr::GrainSpan& gs)
        {
            const double t = (double) ci / (double) nChunks;
            gs.sizeMs = (float) (2.0 * std::pow (200.0, t));
        });

        run ("position jumps every 33 ms, size 20 ms, Classic", [rnd = juce::Random (5), cur = 0.0f] (int ci, warpr::GrainSpan& gs) mutable
        {
            if (ci % 50 == 0)
                cur = rnd.nextFloat();

            gs.sizeMs = 20.0f;
            gs.position = cur;
        });

        run ("position jumps every 33 ms, size 20 ms, Loop", [rnd = juce::Random (6), cur = 0.0f] (int ci, warpr::GrainSpan& gs) mutable
        {
            if (ci % 50 == 0)
                cur = rnd.nextFloat();

            gs.mode = 1;
            gs.sizeMs = 20.0f;
            gs.position = cur;
        });

        run ("shape sweep 0 -> 1, Classic size 60 ms", [=] (int ci, warpr::GrainSpan& gs)
        {
            gs.sizeMs = 60.0f;
            gs.shape = (float) ci / (float) nChunks;
        });

        run ("shape sweep 0 -> 1, Loop size 60 ms", [=] (int ci, warpr::GrainSpan& gs)
        {
            gs.mode = 1;
            gs.sizeMs = 60.0f;
            gs.shape = (float) ci / (float) nChunks;
        });

        run ("scheduler switches Classic <-> Loop every 67 ms", [] (int ci, warpr::GrainSpan& gs)
        {
            gs.mode = (ci / 100) % 2;
            gs.sizeMs = 90.0f;
        });

        run ("playMode crossfade Classic <-> Granular (10 ms)", [w = 0.0f] (int ci, warpr::GrainSpan& gs) mutable
        {
            const float target = ((ci / 150) % 2 == 0) ? 0.0f : 1.0f;
            const float step = (float) (32.0 / (0.010 * kFs));
            gs.w0 = w;
            w = target > w ? std::min (target, w + step) : std::max (target, w - step);
            gs.w1 = w;
            gs.active = gs.w0 > 0.0f || gs.w1 > 0.0f;
            gs.sizeMs = 70.0f;
        });

        run ("fast scan (scan 0.9 loop, dist 0.6)", [] (int, warpr::GrainSpan& gs)
        {
            gs.scan = 0.9f;
            gs.scanDist = 0.6f;
            gs.position = 0.1f;
            gs.sizeMs = 25.0f;
        });

        run ("variation 1 + spread 1 + reverse 0.5 (Loop)", [] (int, warpr::GrainSpan& gs)
        {
            gs.mode = 1;
            gs.variation = 1.0f;
            gs.spread = 1.0f;
            gs.reverse = 0.5f;
            gs.sizeMs = 40.0f;
        });

        run ("direction Reverse <-> Forward every 100 ms", [] (int ci, warpr::GrainSpan& gs)
        {
            gs.reverseDir = (ci / 150) % 2 == 1;
            gs.sizeMs = 50.0f;
        });

        run ("OS 8: size sweep through the half-band up-sampler", sizeSweep (0, 0.5f), 8);
        run ("Ping-Pong region, size 30 ms", [] (int, warpr::GrainSpan& gs) { gs.sizeMs = 30.0f; }, 1, warpr::RegionFold::Mode::PingPong);
    }

    //==========================================================================
    // 7. level normalisation
    //==========================================================================
    void testNormalisation (bool full)
    {
        std::printf ("\n[D7a] level normalisation across grainSize 2 ms .. 2 s (noise source, os 1, shape 0.5, variation 0)\n");

        const auto sd = makeNoiseSample (96000);
        const auto src = samplePcm (*sd);
        const double srcRms = rmsOf (src, 0, src.size());

        std::vector<float> sizes = { 2.0f, 5.0f, 20.0f, 80.0f, 250.0f, 1000.0f };
        if (full)
            sizes = { 2.0f, 3.0f, 5.0f, 10.0f, 20.0f, 40.0f, 80.0f, 160.0f, 250.0f, 500.0f, 1000.0f, 2000.0f };

        for (int sched = 0; sched < 2; ++sched)
        {
            std::vector<double> dbs;
            std::printf ("        %s, output RMS re the source (%.4f):", sched == 0 ? "Classic" : "Loop", srcRms);

            for (float sizeMs : sizes)
            {
                const int total = std::max (24000, (int) (2.4 * (double) sizeMs * 0.001 * kFs));
                const auto out = renderGrainOsc (*sd, 1, total, 1.0, 11u, [sched, sizeMs] (int, warpr::GrainSpan& gs)
                {
                    gs.mode = sched;
                    gs.sizeMs = sizeMs;
                    gs.shape = 0.5f;
                });

                const double db = dbOf (rmsOf (out.l, 0, out.l.size()) / srcRms);
                dbs.push_back (db);
                std::printf (" %.0fms:%+.2f", (double) sizeMs, db);
            }

            std::printf (" dB\n");

            double mean = 0.0, lo = 1.0e9, hi = -1.0e9;
            for (double v : dbs)
            {
                mean += v / (double) dbs.size();
                lo = std::min (lo, v);
                hi = std::max (hi, v);
            }

            // Plan gate: within +-1.5 dB across the size range.  The reference is the mean of the sweep (a constant offset from the
            // source is a property of the window family, not a size dependence): Classic = sqrt(2 mean(w^2)) = -1.25 dB (Hann at hop
            // size / 2, uncorrelated material), Loop = 0 dB (equal-power crossfade).
            const double expected = sched == 0 ? -1.25 : 0.0;
            std::printf ("        (%s: mean %+.2f dB, spread %.2f dB, analytic %+.2f dB)\n", sched == 0 ? "Classic" : "Loop", mean, hi - lo, expected);

            char label[200];
            std::snprintf (label, sizeof (label), "%s: output RMS within +-1.5 dB of the sweep mean across grain sizes (spread %.2f dB)",
                           sched == 0 ? "Classic" : "Loop", hi - lo);
            check (lo >= mean - 1.5 && hi <= mean + 1.5, label);

            // HAND-DERIVED tolerance: finite-window RMS estimates of a replayed noise snippet scatter by a few tenths of a dB
            std::snprintf (label, sizeof (label), "%s: mean level is within 1 dB of the analytic window-family offset (%+.2f dB)",
                           sched == 0 ? "Classic" : "Loop", expected);
            check (std::abs (mean - expected) < 1.0, label);
        }

        // shapes (information only for the non-Hann ones)
        {
            std::printf ("        Classic, size 80 ms, RMS re the source by shape:");
            for (float s : { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f })
            {
                const auto out = renderGrainOsc (*sd, 1, 48000, 1.0, 11u, [s] (int, warpr::GrainSpan& gs) { gs.sizeMs = 80.0f; gs.shape = s; });
                std::printf (" %.2f:%+.2f", (double) s, dbOf (rmsOf (out.l, 0, out.l.size()) / srcRms));
            }
            std::printf (" dB\n");
        }
    }

    //==========================================================================
    // 8. spawn semantics
    //==========================================================================
    void testSpawnSemantics()
    {
        std::printf ("\n[D7a] spawn semantics: reverse, direction, OS invariance, loop modes\n");
        const auto sd = makeMixSample (96000);
        warpr::RegionFold rg;
        rg.configure (*sd, 0.0f, 1.0f, warpr::RegionFold::Mode::Forward);

        auto zeroRead = [] (double, int, float& l, float& r) { l = 0.0f; r = 0.0f; };

        auto spawnStats = [&] (int mode, float reverse, bool reverseDir)
        {
            auto eng = std::make_unique<warpr::GrainEngine>();
            auto sc = std::make_unique<warpr::GrainScratch>();
            auto log = std::make_unique<warpr::GrainSpawnLog>();
            eng->start (3u);

            warpr::GrainSpan sp;
            sp.active = true;
            sp.mode = mode;
            sp.sizeMs = 10.0f;
            sp.reverse = reverse;
            sp.reverseDir = reverseDir;
            sp.sampleRate = kFs;
            sp.hb = &warpr::HalfbandCoeffs::get();
            sp.scratch = sc.get();
            sp.log = log.get();

            for (int i = 0; i < 96000; i += 32)
                eng->render (sp, rg, *sd, 1.0, 1, 32, zeroRead);

            int total = 0, rev = 0;
            for (int i = 0; i < log->count; ++i)
            {
                if (log->entries[ix (i)].preAged)
                    continue;
                ++total;
                rev += log->entries[ix (i)].reversed ? 1 : 0;
            }
            return std::pair<int, int> (total, rev);
        };

        const auto none = spawnStats (1, 0.0f, false);
        const auto all = spawnStats (1, 1.0f, false);
        const auto half = spawnStats (1, 0.5f, false);
        const auto classic = spawnStats (0, 1.0f, false);
        const auto dirRev = spawnStats (0, 0.0f, true);
        const auto dirRevLoop = spawnStats (1, 1.0f, true);
        std::printf ("        (Loop reverse 0: %d/%d reversed; 1: %d/%d; 0.5: %d/%d; Classic with reverse 1: %d/%d; direction Reverse: %d/%d; Loop rev 1 + direction Reverse: %d/%d)\n",
                     none.second, none.first, all.second, all.first, half.second, half.first, classic.second, classic.first,
                     dirRev.second, dirRev.first, dirRevLoop.second, dirRevLoop.first);
        check (none.second == 0 && all.second == all.first && all.first >= 100, "Loop: grainReverse 0 never reverses, 1 always reverses");
        check (half.second > half.first / 4 && half.second < 3 * half.first / 4, "Loop: grainReverse 0.5 reverses about half of the segments (probability)");
        check (classic.second == 0, "grainReverse is active in Loop mode only (Classic ignores it)");
        check (dirRev.second == dirRev.first && dirRevLoop.second == 0, "direction Reverse reverses every grain; it XORs with the Loop probability");

        // OS invariance: the level does not depend on the oversampling factor (the up-sampler has unity gain)
        {
            std::printf ("        RMS of the same granular patch at OS 1 / 2 / 4 / 8:");
            double ref = 0.0;
            bool ok = true;

            for (int os : { 1, 2, 4, 8 })
            {
                const auto out = renderGrainOsc (*sd, os, 48000, 1.0, 5u, [] (int, warpr::GrainSpan& gs) { gs.sizeMs = 60.0f; gs.position = 0.3f; });
                const double r = rmsOf (out.l, out.l.size() / 4, out.l.size());
                ref = os == 1 ? r : ref;
                std::printf (" %.4f", r);
                ok = ok && std::abs (dbOf (r / ref)) < 0.5 && allFinite (out.l);
            }
            std::printf ("\n");
            check (ok, "OS 1 / 2 / 4 / 8 deliver the same level within 0.5 dB (grain engine at min(OS, 2) x, up-sampled to OS)");
        }

        // all three loop modes stay finite and bounded
        {
            bool ok = true;
            for (auto mode : { warpr::RegionFold::Mode::Forward, warpr::RegionFold::Mode::PingPong, warpr::RegionFold::Mode::OneShot })
            {
                for (bool rev : { false, true })
                {
                    const auto out = renderGrainOsc (*sd, 2, 96000, 1.7, 5u, [rev] (int ci, warpr::GrainSpan& gs)
                    {
                        gs.sizeMs = 120.0f;
                        gs.position = 0.9f;
                        gs.reverseDir = rev;
                        gs.variation = 0.5f;
                        gs.mode = (ci / 200) % 2;
                    }, mode);
                    ok = ok && allFinite (out.l) && allFinite (out.r) && peakOf (out.l) < 4.0;
                }
            }
            check (ok, "Forward / Ping-Pong / One-Shot regions x forward / reverse: grains read inside the region, finite and bounded (pitch +9 st)");
        }

        // inactive span / null span are the bit-exact Classic path
        {
            auto render = [&] (const warpr::GrainSpan* gsp)
            {
                warpr::RegionFold r2;
                r2.configure (*sd, 0.0f, 1.0f, warpr::RegionFold::Mode::Forward);
                warpr::UnisonOsc osc;
                osc.start (r2, false, 0, 0.0, 1u);
                std::vector<float> l (2048, 0.0f), r (2048, 0.0f);

                for (int i = 0; i < 2048; i += 128)
                    osc.render (*sd, r2, 0, 1.3, 1.3, 2, 1.0f, 1.0f, 1.0f, 1.0f, l.data() + i, r.data() + i, 128, nullptr, nullptr, gsp);

                return l;
            };

            warpr::GrainSpan inactive;               // active == false, no scratch
            check (bitEqual (render (nullptr), render (&inactive)), "an inactive GrainSpan leaves the Classic oscillator bit-exact");
        }
    }

    //==========================================================================
    // 9. Classic equivalence (processor level)
    //==========================================================================
    void testClassicEquivalence()
    {
        std::printf ("\n[D7a] Classic grain with size = region and no scan approximates Classic playback\n");

        const auto sample = makeTwoCycleSample();

        auto renderPatch = [&] (bool granular, float sizeMs, std::vector<float>* right = nullptr)
        {
            PluginProcessor p;
            useSample (p, sample);
            setParam (p, "unisonVoices", 1.0f);
            setParam (p, "sampleStart", 0.0f);
            setParam (p, "sampleEnd", 1.0f);

            if (granular)
            {
                setParam (p, "playMode", 1.0f);
                setParam (p, "grainMode", 0.0f);
                setParam (p, "grainSize", sizeMs);
                setParam (p, "grainShape", 0.5f);
                setParam (p, "grainPosition", 0.0f);
                setParam (p, "grainScan", 0.0f);
            }

            const auto out = warpr::tests::render (p, warpr::tests::singleNote (60, 0.8f, kFs, 1.2), kFs, 256, 0.8);

            if (right != nullptr)
                *right = chan (out, 1);

            return chan (out, 0);
        };

        const auto classic = renderPatch (false, 20.0f);
        std::vector<float> granR;
        const auto gran = renderPatch (true, 20.0f, &granR);
        const auto gran2 = renderPatch (true, 40.0f);

        const size_t from = 14400;                      // 0.3 s
        const double null1 = nullDb (classic, gran, from);
        const double null2 = nullDb (classic, gran2, from);
        std::printf ("        (null vs Classic playback: size 20 ms (= region, hop = 1 cycle) %.1f dB; size 40 ms (wraps the region, hop = 2 cycles) %.1f dB)\n",
                     null1, null2);
        std::printf ("        (Classic RMS %.4f, granular RMS %.4f)\n", rmsOf (classic, from, classic.size()), rmsOf (gran, from, gran.size()));

        check (peakOf (classic) > 0.1, "the reference render is audible");
        check (null1 <= -35.0, "size = region, shape 0.5, no scan: the granular render nulls against Classic playback by >= 35 dB");
        check (null2 <= -35.0, "size = 2 x region (grain wraps the loop): nulls against Classic playback by >= 35 dB");
        check (bitEqual (gran, granR), "granular playback of a mono sample with spread 0: L and R are bit-identical");
    }

    //==========================================================================
    // 10. processor level
    //==========================================================================
    void testProcessor (bool full)
    {
        std::printf ("\n[D7a] processor level: playMode off is Classic, determinism, voice-level switches, zero allocations, viz\n");

        // ---- playMode Classic is bit-exact whatever the grain parameters say
        {
            auto make = [&] (bool weird)
            {
                PluginProcessor p;
                if (weird)
                {
                    setParam (p, "grainMode", 1.0f);
                    setParam (p, "grainSize", 7.0f);
                    setParam (p, "grainShape", 0.9f);
                    setParam (p, "grainVariation", 1.0f);
                    setParam (p, "grainSpread", 1.0f);
                    setParam (p, "grainScan", 0.7f);
                    setParam (p, "grainPosition", 0.4f);
                    setParam (p, "grainReverse", 1.0f);
                }
                return renderBlocks (p, 60, 256, 52, nullptr);
            };

            const auto a = make (false), b = make (true);
            check (bitEqual (a.l, b.l) && bitEqual (a.r, b.r) && peakOf (a.l) > 0.05, "playMode Classic: grain parameters have no effect (bit-identical to the default patch)");
        }

        // ---- determinism of a granular session with mid-note edits
        {
            auto run = [&]
            {
                PluginProcessor p;
                useSample (p, makeMixSample (96000));
                setParam (p, "unisonVoices", 3.0f);
                setGranular (p, 0, 40.0f);
                setParam (p, "grainVariation", 0.6f);
                setParam (p, "grainSpread", 0.5f);
                setParam (p, "grainScan", 0.4f);
                setParam (p, "grainScanDist", 0.5f);
                return renderBlocks (p, 90, 256, 50, [&p] (int b)
                {
                    if (b == 15) setParam (p, "grainMode", 1.0f);
                    if (b == 30) setParam (p, "grainSize", 15.0f);
                    if (b == 45) setParam (p, "playMode", 0.0f);
                    if (b == 55) setParam (p, "playMode", 1.0f);
                    if (b == 65) setParam (p, "grainPosition", 0.7f);
                    if (b == 75) setParam (p, "grainReverse", 0.5f);
                });
            };

            const auto a = run(), b = run();
            check (bitEqual (a.l, b.l) && bitEqual (a.r, b.r) && peakOf (a.l) > 0.05 && allFinite (a.l),
                   "two identical granular sessions (unison 3, variation, spread, scan, mid-note scheduler / playMode / size / position edits) are bit-identical");
        }

        // ---- voice-level switches: click ratio of the whole chain
        {
            constexpr double kBound = 0.45;      // HAND-DERIVED: source ratio 0.15 + the decimator / DC block / envelope; tune from the printed value
            const auto sample = makeMixSample (96000);

            for (int osIndex : { 1, 3 })
            {
                PluginProcessor p;
                useSample (p, sample);
                setParam (p, "oversampling", (float) osIndex);
                setGranular (p, 0, 60.0f);
                const auto out = renderBlocks (p, 200, 256, 60, [&p] (int b)
                {
                    if (b == 30) setParam (p, "grainMode", 1.0f);
                    if (b == 60) setParam (p, "playMode", 0.0f);
                    if (b == 90) setParam (p, "playMode", 1.0f);
                    if (b == 120) setParam (p, "grainSize", 6.0f);
                    if (b == 140) setParam (p, "grainPosition", 0.55f);
                    if (b == 160) setParam (p, "grainMode", 0.0f);
                    if (b == 180) setParam (p, "grainShape", 0.0f);
                });

                const Click c = clickOf (out.l, 1024, out.l.size());
                std::printf ("        OS %dx: switches + size + position + shape edits: ratio %.4f (max step %.4f, RMS %.4f)\n",
                             1 << osIndex, c.ratio, c.step, c.rms);
                char label[160];
                std::snprintf (label, sizeof (label), "voice level, OS %dx: scheduler / playMode / size / position / shape edits stay click-free (ratio %.3f <= %.2f)",
                               1 << osIndex, c.ratio, kBound);
                check (c.ratio <= kBound && allFinite (out.l) && allFinite (out.r) && c.rms > 0.02, label);
            }
        }

        // ---- zero allocations with an active granular voice
        {
            PluginProcessor p;
            useSample (p, makeMixSample (96000));
            setParam (p, "unisonVoices", 4.0f);
            setParam (p, "oversampling", 2.0f);
            setGranular (p, 0, 50.0f);
            setParam (p, "grainVariation", 0.5f);
            setParam (p, "grainSpread", 0.5f);
            p.setPlayConfigDetails (0, 2, kFs, 256);
            p.prepareToPlay (kFs, 256);

            juce::AudioBuffer<float> buf (2, 256);
            juce::MidiBuffer on;
            on.addEvent (juce::MidiMessage::noteOn (1, 57, 0.9f), 0);
            on.addEvent (juce::MidiMessage::noteOn (1, 64, 0.9f), 0);
            p.processBlock (buf, on);
            juce::MidiBuffer none;

            std::size_t allocations = 0;
            bool sounding = false;
            const float sizes[4] = { 4.0f, 90.0f, 400.0f, 25.0f };

            for (int round = 0; round < 16; ++round)
            {
                setParam (p, "grainMode", (float) (round % 3));                   // allocates (APVTS): uncounted
                setParam (p, "grainSize", sizes[round % 4]);
                setParam (p, "grainShape", 0.2f * (float) (round % 5));
                setParam (p, "playMode", round % 5 == 3 ? 0.0f : 1.0f);
                setParam (p, "grainPosition", 0.1f * (float) (round % 7));

                warpr::tests::ScopedAllocCount counter;
                for (int i = 0; i < 6; ++i)
                {
                    p.processBlock (buf, none);
                    sounding = sounding || p.getSynth().getNumActiveVoices() > 0;
                }
                allocations += counter.count();
            }

            check (sounding, "the counted window contained sounding voices");
            check (allocations == 0, "processBlock performs zero allocations with granular voices (2 notes x unison 4, scheduler / size / shape / playMode changes)");
            p.releaseResources();
        }

        // ---- viz: real grain data through the existing reporting path
        {
            auto grainsSeen = [&] (bool granular, int& maxGrains, bool& inRange, int& maxVoices, float& voicePos)
            {
                PluginProcessor p;
                useSample (p, makeMixSample (96000));
                if (granular)
                    setGranular (p, 0, 60.0f);
                setParam (p, "grainPosition", 0.5f);

                p.setPlayConfigDetails (0, 2, kFs, 256);
                p.prepareToPlay (kFs, 256);
                juce::AudioBuffer<float> buf (2, 256);
                juce::MidiBuffer on, none;
                on.addEvent (juce::MidiMessage::noteOn (1, 60, 0.9f), 0);

                maxGrains = 0;
                maxVoices = 0;
                inRange = true;
                voicePos = 0.0f;
                WarprVizFrame f;

                for (int b = 0; b < 150; ++b)
                {
                    buf.clear();
                    p.processBlock (buf, b == 0 ? on : none);

                    if (p.getBackendImpl().vizFifo().popLatest (f))
                    {
                        maxGrains = std::max (maxGrains, f.numGrains);
                        maxVoices = std::max (maxVoices, f.numVoices);

                        for (int i = 0; i < f.numGrains; ++i)
                        {
                            const auto& g = f.grains[ix (i)];
                            inRange = inRange && g.pos >= 0.0f && g.pos <= 1.0f && g.level >= 0.0f && g.level <= 1.0f;
                        }

                        if (f.numVoices > 0)
                            voicePos = f.voices[0].pos;
                    }
                }

                p.releaseResources();
            };

            int gMax = 0, gVoices = 0, cMax = 0, cVoices = 0;
            bool gRange = true, cRange = true;
            float gPos = 0.0f, cPos = 0.0f;
            grainsSeen (true, gMax, gRange, gVoices, gPos);
            grainsSeen (false, cMax, cRange, cVoices, cPos);
            std::printf ("        (viz: granular note -> up to %d grains, voice playhead %.3f; Classic note -> %d grains, playhead %.3f)\n", gMax, (double) gPos, cMax, (double) cPos);
            check (gMax >= 2 && gMax <= 64 && gRange && gVoices >= 1, "viz: a granular note publishes its sounding grains (pos / level in 0..1) next to the voice playhead");
            check (cMax == 0 && cVoices >= 1, "viz: a Classic note publishes no grains");
            check (std::abs (gPos - 0.5f) < 0.2f, "viz: the granular voice playhead follows the grains (position 0.5 +- scan 0)");
        }

        // ---- full tier: a longer storm of random edits at the voice level stays finite
        if (full)
        {
            PluginProcessor p;
            useSample (p, makeNoiseSample (96000));
            setParam (p, "unisonVoices", 2.0f);
            setGranular (p, 0, 80.0f);
            juce::Random rnd (31337);
            const auto out = renderBlocks (p, 1500, 256, 55, [&] (int b)
            {
                if (b % 7 != 0)
                    return;

                setParam (p, "grainMode", (float) rnd.nextInt (3));
                setParam (p, "grainSize", 2.0f * std::pow (1000.0f, rnd.nextFloat()));
                setParam (p, "grainShape", rnd.nextFloat());
                setParam (p, "grainPosition", rnd.nextFloat());
                setParam (p, "grainVariation", rnd.nextFloat());
                setParam (p, "grainSpread", rnd.nextFloat());
                setParam (p, "grainReverse", rnd.nextFloat());
                setParam (p, "grainScan", rnd.nextFloat());
                setParam (p, "playMode", rnd.nextInt (6) == 0 ? 0.0f : 1.0f);
                setParam (p, "direction", rnd.nextInt (5) == 0 ? 1.0f : 0.0f);
            });
            check (allFinite (out.l) && allFinite (out.r) && peakOf (out.l) <= 1.0 && peakOf (out.l) > 0.01,
                   "storm: 215 random granular parameter edits over 8 s: finite, within the safety knee");
        }
    }
}

//==============================================================================
int runPhaseD7Tests (bool full)
{
    failures = 0;
    printedDetails = 0;

    std::printf ("\n=== Phase D7a: granular engine (CLASSIC + LOOP) ===\n");

    testWindows();
    testUpsampler();
    testScan();
    testPoolAndAllocations();
    testDeterminism();
    testClicks (full);
    testNormalisation (full);
    testSpawnSemantics();
    testClassicEquivalence();
    testProcessor (full);

    return failures;
}

JUCE_END_IGNORE_WARNINGS_GCC_LIKE
