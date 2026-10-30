/*
    TENSION - console test runner. Grows one block of checks per DSP phase.
    Build:  cmake --build build --target TENSION_Tests
    Run:    ./build/plugins/TENSION/TENSION_Tests

    Phase 3.1 (engine skeleton + transparent routing + OutputSafety):
      T0  TensionMapping stub + OutputSafety unit tests (knee, scrub, guard)
      T1  zero-tension bit-exact passthrough (memcmp) matrix + settle-after-excursion + processor level
      T2  mix / output trim / no-zipper
      T3  NaN / Inf injection
      T4  block-size invariance
      T5  allocation guard (global operator new counter armed around process())
      T6  telemetry atomics

    Phase 3.2 (core series modules, driven by the temporary linear stub mapping):
      T7  stub mapping (Phase 3.2 destinations) + LFOEngine (shapes, period, sync, continuity, S&H)
      T8  FilterEngine (extremes / Q cap / cutoff clamp, click-free sweep)
      T9  SaturationEngine (ADAA alias test via juce::dsp::FFT, loudness vs drive, no DC, finite)
      T10 StereoWidth (LR4 flat recombination, mono-sum loss, mid attenuation, low collapse)
      T11 real chain in-circuit (block invariance, NaN/Inf, bounded output, telemetry, transport)
    Regression: T1..T6 run unchanged against the real modules (T2 safety-clamp and T6 telemetry use
    the series-chain bypass hook to keep their Phase 3.1 identity-proc assumption).

    Phase 3.3 (parallel sends and noise, driven by the linear stub mapping):
      T12 NoiseGenerator (xorshift anchors, golden hash, determinism, exact silence at t = 0,
          RMS monotone, spectral centroid rises, engine-level silent-input ramp)
      T13 ReverbEngine (tail rings out ungated, RT60 scales with roomSize, finite after random sweeps)
      T14 DelayEngine (time table, ping-pong alternation, feedback cap, 60 s soak, tempo jump, NaN guard)
      T15 tails regression (loud burst at t = 1, then t -> 0: bit-exact passthrough again once tails decay)
      T16 NaN injection with sends active (feedback reset via the OutputSafety flag path, recovery)
    Note: with the real sources in the chain the T1 "settled back at t = 0" check waits for the tails
    (10 s after the excursion) instead of 0.2 s; the Phase 3.1 bus-algebra tests keep using the stub /
    bypass hooks, which also switch the three Phase 3.3 sources off.
*/
#include "PluginProcessor.h"
#include "DSP/TensionEngine.h"
#include "DSP/OutputSafety.h"
#include "DSP/TensionMapping.h"
#include "DSP/LFOEngine.h"
#include "DSP/FilterEngine.h"
#include "DSP/SaturationEngine.h"
#include "DSP/StereoWidth.h"
#include "DSP/NoiseGenerator.h"
#include "DSP/ReverbEngine.h"
#include "DSP/DelayEngine.h"

#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <vector>

// ---------------------------------------------------------------------------
// Allocation guard: global operator new counts calls while armed.
// (Armed only around engine/processor process calls on the main thread.)
// ---------------------------------------------------------------------------
namespace
{
    std::atomic<long long> g_allocCount { 0 };
    std::atomic<bool>      g_allocArmed { false };
}

void* operator new (std::size_t size)
{
    if (g_allocArmed.load (std::memory_order_relaxed))
        g_allocCount.fetch_add (1, std::memory_order_relaxed);

    if (void* p = std::malloc (size != 0 ? size : 1))
        return p;

    throw std::bad_alloc();
}

void operator delete (void* p) noexcept                     { std::free (p); }
void operator delete (void* p, std::size_t) noexcept        { std::free (p); }

namespace
{
    int failures = 0;

    void check (bool cond, const char* name)
    {
        std::printf ("  %s  %s\n", cond ? "PASS" : "FAIL", name);
        if (! cond) ++failures;
    }

    // -----------------------------------------------------------------------
    // Signal helpers
    // -----------------------------------------------------------------------
    struct Rng
    {
        std::uint32_t s;
        float next() noexcept   // [-1, 1)
        {
            s ^= s << 13; s ^= s >> 17; s ^= s << 5;
            return (float) (s & 0xFFFFFFu) / 8388608.0f - 1.0f;
        }
    };

    struct Signal
    {
        int numCh = 2;
        std::vector<float> ch[2];
        int size() const { return (int) ch[0].size(); }
    };

    Signal makeNoise (int n, int numCh, std::uint32_t seed, float amp, bool negZeros)
    {
        Signal s;
        s.numCh = numCh;
        Rng rng { seed | 1u };

        for (int c = 0; c < numCh; ++c)
        {
            s.ch[c].resize ((size_t) n);
            for (int i = 0; i < n; ++i)
            {
                float v = rng.next() * amp;
                if (negZeros && (i % 997) == 0) v = -0.0f;
                s.ch[c][(size_t) i] = v;
            }
        }
        return s;
    }

    Signal makeSine (int n, double sr, double freq, float amp)
    {
        Signal s;
        s.numCh = 2;
        for (int c = 0; c < 2; ++c)
        {
            s.ch[c].resize ((size_t) n);
            for (int i = 0; i < n; ++i)
                s.ch[c][(size_t) i] = amp * (float) std::sin (2.0 * 3.14159265358979323846 * freq * (double) i / sr);
        }
        return s;
    }

    Signal makeConst (int n, float v)
    {
        Signal s;
        s.numCh = 2;
        for (int c = 0; c < 2; ++c)
            s.ch[c].assign ((size_t) n, v);
        return s;
    }

    bool bitEqual (const Signal& a, const Signal& b, int from, int to)
    {
        for (int c = 0; c < a.numCh; ++c)
            if (std::memcmp (a.ch[c].data() + from, b.ch[c].data() + from, (size_t) (to - from) * sizeof (float)) != 0)
                return false;
        return true;
    }

    float maxAbsDiff (const Signal& a, const Signal& b, int from, int to)
    {
        float m = 0.0f;
        for (int c = 0; c < a.numCh; ++c)
            for (int i = from; i < to; ++i)
                m = std::max (m, std::abs (a.ch[c][(size_t) i] - b.ch[c][(size_t) i]));
        return m;
    }

    bool allFinite (const Signal& s)
    {
        for (int c = 0; c < s.numCh; ++c)
            for (float v : s.ch[c])
                if (! std::isfinite (v)) return false;
        return true;
    }

    float maxSecondDiff (const std::vector<float>& x)
    {
        float m = 0.0f;
        for (size_t i = 2; i < x.size(); ++i)
            m = std::max (m, std::abs (x[i] - 2.0f * x[i - 1] + x[i - 2]));
        return m;
    }

    // Render `in` through the engine in host blocks of `block` samples.
    // paramsAt(startSample) supplies the parameter snapshot for each host block.
    template <typename ParamFn>
    void render (TensionEngine& eng, const Signal& in, Signal& out, int block, ParamFn paramsAt, bool armGuard = false)
    {
        const int n = in.size();
        out.numCh = in.numCh;
        for (int c = 0; c < in.numCh; ++c)
            out.ch[c].assign ((size_t) n, 0.0f);

        juce::AudioBuffer<float> work (in.numCh, block);

        for (int pos = 0; pos < n; pos += block)
        {
            const int len = std::min (block, n - pos);

            for (int c = 0; c < in.numCh; ++c)
                std::memcpy (work.getWritePointer (c), in.ch[c].data() + pos, (size_t) len * sizeof (float));

            juce::AudioBuffer<float> view (work.getArrayOfWritePointers(), in.numCh, len);
            const EngineParams p = paramsAt (pos);
            const HostTransport tr {};

            if (armGuard) g_allocArmed.store (true);
            eng.process (view, p, tr);
            if (armGuard) g_allocArmed.store (false);

            for (int c = 0; c < in.numCh; ++c)
                std::memcpy (out.ch[c].data() + pos, view.getReadPointer (c), (size_t) len * sizeof (float));
        }
    }

    EngineParams makeParams (float t, float mix = 1.0f, float outDb = 0.0f, bool sync = true, int mode = 1)
    {
        EngineParams p;
        p.tension = t; p.mix = mix; p.outputDb = outDb; p.sync = sync; p.riseMode = mode;
        return p;
    }

    void setParam (TENSIONAudioProcessor& p, const char* id, float plainValue)
    {
        if (auto* prm = p.getAPVTS().getParameter (id))
            prm->setValueNotifyingHost (prm->convertTo0to1 (plainValue));
    }

    // -----------------------------------------------------------------------
    // T0 - unit tests
    // -----------------------------------------------------------------------
    void testUnits()
    {
        std::printf ("\n[T0] TensionMapping stub + OutputSafety units\n");

        using namespace TensionDSP;

        const auto t0 = evaluateTensionMapping (0.0f, RiseMode::Cinematic);
        check (t0.engagement == 0.0f && t0.safety == 0.0f, "mapping: e and s exactly 0 at t = 0");

        const auto t1 = evaluateTensionMapping (1.0f, RiseMode::Aggressive);
        check (t1.engagement == 1.0f && t1.safety == 1.0f, "mapping: e and s exactly 1 at t = 1");

        bool mono = true;
        float prevE = 0.0f, prevS = 0.0f;
        for (int i = 0; i <= 1000; ++i)
        {
            const auto tg = evaluateTensionMapping ((float) i / 1000.0f, RiseMode::SlowBurn);
            if (tg.engagement < prevE || tg.safety < prevS) mono = false;
            prevE = tg.engagement; prevS = tg.safety;
        }
        check (mono, "mapping: e and s monotone in t");

        // Soft knee
        bool identity = true;
        for (int i = -8910; i <= 8910; ++i)
        {
            const float x = (float) i / 10000.0f;
            if (OutputSafety::softKnee (x) != x) identity = false;
        }
        check (identity, "knee: exact identity for |x| <= 0.891");

        bool monotone = true, bounded = true, odd = true;
        float prev = OutputSafety::softKnee (OutputSafety::kneeStart);
        for (int i = 0; i < 20000; ++i)
        {
            const float x = OutputSafety::kneeStart + (float) i * 0.001f;
            const float y = OutputSafety::softKnee (x);
            if (y < prev) monotone = false;
            if (y > 1.0f) bounded = false;
            if (OutputSafety::softKnee (-x) != -y) odd = false;
            prev = y;
        }
        check (monotone, "knee: monotone above the knee");
        check (bounded, "knee: output never exceeds 0 dBFS (asymptote 1.0)");
        check (odd, "knee: odd symmetric");
        check (OutputSafety::softKnee (100.0f) > 0.9999f, "knee: large input approaches 1.0");

        const float k = OutputSafety::kneeStart;
        const float cont = std::abs (OutputSafety::softKnee (k + 1.0e-4f) - (k + 1.0e-4f));
        check (cont < 1.0e-6f, "knee: continuous at the knee start");
        const float slope = (OutputSafety::softKnee (k + 2.0e-3f) - OutputSafety::softKnee (k + 1.0e-3f)) / 1.0e-3f;
        check (slope > 0.99f && slope <= 1.001f, "knee: C1 (slope ~1 at the knee)");

        // processSample: s == 0 is bit-exact identity, even > 0 dBFS
        OutputSafety os;
        os.prepare (48000.0);
        check (os.processSample (3.0f, 0.0f) == 3.0f, "safety: s = 0 leaves +9.5 dBFS untouched");
        check (os.processSample (-0.0f, 0.0f) == 0.0f && std::signbit (os.processSample (-0.0f, 0.0f)), "safety: s = 0 preserves -0.0");
        check (os.processSample (3.0f, 1.0f) <= 1.0f, "safety: s = 1 clamps below 0 dBFS");
        const float half = os.processSample (3.0f, 0.5f);
        check (half > os.processSample (3.0f, 1.0f) && half < 3.0f, "safety: s = 0.5 blends");

        // scrub
        const int before = os.getNonFiniteCount();
        check (os.processSample (std::numeric_limits<float>::quiet_NaN(), 0.0f) == 0.0f, "scrub: NaN -> 0");
        check (os.processSample (std::numeric_limits<float>::infinity(), 1.0f) == 0.0f, "scrub: +Inf -> 0");
        check (os.processSample (-std::numeric_limits<float>::infinity(), 1.0f) == 0.0f, "scrub: -Inf -> 0");
        check (os.getNonFiniteCount() == before + 3, "scrub: counter incremented");
        check (os.consumeResetRequest() && ! os.consumeResetRequest(), "scrub: reset flag raised once, then consumed");

        // runaway guard
        OutputSafety g;
        g.prepare (48000.0);
        for (int i = 0; i < 2400; ++i) g.updateGuard (4.0f);          // 50 ms over threshold
        check (! g.isGuardActive(), "guard: not active before 100 ms");
        for (int i = 0; i < 14400; ++i) g.updateGuard (4.0f);         // +300 ms
        check (g.isGuardActive() && g.getFeedbackClamp() < 0.55f, "guard: engages after > 100 ms above +9 dBFS (clamp -> 0.5)");
        for (int i = 0; i < 96000; ++i) g.updateGuard (0.1f);         // 2 s quiet
        check (! g.isGuardActive() && g.getFeedbackClamp() > 0.99f, "guard: recovers when the wet bus calms down");

        OutputSafety g2;
        g2.prepare (48000.0);
        for (int i = 0; i < 48000; ++i) g2.updateGuard (1.5f);        // +3.5 dBFS sustained: below threshold
        check (! g2.isGuardActive(), "guard: does not trigger on normal hot-but-sane levels");
    }

