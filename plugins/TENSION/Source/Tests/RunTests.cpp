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
*/
#include "PluginProcessor.h"
#include "DSP/TensionEngine.h"
#include "DSP/OutputSafety.h"
#include "DSP/TensionMapping.h"

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
            const int n = 48000;
            const Signal in = makeNoise (n, 2, 99u + (std::uint32_t) mode, 1.2f, true);
            Signal out;
            const int switchPos = 28 * 512;   // 14336
            render (eng, in, out, 512, [&] (int pos) { return makeParams (pos < switchPos ? 0.8f : 0.0f, 1.0f, 0.0f, true, mode); });

            const bool exact = bitEqual (in, out, switchPos + 9600, n);
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
        check (tel.noiseLevel.load() == 0.0f && tel.lfoPhaseA.load() == 0.0f, "noiseLevel / lfoPhaseA are 0 in Phase 3.1");
    }
}

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    std::printf ("TENSION tests - Phase 3.1 (engine skeleton + transparent routing)\n");

    testUnits();
    testBitExact();
    testMixOutputZipper();
    testNaNInf();
    testBlockInvariance();
    testNoAllocation();
    testTelemetry();

    std::printf ("\n%s  (%d failure%s)\n", failures == 0 ? "ALL PASS" : "FAILURES", failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
