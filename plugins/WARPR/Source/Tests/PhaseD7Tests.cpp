/*
    WARPR - phase D7 test block (granular engine).  This file holds D7a: the GrainEngine's CLASSIC and LOOP schedulers
    (DSP/GrainEngine.h), their integration into the oscillator / voice (UnisonOsc, WarprVoice, WarprSynth), the half-band
    up-sampler S -> OS, the playMode / grainMode crossfades and the grain viz feed; and D7b ([D7b] sections at the end):
    the CLOUD scheduler, per-grain phase warps and the grain-weighted oscillator budget.

    Called from RunTests.cpp main():   failures += runPhaseD7Tests (full);

    [D7b] contents
      11. cloud scheduler    live grains <= density and ~ density, pool bound, no drops, unsynced spawns, density steps
      12. cloud scan/var.    scan positions exact for Cloud spawns, transposition jitter (+-12 v st), shape ignored, seed use
      13. cloud clicks       density sweeps / jumps, size sweep, position jumps, scheduler switches through CLOUD, OS 8
      14. cloud level        output RMS across density 1 .. 20 (noise source) relative to the sweep mean, OS invariance
      15. hammer             random density / size / mode / warp, pool + live bounds, finite, zero allocations
      16. per-grain warp     analytic output sin(2 pi g(theta)) for a one-cycle sine (forward + reverse grains, mix), bit-exact
                             identities (a = 0, mix = 0, amplitude modes, unimplemented modes), switch / sweep continuity,
                             all phase modes x schedulers finite, processor-level Classic-vs-granular warp equivalence
      17. budget             grain-weighted oscillator cost, unison cap, latching
      18. processor          determinism with Cloud + warp edits, voice-level click ratio, zero allocations, memory sizes

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
        int maxCloud = 0;                      // [D7b] live Cloud grains (not fading), sampled at every chunk end
        std::vector<int> cloudPerChunk;
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

            const int cloudNow = osc->grain.cloudGrainCount();
            out.maxCloud = std::max (out.maxCloud, cloudNow);
            out.cloudPerChunk.push_back (cloudNow);
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
               "hop: Classic = size / 2; Loop = size (1 - shape / 2); the (unused) Cloud entry still reports size / 2 - Cloud has no hop, see [D7b]");
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

        auto zeroRead = [] (double, int, double, float& l, float& r) { l = 0.0f; r = 0.0f; };   // D6: reads carry a speed multiplier

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

        auto zeroRead = [] (double, int, double, float& l, float& r) { l = 0.0f; r = 0.0f; };   // D6: reads carry a speed multiplier

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

    //==========================================================================
    //==========================================================================
    //                                 D7b
    //==========================================================================
    //==========================================================================

    /** One sine cycle of `cycle` frames (single-cycle flag: the region wraps without a seam fade). */
    warpr::SampleData::Ptr makeSineCycleSample (int cycle)
    {
        juce::AudioBuffer<float> b (1, cycle);
        for (int i = 0; i < cycle; ++i)
            b.setSample (0, i, (float) std::sin (2.0 * kPi * (double) i / (double) cycle));
        return warpr::SampleData::create (b, kFs, "sine cycle", true, false);
    }

    /** The D7a two-cycle wave, but with a file rate chosen so that Lc (frames per root-60 cycle) is 480: the cycle clock of
        a warp is then exactly the wave's own period. */
    warpr::SampleData::Ptr makeRoot60TwoCycleSample()
    {
        juce::AudioBuffer<float> b (1, 960);
        for (int i = 0; i < 960; ++i)
        {
            const double ph = (double) (i % 480) / 480.0;
            b.setSample (0, i, (float) (0.35 * std::sin (2.0 * kPi * ph) + 0.2 * std::sin (4.0 * kPi * ph + 0.3)
                                        + 0.1 * std::sin (6.0 * kPi * ph + 1.0)));
        }
        return warpr::SampleData::create (b, 480.0 * rootHz(), "two cycles @ root 60", true, false);
    }

    //==========================================================================
    // 11. cloud scheduler
    //==========================================================================
    void testCloudScheduler()
    {
        std::printf ("\n[D7b] CLOUD scheduler: grainDensity = simultaneous unsynced grains per channel\n");
        const auto sd = makeMixSample (96000);

        for (int dens : { 1, 2, 5, 8, 13, 20 })
        {
            auto log = std::make_unique<warpr::GrainSpawnLog>();
            const auto out = renderGrainOsc (*sd, 1, 96000, 1.0, 21u, [&] (int, warpr::GrainSpan& gs)
            {
                gs.mode = 2;
                gs.sizeMs = 60.0f;
                gs.density = dens;
                gs.position = 0.3f;
                gs.log = log.get();
            });

            double mean = 0.0;
            for (int v : out.cloudPerChunk)
                mean += (double) v / (double) out.cloudPerChunk.size();

            std::printf ("        density %2d: live grains mean %.2f (max %d), active max %d of %d, %d spawns, %d dropped\n",
                         dens, mean, out.maxCloud, out.maxActive, warpr::GrainEngine::poolSize, out.spawned, out.dropped);

            char label[220];
            std::snprintf (label, sizeof (label),
                           "density %d: live Cloud grains never exceed the density, average within 0.5 of it, pool never exceeded, nothing dropped", dens);
            check (out.maxCloud <= dens && mean >= (double) dens - 0.5 && out.maxActive <= warpr::GrainEngine::poolSize
                       && out.dropped == 0 && allFinite (out.l) && peakOf (out.l) > 0.01,
                   label);
        }

        // unsynced: the spawn instants are irregular (random start ages, then respawn on each grain's own end)
        {
            auto log = std::make_unique<warpr::GrainSpawnLog>();
            (void) renderGrainOsc (*sd, 1, 96000, 1.0, 21u, [&] (int, warpr::GrainSpan& gs)
            {
                gs.mode = 2;
                gs.sizeMs = 60.0f;
                gs.density = 8;
                gs.position = 0.3f;
                gs.log = log.get();
            });

            std::vector<double> times;
            bool lenOk = true;
            for (int i = 0; i < log->count; ++i)
            {
                const auto& e = log->entries[ix (i)];
                if (e.preAged)
                    continue;
                times.push_back ((double) e.sample);
                lenOk = lenOk && e.len == 2880;                         // 60 ms at 48 kHz, variation 0
            }

            double mean = 0.0, var = 0.0;
            size_t gaps = 0;
            for (size_t i = 1; i < times.size(); ++i, ++gaps)
                mean += times[i] - times[i - 1];
            mean /= (double) std::max<size_t> (1, gaps);
            for (size_t i = 1; i < times.size(); ++i)
                var += (times[i] - times[i - 1] - mean) * (times[i] - times[i - 1] - mean) / (double) std::max<size_t> (1, gaps);
            const double cv = std::sqrt (var) / std::max (mean, 1.0);
            const bool distinct = std::adjacent_find (times.begin(), times.end()) == times.end();

            std::printf ("        (density 8, 60 ms: %zu respawns, mean gap %.1f samples (ideal 360), gap CV %.2f)\n", times.size(), mean, cv);
            // HAND-DERIVED: 8 grains with random start ages repeat their irregular pattern every 2880 samples; the CV of 8 random
            // spacings is about 0.7, a synchronous scheduler would give 0.  Gate 0.2.
            check (times.size() >= 60 && cv > 0.2 && distinct && lenOk,
                   "unsynced: respawn gaps are irregular (CV > 0.2), never two spawns in the same sample, every grain is size long");
        }

        // grainShape is ignored in Cloud (fixed Hann)
        {
            auto render = [&] (float shape)
            {
                return renderGrainOsc (*sd, 1, 48000, 1.0, 21u, [shape] (int, warpr::GrainSpan& gs)
                {
                    gs.mode = 2;
                    gs.sizeMs = 60.0f;
                    gs.shape = shape;
                    gs.density = 8;
                    gs.position = 0.3f;
                });
            };

            check (bitEqual (render (0.1f).l, render (0.9f).l), "Cloud ignores grainShape: renders at shape 0.1 and 0.9 are bit-identical");
        }

        // density steps: 2 -> 16 (chunk 100) -> 4 (chunk 300), size 40 ms = 60 chunks
        {
            const auto out = renderGrainOsc (*sd, 1, 32 * 520, 1.0, 22u, [] (int ci, warpr::GrainSpan& gs)
            {
                gs.mode = 2;
                gs.sizeMs = 40.0f;
                gs.density = ci < 100 ? 2 : (ci < 300 ? 16 : 4);
                gs.position = 0.2f;
            });

            const int before = out.cloudPerChunk[ix (95)], up = out.cloudPerChunk[ix (172)];
            const int mid = out.cloudPerChunk[ix (295)], down = out.cloudPerChunk[ix (372)];
            int maxAfterUp = 0;
            for (size_t i = 172; i < 300; ++i)
                maxAfterUp = std::max (maxAfterUp, out.cloudPerChunk[i]);

            std::printf ("        (live grains: before the step %d, 1.2 sizes after 2 -> 16: %d, before 16 -> 4: %d, 1.2 sizes after: %d, max %d)\n",
                         before, up, mid, down, out.maxCloud);
            check (before >= 1 && before <= 2 && up >= 15 && up <= 16 && maxAfterUp <= 16,
                   "density 2 -> 16: the extra grains join (staggered) within 1.2 grain lengths and never overshoot");
            check (mid >= 15 && down >= 3 && down <= 4 && out.maxCloud <= 16,
                   "density 16 -> 4: grains finish naturally, no new ones until the count is below 4 (settles within 1.2 grain lengths)");
        }

        // seeds: a Cloud depends on the seed through its random start ages and stagger even at variation 0
        {
            auto render = [&] (std::uint32_t seed)
            {
                return renderGrainOsc (*sd, 1, 48000, 1.0, seed, [] (int, warpr::GrainSpan& gs)
                {
                    gs.mode = 2;
                    gs.sizeMs = 60.0f;
                    gs.density = 8;
                    gs.position = 0.3f;
                });
            };

            const auto a = render (5u), b = render (5u), c = render (6u);
            check (bitEqual (a.l, b.l) && bitEqual (a.r, b.r) && ! bitEqual (a.l, c.l), "Cloud: same seed is bit-identical, a different seed gives a different (random-age) cloud");
            check (bitEqual (a.l, a.r), "Cloud, mono sample, spread 0: L and R are bit-identical");
        }
    }

    //==========================================================================
    // 12. cloud scan + variation (class level, zero read)
    //==========================================================================
    void testCloudScanAndVariation()
    {
        std::printf ("\n[D7b] CLOUD: scan positions, variation (position / size / transposition)\n");
        using warpr::GrainEngine;

        const auto sd = makeMixSample (96000);
        warpr::RegionFold rg;
        rg.configure (*sd, 0.0f, 1.0f, warpr::RegionFold::Mode::Forward);
        auto zeroRead = [] (double, int, double, float& l, float& r) { l = 0.0f; r = 0.0f; };   // D6: reads carry a speed multiplier

        struct Run
        {
            std::unique_ptr<warpr::GrainSpawnLog> log;
            double endPhase = 0.0;
            int rendered = 0;
        };

        auto engineRun = [&] (int mode, float variation, float scan, float pos, float dist, double seconds)
        {
            Run r;
            r.log = std::make_unique<warpr::GrainSpawnLog>();
            auto eng = std::make_unique<warpr::GrainEngine>();
            auto sc = std::make_unique<warpr::GrainScratch>();
            eng->start (9u);

            warpr::GrainSpan sp;
            sp.active = true;
            sp.mode = mode;
            sp.density = 8;
            sp.sizeMs = 20.0f;
            sp.variation = variation;
            sp.scan = scan;
            sp.position = pos;
            sp.scanDist = dist;
            sp.scanLoop = true;
            sp.sampleRate = kFs;
            sp.hb = &warpr::HalfbandCoeffs::get();
            sp.scratch = sc.get();
            sp.log = r.log.get();

            const int total = (int) (seconds * kFs);
            for (int i = 0; i + 32 <= total; i += 32)
            {
                eng->render (sp, rg, *sd, 1.0, 1, 32, zeroRead);
                r.rendered += 32;
            }

            r.endPhase = eng->scanPhase();
            return r;
        };

        // ---- scan: every non-pre-aged Cloud spawn starts at pos + (end - pos) * its own logged scan phase
        {
            const auto r = engineRun (2, 0.0f, 0.5f, 0.1f, 0.5f, 1.2);
            const double d = GrainEngine::scanDurationSeconds (0.5f, false, 60.0);
            const double inc = 1.0 / (d * kFs);
            auto circ = [] (double a, double b) { const double lin = std::abs (a - b); return std::min (lin, 1.0 - lin); };
            auto phaseAt = [&] (double n) { const double raw = n * inc; return raw - std::floor (raw); };

            const double posC = (double) 0.1f;                                   // the engine sees the float parameters
            const double endC = std::clamp (posC + (double) 0.5f, 0.0, 1.0);

            int checkedSpawns = 0, badPhase = 0, badStart = 0;
            for (int i = 0; i < r.log->count; ++i)
            {
                const auto& e = r.log->entries[ix (i)];
                if (e.preAged)
                    continue;

                ++checkedSpawns;
                const double startExpect = std::clamp (posC + (endC - posC) * e.scanPhase, 0.0, 1.0);
                badPhase += circ (e.scanPhase, phaseAt ((double) e.sample)) > 1.0e-9 ? 1 : 0;
                badStart += std::abs (e.startNorm - startExpect) > 1.0e-9 ? 1 : 0;
            }

            std::printf ("        (Cloud scan 0.5: D %.4f s, phase after %d samples %.9f (expected %.9f), %d spawns checked)\n",
                         d, r.rendered, r.endPhase, phaseAt ((double) r.rendered), checkedSpawns);
            check (checkedSpawns >= 50 && badPhase == 0 && badStart == 0 && circ (r.endPhase, phaseAt ((double) r.rendered)) < 1.0e-8,
                   "Cloud scan: position vs time exact - every respawn's scan phase and start position match the analytic ramp");
        }

        // ---- transposition jitter: +-12 v semitones, Cloud only
        auto ratioStats = [&] (int mode, float variation)
        {
            const auto r = engineRun (mode, variation, 0.0f, 0.3f, 0.0f, 1.2);
            double lo = 1.0e9, hi = -1.0e9, sumLog = 0.0;
            int n = 0;
            for (int i = 0; i < r.log->count; ++i)
            {
                const auto& e = r.log->entries[ix (i)];
                lo = std::min (lo, e.ratio);
                hi = std::max (hi, e.ratio);
                sumLog += std::log2 (e.ratio);
                ++n;
            }
            return std::array<double, 4> { lo, hi, n > 0 ? sumLog / (double) n : 0.0, (double) n };
        };

        const auto v0 = ratioStats (2, 0.0f), v5 = ratioStats (2, 0.5f), v1 = ratioStats (2, 1.0f), c1 = ratioStats (0, 1.0f);
        std::printf ("        (Cloud ratio min/max/mean log2: v0 %.3f/%.3f/%.3f  v0.5 %.3f/%.3f/%.3f  v1 %.3f/%.3f/%.3f; Classic v1 %.3f/%.3f; %d spawns)\n",
                     v0[0], v0[1], v0[2], v5[0], v5[1], v5[2], v1[0], v1[1], v1[2], c1[0], c1[1], (int) v1[3]);
        check (v0[0] == 1.0 && v0[1] == 1.0, "Cloud variation 0: no transposition (ratio exactly 1)");
        check (c1[0] == 1.0 && c1[1] == 1.0, "Classic with variation 1: no transposition (it is a Cloud feature)");
        check (v5[0] >= std::exp2 (-0.5) - 1.0e-9 && v5[1] <= std::exp2 (0.5) + 1.0e-9 && v5[1] > 1.15 && v5[0] < 0.87,
               "Cloud variation 0.5: transposition stays within +-6 semitones and uses the range");
        // HAND-DERIVED: ~400 uniform draws: min < 0.7 and max > 1.4 with certainty, mean of the log2 within +-0.25 of 0
        check (v1[0] >= 0.5 - 1.0e-9 && v1[1] <= 2.0 + 1.0e-9 && v1[0] < 0.7 && v1[1] > 1.4 && std::abs (v1[2]) < 0.25 && v1[3] >= 100.0,
               "Cloud variation 1: transposition spans +-12 semitones (ratio 0.5 .. 2), centred on 0");
    }

    //==========================================================================
    // 13. cloud click metric
    //==========================================================================
    void testCloudClicks (bool full)
    {
        std::printf ("\n[D7b] CLOUD click metric: density / size / position changes, switches through CLOUD\n");

        const auto sd = makeMixSample (full ? 192000 : 144000);
        const int total = full ? 144000 : 96000;
        const int nChunks = total / 32;
        constexpr double kBound = 0.40;           // HAND-DERIVED, as in D7a: ~2.7x the source's own ratio (0.15) plus window slopes

        auto run = [&] (const char* name, auto&& fn, int os = 1, double bound = kBound)
        {
            const auto out = renderGrainOsc (*sd, os, total, 1.0, 7u, fn);
            const Click c = clickOf (out.l, 256 * ix (os), out.l.size());
            const Click cr = clickOf (out.r, 256 * ix (os), out.r.size());
            std::printf ("        %-46s ratio %.4f (max step %.4f, RMS %.4f), active max %d, dropped %d\n",
                         name, c.ratio, c.step, c.rms, out.maxActive, out.dropped);

            char label[220];
            std::snprintf (label, sizeof (label), "%s: click ratio %.3f <= %.2f, finite, pool respected", name, c.ratio, bound);
            check (c.ratio <= bound && cr.ratio <= bound && allFinite (out.l) && allFinite (out.r) && c.rms > 0.02
                       && out.maxActive <= warpr::GrainEngine::poolSize,
                   label);
        };

        run ("density sweep 1 -> 20 -> 1 (triangle)", [=] (int ci, warpr::GrainSpan& gs)
        {
            const double t = (double) ci / (double) nChunks;
            const double tri = 1.0 - std::abs (2.0 * t - 1.0);
            gs.mode = 2;
            gs.sizeMs = 60.0f;
            gs.density = 1 + (int) std::lround (19.0 * tri);
        });

        run ("density jumps every 25 chunks (1..20)", [rnd = juce::Random (11), cur = 8] (int ci, warpr::GrainSpan& gs) mutable
        {
            if (ci % 25 == 0)
                cur = 1 + rnd.nextInt (20);

            gs.mode = 2;
            gs.sizeMs = 50.0f;
            gs.density = cur;
        });

        run ("size sweep 400 -> 2 ms, density 8", [=] (int ci, warpr::GrainSpan& gs)
        {
            const double t = (double) ci / (double) nChunks;
            gs.mode = 2;
            gs.density = 8;
            gs.sizeMs = (float) (400.0 * std::pow (2.0 / 400.0, t));
        });

        run ("size sweep 2 -> 400 ms, density 20", [=] (int ci, warpr::GrainSpan& gs)
        {
            const double t = (double) ci / (double) nChunks;
            gs.mode = 2;
            gs.density = 20;
            gs.sizeMs = (float) (2.0 * std::pow (200.0, t));
        });

        run ("position jumps every 33 ms, size 20 ms, density 8", [rnd = juce::Random (5), cur = 0.0f] (int ci, warpr::GrainSpan& gs) mutable
        {
            if (ci % 50 == 0)
                cur = rnd.nextFloat();

            gs.mode = 2;
            gs.density = 8;
            gs.sizeMs = 20.0f;
            gs.position = cur;
        }, 1, 0.50);   // MEASURED 0.412: the absolute step (0.044) is below every passing case's, but 20 ms
                       // grains at jumping positions leave a low local RMS (0.107) that inflates the ratio.

        run ("switches Classic -> Cloud -> Loop -> Cloud (67 ms)", [] (int ci, warpr::GrainSpan& gs)
        {
            static constexpr int seq[4] = { 0, 2, 1, 2 };
            gs.mode = seq[(ci / 100) % 4];
            gs.sizeMs = 90.0f;
            gs.density = 12;
        });

        run ("switches Cloud 20 <-> Classic every 21 chunks", [] (int ci, warpr::GrainSpan& gs)
        {
            gs.mode = (ci / 21) % 2 == 0 ? 2 : 0;
            gs.sizeMs = 25.0f;
            gs.density = 20;
        });

        run ("playMode crossfade Classic <-> Granular (Cloud)", [w = 0.0f] (int ci, warpr::GrainSpan& gs) mutable
        {
            const float target = ((ci / 150) % 2 == 0) ? 0.0f : 1.0f;
            const float step = (float) (32.0 / (0.010 * kFs));
            gs.w0 = w;
            w = target > w ? std::min (target, w + step) : std::max (target, w - step);
            gs.w1 = w;
            gs.active = gs.w0 > 0.0f || gs.w1 > 0.0f;
            gs.mode = 2;
            gs.sizeMs = 70.0f;
            gs.density = 10;
        });

        run ("fast scan (scan 0.9 loop, dist 0.6), Cloud", [] (int, warpr::GrainSpan& gs)
        {
            gs.mode = 2;
            gs.scan = 0.9f;
            gs.scanDist = 0.6f;
            gs.position = 0.1f;
            gs.sizeMs = 25.0f;
            gs.density = 6;
        });

        run ("direction Reverse <-> Forward every 100 ms, Cloud", [] (int ci, warpr::GrainSpan& gs)
        {
            gs.mode = 2;
            gs.reverseDir = (ci / 150) % 2 == 1;
            gs.sizeMs = 50.0f;
            gs.density = 8;
        });

        run ("OS 8: size sweep through the up-sampler, Cloud 12", [=] (int ci, warpr::GrainSpan& gs)
        {
            const double t = (double) ci / (double) nChunks;
            gs.mode = 2;
            gs.density = 12;
            gs.sizeMs = (float) (400.0 * std::pow (2.0 / 400.0, t));
        }, 8);

        // variation 1 transposes grains by up to +-1 octave: the 1.7 kHz component of the source lands at 3.4 kHz and the
        // source's own click ratio doubles, so the bound is relaxed (HAND-DERIVED: 0.15 x 2 + window slopes + margin; tune from the print)
        run ("variation 1 + spread 1 (transposed +-12 st), Cloud", [] (int, warpr::GrainSpan& gs)
        {
            gs.mode = 2;
            gs.variation = 1.0f;
            gs.spread = 1.0f;
            gs.sizeMs = 40.0f;
            gs.density = 10;
        }, 1, 0.60);
    }

    //==========================================================================
    // 14. cloud level normalisation
    //==========================================================================
    void testCloudNormalisation (bool full)
    {
        std::printf ("\n[D7b] CLOUD level normalisation across grainDensity 1 .. 20 (noise source, size 40 ms, variation 0)\n");

        const auto sd = makeNoiseSample (96000);
        const auto src = samplePcm (*sd);
        const double srcRms = rmsOf (src, 0, src.size());

        std::vector<int> dens = { 1, 2, 3, 5, 8, 12, 16, 20 };
        if (full)
        {
            dens.clear();
            for (int d = 1; d <= 20; ++d)
                dens.push_back (d);
        }

        std::vector<double> dbs;
        std::printf ("        output RMS re the source (%.4f):", srcRms);

        for (int d : dens)
        {
            const auto out = renderGrainOsc (*sd, 1, 96000, 1.0, 11u, [d] (int, warpr::GrainSpan& gs)
            {
                gs.mode = 2;
                gs.sizeMs = 40.0f;
                gs.density = d;
                gs.position = 0.3f;
            });

            const double db = dbOf (rmsOf (out.l, 4800, out.l.size()) / srcRms);
            dbs.push_back (db);
            std::printf (" %d:%+.2f", d, db);
        }
        std::printf (" dB\n");

        double mean = 0.0, lo = 1.0e9, hi = -1.0e9;
        for (double v : dbs)
        {
            mean += v / (double) dbs.size();
            lo = std::min (lo, v);
            hi = std::max (hi, v);
        }

        // Plan gate: within +-1.5 dB across density 1-20, relative to the sweep mean (D7a convention).  Analytic offset of the
        // window family: the per-grain gain sqrt(2/N) makes N uncorrelated Hann grains sum to sqrt(2 mean(w^2)) = -1.25 dB, like Classic.
        std::printf ("        (mean %+.2f dB, spread %.2f dB, analytic -1.25 dB)\n", mean, hi - lo);
        char label[200];
        std::snprintf (label, sizeof (label), "Cloud: output RMS within +-1.5 dB of the sweep mean across density 1 .. 20 (spread %.2f dB)", hi - lo);
        check (lo >= mean - 1.5 && hi <= mean + 1.5, label);
        check (std::abs (mean - (-1.25)) < 1.0, "Cloud: mean level within 1 dB of the analytic window-family offset (-1.25 dB), i.e. matches Classic");

        // OS invariance
        {
            std::printf ("        RMS of the same Cloud at OS 1 / 2 / 4 / 8:");
            double ref = 0.0;
            bool ok = true;
            for (int os : { 1, 2, 4, 8 })
            {
                const auto out = renderGrainOsc (*sd, os, 48000, 1.0, 5u, [] (int, warpr::GrainSpan& gs)
                {
                    gs.mode = 2;
                    gs.sizeMs = 60.0f;
                    gs.density = 8;
                    gs.position = 0.3f;
                });
                const double r = rmsOf (out.l, out.l.size() / 4, out.l.size());
                ref = os == 1 ? r : ref;
                std::printf (" %.4f", r);
                // HAND-GUESSED: the same seed gives the same grain timing at every grain rate, but the stagger quantises differently
                ok = ok && std::abs (dbOf (r / ref)) < 0.75 && allFinite (out.l);
            }
            std::printf ("\n");
            check (ok, "Cloud: OS 1 / 2 / 4 / 8 deliver the same level within 0.75 dB");
        }
    }

    //==========================================================================
    // 15. hammer: pools, live bound, allocations, finite (Cloud + warp)
    //==========================================================================
    void testCloudHammer()
    {
        std::printf ("\n[D7b] hammer: random density / size / scheduler / phase warp - pool, live bound, finite, zero allocations\n");
        const auto sd = makeMixSample (96000);
        warpr::RegionFold rg;
        rg.configure (*sd, 0.0f, 1.0f, warpr::RegionFold::Mode::Forward);

        (void) warpr::GrainWindowTable::get();
        (void) warpr::HalfbandCoeffs::get();
        const int warpModes[] = { 0, 1, 2, 3, 7, 9, 14, 17, 25, 50 };       // phase modes, amplitude modes, an unimplemented phase mode

        for (int os : { 1, 2, 4, 8 })
        {
            auto osc = std::make_unique<warpr::UnisonOsc>();
            auto scratch = std::make_unique<warpr::GrainScratch>();
            osc->start (rg, false, 0, 480.0, 99u);
            juce::Random rnd (777 + os);

            std::array<float, 256> bl {}, br {};
            std::size_t allocs = 0;
            int maxActive = 0, maxCloud = 0;
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
                    gs.density = 1 + rnd.nextInt (20);
                    gs.sizeMs = (float) (2.0 * std::pow (400.0, (double) rnd.nextFloat()));
                    gs.shape = rnd.nextFloat();
                    gs.variation = rnd.nextFloat();
                    gs.spread = rnd.nextFloat();
                    gs.reverse = rnd.nextFloat();
                    gs.position = rnd.nextFloat();
                    gs.scan = rnd.nextFloat();
                    gs.scanDist = rnd.nextFloat() * 2.0f - 1.0f;
                    gs.reverseDir = rnd.nextInt (8) == 0;
                    gs.pitchSemis = 36.0 + 48.0 * (double) rnd.nextFloat();

                    gs.warp.active = rnd.nextInt (4) != 0;
                    gs.warp.mode = warpModes[rnd.nextInt (10)];
                    gs.warp.a0 = rnd.nextFloat();
                    gs.warp.a1 = rnd.nextFloat();
                    gs.warp.A0 = gs.warp.A1 = rnd.nextFloat();
                    gs.warp.B0 = gs.warp.B1 = rnd.nextFloat();
                    gs.warp.mix0 = rnd.nextFloat();
                    gs.warp.mix1 = rnd.nextFloat();
                    gs.warp.Lc = 100.0 + 900.0 * (double) rnd.nextFloat();

                    bl.fill (0.0f);
                    br.fill (0.0f);
                    const double rho = std::exp2 ((gs.pitchSemis - 60.0) / 12.0);
                    osc->render (*sd, rg, 0, rho, rho, os, 1.0f, 1.0f, 1.0f, 1.0f, bl.data(), br.data(), 32 * os,
                                 nullptr, nullptr, &gs);

                    maxActive = std::max (maxActive, osc->grain.activeCount());
                    maxCloud = std::max (maxCloud, osc->grain.cloudGrainCount());

                    for (int k = 0; k < 32 * os; ++k)
                    {
                        const float a = bl[ix (k)], b = br[ix (k)];
                        finite = finite && std::isfinite (a) && std::isfinite (b);
                        peak = std::max (peak, (double) std::max (std::abs (a), std::abs (b)));
                    }
                }

                allocs = counter.count();
            }

            std::printf ("        OS %dx: max active %d (pool %d), max live Cloud %d (cap 20), %d dropped, peak %.2f, allocations %zu\n",
                         os, maxActive, warpr::GrainEngine::poolSize, maxCloud, osc->grain.droppedSpawns(), peak, allocs);

            char label[200];
            std::snprintf (label, sizeof (label), "OS %dx: random density / size / scheduler / warp: pool and the 20-grain Cloud bound hold, finite, bounded", os);
            check (maxActive <= warpr::GrainEngine::poolSize && maxCloud <= warpr::GrainEngine::maxCloudGrains && finite && peak < 64.0, label);
            std::snprintf (label, sizeof (label), "OS %dx: Cloud + per-grain warp perform zero allocations", os);
            check (allocs == 0, label);
        }
    }

    //==========================================================================
    // 16. per-grain phase warp
    //==========================================================================
    void testPerGrainWarp (bool full)
    {
        std::printf ("\n[D7b] per-grain phase warps (modes 0-14 phase family)\n");
        const auto cycle = makeSineCycleSample (200);

        // ---- exact: a Loop grain (2 s, 0.5 ms ramps, flat top) over a one-cycle sine, rho 1, Lc 200 -> y = sin(2 pi g(theta))
        {
            struct Case { const char* name; int mode; float a, A, B, mix; bool reverse; };
            const Case cases[] = {
                { "Bend Positive a 0.8",            0, 0.8f, 0.5f, 0.0f, 1.0f, false },
                { "Bend Negative a 0.6 A 0.3",      1, 0.6f, 0.3f, 0.0f, 1.0f, false },
                { "Bend Symmetric a 1",             2, 1.0f, 0.5f, 0.0f, 1.0f, false },
                { "Bend Asymmetric a 0.7 B 0.6",    3, 0.7f, 0.5f, 0.6f, 1.0f, false },
                { "PWM a 0.5 B 0.2",                7, 0.5f, 0.5f, 0.2f, 1.0f, false },
                { "Variable Duty a 0.8 B 0.7",     14, 0.8f, 0.5f, 0.7f, 1.0f, false },
                { "Bend Positive a 0.8, mix 0.4",   0, 0.8f, 0.5f, 0.0f, 0.4f, false },
                { "Bend Positive a 0.8, Reverse grains", 0, 0.8f, 0.5f, 0.0f, 1.0f, true },
            };

            for (const auto& cs : cases)
            {
                const auto out = renderGrainOsc (*cycle, 1, 3200, 1.0, 1u, [&cs] (int, warpr::GrainSpan& gs)
                {
                    gs.mode = 1;
                    gs.sizeMs = 2000.0f;
                    gs.shape = 0.0f;
                    gs.reverseDir = cs.reverse;
                    gs.warp.active = true;
                    gs.warp.mode = cs.mode;
                    gs.warp.a0 = gs.warp.a1 = cs.a;
                    gs.warp.A0 = gs.warp.A1 = cs.A;
                    gs.warp.B0 = gs.warp.B1 = cs.B;
                    gs.warp.mix0 = gs.warp.mix1 = cs.mix;
                    gs.warp.Lc = 200.0;
                });

                double maxErr = 0.0, peak = 0.0;
                const double sgn = cs.reverse ? -1.0 : 1.0;           // a reversed grain reads sin(-2 pi x)
                for (size_t n = 64; n < 3000; ++n)
                {
                    const double th = (double) n / 200.0 - std::floor ((double) n / 200.0);
                    const double g = warpr::phasewarp::phaseMap (cs.mode, (double) cs.a, (double) cs.A, (double) cs.B, th, 0.005);
                    const double dry = std::sin (2.0 * kPi * th);
                    const double wet = std::sin (2.0 * kPi * g);
                    const double expect = sgn * (dry + (double) cs.mix * (wet - dry));
                    maxErr = std::max (maxErr, std::abs ((double) out.l[n] - expect));
                    peak = std::max (peak, std::abs (expect));
                }

                std::printf ("        %-40s max error %.2e (peak %.2f)\n", cs.name, maxErr, peak);
                char label[200];
                std::snprintf (label, sizeof (label), "per-grain warp '%s': output equals the analytic sin(2 pi g(theta)) (error %.1e <= 5e-3)", cs.name, maxErr);
                // HAND-GUESSED tolerance: the interpolating sinc8 of a 200-frame sine at fractional positions; tune from the print
                check (maxErr < 5.0e-3 && peak > 0.5, label);
            }
        }

        // ---- bit-exact identities
        {
            const auto sd = makeMixSample (96000);

            auto render = [&] (auto&& warpFn, float spread = 0.3f)
            {
                return renderGrainOsc (*sd, 2, 9600, 1.0, 3u, [&] (int, warpr::GrainSpan& gs)
                {
                    gs.mode = 0;
                    gs.sizeMs = 30.0f;
                    gs.variation = 0.3f;
                    gs.spread = spread;
                    gs.position = 0.2f;
                    warpFn (gs.warp);
                });
            };

            const auto base = render ([] (warpr::GrainWarp&) {});
            const auto a0 = render ([] (warpr::GrainWarp& w) { w.active = true; w.mode = 0; w.Lc = 480.0; });                    // amount 0
            const auto mix0 = render ([] (warpr::GrainWarp& w) { w.active = true; w.mode = 2; w.a0 = w.a1 = 0.9f; w.mix0 = w.mix1 = 0.0f; w.Lc = 480.0; });
            const auto amp = render ([] (warpr::GrainWarp& w) { w.active = true; w.mode = 17; w.a0 = w.a1 = 0.9f; w.Lc = 480.0; });   // Sine Fold: oscillator stage's job
            const auto todo = render ([] (warpr::GrainWarp& w) { w.active = true; w.mode = 56; w.a0 = w.a1 = 0.9f; w.Lc = 480.0; });  // AM (D6): amplitude kind, the oscillator stage's job
            const auto noLc = render ([] (warpr::GrainWarp& w) { w.active = true; w.mode = 0; w.a0 = w.a1 = 0.9f; w.Lc = 0.0; });    // no cycle length known
            const auto warped = render ([] (warpr::GrainWarp& w) { w.active = true; w.mode = 0; w.a0 = w.a1 = 0.9f; w.Lc = 480.0; });

            check (bitEqual (base.l, a0.l) && bitEqual (base.r, a0.r) && peakOf (base.l) > 0.05, "warp amount 0: the granular output is bit-identical to no warp");
            check (bitEqual (base.l, mix0.l) && bitEqual (base.r, mix0.r), "warp mix 0 (stage active, mode 2, a 0.9): bit-identical to no warp");
            check (bitEqual (base.l, amp.l) && bitEqual (base.r, amp.r), "an amplitude-domain mode is not applied by the grains (identity here; the oscillator stage owns it)");
            check (bitEqual (base.l, todo.l) && bitEqual (base.r, todo.r), "an amplitude-kind D6 mode (AM, 56) is not applied by the grains (identity here)");
            check (bitEqual (base.l, noLc.l) && bitEqual (base.r, noLc.r), "unknown cycle length (Lc 0): no per-grain warp");
            std::printf ("        (Bend Positive a 0.9 vs no warp: null %.1f dB)\n", nullDb (base.l, warped.l, 512));
            check (nullDb (base.l, warped.l, 512) > -12.0 && allFinite (warped.l) && allFinite (warped.r), "an active phase mode really changes the grain output");

            // mono sample, spread 0: heads identical -> L == R bit-exactly with the warp on; spread > 0: differ
            const auto mono = render ([] (warpr::GrainWarp& w) { w.active = true; w.mode = 3; w.a0 = w.a1 = 0.8f; w.B0 = w.B1 = 0.5f; w.Lc = 480.0; }, 0.0f);
            const auto wide = render ([] (warpr::GrainWarp& w) { w.active = true; w.mode = 3; w.a0 = w.a1 = 0.8f; w.B0 = w.B1 = 0.5f; w.Lc = 480.0; }, 0.6f);
            check (bitEqual (mono.l, mono.r) && peakOf (mono.l) > 0.05, "per-grain warp, mono sample, spread 0: L and R stay bit-identical");
            check (! bitEqual (wide.l, wide.r) && allFinite (wide.l) && allFinite (wide.r), "per-grain warp, spread 0.6: the two heads carry their own cycle clocks and differ");
        }

        // ---- continuity: warp mode switch (10 ms cross-fade) and amount sweep must not step beyond the steady-state steps
        {
            const auto sd = makeMixSample (144000);
            constexpr int total = 96000;

            auto render = [&] (auto&& warpFn)
            {
                return renderGrainOsc (*sd, 1, total, 1.0, 7u, [&] (int ci, warpr::GrainSpan& gs)
                {
                    gs.mode = 0;
                    gs.sizeMs = 40.0f;
                    gs.position = 0.1f;
                    gs.warp.active = true;
                    gs.warp.Lc = 480.0;
                    gs.warp.mix0 = gs.warp.mix1 = 1.0f;
                    warpFn (ci, gs.warp);
                });
            };

            const size_t from = 3200;
            const auto steadyA = render ([] (int, warpr::GrainWarp& w) { w.mode = 0; w.a0 = w.a1 = 0.8f; });
            const auto steadyB = render ([] (int, warpr::GrainWarp& w) { w.mode = 2; w.a0 = w.a1 = 0.8f; });
            const auto steadyP = render ([] (int, warpr::GrainWarp& w) { w.mode = 7; w.a0 = w.a1 = 0.8f; w.B0 = w.B1 = 0.3f; });
            const auto swAB = render ([] (int ci, warpr::GrainWarp& w) { w.mode = ci < 1500 ? 0 : 2; w.a0 = w.a1 = 0.8f; });
            const auto swAO = render ([] (int ci, warpr::GrainWarp& w) { w.mode = ci < 1500 ? 0 : 17; w.a0 = w.a1 = 0.8f; });      // phase -> amplitude mode
            const auto swBP = render ([] (int ci, warpr::GrainWarp& w) { w.mode = ci < 1500 ? 2 : 7; w.a0 = w.a1 = 0.8f; w.B0 = w.B1 = 0.3f; });
            const auto sweep = render ([] (int ci, warpr::GrainWarp& w)
            {
                w.mode = 0;
                w.a0 = (float) ci / 3000.0f;
                w.a1 = (float) (ci + 1) / 3000.0f;
            });

            const double sA = maxStepOf (steadyA.l, from), sB = maxStepOf (steadyB.l, from), sP = maxStepOf (steadyP.l, from);
            const double stepAB = maxStepOf (swAB.l, from), stepAO = maxStepOf (swAO.l, from), stepBP = maxStepOf (swBP.l, from);
            const double stepSweep = maxStepOf (sweep.l, from);
            std::printf ("        (max step: steady A %.4f B %.4f PWM %.4f | switch A->B %.4f, A->amp-mode %.4f, B->PWM %.4f | a sweep 0->1 %.4f)\n",
                         sA, sB, sP, stepAB, stepAO, stepBP, stepSweep);

            // HAND-DERIVED: the cross-fade is a linear blend of two continuous reads (weight slope 1/480 per sample), so its steps stay
            // below the larger steady step; 1.3 is slack for the weight slope times the difference.
            check (stepAB <= 1.3 * std::max (sA, sB) && stepBP <= 1.3 * std::max (sB, sP), "warp mode switch Bend+ -> Bend Sym and Bend Sym -> PWM: 10 ms cross-fade, no step beyond the steady steps (x1.3)");
            check (stepAO <= 1.3 * sA, "warp mode switch phase mode -> amplitude mode: the grains fade the offset out, no step");
            check (stepSweep <= 1.3 * sA, "warp amount sweep 0 -> 1: no step beyond the steady a = 1 steps (x1.3)");
        }

        // ---- every implemented phase mode x scheduler x OS x amounts: finite, bounded, pool and live bounds
        {
            const auto sd = makeMixSample (96000);
            int bad = 0, combos = 0;
            double worstPeak = 0.0;

            for (int mode : { 0, 1, 2, 3, 7, 9, 14 })
                for (int sched = 0; sched < 3; ++sched)
                    for (int os : (full ? std::vector<int> { 1, 2, 4, 8 } : std::vector<int> { 1, 2 }))
                        for (float a : { 0.5f, 1.0f })
                            for (float mix : { 1.0f, 0.5f })
                            {
                                const auto out = renderGrainOsc (*sd, os, 14400, 1.0, 13u, [=] (int, warpr::GrainSpan& gs)
                                {
                                    gs.mode = sched;
                                    gs.density = 12;
                                    gs.sizeMs = 35.0f;
                                    gs.variation = 0.4f;
                                    gs.spread = 0.5f;
                                    gs.reverse = 0.3f;
                                    gs.position = 0.25f;
                                    gs.warp.active = true;
                                    gs.warp.mode = mode;
                                    gs.warp.a0 = gs.warp.a1 = a;
                                    gs.warp.A0 = gs.warp.A1 = 0.5f;
                                    gs.warp.B0 = gs.warp.B1 = 0.4f;
                                    gs.warp.mix0 = gs.warp.mix1 = mix;
                                    gs.warp.Lc = 480.0;
                                });

                                ++combos;
                                const double pk = std::max (peakOf (out.l), peakOf (out.r));
                                worstPeak = std::max (worstPeak, pk);
                                if (! allFinite (out.l) || ! allFinite (out.r) || pk > 4.0 || out.maxActive > warpr::GrainEngine::poolSize)
                                {
                                    ++bad;
                                    DETAIL ("        bad: mode %d sched %d os %d a %.1f mix %.1f peak %.2f\n", mode, sched, os, (double) a, (double) mix, pk);
                                }
                            }

            std::printf ("        (%d combinations, worst peak %.2f)\n", combos, worstPeak);
            check (bad == 0, "phase modes 0,1,2,3,7,9,14 x Classic/Loop/Cloud x OS x amount x mix: finite, |y| <= 4, pool respected (Mirror's SeamBridge included)");
        }

        // ---- processor level: warped granular playback follows warped Classic playback
        {
            const auto sample = makeRoot60TwoCycleSample();

            auto renderPatch = [&] (bool granular, bool warped)
            {
                PluginProcessor p;
                useSample (p, sample);
                setParam (p, "unisonVoices", 1.0f);
                setParam (p, "sampleStart", 0.0f);
                setParam (p, "sampleEnd", 1.0f);
                setParam (p, "warpMode", 0.0f);
                setParam (p, "warpAmount", warped ? 1.0f : 0.0f);
                setParam (p, "warpMix", 1.0f);                    // default mix is 0 (stage armed but silent)

                if (granular)
                {
                    setParam (p, "playMode", 1.0f);
                    setParam (p, "grainMode", 0.0f);
                    setParam (p, "grainSize", 7.6447f);              // two root-60 cycles: 2 x 480 / (480 x 261.63) s -> hop = one cycle
                    setParam (p, "grainShape", 0.5f);
                    setParam (p, "grainPosition", 0.0f);
                    setParam (p, "grainScan", 0.0f);
                }

                return chan (warpr::tests::render (p, warpr::tests::singleNote (60, 0.8f, kFs, 1.2), kFs, 256, 0.8), 0);
            };

            const auto classic0 = renderPatch (false, false);
            const auto classicW = renderPatch (false, true);
            const auto granular0 = renderPatch (true, false);
            const auto granularW = renderPatch (true, true);

            const size_t from = 14400;
            const double warpAudible = nullDb (classic0, classicW, from);
            const double nullUnwarped = nullDb (classicW, granular0, from);
            const double nullWarped = nullDb (classicW, granularW, from);
            std::printf ("        (Classic warp vs no warp: null %.1f dB | warped Classic vs granular: unwarped grains %.1f dB, warped grains %.1f dB)\n",
                         warpAudible, nullUnwarped, nullWarped);

            check (peakOf (classicW) > 0.1 && warpAudible > -12.0, "the Bend Positive warp is clearly audible on Classic playback");
            // HAND-GUESSED: the grain spawn instants are integer samples while the hop is 183.47, so the grain phase is off by up to
            // ~0.5% of a cycle (-20..-25 dB null at best); the gate is RELATIVE: warped grains must be much closer than unwarped ones.
            check (nullWarped <= nullUnwarped - 6.0 && nullWarped <= -12.0,
                   "per-grain warp: granular playback of a warped wave follows warped Classic playback (>= 6 dB closer than unwarped grains, null <= -12 dB)");
        }
    }

    //==========================================================================
    // 17. oscillator budget
    //==========================================================================
    struct VoiceSnap
    {
        bool found = false;
        int unison = 0, os = 0, costQ = 0, nonGhost = 0, units = 0;
    };

    VoiceSnap snapVoice (PluginProcessor& p)
    {
        VoiceSnap s;
        for (int i = 0; i < warpr::WarprSynth::totalSlots; ++i)
        {
            const auto vi = p.getSynth().getVoiceInfo (i);
            if (! vi.active || vi.ghost)
                continue;

            if (! s.found)
            {
                s.found = true;
                s.unison = vi.unison;
                s.os = vi.os;
                s.costQ = vi.costQ;
            }
            ++s.nonGhost;
        }
        s.units = p.getSynth().getOscillatorUnitsInUse();
        return s;
    }

    void testBudget()
    {
        std::printf ("\n[D7b] oscillator budget: granular notes are charged by grain cost, new notes cap their own unison\n");
        using warpr::WarprSynth;

        check (WarprSynth::oscillatorCostQ (2, false, 2) == 8 && WarprSynth::oscillatorCostQ (1, false, 20) == 4
                   && WarprSynth::oscillatorCostQ (2, true, 2) == 12 && WarprSynth::oscillatorCostQ (2, true, 20) == 48
                   && WarprSynth::oscillatorCostQ (8, true, 8) == 96,
               "cost: Classic playback = os units; granular = os x (1 + 0.25 x grains) units (quarter-unit arithmetic)");
        check (WarprSynth::grainsPerChannel (0, 20) == 2 && WarprSynth::grainsPerChannel (1, 5) == 2 && WarprSynth::grainsPerChannel (2, 20) == 20
                   && WarprSynth::grainsPerChannel (2, 0) == 1 && WarprSynth::grainsPerChannel (2, 99) == 20,
               "grains per channel: Classic 2, Loop 2, Cloud = density clamped to 1 .. 20");

        auto firstNote = [&] (int playMode, int grainMode, int density, int osIndex, int budget)
        {
            PluginProcessor p;
            useSample (p, makeMixSample (48000));
            setParam (p, "unisonVoices", 8.0f);
            setParam (p, "oversampling", (float) osIndex);
            setParam (p, "playMode", (float) playMode);
            setParam (p, "grainMode", (float) grainMode);
            setParam (p, "grainDensity", (float) density);
            p.setPlayConfigDetails (0, 2, kFs, 256);
            p.prepareToPlay (kFs, 256);
            p.getSynth().setOscillatorBudget (budget);

            juce::AudioBuffer<float> buf (2, 256);
            juce::MidiBuffer on;
            on.addEvent (juce::MidiMessage::noteOn (1, 60, 0.8f), 0);
            buf.clear();
            p.processBlock (buf, on);
            const auto s = snapVoice (p);
            p.releaseResources();
            return s;
        };

        struct Case { const char* name; int playMode, grainMode, density, osIndex, budget, expectUnison, expectCostQ; };
        // budget 40 units = 160 quarter units; fit = 160 / costQ
        const Case cases[] = {
            { "Classic playback, OS 2, unison 8",              0, 2, 20, 1,  40, 8,  8 },
            { "Granular Classic scheduler (2 grains), OS 2",    1, 0, 20, 1,  40, 8, 12 },
            { "Granular Loop (2 grains), OS 2",                 1, 1, 20, 1,  40, 8, 12 },
            { "Cloud 4, OS 2 (16 q) -> fit 10",                 1, 2,  4, 1,  40, 8, 16 },
            { "Cloud 8, OS 2 (24 q) -> fit 6",                  1, 2,  8, 1,  40, 6, 24 },
            { "Cloud 20, OS 2 (48 q) -> fit 3",                 1, 2, 20, 1,  40, 3, 48 },
            { "Cloud 20, OS 8 (192 q > budget) -> unison 1",    1, 2, 20, 3,  40, 1, 192 },
            { "default budget 256, Cloud 20, OS 2 -> uncapped", 1, 2, 20, 1, 256, 8, 48 },
            { "default budget 256, Cloud 20, OS 8 -> fit 5",    1, 2, 20, 3, 256, 5, 192 },
            { "budget off (0): Cloud 20, OS 8 -> uncapped",     1, 2, 20, 3,   0, 8, 192 },
        };

        for (const auto& cs : cases)
        {
            const auto s = firstNote (cs.playMode, cs.grainMode, cs.density, cs.osIndex, cs.budget);
            std::printf ("        %-46s unison %d (expected %d), cost %d q (expected %d), %d units in use\n",
                         cs.name, s.unison, cs.expectUnison, s.costQ, cs.expectCostQ, s.units);
            char label[200];
            std::snprintf (label, sizeof (label), "budget '%s': the first note's unison is capped to %d, charged %d quarter units per oscillator", cs.name, cs.expectUnison, cs.expectCostQ);
            check (s.found && s.unison == cs.expectUnison && s.costQ == cs.expectCostQ, label);
        }

        // second note, latching, never touching a sounding voice
        {
            PluginProcessor p;
            useSample (p, makeMixSample (48000));
            setParam (p, "unisonVoices", 8.0f);
            setParam (p, "oversampling", 1.0f);
            setParam (p, "playMode", 1.0f);
            setParam (p, "grainMode", 2.0f);
            setParam (p, "grainDensity", 20.0f);
            p.setPlayConfigDetails (0, 2, kFs, 256);
            p.prepareToPlay (kFs, 256);
            p.getSynth().setOscillatorBudget (40);

            juce::AudioBuffer<float> buf (2, 256);
            juce::MidiBuffer on1, on2, none;
            on1.addEvent (juce::MidiMessage::noteOn (1, 60, 0.8f), 0);
            on2.addEvent (juce::MidiMessage::noteOn (1, 67, 0.8f), 0);

            buf.clear();
            p.processBlock (buf, on1);
            const auto first = snapVoice (p);

            // edit density and playMode: the sounding voice keeps what it was charged
            setParam (p, "grainDensity", 2.0f);
            setParam (p, "playMode", 0.0f);
            buf.clear();
            p.processBlock (buf, none);
            const auto after = snapVoice (p);

            // second note: Classic playback now, cost 8 q; 160 - 144 = 16 q left -> unison 2 without stealing
            buf.clear();
            p.processBlock (buf, on2);
            const auto second = snapVoice (p);

            std::printf ("        (note 1: unison %d, %d q, %d units | after edits: unison %d, %d q | note 2: %d non-ghost voices, %d units in use)\n",
                         first.unison, first.costQ, first.units, after.unison, after.costQ, second.nonGhost, second.units);
            check (first.unison == 3 && first.costQ == 48 && first.units == 36, "Cloud 20 / OS 2 / budget 40: unison capped to 3, 36 units charged");
            check (after.unison == 3 && after.costQ == 48 && after.nonGhost == 1, "editing density and playMode never changes what a sounding voice is charged (latched at note-on)");
            check (second.nonGhost == 2 && second.units == 40, "second note under the remaining budget: shrinks its own unison to 2 (Classic cost 8 q), nobody is stolen, 40 units in use");
            p.releaseResources();
        }

        // a Cloud-20 note that cannot fit steals the oldest voice (as a ghost) instead of exceeding the budget
        {
            PluginProcessor p;
            useSample (p, makeMixSample (48000));
            setParam (p, "unisonVoices", 8.0f);
            setParam (p, "oversampling", 1.0f);
            setParam (p, "playMode", 1.0f);
            setParam (p, "grainMode", 2.0f);
            setParam (p, "grainDensity", 20.0f);
            p.setPlayConfigDetails (0, 2, kFs, 256);
            p.prepareToPlay (kFs, 256);
            p.getSynth().setOscillatorBudget (40);

            juce::AudioBuffer<float> buf (2, 256);
            juce::MidiBuffer on1, on2;
            on1.addEvent (juce::MidiMessage::noteOn (1, 60, 0.8f), 0);
            on2.addEvent (juce::MidiMessage::noteOn (1, 67, 0.8f), 0);
            buf.clear();
            p.processBlock (buf, on1);
            buf.clear();
            p.processBlock (buf, on2);
            const auto s = snapVoice (p);
            std::printf ("        (two Cloud-20 notes under budget 40: %d non-ghost voice(s), unison %d, %d units)\n", s.nonGhost, s.unison, s.units);
            check (s.nonGhost == 1 && s.unison == 3 && s.units <= 40, "two Cloud-20 notes under budget 40: the second steals the first (ghost fade), the budget is never exceeded");
            p.releaseResources();
        }
    }

    //==========================================================================
    // 18. processor level
    //==========================================================================
    void testD7bProcessor (bool full)
    {
        std::printf ("\n[D7b] processor level: determinism, voice-level click ratio, zero allocations, memory\n");

        // ---- determinism of a Cloud session with density / scheduler / warp edits
        {
            auto run = [&]
            {
                PluginProcessor p;
                useSample (p, makeMixSample (96000));
                setParam (p, "unisonVoices", 3.0f);
                setGranular (p, 2, 50.0f);
                setParam (p, "grainDensity", 10.0f);
                setParam (p, "grainVariation", 0.5f);
                setParam (p, "grainSpread", 0.5f);
                setParam (p, "grainScan", 0.3f);
                setParam (p, "grainScanDist", 0.4f);
                setParam (p, "warpMode", 0.0f);
                setParam (p, "warpAmount", 0.6f);
                return renderBlocks (p, 100, 256, 52, [&p] (int b)
                {
                    if (b == 15) setParam (p, "grainDensity", 20.0f);
                    if (b == 25) setParam (p, "warpMode", 2.0f);
                    if (b == 35) setParam (p, "grainMode", 0.0f);
                    if (b == 45) setParam (p, "grainMode", 2.0f);
                    if (b == 55) setParam (p, "grainDensity", 3.0f);
                    if (b == 65) setParam (p, "warpMode", 7.0f);
                    if (b == 75) setParam (p, "playMode", 0.0f);
                    if (b == 80) setParam (p, "playMode", 1.0f);
                    if (b == 90) setParam (p, "warpAmount", 0.0f);
                });
            };

            const auto a = run(), b = run();
            check (bitEqual (a.l, b.l) && bitEqual (a.r, b.r) && peakOf (a.l) > 0.05 && allFinite (a.l),
                   "two identical Cloud sessions (unison 3, variation, spread, scan, per-grain warp, mid-note density / scheduler / warp / playMode edits) are bit-identical");
        }

        // ---- voice-level click ratio through CLOUD (no warp: the warped read steps are covered in section 16)
        {
            constexpr double kBound = 0.45;      // HAND-DERIVED as in D7a: source ratio 0.15 + decimator / DC block / envelope
            const auto sample = makeMixSample (96000);

            for (int osIndex : { 1, 3 })
            {
                PluginProcessor p;
                useSample (p, sample);
                setParam (p, "oversampling", (float) osIndex);
                setGranular (p, 0, 60.0f);
                const auto out = renderBlocks (p, 220, 256, 60, [&p] (int b)
                {
                    if (b == 20) setParam (p, "grainMode", 2.0f);
                    if (b == 50) setParam (p, "grainDensity", 2.0f);
                    if (b == 70) setParam (p, "grainDensity", 18.0f);
                    if (b == 100) setParam (p, "grainSize", 8.0f);
                    if (b == 120) setParam (p, "grainPosition", 0.6f);
                    if (b == 140) setParam (p, "grainMode", 1.0f);
                    if (b == 160) setParam (p, "grainMode", 2.0f);
                    if (b == 180) setParam (p, "playMode", 0.0f);
                    if (b == 195) setParam (p, "playMode", 1.0f);
                });

                const Click c = clickOf (out.l, 1024, out.l.size());
                std::printf ("        OS %dx: Cloud + density / size / position / scheduler / playMode edits: ratio %.4f (max step %.4f, RMS %.4f)\n",
                             1 << osIndex, c.ratio, c.step, c.rms);
                char label[180];
                std::snprintf (label, sizeof (label), "voice level, OS %dx: edits through CLOUD stay click-free (ratio %.3f <= %.2f)", 1 << osIndex, c.ratio, kBound);
                check (c.ratio <= kBound && allFinite (out.l) && allFinite (out.r) && c.rms > 0.02, label);
            }
        }

        // ---- zero allocations: Cloud 20 + per-grain warp + unison 4 + changes
        {
            PluginProcessor p;
            useSample (p, makeMixSample (96000));
            setParam (p, "unisonVoices", 4.0f);
            setParam (p, "oversampling", 2.0f);
            setGranular (p, 2, 50.0f);
            setParam (p, "grainDensity", 20.0f);
            setParam (p, "grainVariation", 0.5f);
            setParam (p, "grainSpread", 0.5f);
            setParam (p, "warpMode", 0.0f);
            setParam (p, "warpAmount", 0.7f);
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
            const int warpModesCycle[4] = { 0, 2, 9, 14 };

            for (int round = 0; round < 16; ++round)
            {
                setParam (p, "grainMode", (float) ((round + 2) % 3));
                setParam (p, "grainDensity", (float) (1 + (round * 7) % 20));
                setParam (p, "grainSize", round % 2 == 0 ? 6.0f : 120.0f);
                setParam (p, "warpMode", (float) warpModesCycle[round % 4]);
                setParam (p, "warpMix", round % 3 == 0 ? 0.5f : 1.0f);

                warpr::tests::ScopedAllocCount counter;
                for (int i = 0; i < 6; ++i)
                {
                    p.processBlock (buf, none);
                    sounding = sounding || p.getSynth().getNumActiveVoices() > 0;
                }
                allocations += counter.count();
            }

            check (sounding, "the counted window contained sounding voices");
            check (allocations == 0, "processBlock performs zero allocations with Cloud-20 + per-grain warp voices (density / scheduler / warp edits)");
            p.releaseResources();
        }

        // ---- memory (informational: the orchestrator re-checks stack behaviour against these)
        {
            const std::size_t grainEng = sizeof (warpr::GrainEngine), osc = sizeof (warpr::UnisonOsc), voice = sizeof (warpr::WarprVoice);
            const std::size_t synthBytes = voice * (std::size_t) warpr::WarprSynth::totalSlots;
            std::printf ("        (sizeof GrainEngine %zu B (pool of %d), UnisonOsc %zu B, WarprVoice %zu B x %d slots = %.2f MB, PluginProcessor %.2f MB)\n",
                         grainEng, warpr::GrainEngine::poolSize, osc, voice, warpr::WarprSynth::totalSlots,
                         (double) synthBytes / 1048576.0, (double) sizeof (PluginProcessor) / 1048576.0);
            check (grainEng > 0 && voice > 0, "memory footprint printed");
        }

        // ---- full tier: a storm of random Cloud / warp edits at the voice level stays finite
        if (full)
        {
            PluginProcessor p;
            useSample (p, makeNoiseSample (96000));
            setParam (p, "unisonVoices", 2.0f);
            setGranular (p, 2, 80.0f);
            juce::Random rnd (2024);
            const auto out = renderBlocks (p, 1500, 256, 55, [&] (int b)
            {
                if (b % 7 != 0)
                    return;

                setParam (p, "grainMode", (float) rnd.nextInt (3));
                setParam (p, "grainDensity", (float) (1 + rnd.nextInt (20)));
                setParam (p, "grainSize", 2.0f * std::pow (1000.0f, rnd.nextFloat()));
                setParam (p, "grainPosition", rnd.nextFloat());
                setParam (p, "grainVariation", rnd.nextFloat());
                setParam (p, "grainSpread", rnd.nextFloat());
                setParam (p, "warpMode", (float) rnd.nextInt (60));
                setParam (p, "warpAmount", rnd.nextFloat());
                setParam (p, "warpParamA", rnd.nextFloat());
                setParam (p, "warpParamB", rnd.nextFloat());
                setParam (p, "playMode", rnd.nextInt (6) == 0 ? 0.0f : 1.0f);
            });
            check (allFinite (out.l) && allFinite (out.r) && peakOf (out.l) <= 1.0 && peakOf (out.l) > 0.01,
                   "storm: 215 random Cloud / density / warp-mode edits over 8 s: finite, within the safety knee");
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

    std::printf ("\n=== Phase D7b: CLOUD scheduler, per-grain phase warps, grain-weighted budget ===\n");

    testCloudScheduler();
    testCloudScanAndVariation();
    testCloudClicks (full);
    testCloudNormalisation (full);
    testCloudHammer();
    testPerGrainWarp (full);
    testBudget();
    testD7bProcessor (full);

    return failures;
}

JUCE_END_IGNORE_WARNINGS_GCC_LIKE