    // -----------------------------------------------------------------------
    // T1 - zero-tension bit-exact passthrough
    // -----------------------------------------------------------------------
    void testBitExact()
    {
        std::printf ("\n[T1] zero-tension bit-exact passthrough\n");

        const int    blocks[] = { 1, 32, 512, 4096 };
        const double rates[]  = { 44100.0, 48000.0, 96000.0, 192000.0 };

        int configs = 0, bad = 0;

        for (double sr : rates)
            for (int nCh = 1; nCh <= 2; ++nCh)
                for (int mode = 0; mode < 3; ++mode)
                    for (int sync = 0; sync < 2; ++sync)
                        for (int block : blocks)
                        {
                            TensionEngine eng;
                            eng.prepare (sr, block, 2);

                            const int n = (int) (sr * 0.1);
                            const Signal in = makeNoise (n, nCh, 1234u + (std::uint32_t) configs, 1.2f, true);
                            Signal out;

                            const float mix = mode == 0 ? 1.0f : (mode == 1 ? 0.37f : 0.0f);   // "mix anything"
                            const EngineParams p = makeParams (0.0f, mix, 0.0f, sync != 0, mode);
                            render (eng, in, out, block, [&] (int) { return p; });

                            ++configs;
                            if (! bitEqual (in, out, 0, n)) ++bad;
                        }

        std::printf ("  (%d configs: block {1,32,512,4096} x rate {44.1k..192k} x mono/stereo x 3 modes x sync on/off)\n", configs);
        check (bad == 0, "t = 0 output is memcmp-identical to input (incl. > 0 dBFS and -0.0 samples)");

        // Host block larger than the prepared maximum is segmented, still exact.
        {
            TensionEngine eng;
            eng.prepare (48000.0, 64, 2);
            const Signal in = makeNoise (20000, 2, 77u, 1.2f, true);
            Signal out;
            render (eng, in, out, 4096, [&] (int) { return makeParams (0.0f); });
            check (bitEqual (in, out, 0, in.size()), "host block (4096) > prepared max (64): still bit-exact");
        }

        // Excursion: engage, then return to zero -> bit-exact again once settled.
        for (int mode = 0; mode < 3; ++mode)
        {
            TensionEngine eng;
            eng.prepare (48000.0, 512, 2);
            // Phase 3.3: delay / reverb tails now ring after t -> 0, so the exact-passthrough point is
            // "after the tails decayed below -120 dBFS" (the architecture's transparency criterion).
            const int n = 48000 * 14;
            const Signal in = makeNoise (n, 2, 99u + (std::uint32_t) mode, 1.2f, true);
            Signal out;
            const int switchPos = 28 * 512;   // 14336
            render (eng, in, out, 512, [&] (int pos) { return makeParams (pos < switchPos ? 0.8f : 0.0f, 1.0f, 0.0f, true, mode); });

            const bool exact = bitEqual (in, out, switchPos + 48000 * 10, n);
            const bool fin   = allFinite (out);
            char name[96];
            std::snprintf (name, sizeof name, "settled back at t = 0 -> bit-exact again (mode %d)", mode);
            check (exact && fin, name);
        }

        // Processor level (APVTS -> processBlock), mono + stereo, setPlayConfigDetails BEFORE prepareToPlay.
        for (int nCh = 1; nCh <= 2; ++nCh)
            for (double sr : { 44100.0, 192000.0 })
            {
                bool ok = true;
                for (int mode = 0; mode < 3; ++mode)
                    for (int sync = 0; sync < 2; ++sync)
                    {
                        TENSIONAudioProcessor proc;
                        const int block = 512;
                        proc.setPlayConfigDetails (nCh, nCh, sr, block);
                        proc.prepareToPlay (sr, block);

                        setParam (proc, TensionParams::tension, 0.0f);
                        setParam (proc, TensionParams::mix, 0.6f);
                        setParam (proc, TensionParams::riseMode, (float) mode);
                        setParam (proc, TensionParams::sync, sync != 0 ? 1.0f : 0.0f);

                        if (proc.getTotalNumInputChannels() != nCh) ok = false;

                        juce::MidiBuffer midi;
                        for (int b = 0; b < 4; ++b)
                        {
                            const Signal src = makeNoise (block, nCh, 555u + (std::uint32_t) b, 1.2f, true);
                            juce::AudioBuffer<float> buf (nCh, block);
                            for (int c = 0; c < nCh; ++c)
                                std::memcpy (buf.getWritePointer (c), src.ch[c].data(), (size_t) block * sizeof (float));

                            proc.processBlock (buf, midi);

                            for (int c = 0; c < nCh; ++c)
                                if (std::memcmp (buf.getReadPointer (c), src.ch[c].data(), (size_t) block * sizeof (float)) != 0)
                                    ok = false;
                        }

                        if (proc.getTailLengthSeconds() != 8.0 || proc.getLatencySamples() != 0) ok = false;
                    }

                char name[96];
                std::snprintf (name, sizeof name, "processor %s @ %.0f Hz: bit-exact, tail 8 s, latency 0", nCh == 1 ? "mono" : "stereo", sr);
                check (ok, name);
            }
    }

    // -----------------------------------------------------------------------
    // T2 - mix / output / no zipper
    // -----------------------------------------------------------------------
    void testMixOutputZipper()
    {
        std::printf ("\n[T2] mix / output trim / zipper\n");

        const int n = 24000;
        const Signal in = makeNoise (n, 2, 4242u, 0.8f, false);

        // (bus algebra is observable only through the test stub: proc = 0.5 * dry)
        {
            TensionEngine eng;
            eng.prepare (48000.0, 512, 2);
            eng.setStubProcGainForTesting (0.5f);
            Signal out;
            render (eng, in, out, 512, [&] (int) { return makeParams (1.0f, 0.0f); });
            check (bitEqual (in, out, n / 2, n), "mix 0 -> dry, bit-exact (tension 1)");
        }
        {
            TensionEngine eng;
            eng.prepare (48000.0, 512, 2);
            eng.setStubProcGainForTesting (0.5f);
            Signal out, expect = in;
            for (int c = 0; c < 2; ++c) for (auto& v : expect.ch[c]) v *= 0.5f;
            render (eng, in, out, 512, [&] (int) { return makeParams (1.0f, 1.0f); });
            check (maxAbsDiff (out, expect, n / 2, n) < 1.0e-6f, "mix 1, tension 1 -> wet (0.5 x dry)");
        }
        {
            TensionEngine eng;
            eng.prepare (48000.0, 512, 2);
            eng.setStubProcGainForTesting (0.5f);
            Signal out, expect = in;
            for (int c = 0; c < 2; ++c) for (auto& v : expect.ch[c]) v *= 0.75f;
            render (eng, in, out, 512, [&] (int) { return makeParams (1.0f, 0.5f); });
            check (maxAbsDiff (out, expect, n / 2, n) < 1.0e-6f, "mix 0.5 -> dry*0.5 + wet*0.5 (0.75 x dry)");
        }

        // Output trim (after safety): constant 0.5 input.
        {
            const float dbs[] = { -6.0f, 12.0f, -24.0f, 0.0f };
            for (float db : dbs)
            {
                TensionEngine eng;
                eng.prepare (48000.0, 512, 2);
                const Signal c = makeConst (12000, 0.5f);
                Signal out;
                render (eng, c, out, 512, [&] (int pos) { return makeParams (0.0f, 1.0f, pos < 512 ? 0.0f : db); });

                const float expectedGain = juce::Decibels::decibelsToGain (db);
                const float measured = out.ch[0].back() / 0.5f;

                bool smooth = true;      // transition has no jumps
                for (size_t i = 513; i < out.ch[0].size(); ++i)
                    if (std::abs (out.ch[0][i] - out.ch[0][i - 1]) > 0.01f) smooth = false;

                char name[96];
                std::snprintf (name, sizeof name, "output %+.0f dB -> gain %.6f (smooth, settles)", db, expectedGain);
                check (std::abs (measured - expectedGain) < 1.0e-6f && smooth, name);
            }
            check (std::abs (juce::Decibels::decibelsToGain (-6.0f) - 0.501187f) < 1.0e-6f, "output -6 dB == 0.501187");
        }

        // Output trim applies AFTER safety: +12 dB trim may exceed 0 dBFS intentionally.
        {
            TensionEngine eng;
            eng.prepare (48000.0, 512, 2);
            eng.setSeriesChainEnabledForTesting (false);   // Phase 3.1 identity proc (DC input would be filtered away otherwise)
            const Signal c = makeConst (12000, 2.0f);
            Signal out;
            render (eng, c, out, 512, [&] (int) { return makeParams (1.0f, 1.0f, 12.0f); });
            const float last = out.ch[0].back();
            check (last > 1.0f && last <= 1.0f * juce::Decibels::decibelsToGain (12.0f) + 1.0e-4f,
                   "safety clamps to <= 0 dBFS first, +12 dB trim then lifts it (trim AFTER safety)");
        }

        // No zipper: 100 Hz sine, proc = 0.5x so engagement is audible as a gain move.
        {
            const int len = 72000;
            const Signal sine = makeSine (len, 48000.0, 100.0, 0.5f);

            // Baseline second difference of the plain sine (no processing).
            const float baseline = maxSecondDiff (sine.ch[0]);
            const float bound = 1.0e-3f;

            // A: abrupt tension jump 0 -> 1 at 0.1 s
            {
                TensionEngine eng;
                eng.prepare (48000.0, 512, 2);
                eng.setStubProcGainForTesting (0.5f);
                Signal out;
                render (eng, sine, out, 512, [&] (int pos) { return makeParams (pos < 4608 ? 0.0f : 1.0f); });
                const float m = maxSecondDiff (out.ch[0]);
                std::printf ("  (abrupt jump: max |second difference| %.3e, plain sine %.3e)\n", (double) m, (double) baseline);
                const bool engaged = std::abs (out.ch[0][(size_t) len - 1]) <= 0.5f && std::abs (out.ch[0][(size_t) len - 1] - 0.5f * sine.ch[0][(size_t) len - 1]) < 1.0e-5f;
                check (m < bound && engaged, "no zipper on an abrupt tension jump (and the jump is audible)");
            }

            // B: slow ramp over 1 s, new target every 64-sample host block
            {
                TensionEngine eng;
                eng.prepare (48000.0, 64, 2);
                eng.setStubProcGainForTesting (0.5f);
                Signal out;
                render (eng, sine, out, 64, [&] (int pos)
                {
                    const float t = std::clamp ((float) pos / 48000.0f, 0.0f, 1.0f);
                    return makeParams (t);
                });
                const float m = maxSecondDiff (out.ch[0]);
                std::printf ("  (1 s ramp: max |second difference| %.3e)\n", (double) m);
                check (m < bound, "no zipper while tension ramps (per-block automation)");
            }

            // C: output trim and mix automation without zipper
            {
                TensionEngine eng;
                eng.prepare (48000.0, 512, 2);
                eng.setStubProcGainForTesting (0.5f);
                Signal out;
                render (eng, sine, out, 512, [&] (int pos)
                {
                    return makeParams (1.0f, pos < 24576 ? 1.0f : 0.0f, pos < 36864 ? 0.0f : -12.0f);
                });
                const float m = maxSecondDiff (out.ch[0]);
                std::printf ("  (mix + output steps: max |second difference| %.3e)\n", (double) m);
                check (m < bound, "no zipper on mix and output steps");
            }
        }
    }

    // -----------------------------------------------------------------------
    // T3 - NaN / Inf injection
    // -----------------------------------------------------------------------
    void testNaNInf()
    {
        std::printf ("\n[T3] NaN / Inf injection\n");

        const float nan = std::numeric_limits<float>::quiet_NaN();
        const float inf = std::numeric_limits<float>::infinity();

        for (float t : { 0.0f, 0.7f, 1.0f })
            for (float mix : { 1.0f, 0.6f, 0.0f })
            {
                TensionEngine eng;
                eng.prepare (48000.0, 512, 2);
                eng.setStubProcGainForTesting (0.5f);

                Signal in = makeNoise (8192, 2, 31337u, 0.5f, false);
                in.ch[0][100]  = nan;
                in.ch[0][1000] = inf;
                in.ch[0][2000] = -inf;
                in.ch[1][1500] = nan;
                in.ch[1][3000] = inf;

                Signal out;
                render (eng, in, out, 512, [&] (int) { return makeParams (t, mix); });

                char name[96];
                std::snprintf (name, sizeof name, "finite output with injected NaN/Inf (t %.1f, mix %.1f)", (double) t, (double) mix);
                check (allFinite (out), name);

                if (t == 0.0f && mix == 1.0f)
                {
                    check (out.ch[0][100] == 0.0f && out.ch[0][1000] == 0.0f && out.ch[0][2000] == 0.0f, "scrubbed samples are exactly 0");
                    check (eng.getSafety().getNonFiniteCount() >= 5, "scrub counter counts every event");
                    check (eng.getFeedbackResetCount() >= 1, "scrub event triggers a feedback-module reset at a block boundary");
                    // neighbours untouched
                    check (out.ch[0][101] == in.ch[0][101] && out.ch[1][1501] == in.ch[1][1501], "neighbouring samples unharmed");
                }
            }
    }

    // -----------------------------------------------------------------------
    // T4 - block size invariance
    // -----------------------------------------------------------------------
    void testBlockInvariance()
    {
        std::printf ("\n[T4] block-size invariance\n");

        const int n = 6 * 4096;
        const Signal in = makeNoise (n, 2, 2024u, 0.6f, false);

        auto schedule = [] (bool zero)
        {
            return [zero] (int pos)
            {
                const float t = zero ? 0.0f : (pos < 8192 ? 0.3f : (pos < 16384 ? 0.9f : 0.0f));
                return makeParams (t, 0.7f, -3.0f, true, 1);
            };
        };

        for (bool zero : { true, false })
        {
            Signal ref;
            {
                TensionEngine eng;
                eng.prepare (48000.0, 32, 2);
                eng.setStubProcGainForTesting (0.5f);
                render (eng, in, ref, 32, schedule (zero));
            }

            for (int block : { 512, 4096 })
            {
                TensionEngine eng;
                eng.prepare (48000.0, block, 2);
                eng.setStubProcGainForTesting (0.5f);
                Signal out;
                render (eng, in, out, block, schedule (zero));

                const float d = maxAbsDiff (ref, out, 0, n);
                char name[128];
                std::snprintf (name, sizeof name, "block %4d vs 32 (%s): max diff %.2e", block, zero ? "t = 0, bit-exact" : "t ramps", (double) d);
                check (zero ? bitEqual (ref, out, 0, n) : d <= 1.0e-6f, name);
            }
        }
    }

    // -----------------------------------------------------------------------
    // T5 - allocation guard
    // -----------------------------------------------------------------------
    void testNoAllocation()
    {
        std::printf ("\n[T5] no allocation in process()\n");

        {
            TensionEngine eng;
            eng.prepare (48000.0, 512, 2);

            Signal in = makeNoise (512 * 120, 2, 8u, 1.0f, true);
            in.ch[0][700] = std::numeric_limits<float>::quiet_NaN();
            in.ch[1][4000] = std::numeric_limits<float>::infinity();

            Signal warm;
            render (eng, makeNoise (2048, 2, 3u, 0.5f, false), warm, 512, [&] (int) { return makeParams (0.0f); });

            g_allocCount.store (0);
            Signal out;
            render (eng, in, out, 512, [&] (int pos)
            {
                const float t = 0.5f + 0.5f * std::sin ((float) pos * 0.001f);
                return makeParams (pos % 7 == 0 ? 0.0f : t, 0.8f, -3.0f, (pos & 1) != 0, (pos / 512) % 3);
            }, true);
            const long long count = g_allocCount.load();
            std::printf ("  (engine: %lld allocations during armed process calls)\n", count);
            check (count == 0, "TensionEngine::process is allocation-free");
        }

        {
            TENSIONAudioProcessor proc;
            proc.setPlayConfigDetails (2, 2, 48000.0, 512);
            proc.prepareToPlay (48000.0, 512);

            juce::AudioBuffer<float> buf (2, 512);
            juce::MidiBuffer midi;
            const Signal src = makeNoise (512, 2, 5u, 0.7f, false);

            setParam (proc, TensionParams::tension, 0.6f);
            for (int c = 0; c < 2; ++c)
                std::memcpy (buf.getWritePointer (c), src.ch[c].data(), 512 * sizeof (float));
            proc.processBlock (buf, midi);   // warm-up

            g_allocCount.store (0);
            for (int b = 0; b < 100; ++b)
            {
                for (int c = 0; c < 2; ++c)
                    std::memcpy (buf.getWritePointer (c), src.ch[c].data(), 512 * sizeof (float));

                g_allocArmed.store (true);
                proc.processBlock (buf, midi);
                g_allocArmed.store (false);
            }
            const long long count = g_allocCount.load();
            std::printf ("  (processor: %lld allocations during armed processBlock calls)\n", count);
            check (count == 0, "TENSIONAudioProcessor::processBlock is allocation-free");
        }
    }

    // -----------------------------------------------------------------------
    // T6 - telemetry
    // -----------------------------------------------------------------------
    void testTelemetry()
    {
        std::printf ("\n[T6] telemetry atomics\n");

        TensionEngine eng;
        eng.prepare (48000.0, 512, 2);
        eng.setSeriesChainEnabledForTesting (false);       // bus telemetry against the identity proc
        const Signal sine = makeSine (512 * 40, 48000.0, 220.0, 0.5f);
        Signal out;
        render (eng, sine, out, 512, [&] (int pos) { return makeParams (pos == 0 ? 0.0f : 0.5f); });

        const auto& tel = eng.getTelemetry();
        const float ts = tel.tensionSmoothed.load();
        const float pk = tel.outPeak.load();
        const float rms = tel.wetRms.load();
        std::printf ("  (tension %.3f  outPeak %.3f  wetRms %.3f)\n", (double) ts, (double) pk, (double) rms);

        check (std::abs (ts - 0.5f) < 1.0e-4f, "tensionSmoothed reaches the target");
        check (pk > 0.4f && pk <= 0.55f, "outPeak tracks the output peak");
        check (std::abs (rms - 0.5f / std::sqrt (2.0f)) < 0.03f, "wetRms ~ wet-bus RMS");
        const float ph = tel.lfoPhaseA.load();
        check (tel.noiseLevel.load() == 0.0f && ph >= 0.0f && ph < 1.0f, "noiseLevel is 0 (Phase 3.3), lfoPhaseA published in [0,1)");
    }

    // =======================================================================
    // Phase 3.2 helpers
    // =======================================================================
    constexpr double kPi = 3.14159265358979323846;

    double rmsOf (const std::vector<float>& x, int from, int to)
    {
        double acc = 0.0;
        for (int i = from; i < to; ++i) acc += (double) x[(size_t) i] * (double) x[(size_t) i];
        return std::sqrt(acc / (double) std::max (1, to - from));
    }

    double toDb (double ratio) { return 20.0 * std::log10 (std::max (ratio, 1.0e-30)); }

    float maxSecondDiffFrom (const std::vector<float>& x, int from)
    {
        float m = 0.0f;
        for (size_t i = (size_t) std::max (2, from); i < x.size(); ++i)
            m = std::max (m, std::abs (x[i] - 2.0f * x[i - 1] + x[i - 2]));
        return m;
    }

    // Paul Kellett pink noise, normalised to a target RMS over the whole vector.
    std::vector<float> makePink (int n, std::uint32_t seed, float targetRms)
    {
        Rng rng { seed | 1u };
        float b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0;
        std::vector<float> out ((size_t) n);

        for (int i = 0; i < n; ++i)
        {
            const float w = rng.next();
            b0 = 0.99886f * b0 + w * 0.0555179f;
            b1 = 0.99332f * b1 + w * 0.0750759f;
            b2 = 0.96900f * b2 + w * 0.1538520f;
            b3 = 0.86650f * b3 + w * 0.3104856f;
            b4 = 0.55000f * b4 + w * 0.5329522f;
            b5 = -0.7616f * b5 - w * 0.0168980f;
            out[(size_t) i] = b0 + b1 + b2 + b3 + b4 + b5 + b6 + w * 0.5362f;
            b6 = w * 0.115926f;
        }

        const double r = rmsOf (out, 0, n);
        const float g = (float) ((double) targetRms / std::max (r, 1.0e-12));
        for (auto& v : out) v *= g;
        return out;
    }

    // =======================================================================
    // T7 - stub mapping + LFOEngine
    // =======================================================================
    void testLFO()
    {
        std::printf ("\n[T7] Phase 3.2 stub mapping + LFOEngine\n");

        using namespace TensionDSP;
        const double fs = 48000.0;

        // ---- stub mapping (temporary linear destinations) ----
        {
            const auto a = evaluateTensionMapping (0.0f, RiseMode::Cinematic);
            const auto b = evaluateTensionMapping (1.0f, RiseMode::Cinematic);
            check (a.satDrive == 1.0f && a.satAmount == 0.0f && a.widthFactor == 1.0f && a.lowSideCollapse == 0.0f
                   && a.lfoAmpDepth == 0.0f && a.lfoMoveDepth == 0.0f && a.bpBlend == 0.0f && a.inputTrim == 1.0f,
                   "stub mapping: neutral destinations at t = 0");
            check (std::abs (b.satDrive - 4.2f) < 1e-5f && std::abs (b.widthFactor - 2.1f) < 1e-5f && std::abs (b.hpCutoffHz - 640.0f) < 1e-3f
                   && std::abs (b.bpCenterHz - 9000.0f) < 1e-2f && std::abs (b.bpQ - 3.2f) < 1e-5f && std::abs (b.satAmount - 0.85f) < 1e-5f,
                   "stub mapping: Cinematic end values at t = 1");

            bool mono = true;
            auto prev = evaluateTensionMapping (0.0f, RiseMode::Cinematic);
            for (int i = 1; i <= 200; ++i)
            {
                const auto c = evaluateTensionMapping ((float) i / 200.0f, RiseMode::Cinematic);
                if (c.satDrive < prev.satDrive || c.hpCutoffHz < prev.hpCutoffHz || c.widthFactor < prev.widthFactor
                    || c.lfoRateHz < prev.lfoRateHz || c.lfoCyclesPerBeat < prev.lfoCyclesPerBeat) mono = false;
                prev = c;
            }
            check (mono, "stub mapping: destinations monotone in t");
        }

        // ---- shapes: range, reach, period accuracy (free mode, 1 Hz @ 48 kHz) ----
        const LFOEngine::Shape shapes[] = { LFOEngine::Shape::Sine, LFOEngine::Shape::Triangle, LFOEngine::Shape::Saw, LFOEngine::Shape::ReverseSaw };
        const char* names[] = { "sine", "triangle", "saw", "reverse saw" };

        for (int si = 0; si < 4; ++si)
        {
            LFOEngine l;
            l.prepare (fs);
            l.setShape (shapes[si]);
            l.setTargetRateHz (1.0f);
            l.reset();

            const int n = 3 * 48000;
            std::vector<float> y ((size_t) n);
            for (int i = 0; i < n; ++i) y[(size_t) i] = l.next();

            const float mn = *std::min_element (y.begin(), y.end());
            const float mx = *std::max_element (y.begin(), y.end());

            std::vector<int> marks;
            for (int i = 1; i < n; ++i)
            {
                bool hit = false;
                if (si <= 1)      hit = (y[(size_t) i - 1] < 0.0f && y[(size_t) i] >= 0.0f);
                else if (si == 2) hit = (y[(size_t) i] < y[(size_t) i - 1] - 1.5f);
                else              hit = (y[(size_t) i] > y[(size_t) i - 1] + 1.5f);
                if (hit) marks.push_back (i);
            }

            bool periodOk = marks.size() >= 2;
            for (size_t k = 1; k < marks.size(); ++k)
                if (std::abs (marks[k] - marks[k - 1] - 48000) > 1) periodOk = false;

            char name[128];
            std::snprintf (name, sizeof name, "%s: range within [-1,1], reaches both extremes", names[si]);
            check (mn >= -1.000001f && mx <= 1.000001f && mx > 0.999f && mn < -0.999f, name);
            std::snprintf (name, sizeof name, "%s: 1 Hz @ 48 kHz period = 48000 +/- 1 samples (%d cycles seen)", names[si], (int) marks.size());
            check (periodOk, name);
        }

        // ---- sync rate = bpm-scaled value ----
        {
            check (std::abs (LFOEngine::syncRateHz (120.0, 1.0) - 2.0) < 1e-9, "sync: 120 BPM x 1 cycle/beat = 2 Hz (ladder rung is exact)");
            check (std::abs (LFOEngine::syncRateHz (90.0, 0.5) - 0.75) < 1e-9, "sync: 90 BPM x 1/2 = 0.75 Hz");
            check (std::abs (LFOEngine::syncRateHz (140.0, 4.0) - 140.0 / 60.0 * 4.0) < 1e-9, "sync: 140 BPM x 4 = bpm/60 * 4");
            check (std::abs (LFOEngine::syncRateHz (120.0, 1.5, 0.0) - 3.0) < 1e-12, "sync: snap strength 0 = pure continuous tempo scaling");

            bool mono = true, bounded = true;
            double prev = 0.0;
            for (int i = 0; i <= 4000; ++i)
            {
                const double cpb = 0.125 * std::pow (32.0, (double) i / 4000.0);        // 1/8 .. 4
                const double s = LFOEngine::snapCyclesPerBeat (cpb, LFOEngine::kSnapStrength);
                if (i > 0 && s <= prev) mono = false;
                if (s < 0.125 * 0.999999 || s > 4.0 * 1.000001) bounded = false;
                prev = s;
            }
            check (mono && bounded, "sync: soft-snap is strictly monotone and stays within the 1/8..4 ladder");

            // dwell: the same log-step of the knob moves the rate LESS near a rung than between rungs
            const double near = std::log2 (LFOEngine::snapCyclesPerBeat (1.02, 0.6)) - std::log2 (LFOEngine::snapCyclesPerBeat (0.98, 0.6));
            const double mid  = std::log2 (LFOEngine::snapCyclesPerBeat (1.44, 0.6)) - std::log2 (LFOEngine::snapCyclesPerBeat (1.38, 0.6)) ;
            check (near / (std::log2 (1.02 / 0.98)) < 0.6 && mid / std::log2 (1.44 / 1.38) > 1.2, "sync: ladder dwell (slope < 1 at a rung, > 1 between rungs)");

            LFOEngine l;
            l.prepare (fs);
            l.setShape (LFOEngine::Shape::Saw);
            l.setSyncTarget (90.0, 2.0);                 // 3 Hz -> 16000 samples
            l.reset();
            int prevWrap = -1, lastPeriod = 0;
            float last = l.next();
            for (int i = 1; i < 60000; ++i)
            {
                const float y = l.next();
                if (y < last - 1.5f) { if (prevWrap >= 0) lastPeriod = i - prevWrap; prevWrap = i; }
                last = y;
            }
            check (std::abs (lastPeriod - 16000) <= 1, "sync: LFO at 90 BPM x 2 cycles/beat has a 16000-sample period");
        }

        // ---- phase continuity on a rate change ----
        {
            LFOEngine l;
            l.prepare (fs);
            l.setShape (LFOEngine::Shape::Sine);
            l.setTargetRateHz (1.0f);
            l.reset();

            float maxStep = 0.0f, prevY = l.next();
            auto run = [&] (int samples)
            {
                for (int i = 0; i < samples; i += 32)
                {
                    l.controlStep (32);
                    for (int k = 0; k < 32; ++k)
                    {
                        const float y = l.next();
                        maxStep = std::max (maxStep, std::abs (y - prevY));
                        prevY = y;
                    }
                }
            };
            run (48000);
            l.setTargetRateHz (8.0f);          // 8x jump of the target
            run (96000);

            const double bound = 2.0 * kPi * 8.0 / fs * 1.05;
            std::printf ("  (max |sample step| across the rate change: %.3e, bound %.3e, rate now %.3f Hz)\n", (double) maxStep, bound, l.getCurrentRateHz());
            check ((double) maxStep <= bound && std::abs (l.getCurrentRateHz() - 8.0) < 0.05, "rate change: no phase jump (step bounded by the new rate), slew converges");
        }

        // ---- S&H determinism + slew ----
        {
            auto render = [&] (std::uint32_t seed, int n)
            {
                LFOEngine l;
                l.prepare (fs);
                l.setShape (LFOEngine::Shape::SampleHold);
                l.setSeed (seed);
                l.setTargetRateHz (5.0f);
                l.reset();
                std::vector<float> y ((size_t) n);
                for (int i = 0; i < n; ++i) y[(size_t) i] = l.next();
                return y;
            };

            const auto a = render (0xC0FFEEu, 96000);
            const auto b = render (0xC0FFEEu, 96000);
            const auto c = render (0xBADF00Du, 96000);

            check (std::memcmp (a.data(), b.data(), a.size() * sizeof (float)) == 0, "S&H: same seed -> bit-identical sequence");
            check (std::memcmp (a.data(), c.data(), a.size() * sizeof (float)) != 0, "S&H: different seed -> different sequence");

            float maxStep = 0.0f, mn = 1.0f, mx = -1.0f;
            for (size_t i = 1; i < a.size(); ++i) maxStep = std::max (maxStep, std::abs (a[i] - a[i - 1]));
            for (float v : a) { mn = std::min (mn, v); mx = std::max (mx, v); }
            const double sd = rmsOf (a, 0, (int) a.size());

            check (mn >= -1.0f && mx < 1.0f && sd > 0.2, "S&H: bipolar range, genuinely random plateaus");
            check (maxStep < 0.03f, "S&H: 2 ms slew keeps the sample-to-sample step small (no clicks)");

            // setSeed + reset restores the sequence on the same instance
            LFOEngine l;
            l.prepare (fs);
            l.setShape (LFOEngine::Shape::SampleHold);
            l.setSeed (0xC0FFEEu);
            l.setTargetRateHz (5.0f);
            l.reset();
            for (int i = 0; i < 70000; ++i) (void) l.next();
            l.setSeed (0xC0FFEEu);
            l.reset();
            bool same = true;
            for (size_t i = 0; i < 20000; ++i) if (l.next() != a[i]) same = false;
            check (same, "S&H: setSeed + reset() reproduces the sequence");
        }

        // ---- lfoB configuration: 90 degree offset, 0.75 rate ratio, ppq alignment ----
        {
            LFOEngine b;
            b.prepare (fs);
            b.setShape (LFOEngine::Shape::Sine);
            b.setPhaseOffset (0.25f);
            b.setRateScale (0.75f);
            b.setTargetRateHz (2.0f);
            b.reset();
            check (std::abs (b.value() - 1.0f) < 1e-5f && std::abs (b.getCurrentRateHz() - 1.5) < 1e-9, "lfoB: 90 degree phase offset, rate = 0.75 x 2 Hz");

            LFOEngine a;
            a.prepare (fs);
            a.setShape (LFOEngine::Shape::Sine);
            a.setSyncTarget (120.0, 1.0);
            a.reset();
            a.alignToPpq (2.25);                       // frac(2.25 * 1 cycle/beat) = 0.25 -> sine peak
            check (std::abs (a.value() - 1.0f) < 1e-5f, "transport start: phase re-aligned to frac(ppq * cyclesPerBeat)");

            LFOEngine f;
            f.prepare (fs);
            f.setTargetRateHz (3.0f);
            f.reset();
            for (int i = 0; i < 1000; ++i) (void) f.next();
            const float before = f.getPhase();
            f.alignToPpq (7.3);
            check (f.getPhase() == before, "free mode: ppq alignment is a no-op");
        }
    }

    // =======================================================================
    // T8 - FilterEngine
    // =======================================================================
    void testFilter()
    {
        std::printf ("\n[T8] FilterEngine\n");

        for (double fs : { 44100.0, 48000.0, 192000.0 })
        {
            FilterEngine f;
            f.prepare (fs);

            FilterEngine::Targets hi;
            hi.hpHz = 1.0e9f; hi.bpCenterHz = 1.0e9f; hi.bpQ = 1000.0f; hi.bpBlend = 1.0f; hi.lfoDepthOct = 100.0f;
            FilterEngine::Targets lo;
            lo.hpHz = -5.0f; lo.bpCenterHz = 1.0e-3f; lo.bpQ = 0.0f; lo.bpBlend = 1.0f; lo.lfoDepthOct = 100.0f;

            f.setTargets (hi, 1.0f, true, 32);
            const float ceilingHz = (float) (0.45 * fs) + 0.05f;
            const bool capsHi = f.getAppliedQ() <= 4.0f && f.getAppliedHpHz() <= ceilingHz && f.getAppliedBpHz() <= ceilingHz;
            f.setTargets (lo, -1.0f, true, 32);
            const bool capsLo = f.getAppliedQ() > 0.0f && f.getAppliedHpHz() >= 20.0f && f.getAppliedBpHz() >= 20.0f;

            char name[128];
            std::snprintf (name, sizeof name, "%.0f Hz: Q capped <= 4.0, cutoffs clamped to [20 Hz, 0.45 fs] (incl. LFO modulation)", fs);
            check (capsHi && capsLo, name);

            // noise at the extremes, alternating every 1000 samples with the LFO flipping every period
            Rng rng { 777u };
            bool finite = true;
            float peak = 0.0f;
            const int total = (int) (fs * 4.0);
            std::vector<float> l ((size_t) total), r ((size_t) total);
            for (int i = 0; i < total; ++i) { l[(size_t) i] = rng.next(); r[(size_t) i] = rng.next(); }

            f.reset();
            int period = 0;
            for (int pos = 0; pos < total; pos += 32, ++period)
            {
                const int n = std::min (32, total - pos);
                f.setTargets (((pos / 1000) & 1) ? hi : lo, (period & 1) ? 1.0f : -1.0f, false, 32);
                f.process (l.data() + pos, r.data() + pos, n);
            }
            for (int i = 0; i < total; ++i)
            {
                if (! std::isfinite (l[(size_t) i]) || ! std::isfinite (r[(size_t) i])) finite = false;
                peak = std::max (peak, std::max (std::abs (l[(size_t) i]), std::abs (r[(size_t) i])));
            }
            std::snprintf (name, sizeof name, "%.0f Hz: full-scale noise at extreme settings is finite and bounded (peak %.2f)", fs, (double) peak);
            check (finite && peak < 100.0f, name);
        }

        // ---- click-free cutoff sweep: smoothed vs unsmoothed target jump ----
        {
            const double fs = 48000.0;
            const int n = 72000, jumpAt = 24000;
            const Signal sine = makeSine (n, fs, 100.0, 0.5f);

            auto run = [&] (bool smoothed)
            {
                FilterEngine f;
                f.prepare (fs);
                std::vector<float> l = sine.ch[0], r = sine.ch[1];

                FilterEngine::Targets tg;
                tg.hpHz = 20.0f; tg.bpBlend = 0.0f;
                f.setTargets (tg, 0.0f, true, 32);

                for (int pos = 0; pos < n; pos += 32)
                {
                    tg.hpHz = pos < jumpAt ? 20.0f : 2000.0f;
                    f.setTargets (tg, 0.0f, ! smoothed, 32);
                    f.process (l.data() + pos, r.data() + pos, std::min (32, n - pos));
                }
                return l;
            };

            const auto smooth = run (true);
            const auto hard   = run (false);
            const float ms = maxSecondDiffFrom (smooth, 4800);
            const float mh = maxSecondDiffFrom (hard,   4800);

            std::printf ("  (HP 20 Hz -> 2 kHz on a 100 Hz sine: max |second difference| smoothed %.3e, unsmoothed %.3e)\n", (double) ms, (double) mh);
            check (ms < 0.02f && ms < 0.25f * mh, "cutoff sweep: multiplicative smoothing is click-free (far below the unsmoothed jump)");
            check (std::abs (smooth.back()) < 0.05f * 0.5f + 0.01f, "cutoff sweep: the 2 kHz HP really removed the 100 Hz tone");
        }

        // ---- LFO modulation moves the cutoff by octaves ----
        {
            FilterEngine f;
            f.prepare (48000.0);
            FilterEngine::Targets tg;
            tg.hpHz = 400.0f; tg.bpCenterHz = 1000.0f; tg.lfoDepthOct = 1.0f;
            f.setTargets (tg, 1.0f, true, 32);
            const float up = f.getAppliedHpHz();
            f.setTargets (tg, -1.0f, true, 32);
            const float down = f.getAppliedHpHz();
            check (std::abs (up - 800.0f) < 0.5f && std::abs (down - 200.0f) < 0.5f, "lfoA modulation: cutoff * 2^(depth * lfo) in octaves");
        }
    }

    // =======================================================================
    // T9 - SaturationEngine
    // =======================================================================
    double aliasToFundamentalDb (float drive, float peakAmp)
    {
        const double fs = 48000.0;
        constexpr int order = 14;
        constexpr int N = 1 << order;
        const int k0 = (int) std::lround (5000.0 * (double) N / fs);       // coherent bin
        const double f0 = (double) k0 * fs / (double) N;
        const int settle = 8192;

        SaturationEngine s;
        s.prepare (fs);
        s.setTargets (drive, 1.0f, true);

        std::vector<float> l ((size_t) (settle + N)), r;
        for (size_t i = 0; i < l.size(); ++i)
            l[i] = peakAmp * (float) std::sin (2.0 * kPi * f0 * (double) i / fs);
        r = l;

        for (size_t pos = 0; pos < l.size(); pos += 32)
            s.process (l.data() + pos, r.data() + pos, (int) std::min<size_t> (32, l.size() - pos));

        std::vector<float> data ((size_t) (2 * N), 0.0f);
        for (int i = 0; i < N; ++i)
        {
            const float w = 0.5f - 0.5f * (float) std::cos (2.0 * kPi * (double) i / (double) N);   // Hann
            data[(size_t) i] = l[(size_t) (settle + i)] * w;
        }

        juce::dsp::FFT fft (order);
        fft.performRealOnlyForwardTransform (data.data(), true);

        std::vector<double> power ((size_t) (N / 2 + 1));
        for (int k = 0; k <= N / 2; ++k)
            power[(size_t) k] = (double) data[(size_t) (2 * k)] * data[(size_t) (2 * k)] + (double) data[(size_t) (2 * k + 1)] * data[(size_t) (2 * k + 1)];

        constexpr int guard = 8;
        std::vector<char> excluded ((size_t) (N / 2 + 1), 0);
        for (int k = 0; k <= guard; ++k) excluded[(size_t) k] = 1;                       // DC leakage
        for (int h = 1; h * k0 <= N / 2; ++h)                                            // true harmonics
            for (int k = h * k0 - guard; k <= h * k0 + guard; ++k)
                if (k >= 0 && k <= N / 2) excluded[(size_t) k] = 1;

        double fund = 0.0, alias = 0.0;
        for (int k = k0 - guard; k <= k0 + guard; ++k) fund += power[(size_t) k];
        for (int k = 0; k <= N / 2; ++k) if (! excluded[(size_t) k]) alias += power[(size_t) k];

        return 10.0 * std::log10 (std::max (alias, 1.0e-30) / fund);
    }

    void testSaturation()
    {
        std::printf ("\n[T9] SaturationEngine\n");

        // ---- alias test (5 kHz @ 48 kHz, aliased-bin energy vs fundamental) ----
        {
            // Reference level: -18 dBFS peak (the same reference level as the loudness criterion).
            const double a7  = aliasToFundamentalDb (7.0f, 0.12589254f);
            const double a42 = aliasToFundamentalDb (4.2f, 0.12589254f);
            std::printf ("  (alias energy vs fundamental, -18 dBFS: drive 7.0 %.1f dB, drive 4.2 %.1f dB)\n", a7, a42);
            check (a7 < -40.0 && a42 < -40.0, "5 kHz sine, max drive: aliased-bin energy > 40 dB below the fundamental");

            // Informational only (not asserted): hot levels where first-order ADAA is expected to run out of margin.
            std::printf ("  (informational, hot levels: drive 7 @ -6 dBFS %.1f dB, drive 4.2 @ -12 dBFS %.1f dB)\n",
                         aliasToFundamentalDb (7.0f, 0.5f), aliasToFundamentalDb (4.2f, 0.25f));
        }

        // ---- loudness vs drive at -18 dBFS pink noise ----
        {
            const double fs = 48000.0;
            const int n = 4 * 48000;
            const auto pinkL = makePink (n, 11u, 0.12589254f);
            const auto pinkR = makePink (n, 23u, 0.12589254f);

            const float drives[] = { 1.0f, 1.5f, 2.0f, 3.0f, 4.2f, 5.5f, 7.0f };
            double ref = 0.0, worst = 0.0;

            for (float d : drives)
            {
                SaturationEngine s;
                s.prepare (fs);
                s.setTargets (d, 1.0f, true);
                std::vector<float> l = pinkL, r = pinkR;
                for (int pos = 0; pos < n; pos += 32)
                    s.process (l.data() + pos, r.data() + pos, std::min (32, n - pos));

                const double rms = rmsOf (l, 24000, n);
                if (d == 1.0f) ref = rms;
                worst = std::max (worst, std::abs (toDb (rms / ref)));
            }
            std::printf ("  (worst RMS deviation vs drive 1 over drive 1..7: %.2f dB)\n", worst);
            check (worst < 1.5, "loudness change < 1.5 dB over the drive range (-18 dBFS pink noise, amount 1)");
        }

        // ---- no DC at the output ----
        {
            const double fs = 48000.0;
            const int n = 3 * 48000;
            for (float drive : { 1.0f, 4.2f, 7.0f })
            {
                SaturationEngine s;
                s.prepare (fs);
                s.setTargets (drive, 1.0f, true);
                const Signal sine = makeSine (n, fs, 1000.0, 0.5f);
                std::vector<float> l = sine.ch[0], r = sine.ch[1];
                for (int pos = 0; pos < n; pos += 32)
                    s.process (l.data() + pos, r.data() + pos, std::min (32, n - pos));

                double mean = 0.0;
                for (int i = 2 * 48000; i < n; ++i) mean += l[(size_t) i];
                mean /= 48000.0;

                char name[96];
                std::snprintf (name, sizeof name, "no DC at the output, drive %.1f (mean %.2e < 1e-4)", (double) drive, std::abs (mean));
                check (std::abs (mean) < 1.0e-4, name);
            }
        }

        // ---- parallel amount, finiteness, fallbacks ----
        {
            const Signal noise = makeNoise (48000, 2, 5u, 0.5f, false);

            SaturationEngine s;
            s.prepare (48000.0);
            s.setTargets (7.0f, 0.0f, true);
            std::vector<float> l = noise.ch[0], r = noise.ch[1];
            for (int pos = 0; pos < 48000; pos += 32)
                s.process (l.data() + pos, r.data() + pos, 32);
            check (std::memcmp (l.data(), noise.ch[0].data(), l.size() * sizeof (float)) == 0
                   && std::memcmp (r.data(), noise.ch[1].data(), r.size() * sizeof (float)) == 0,
                   "amount 0 returns the input exactly");

            // |x| huge (fast path |u| > 9), constant DC (|dx| < 1e-5 fallback), tiny steps
            SaturationEngine h;
            h.prepare (48000.0);
            h.setTargets (8.0f, 1.0f, true);
            std::vector<float> a (4096), b (4096);
            Rng rng { 99u };
            for (size_t i = 0; i < a.size(); ++i)
            {
                if (i < 1024)      a[i] = rng.next() * 1.0e6f;        // fast path
                else if (i < 2048) a[i] = 0.3f;                       // constant: fallback branch
                else if (i < 3072) a[i] = 0.3f + 1.0e-6f * (float) (i & 7);   // sub-threshold deltas
                else               a[i] = rng.next() * 0.2f;
                b[i] = a[i];
            }
            for (size_t pos = 0; pos < a.size(); pos += 32)
                h.process (a.data() + pos, b.data() + pos, 32);

            bool finite = true;
            for (size_t i = 0; i < a.size(); ++i) if (! std::isfinite (a[i]) || ! std::isfinite (b[i])) finite = false;
            check (finite, "ADAA stays finite on huge inputs, constant input and sub-1e-5 deltas");

            // even harmonics from the bias: 0.2-peak 1 kHz sine at drive 4.2 has energy at 2 kHz
            SaturationEngine e;
            e.prepare (48000.0);
            e.setTargets (4.2f, 1.0f, true);
            const Signal sine = makeSine (48000, 48000.0, 1000.0, 0.2f);
            std::vector<float> el = sine.ch[0], er = sine.ch[1];
            for (int pos = 0; pos < 48000; pos += 32)
                e.process (el.data() + pos, er.data() + pos, 32);
            double c2 = 0.0, s2 = 0.0;
            for (int i = 24000; i < 48000; ++i)
            {
                c2 += (double) el[(size_t) i] * std::cos (2.0 * kPi * 2000.0 * (double) i / 48000.0);
                s2 += (double) el[(size_t) i] * std::sin (2.0 * kPi * 2000.0 * (double) i / 48000.0);
            }
            const double h2 = 2.0 * std::sqrt (c2 * c2 + s2 * s2) / 24000.0;
            check (h2 > 1.0e-3, "asymmetric bias produces a 2nd harmonic (even-harmonic warmth)");
        }
    }

    // =======================================================================
    // T10 - StereoWidth
    // =======================================================================
    void testWidth()
    {
        std::printf ("\n[T10] StereoWidth\n");

        const double fs = 48000.0;
        const int skip = 9600, win = 38400;                 // win = integer number of cycles for every test frequency
        const int total = skip + win;

        auto renderStereo = [&] (StereoWidth& w, std::vector<float>& l, std::vector<float>& r, const float* lfo)
        {
            for (int pos = 0; pos < total; pos += 32)
                w.process (l.data() + pos, r.data() + pos, std::min (32, total - pos), lfo != nullptr ? lfo + pos : nullptr);
        };

        // ---- LR4 recombination flat at width 1 (per channel, all-pass) ----
        {
            double worst = 0.0;
            for (double f : { 40.0, 150.0, 600.0, 3000.0, 12000.0 })
            {
                StereoWidth w;
                w.prepare (fs);
                w.setTargets (1.0f, 0.0f, 0.0f, true);

                std::vector<float> l ((size_t) total), r ((size_t) total);
                for (int i = 0; i < total; ++i)
                {
                    l[(size_t) i] = 0.5f * (float) std::sin (2.0 * kPi * f * (double) i / fs);
                    r[(size_t) i] = 0.4f * (float) std::sin (2.0 * kPi * f * (double) i / fs + 0.9);     // different level + phase
                }
                const auto l0 = l, r0 = r;
                renderStereo (w, l, r, nullptr);

                worst = std::max (worst, std::abs (toDb (rmsOf (l, skip, total) / rmsOf (l0, skip, total))));
                worst = std::max (worst, std::abs (toDb (rmsOf (r, skip, total) / rmsOf (r0, skip, total))));
            }
            std::printf ("  (width 1: worst |out/in| deviation %.4f dB)\n", worst);
            check (worst < 0.05, "LR4 recombination is flat at width 1 (|out/in| within 0.05 dB, 40 Hz .. 12 kHz)");
        }

        // ---- mid never attenuated by more than 1 dB ----
        {
            double lowest = 0.0, highestHF = -100.0;
            for (float width : { 1.5f, 2.1f, 2.4f })
                for (double f : { 60.0, 300.0, 1000.0, 5000.0, 12000.0 })
                {
                    StereoWidth w;
                    w.prepare (fs);
                    w.setTargets (width, 0.8f, 0.0f, true);

                    std::vector<float> l ((size_t) total), r;
                    for (int i = 0; i < total; ++i)
                        l[(size_t) i] = 0.5f * (float) std::sin (2.0 * kPi * f * (double) i / fs);
                    r = l;                                                     // pure mid
                    const auto l0 = l;
                    renderStereo (w, l, r, nullptr);

                    const double db = toDb (rmsOf (l, skip, total) / rmsOf (l0, skip, total));
                    lowest = std::min (lowest, db);
                    if (f >= 1000.0) highestHF = std::max (highestHF, db);
                }
            std::printf ("  (mono input, widths 1.5..2.4: lowest gain %.3f dB)\n", lowest);
            check (lowest >= -1.001, "mid (mono) content never attenuated by more than 1 dB at any width");
            check (highestHF <= 0.01, "mid compensation never boosts");
        }

        // ---- mono-sum loss at max width (with lfoB sway + rotation active) ----
        {
            const int n = 96000;
            Rng ra { 11u }, rb { 22u }, rc { 33u };
            std::vector<float> l ((size_t) n), r ((size_t) n), lfo ((size_t) n);

            LFOEngine lfoB;
            lfoB.prepare (fs);
            lfoB.setShape (LFOEngine::Shape::Sine);
            lfoB.setTargetRateHz (1.5f);
            lfoB.reset();

            for (int i = 0; i < n; ++i)
            {
                const float c = rc.next(), a = ra.next(), b = rb.next();
                l[(size_t) i] = 0.5f * (c + 0.7f * a);
                r[(size_t) i] = 0.5f * (c + 0.7f * b);
                lfo[(size_t) i] = lfoB.next();
            }

            double mIn = 0.0, sIn = 0.0;
            for (int i = 24000; i < n; ++i)
            {
                const double m = 0.5 * ((double) l[(size_t) i] + r[(size_t) i]), s = 0.5 * ((double) l[(size_t) i] - r[(size_t) i]);
                mIn += m * m; sIn += s * s;
            }

            StereoWidth w;
            w.prepare (fs);
            w.setTargets (2.4f, 0.8f, 0.5f, true);
            for (int pos = 0; pos < n; pos += 32)
                w.process (l.data() + pos, r.data() + pos, std::min (32, n - pos), lfo.data() + pos);

            double mOut = 0.0, sOut = 0.0;
            bool finite = true;
            for (int i = 24000; i < n; ++i)
            {
                if (! std::isfinite (l[(size_t) i]) || ! std::isfinite (r[(size_t) i])) finite = false;
                const double m = 0.5 * ((double) l[(size_t) i] + r[(size_t) i]), s = 0.5 * ((double) l[(size_t) i] - r[(size_t) i]);
                mOut += m * m; sOut += s * s;
            }

            const double lossDb = 10.0 * std::log10 (mIn / mOut);
            std::printf ("  (max width 2.4 + sway/rotation: mono-sum loss %.2f dB, side gain %.2f dB)\n", lossDb, 10.0 * std::log10 (sOut / sIn));
            check (finite && lossDb <= 3.0, "mono-sum energy loss <= 3 dB at max width (lfoB sway + rotation on)");
        }

        // ---- widening actually widens (uncorrelated noise) ----
        {
            const int n = 96000;
            Rng ra { 5u }, rb { 6u };
            std::vector<float> l ((size_t) n), r ((size_t) n);
            for (int i = 0; i < n; ++i) { l[(size_t) i] = 0.4f * ra.next(); r[(size_t) i] = 0.4f * rb.next(); }

            double sIn = 0.0;
            for (int i = 24000; i < n; ++i) { const double s = 0.5 * ((double) l[(size_t) i] - r[(size_t) i]); sIn += s * s; }

            StereoWidth w;
            w.prepare (fs);
            w.setTargets (2.1f, 0.0f, 0.0f, true);
            for (int pos = 0; pos < n; pos += 32)
                w.process (l.data() + pos, r.data() + pos, std::min (32, n - pos), nullptr);

            double sOut = 0.0;
            for (int i = 24000; i < n; ++i) { const double s = 0.5 * ((double) l[(size_t) i] - r[(size_t) i]); sOut += s * s; }
            check (sOut / sIn > 3.0, "width 2.1 expands the side channel (~ w^2 in energy)");
        }

        // ---- low-band side collapse ----
        {
            StereoWidth w;
            w.prepare (fs);
            w.setTargets (1.0f, 1.0f, 0.0f, true);

            std::vector<float> l ((size_t) total), r ((size_t) total);
            for (int i = 0; i < total; ++i)
            {
                l[(size_t) i] = 0.5f * (float) std::sin (2.0 * kPi * 60.0 * (double) i / fs);
                r[(size_t) i] = -l[(size_t) i];                               // pure side at 60 Hz
            }
            const auto l0 = l;
            renderStereo (w, l, r, nullptr);
            const double ratio = rmsOf (l, skip, total) / rmsOf (l0, skip, total);
            std::printf ("  (60 Hz pure side, k = 1: gain %.3f, expected ~0.2)\n", ratio);
            check (ratio > 0.12 && ratio < 0.35, "lows: side collapses by (1 - 0.8k) while the widener is bypassed");
        }
    }

    // =======================================================================
    // T11 - real chain in circuit
    // =======================================================================
    void testRealChain()
    {
        std::printf ("\n[T11] real series chain in-circuit\n");

        // ---- block-size invariance with the real modules (ramps + sync on/off, absolute control grid) ----
        {
            const int n = 6 * 4096;
            const Signal in = makeNoise (n, 2, 2025u, 0.5f, false);

            for (bool sync : { true, false })
            {
                auto schedule = [sync] (int pos)
                {
                    const float t = pos < 8192 ? 0.3f : (pos < 16384 ? 0.9f : 0.0f);
                    return makeParams (t, 0.7f, -3.0f, sync, 2);
                };

                Signal ref;
                {
                    TensionEngine eng;
                    eng.prepare (48000.0, 32, 2);
                    render (eng, in, ref, 32, schedule);
                }

                for (int block : { 64, 512, 4096 })
                {
                    TensionEngine eng;
                    eng.prepare (48000.0, block, 2);
                    Signal out;
                    render (eng, in, out, block, schedule);

                    const float d = maxAbsDiff (ref, out, 0, n);
                    char name[128];
                    std::snprintf (name, sizeof name, "real chain, block %4d vs 32 (sync %d): max diff %.2e", block, sync ? 1 : 0, (double) d);
                    check (d <= 1.0e-6f && allFinite (out), name);
                }
            }
        }

        // ---- the chain actually does something, stays finite and below the safety ceiling ----
        {
            TensionEngine eng;
            eng.prepare (48000.0, 512, 2);
            const Signal in = makeNoise (48000, 2, 4711u, 0.5f, false);
            Signal out;
            render (eng, in, out, 512, [&] (int) { return makeParams (1.0f, 1.0f, 0.0f, true, 1); });

            float peak = 0.0f;
            for (int c = 0; c < 2; ++c) for (float v : out.ch[c]) peak = std::max (peak, std::abs (v));
            const float d = maxAbsDiff (in, out, 24000, 48000);
            std::printf ("  (t = 1: peak %.3f, max |out - in| %.3f)\n", (double) peak, (double) d);
            check (allFinite (out) && d > 0.05f && peak <= 1.0001f, "t = 1: processed (differs from input), finite, <= 0 dBFS after the safety knee");
        }

        // ---- NaN / Inf with the real modules: finite output, no poisoned state ----
        {
            const float nan = std::numeric_limits<float>::quiet_NaN();
            const float inf = std::numeric_limits<float>::infinity();

            for (float t : { 0.4f, 1.0f })
            {
                TensionEngine eng;
                eng.prepare (48000.0, 512, 2);

                Signal in = makeNoise (16384, 2, 2718u, 0.4f, false);
                in.ch[0][100] = nan; in.ch[0][1000] = inf; in.ch[1][1500] = -inf; in.ch[1][3000] = nan;

                Signal out;
                render (eng, in, out, 512, [&] (int) { return makeParams (t, 1.0f, 0.0f, true, 1); });

                const double tailRms = rmsOf (out.ch[0], 12000, 16384);
                char name[128];
                std::snprintf (name, sizeof name, "NaN/Inf into the real chain (t %.1f): finite, recovers (tail RMS %.3f)", (double) t, tailRms);
                check (allFinite (out) && tailRms > 0.02, name);
            }
        }

        // ---- mono host through the real chain ----
        {
            TensionEngine eng;
            eng.prepare (48000.0, 512, 2);
            const Signal in = makeNoise (24000, 1, 31u, 0.4f, false);
            Signal out;
            render (eng, in, out, 512, [&] (int) { return makeParams (0.8f, 1.0f, 0.0f, true, 1); });
            check (allFinite (out) && maxAbsDiff (in, out, 12000, 24000) > 0.02f, "mono host: processed and finite");
        }

        // ---- transport start: sync LFO alignment path, finite, phase published ----
        {
            TensionEngine eng;
            eng.prepare (48000.0, 512, 2);

            juce::AudioBuffer<float> buf (2, 512);
            bool ok = true;
            HostTransport tr;
            tr.bpm = 140.0; tr.hasBpm = true; tr.ppq = 8.37; tr.hasPpq = true; tr.isPlaying = true;

            for (int b = 0; b < 40; ++b)
            {
                const Signal src = makeNoise (512, 2, 900u + (std::uint32_t) b, 0.4f, false);
                for (int c = 0; c < 2; ++c)
                    std::memcpy (buf.getWritePointer (c), src.ch[c].data(), 512 * sizeof (float));

                eng.process (buf, makeParams (0.7f, 1.0f, 0.0f, true, 0), tr);
                tr.ppq += 512.0 / 48000.0 * 140.0 / 60.0;

                for (int c = 0; c < 2; ++c)
                    for (int i = 0; i < 512; ++i)
                        if (! std::isfinite (buf.getReadPointer (c)[i])) ok = false;
            }
            const float ph = eng.getTelemetry().lfoPhaseA.load();
            check (ok && ph >= 0.0f && ph < 1.0f, "playing transport with host BPM/ppq: finite, lfoPhaseA in [0,1)");
        }
    }

    // =======================================================================
    // Phase 3.3 helpers
    // =======================================================================
    std::uint64_t fnv1a (const float* data, size_t count, std::uint64_t h = 1469598103934665603ull)
    {
        for (size_t i = 0; i < count; ++i)
        {
            std::uint32_t bits = 0;
            std::memcpy (&bits, &data[i], sizeof bits);

            for (int b = 0; b < 4; ++b)
            {
                h ^= (std::uint64_t) ((bits >> (8 * b)) & 0xFFu);
                h *= 1099511628211ull;
            }
        }
        return h;
    }

    NoiseGenerator::Targets noiseTargetsAt (float t)
    {
        const auto tg = TensionDSP::evaluateTensionMapping (t, TensionDSP::RiseMode::Cinematic);
        NoiseGenerator::Targets nt;
        nt.gain        = tg.noiseGain;
        nt.brightness  = tg.noiseBrightness;
        nt.hpfHz       = tg.noiseHpfHz;
        nt.topHz       = tg.noiseTopHz;
        nt.filteredAmt = tg.noiseFilteredAmt;
        nt.tremDepth   = 0.0f;
        return nt;
    }

    // n must be a multiple of 32. lfo == 1 (no tremolo).
    void renderNoise (float t, int n, std::vector<float>& l, std::vector<float>& r, std::uint32_t seed = 0x5EED1234u)
    {
        NoiseGenerator ng;
        ng.prepare (48000.0);
        ng.setSeed (seed);
        ng.reset();

        const auto nt = noiseTargetsAt (t);
        const std::vector<float> ones (32, 1.0f);
        l.assign ((size_t) n, 0.0f);
        r.assign ((size_t) n, 0.0f);

        for (int pos = 0; pos < n; pos += 32)
        {
            ng.setTargets (nt, pos == 0, 32);
            ng.process (l.data() + pos, r.data() + pos, 32, ones.data());
        }
    }

    double spectralCentroidHz (const std::vector<float>& x, double fs)
    {
        constexpr int order = 14;
        constexpr int N = 1 << order;
        std::vector<float> data ((size_t) (2 * N), 0.0f);

        for (int i = 0; i < N; ++i)
        {
            const float w = 0.5f - 0.5f * (float) std::cos (2.0 * kPi * (double) i / (double) N);
            data[(size_t) i] = x[(size_t) i] * w;
        }

        juce::dsp::FFT fft (order);
        fft.performRealOnlyForwardTransform (data.data(), true);

        double num = 0.0, den = 0.0;
        for (int k = 1; k <= N / 2; ++k)
        {
            const double re = data[(size_t) (2 * k)], im = data[(size_t) (2 * k + 1)];
            const double mag = std::sqrt (re * re + im * im);
            num += mag * ((double) k * fs / (double) N);
            den += mag;
        }
        return num / std::max (den, 1.0e-30);
    }

    float u01 (Rng& r) { return 0.5f * (r.next() + 1.0f); }

    // =======================================================================
    // T12 - NoiseGenerator
    // =======================================================================
    void testNoise()
    {
        std::printf ("\n[T12] NoiseGenerator\n");
        const double fs = 48000.0;

        // ---- stub mapping (Phase 3.3 destinations) ----
        {
            using namespace TensionDSP;
            const auto z = evaluateTensionMapping (0.0f, RiseMode::Cinematic);
            check (z.noiseGain == 0.0f && z.reverbReturn == 0.0f && z.delayReturn == 0.0f && z.reverbSend == 0.0f
                   && z.delaySend == 0.0f && z.delayFeedback == 0.0f, "stub mapping 3.3: noise / returns / sends / feedback exactly 0 at t = 0");

            bool mono = true;
            float prevN = 0.0f, prevR = 0.0f, prevD = 0.0f;
            bool capOk = true;
            for (int i = 0; i <= 1000; ++i)
            {
                const auto c = evaluateTensionMapping ((float) i / 1000.0f, RiseMode::Aggressive);
                if (c.noiseGain < prevN || c.reverbReturn < prevR || c.delayReturn < prevD) mono = false;
                if (c.delayFeedback > DelayEngine::kHardFeedbackCap) capOk = false;
                prevN = c.noiseGain; prevR = c.reverbReturn; prevD = c.delayReturn;
            }
            check (mono && capOk, "stub mapping 3.3: noise gain / returns monotone in t, feedback below the 0.85 cap");

            const auto lowT = evaluateTensionMapping (0.04f, RiseMode::Cinematic);
            const auto mid  = evaluateTensionMapping (0.25f, RiseMode::Cinematic);
            check (lowT.noiseGain == 0.0f && mid.noiseGain > 0.0f, "stub mapping 3.3: noise gate opens above 4% and has emerged by 25%");
        }

        // ---- xorshift32 anchors (Marsaglia: seed 1 -> 270369, 67634689) ----
        {
            std::uint32_t s = 1u;
            const std::uint32_t a = NoiseGenerator::xorshift32 (s);
            const std::uint32_t b = NoiseGenerator::xorshift32 (s);
            check (a == 270369u && b == 67634689u, "xorshift32: seed 1 gives 270369, 67634689");
        }

        // ---- golden hash of the first 4096 raw samples (fixed seed) vs an independent reference ----
        const std::uint32_t seed = 0xC0DE1234u;
        {
            std::vector<float> gl (4096), gr (4096), rl (4096), rr (4096);
            {
                NoiseGenerator g;
                g.prepare (fs);
                g.setSeed (seed);
                g.reset();
                g.renderRawWhiteForTesting (gl.data(), gr.data(), 4096);
            }
            {
                std::uint32_t a = seed, b = seed ^ 0x9E3779B9u;
                for (size_t i = 0; i < 4096; ++i)
                {
                    a ^= a << 13; a ^= a >> 17; a ^= a << 5;
                    b ^= b << 13; b ^= b >> 17; b ^= b << 5;
                    rl[i] = (float) (a >> 8) / 8388608.0f - 1.0f;
                    rr[i] = (float) (b >> 8) / 8388608.0f - 1.0f;
                }
            }
            const std::uint64_t hGen = fnv1a (gr.data(), 4096, fnv1a (gl.data(), 4096));
            const std::uint64_t hRef = fnv1a (rr.data(), 4096, fnv1a (rl.data(), 4096));
            std::printf ("  (golden hash, seed 0x%08X, 4096 samples/ch: %016llx)\n", (unsigned) seed, (unsigned long long) hGen);
            check (hGen == hRef, "golden hash: first 4096 samples per channel match the independent xorshift32 reference");

            float mn = 1.0f, mx = -1.0f;
            double dot = 0.0, el = 0.0, er = 0.0;
            for (size_t i = 0; i < 4096; ++i)
            {
                mn = std::min (mn, std::min (gl[i], gr[i]));
                mx = std::max (mx, std::max (gl[i], gr[i]));
                dot += (double) gl[i] * gr[i]; el += (double) gl[i] * gl[i]; er += (double) gr[i] * gr[i];
            }
            check (mn >= -1.0f && mx < 1.0f, "white: uniform range [-1, 1)");
            check (std::abs (dot / std::sqrt (el * er)) < 0.05, "L / R streams are decorrelated (seedR = S ^ 0x9E3779B9)");
        }

        // ---- determinism: setSeed + reset restore a bit-identical stream (raw and processed) ----
        {
            NoiseGenerator g;
            g.prepare (fs);
            g.setSeed (seed);
            g.reset();
            std::vector<float> a (4096), ar (4096), b (4096), br (4096), junk (5000), junkR (5000);
            g.renderRawWhiteForTesting (a.data(), ar.data(), 4096);
            g.renderRawWhiteForTesting (junk.data(), junkR.data(), 5000);
            g.setSeed (seed);
            g.reset();
            g.renderRawWhiteForTesting (b.data(), br.data(), 4096);
            check (std::memcmp (a.data(), b.data(), 4096 * sizeof (float)) == 0 && std::memcmp (ar.data(), br.data(), 4096 * sizeof (float)) == 0,
                   "setSeed + reset() reproduces the raw stream bit-identically");

            std::vector<float> p1l, p1r, p2l, p2r, p3l, p3r;
            renderNoise (0.7f, 4096, p1l, p1r, seed);
            renderNoise (0.7f, 4096, p2l, p2r, seed);
            renderNoise (0.7f, 4096, p3l, p3r, seed + 1u);
            check (std::memcmp (p1l.data(), p2l.data(), 4096 * sizeof (float)) == 0 && std::memcmp (p1r.data(), p2r.data(), 4096 * sizeof (float)) == 0,
                   "processed noise is bit-identical for the same seed");
            check (std::memcmp (p1l.data(), p3l.data(), 4096 * sizeof (float)) != 0, "a different seed gives different noise");
        }

        // ---- RNG advances at gain 0: output after a gain-0 history equals the always-on output ----
        {
            NoiseGenerator A, B;
            for (auto* g : { &A, &B }) { g->prepare (fs); g->setSeed (seed); g->reset(); }

            auto nt = noiseTargetsAt (0.7f);
            auto nt0 = nt;
            nt0.gain = 0.0f;
            const std::vector<float> ones (32, 1.0f);
            std::vector<float> al (32), ar (32), bl (32), br (32);
            bool same = true;

            for (int c = 0; c < 64; ++c)
            {
                A.setTargets (c < 30 ? nt0 : nt, c <= 30, 32);
                B.setTargets (nt, c == 0, 32);
                A.process (al.data(), ar.data(), 32, ones.data());
                B.process (bl.data(), br.data(), 32, ones.data());

                if (c >= 30 && (std::memcmp (al.data(), bl.data(), 32 * sizeof (float)) != 0 || std::memcmp (ar.data(), br.data(), 32 * sizeof (float)) != 0))
                    same = false;
            }
            check (same, "RNG advances at gain 0: noise after a silent history is bit-identical to the always-on stream");
        }

        // ---- exact silence at t = 0; RMS monotone; centroid rises ----
        {
            std::vector<float> l, r;
            renderNoise (0.0f, 48000, l, r);
            bool zero = true;
            for (size_t i = 0; i < l.size(); ++i) if (l[i] != 0.0f || r[i] != 0.0f) zero = false;
            check (zero, "t = 0: noise output is EXACTLY zero for 1 s");

            double prev = 0.0;
            bool mono = true;
            std::printf ("  (noise RMS by tension:");
            for (int k = 1; k <= 10; ++k)
            {
                renderNoise ((float) k / 10.0f, 32768, l, r);
                const double rms = rmsOf (l, 16384, 32768);
                std::printf (" %.1f:%.2e", (double) k / 10.0, rms);
                if (rms <= prev) mono = false;
                prev = rms;
            }
            std::printf (")\n");
            check (mono && prev > 0.1, "noise RMS strictly monotone in tension (0.1 .. 1.0), loud at t = 1");

            std::vector<float> l3, r3, l6, r6, l10, r10;
            renderNoise (0.3f, 16384, l3, r3);
            renderNoise (0.6f, 16384, l6, r6);
            renderNoise (1.0f, 16384, l10, r10);
            const double c3 = spectralCentroidHz (l3, fs), c6 = spectralCentroidHz (l6, fs), c10 = spectralCentroidHz (l10, fs);
            std::printf ("  (spectral centroid: t 0.3 -> %.0f Hz, 0.6 -> %.0f Hz, 1.0 -> %.0f Hz)\n", c3, c6, c10);
            check (c3 < c6 && c6 < c10, "spectral centroid rises with tension");
        }

        // ---- engine level: silent input + tension ramp produces noise, exact silence at t = 0 ----
        {
            TensionEngine eng;
            eng.prepare (fs, 512, 2);
            const Signal in = makeConst (6 * 48000, 0.0f);
            Signal out;
            render (eng, in, out, 512, [&] (int pos)
            {
                const float t = pos < 48000 ? 0.0f : std::min (1.0f, (float) (pos - 48000) / (3.0f * 48000.0f));
                return makeParams (t, 1.0f, 0.0f, true, 1);
            });

            bool silent = true;
            for (int c = 0; c < 2; ++c)
                for (int i = 0; i < 40000; ++i)
                    if (out.ch[c][(size_t) i] != 0.0f) silent = false;
            check (silent, "engine: silent input at t = 0 gives all-zero output");

            const double a = rmsOf (out.ch[0], 96000, 115200), b = rmsOf (out.ch[0], 134400, 153600);
            const double c = rmsOf (out.ch[0], 163200, 182400), d = rmsOf (out.ch[0], 211200, 230400);
            std::printf ("  (engine silent-input ramp RMS: %.2e  %.2e  %.2e  %.2e)\n", a, b, c, d);
            check (allFinite (out) && a > 1.0e-5 && a < b && b < c && c < d, "engine: silent input + tension ramp generates noise, level rises monotonically");

            // stereo decorrelation survives the engine
            double dot = 0.0, el = 0.0, er = 0.0;
            for (int i = 211200; i < 230400; ++i) { dot += (double) out.ch[0][(size_t) i] * out.ch[1][(size_t) i]; el += (double) out.ch[0][(size_t) i] * out.ch[0][(size_t) i]; er += (double) out.ch[1][(size_t) i] * out.ch[1][(size_t) i]; }
            check (std::abs (dot / std::sqrt (el * er)) < 0.5, "engine: noise is decorrelated L/R at t = 1");
            check (eng.getTelemetry().noiseLevel.load() > 0.01f, "telemetry: noiseLevel published (Phase 3.3)");
        }
    }

    // =======================================================================
    // T13 - ReverbEngine
    // =======================================================================
    template <typename TargetsFn>
    void runReverb (const std::vector<float>& inL, const std::vector<float>& inR, TargetsFn targetsAt, std::vector<float>& oL, std::vector<float>& oR)
    {
        ReverbEngine rv;
        rv.prepare (48000.0);
        const int n = (int) inL.size();
        oL.assign ((size_t) n, 0.0f);
        oR.assign ((size_t) n, 0.0f);

        for (int pos = 0; pos < n; pos += 32)
        {
            rv.setTargets (targetsAt (pos), pos == 0, 32);
            rv.process (inL.data() + pos, inR.data() + pos, oL.data() + pos, oR.data() + pos, 32);
        }
    }

    void testReverb()
    {
        std::printf ("\n[T13] ReverbEngine\n");
        const double fs = 48000.0;

        // ---- tail rings out after the send / return targets drop to 0 (no gating) ----
        {
            const int n = 3 * 48000;
            std::vector<float> inL ((size_t) n, 0.0f), inR ((size_t) n, 0.0f), oL, oR;
            Rng rng { 4242u };
            for (int i = 0; i < 24000; ++i) { inL[(size_t) i] = 0.5f * rng.next(); inR[(size_t) i] = 0.5f * rng.next(); }

            runReverb (inL, inR, [] (int pos)
            {
                ReverbEngine::Targets t;
                t.roomSize = 0.9f; t.damping = 0.3f; t.width = 1.0f;
                t.sendGain = pos < 24000 ? 1.0f : 0.0f;
                t.returnGain = pos < 24000 ? 0.5f : 0.0f;
                return t;
            }, oL, oR);

            const double burst = rmsOf (oL, 12000, 24000);
            const double tail1 = rmsOf (oL, 24000 + 4800, 24000 + 9600);
            const double tail2 = rmsOf (oL, 24000 + 48000, 24000 + 52800);
            std::printf ("  (burst RMS %.3e, tail +0.1..0.2 s %.3e, tail +1.0..1.1 s %.3e)\n", burst, tail1, tail2);
            check (allFinite ([&] { Signal s; s.numCh = 2; s.ch[0] = oL; s.ch[1] = oR; return s; }()), "reverb output finite");
            check (tail1 > 0.1 * burst && tail1 > 1.0e-4, "tail rings out after send / return drop to 0 (not gated by the smoothers)");
            check (tail2 > 1.0e-5 && tail2 < tail1, "tail keeps evolving and decays (reverb instance is always processed)");
        }

        // ---- RT time scales with roomSize (impulse, energy decay to -40 dB) ----
        {
            const int n = 10 * 48000;
            std::vector<float> inL ((size_t) n, 0.0f), inR ((size_t) n, 0.0f), oL, oR;
            inL[0] = inR[0] = 1.0f;

            double t40[3] = {};
            const float rooms[3] = { 0.3f, 0.6f, 0.9f };

            for (int k = 0; k < 3; ++k)
            {
                runReverb (inL, inR, [&] (int)
                {
                    ReverbEngine::Targets t;
                    t.sendGain = 1.0f; t.returnGain = 1.0f; t.roomSize = rooms[k]; t.damping = 0.5f; t.width = 1.0f;
                    return t;
                }, oL, oR);

                constexpr int W = 2048;
                const int nw = n / W;
                std::vector<double> e ((size_t) nw, 0.0);
                double peak = 0.0;
                for (int w = 0; w < nw; ++w)
                {
                    for (int i = w * W; i < (w + 1) * W; ++i) e[(size_t) w] += (double) oL[(size_t) i] * oL[(size_t) i] + (double) oR[(size_t) i] * oR[(size_t) i];
                    peak = std::max (peak, e[(size_t) w]);
                }
                int last = 0;
                for (int w = 0; w < nw; ++w) if (e[(size_t) w] > peak * 1.0e-4) last = w;
                t40[k] = (double) (last + 1) * W / fs;
            }
            std::printf ("  (T40 by roomSize 0.3 / 0.6 / 0.9: %.2f s / %.2f s / %.2f s)\n", t40[0], t40[1], t40[2]);
            check (t40[0] < t40[1] && t40[1] < t40[2] && t40[2] < 9.5, "RT time scales monotonically with roomSize");
        }

        // ---- finite after random parameter sweeps ----
        {
            const int n = 10 * 48000;
            Rng rng { 777u }, rp { 91u };
            std::vector<float> inL ((size_t) n), inR ((size_t) n), oL, oR;
            for (int i = 0; i < n; ++i) { inL[(size_t) i] = 0.9f * rng.next(); inR[(size_t) i] = 0.9f * rng.next(); }

            runReverb (inL, inR, [&] (int)
            {
                ReverbEngine::Targets t;
                t.sendGain = u01 (rp); t.returnGain = u01 (rp);
                t.roomSize = u01 (rp); t.damping = u01 (rp); t.width = u01 (rp);
                t.preDelayMs = 80.0f * u01 (rp);
                t.returnLpfHz = 500.0f + 19000.0f * u01 (rp);
                t.sendHpfHz = 20.0f + 380.0f * u01 (rp);
                return t;
            }, oL, oR);

            float peak = 0.0f;
            bool fin = true;
            for (size_t i = 0; i < oL.size(); ++i)
            {
                if (! std::isfinite (oL[i]) || ! std::isfinite (oR[i])) fin = false;
                peak = std::max (peak, std::max (std::abs (oL[i]), std::abs (oR[i])));
            }
            std::printf ("  (random sweep: peak %.2f)\n", (double) peak);
            check (fin && peak < 50.0f, "finite and bounded after 10 s of random parameter sweeps (all ranges, every control tick)");
        }

        // ---- engine level: tail rings after tension drops ----
        {
            TensionEngine eng;
            eng.prepare (fs, 512, 2);
            Signal in = makeConst (4 * 48000, 0.0f);
            Rng rng { 31u };
            for (int c = 0; c < 2; ++c) for (int i = 0; i < 48000; ++i) in.ch[c][(size_t) i] = 0.5f * rng.next();

            Signal out;
            render (eng, in, out, 512, [&] (int pos) { return makeParams (pos < 48000 ? 0.9f : 0.0f, 1.0f, 0.0f, true, 1); });

            const double ring = rmsOf (out.ch[0], 48000 + 2400, 48000 + 16800);
            std::printf ("  (engine ring-out RMS 50..350 ms after t -> 0: %.3e)\n", ring);
            check (allFinite (out) && ring > 1.0e-3, "engine: delay + reverb returns ring out after tension drops to 0 (no gating)");
        }
    }

    // =======================================================================
    // T14 - DelayEngine
    // =======================================================================
    void testDelay()
    {
        std::printf ("\n[T14] DelayEngine\n");
        const double fs = 48000.0;

        // ---- time table ----
        {
            const bool ok =
                std::abs (DelayEngine::computeDelayMs (true, 120.0, 1) - 375.0) < 1.0e-9
                && std::abs (DelayEngine::computeDelayMs (true, 120.0, 0) - 750.0) < 1.0e-9
                && std::abs (DelayEngine::computeDelayMs (true, 120.0, 2) - 60000.0 / 120.0 / 6.0) < 1.0e-9
                && std::abs (DelayEngine::computeDelayMs (true, 40.0, 0) - 1125.0) < 1.0e-9       // 2250 ms halved
                && DelayEngine::computeDelayMs (true, 20.0, 0) <= 2000.0
                && DelayEngine::computeDelayMs (false, 90.0, 0) == 560.0
                && DelayEngine::computeDelayMs (false, 90.0, 1) == 375.0
                && DelayEngine::computeDelayMs (false, 90.0, 2) == 190.0;
            check (ok, "delay time: dotted 1/4 | dotted 1/8 | 1/16 triplet by mode, halved above 2 s, free 560 / 375 / 190 ms");
        }

        // ---- ping-pong alternation at the division time ----
        {
            const double D = 4800.0;                              // 100 ms
            const int n = 48000;
            DelayEngine d;
            d.prepare (fs);

            DelayEngine::Targets tg;
            tg.sendGain = 1.0f; tg.returnGain = 1.0f; tg.feedback = 0.6f; tg.fbLpfHz = 9000.0f; tg.delayMs = 100.0f;

            std::vector<float> inL ((size_t) n, 0.0f), inR ((size_t) n, 0.0f), oL ((size_t) n), oR ((size_t) n);
            inL[0] = inR[0] = 1.0f;

            for (int pos = 0; pos < n; pos += 32)
            {
                d.setTargets (tg, pos == 0, 32);
                d.process (inL.data() + pos, inR.data() + pos, oL.data() + pos, oR.data() + pos, 32, nullptr);
            }

            bool alternates = true, timing = true;
            for (int k = 1; k <= 5; ++k)
            {
                const auto& prim = (k % 2 == 1) ? oL : oR;
                const auto& oth  = (k % 2 == 1) ? oR : oL;
                const int from = (int) (k * D) - 4, to = (int) (k * D) + 3 * k + 8;
                double ep = 0.0, eo = 0.0;
                float best = 0.0f;
                int arg = from;

                for (int i = from; i < to; ++i)
                {
                    ep += (double) prim[(size_t) i] * prim[(size_t) i];
                    eo += (double) oth[(size_t) i] * oth[(size_t) i];
                    if (std::abs (prim[(size_t) i]) > best) { best = std::abs (prim[(size_t) i]); arg = i; }
                }

                if (! (ep > 1.0e-4 && eo < 1.0e-4 * ep)) alternates = false;
                if (std::abs ((double) arg - k * D) > 3 * k + 3) timing = false;
            }
            check (alternates, "ping-pong: impulse appears alternately in L, R, L, R, L (the other channel stays silent)");
            check (timing, "ping-pong: echoes land on k * division time (Lagrange3rd, one read-pointer advance per sample)");
            const float firstEcho = std::max ({ std::abs (oL[4799]), std::abs (oL[4800]), std::abs (oL[4801]) });
            check (firstEcho > 0.95f && std::abs (oL[4800]) > 0.95f, "first echo is the delayed impulse at N samples (pop-then-push, return gain 1)");
        }

        // ---- feedback hard cap ----
        {
            DelayEngine d;
            d.prepare (fs);
            DelayEngine::Targets tg;
            tg.feedback = 5.0f;
            d.setTargets (tg, true, 32);
            check (d.getFeedbackTarget() <= 0.85f + 1.0e-6f && d.getFeedbackTarget() > 0.84f, "feedback hard-capped at 0.85 in code");
        }

        // ---- 60 s soak at max feedback ----
        {
            DelayEngine d;
            d.prepare (fs);
            DelayEngine::Targets tg;
            tg.sendGain = 1.0f; tg.returnGain = 1.0f; tg.feedback = 1.0f; tg.fbLpfHz = 3200.0f; tg.delayMs = 190.0f; tg.lfoDepthMs = 0.9f;

            Rng ra { 1u }, rb { 2u };
            std::vector<float> inL (32), inR (32), oL (32), oR (32), lfo (32);
            float peak = 0.0f;
            bool fin = true;
            double ph = 0.0;
            const int chunks = 60 * 48000 / 32;

            for (int c = 0; c < chunks; ++c)
            {
                for (int i = 0; i < 32; ++i)
                {
                    inL[(size_t) i] = 0.9f * ra.next();
                    inR[(size_t) i] = 0.9f * rb.next();
                    lfo[(size_t) i] = (float) std::sin (ph);
                    ph += 2.0 * kPi * 0.7 / fs;
                }
                d.setTargets (tg, c == 0, 32);
                d.process (inL.data(), inR.data(), oL.data(), oR.data(), 32, lfo.data());

                for (int i = 0; i < 32; ++i)
                {
                    if (! std::isfinite (oL[(size_t) i]) || ! std::isfinite (oR[(size_t) i])) fin = false;
                    peak = std::max (peak, std::max (std::abs (oL[(size_t) i]), std::abs (oR[(size_t) i])));
                }
            }
            std::printf ("  (60 s soak at feedback 1.0 -> capped 0.85: peak %.3f)\n", (double) peak);
            check (fin && peak <= 2.0f && ! d.consumeFailure(), "60 s soak at max feedback: finite, peak bounded (<= 2.0), loop guard never tripped");
        }

        // ---- tempo jump click-free through the 250 ms slew ----
        {
            const int n = 6 * 48000;
            DelayEngine d;
            d.prepare (fs);

            DelayEngine::Targets tg;
            tg.sendGain = 1.0f; tg.returnGain = 1.0f; tg.feedback = 0.3f; tg.fbLpfHz = 9000.0f;

            std::vector<float> in ((size_t) n), oL ((size_t) n), oR ((size_t) n);
            for (int i = 0; i < n; ++i)
            {
                const double fade = std::min (1.0, (double) i / 9600.0);
                in[(size_t) i] = 0.5f * (float) (std::sin (2.0 * kPi * 100.0 * (double) i / fs) * (0.5 - 0.5 * std::cos (kPi * fade)));
            }

            double prevDelay = 0.0, maxStep = 0.0;
            for (int pos = 0; pos < n; pos += 32)
            {
                tg.delayMs = (float) DelayEngine::computeDelayMs (true, pos < 48000 ? 120.0 : 60.0, 1);   // 375 -> 750 ms
                d.setTargets (tg, pos == 0, 32);
                d.process (in.data() + pos, in.data() + pos, oL.data() + pos, oR.data() + pos, 32, nullptr);

                const double cur = d.getCurrentDelaySamples();
                if (pos > 0) maxStep = std::max (maxStep, std::abs (cur - prevDelay) / 32.0);
                prevDelay = cur;
            }

            const float m = maxSecondDiffFrom (oL, 20000);
            std::printf ("  (tempo jump 120 -> 60 BPM: max |second difference| %.3e, max delay slew %.2f samples/sample, final delay %.0f)\n",
                         (double) m, maxStep, d.getCurrentDelaySamples());
            // Bound derivation. Output y(n) = x(n - delay(n)); with read speed s = 1 - delay' in [0.1, 1.0]:
            //   y'' ~ s^2 x'' + s' x'.   x = sum of the 100 Hz sine (0.5) and its filtered feedback echoes
            //   (0.3 * ~0.5 HPF gain * ...): peak A <= 0.65, w = 2 pi 100 / 48000 = 0.01309.
            //   curvature term  A w^2        = 0.65 * 1.71e-4          = 1.1e-4
            //   speed-change term: delay velocity is low-passed over 20 ms and capped at 0.9, so
            //   |s'| <= 0.9 / 960 = 9.4e-4 -> s' * A w = 9.4e-4 * 8.5e-3 = 8e-6
            //   float delay quantisation (ulp 0.004 at 36000) -> ~ +-0.002 sample jitter * A w, 2nd diff ~ 7e-5
            //   sum ~ 2e-4; bound 4e-4 (2x margin). The previous plain one-pole slew started with a velocity
            //   STEP of 1.5 (read speed +1 -> -0.5): kink = 1.5 * 0.5 * w = 9.8e-3, i.e. 25x over this bound.
            check (m < 4.0e-4f, "tempo jump is click-free (second-difference bound through the capped, velocity-smoothed 250 ms slew)");
            check (maxStep > 0.5 && maxStep <= 0.9 + 1.0e-6 && std::abs (d.getCurrentDelaySamples() - 36000.0) < 50.0,
                   "delay time glides at <= 0.9 samples/sample (read head never stalls) and converges on the new tempo");
        }

        // ---- NaN into the loop: lines reset, flag raised, recovers ----
        {
            const int n = 48000;
            DelayEngine d;
            d.prepare (fs);
            DelayEngine::Targets tg;
            tg.sendGain = 1.0f; tg.returnGain = 1.0f; tg.feedback = 0.6f; tg.delayMs = 100.0f;

            std::vector<float> inL ((size_t) n, 0.0f), inR ((size_t) n, 0.0f), oL ((size_t) n), oR ((size_t) n);
            inL[10] = std::numeric_limits<float>::quiet_NaN();
            inL[4000] = inR[4000] = 1.0f;

            for (int pos = 0; pos < n; pos += 32)
            {
                d.setTargets (tg, pos == 0, 32);
                d.process (inL.data() + pos, inR.data() + pos, oL.data() + pos, oR.data() + pos, 32, nullptr);
            }

            bool fin = true;
            for (size_t i = 0; i < (size_t) n; ++i) if (! std::isfinite (oL[i]) || ! std::isfinite (oR[i])) fin = false;

            float best = 0.0f; int arg = 0;
            for (int i = 8700; i < 8900; ++i) if (std::abs (oL[(size_t) i]) > best) { best = std::abs (oL[(size_t) i]); arg = i; }

            check (fin, "delay: NaN on the loop write -> output stays finite");
            check (d.consumeFailure() && ! d.consumeFailure(), "delay: loop guard raises the failure flag exactly once");
            check (best > 0.5f && std::abs (arg - 8800) <= 3, "delay: after the reset the loop works again (clean echo at 4000 + 4800)");
        }
    }

    // =======================================================================
    // T15 - tails regression (bit-exact passthrough returns once the tails decay)
    // =======================================================================
    void testTailsRegression()
    {
        std::printf ("\n[T15] tails regression\n");

        for (int mode : { 0, 2 })
            for (bool sync : { true, false })
            {
                TensionEngine eng;
                eng.prepare (48000.0, 512, 2);

                const int n = 14 * 48000;
                const int sw = 48000;
                const Signal in = makeNoise (n, 2, 6060u + (std::uint32_t) mode, 0.9f, true);
                Signal out;
                render (eng, in, out, 512, [&] (int pos) { return makeParams (pos < sw ? 1.0f : 0.0f, 1.0f, 0.0f, sync, mode); });

                const bool tailPresent = ! bitEqual (in, out, sw + 2400, sw + 7200);
                const bool exact = bitEqual (in, out, sw + 9 * 48000, n);

                char name[160];
                std::snprintf (name, sizeof name, "loud burst at t = 1 then t -> 0 (mode %d, sync %d): tails ring, then bit-exact passthrough again (-120 dBFS)", mode, sync ? 1 : 0);
                check (allFinite (out) && tailPresent && exact && eng.isAtRest(), name);
            }
    }

    // =======================================================================
    // T16 - NaN injection with sends active
    // =======================================================================
    void testNaNWithSends()
    {
        std::printf ("\n[T16] NaN / Inf injection with the sends active\n");

        const float nan = std::numeric_limits<float>::quiet_NaN();
        const float inf = std::numeric_limits<float>::infinity();

        for (int mode = 0; mode < 3; ++mode)
        {
            TensionEngine eng;
            eng.prepare (48000.0, 512, 2);

            Signal in = makeNoise (5 * 16384, 2, 8128u + (std::uint32_t) mode, 0.4f, false);
            in.ch[0][100] = nan; in.ch[0][1000] = inf; in.ch[1][1500] = -inf; in.ch[1][3000] = nan; in.ch[0][20000] = nan;

            Signal out;
            render (eng, in, out, 512, [&] (int) { return makeParams (0.7f, 0.6f, 0.0f, true, mode); });

            const double tailRms = rmsOf (out.ch[0], 5 * 16384 - 8192, 5 * 16384);
            char name[160];
            std::snprintf (name, sizeof name, "mode %d, t 0.7, mix 0.6: finite, feedback modules reset via the OutputSafety flag, recovers (tail RMS %.3f)", mode, tailRms);
            check (allFinite (out) && eng.getFeedbackResetCount() >= 1 && tailRms > 0.02, name);
        }

        // reverb loop guard
        {
            ReverbEngine rv;
            rv.prepare (48000.0);
            ReverbEngine::Targets tg;
            tg.sendGain = 1.0f; tg.returnGain = 1.0f; tg.roomSize = 0.8f;

            const int n = 4 * 48000;
            Rng rng { 5u };
            std::vector<float> inL ((size_t) n), inR ((size_t) n), oL ((size_t) n), oR ((size_t) n);
            for (int i = 0; i < n; ++i) { inL[(size_t) i] = 0.3f * rng.next(); inR[(size_t) i] = 0.3f * rng.next(); }
            inL[100] = nan;

            for (int pos = 0; pos < n; pos += 32)
            {
                rv.setTargets (tg, pos == 0, 32);
                rv.process (inL.data() + pos, inR.data() + pos, oL.data() + pos, oR.data() + pos, 32);
            }

            bool fin = true;
            for (size_t i = 0; i < (size_t) n; ++i) if (! std::isfinite (oL[i]) || ! std::isfinite (oR[i])) fin = false;
            check (fin && rv.consumeFailure() && rmsOf (oL, n - 24000, n) > 1.0e-3, "reverb: NaN input -> state cleared, flag raised, output finite and alive again");
        }
    }
}


int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    std::printf ("TENSION tests - Phase 3.3 (parallel sends and noise + Phase 3.1/3.2 regression)\n");

    testUnits();
    testBitExact();
    testMixOutputZipper();
    testNaNInf();
    testBlockInvariance();
    testNoAllocation();
    testTelemetry();

    testLFO();
    testFilter();
    testSaturation();
    testWidth();
    testRealChain();

    testNoise();
    testReverb();
    testDelay();
    testTailsRegression();
    testNaNWithSends();

    std::printf ("\n%s  (%d failure%s)\n", failures == 0 ? "ALL PASS" : "FAILURES", failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
