/*
    WARPR - phase D2 test block (warp framework + bending modes + viz feeds).

    Called from RunTests.cpp main():   failures += runPhaseD2Tests (full);

    Class-level tests drive UnisonOsc::render() directly (OS-rate bus, no decimator) with a single-cycle
    sine / the default saw / noise / a stereo sample, so a mode is exercised in isolation.  Processor-level
    tests cover the voice plumbing (smoothing, enable ramp, DC after the blockers) and the viz feed.

    Quick tier (default) is a few seconds; `--full` widens the parameter cube (A, B in {0, 0.5, 1}).

    House rules applied: setPlayConfigDetails BEFORE prepareToPlay, zero-allocation proof through
    ScopedAllocCount, deterministic seeded randomness.
*/

#include "PluginProcessor.h"
#include "DSP/Adaa.h"
#include "DSP/AmpWarp.h"
#include "DSP/DiscreteSwitchFade.h"
#include "DSP/FastMath.h"
#include "DSP/PhaseWarp.h"
#include "DSP/UnisonOsc.h"
#include "DSP/VizAnalyzer.h"
#include "DSP/WarpModeTable.h"
#include "DSP/WarpStage.h"
#include "Parameters/ParamTable.h"
#include "Tests/AllocHook.h"
#include "Tests/OfflineRender.h"
#include "Tests/Scanners.h"
#include "Tests/WarpTestKit.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// Bit-exact float equality is the point of several assertions in this file (bypass == dry, determinism,
// unimplemented modes == identity), so the warning is suppressed here only.
JUCE_BEGIN_IGNORE_WARNINGS_GCC_LIKE ("-Wfloat-equal")

int runPhaseD2Tests (bool full);

namespace
{
    int failures = 0;
    int printedDetails = 0;

    void check (bool cond, const char* name)
    {
        std::printf ("  %s  %s\n", cond ? "PASS" : "FAIL", name);
        if (! cond) ++failures;
    }

    /** Prints a failure detail line (bounded, so a systematic failure does not flood the log). */
   #define DETAIL(...) do { if (printedDetails++ < 40) std::printf (__VA_ARGS__); } while (false)

    // fixtures, the one-oscillator renderer and the signal measures are shared with the later phases
    using namespace warpr::tests::kit;

    void setParam (PluginProcessor& p, const char* id, float value)
    {
        if (auto* prm = p.apvts.getParameter (id))
        {
            prm->setValueNotifyingHost (prm->convertTo0to1 (value));
        }
        else
        {
            std::printf ("  FAIL  unknown parameter '%s'\n", id);
            ++failures;
        }
    }

    /** Maximum |g'(theta)| of a phase mode (numerical, over the whole cycle). */
    double maxSlope (int mode, double a, double A, double B, double dTheta)
    {
        double m = 1.0;
        constexpr double h = 1.0e-4;
        for (double t = 0.0; t + h <= 1.0; t += h)
        {
            const double g0 = t + warpr::phasewarp::delta (mode, a, A, B, t, dTheta);
            const double g1 = (t + h) + warpr::phasewarp::delta (mode, a, A, B, t + h, dTheta);
            m = std::max (m, std::abs ((g1 - g0) / h));
        }
        return m;
    }

    //==========================================================================
    // 1. mode table
    //==========================================================================
    void testModeTable()
    {
        std::printf ("\n[D2] WarpModeTable: 60 rows, names == spec, all implemented (D2 + D3 + D6)\n");

        int nameMismatch = 0, implementedWrong = 0, familyWrong = 0;

        for (int i = 0; i < warpr::kNumWarpModes; ++i)
        {
            const auto& m = warpr::kWarpModes[ix (i)];
            if (std::string (m.name) != std::string (warpr::choices::warpMode[i]))
            {
                ++nameMismatch;
                DETAIL ("        name mismatch at %d: '%s' vs spec '%s'\n", i, m.name, warpr::choices::warpMode[i]);
            }

            if (m.implemented != (i < warpr::kNumImplementedWarpModes))
                ++implementedWrong;
        }

        const int hModes[] = { 0, 1, 2, 3, 7, 9, 14 }, vModes[] = { 4, 5, 6, 11, 12, 13 }, vtModes[] = { 8, 10 };
        for (int i : hModes)  if (warpr::warpModeInfo (i).family != warpr::WarpFamily::H  || warpr::warpModeInfo (i).kind != warpr::WarpKind::Phase) ++familyWrong;
        for (int i : vModes)  if (warpr::warpModeInfo (i).family != warpr::WarpFamily::V  || warpr::warpModeInfo (i).kind != warpr::WarpKind::Amp)   ++familyWrong;
        for (int i : vtModes) if (warpr::warpModeInfo (i).family != warpr::WarpFamily::VT || warpr::warpModeInfo (i).kind != warpr::WarpKind::Amp)   ++familyWrong;

        check (warpr::kWarpModes.size() == 60 && warpr::kNumWarpModes == 60, "table has exactly 60 rows");
        check (nameMismatch == 0, "every row name equals choices::warpMode[i] (parameter-spec order and spelling)");
        check (implementedWrong == 0 && warpr::kNumImplementedWarpModes == 60, "all 60 indices are implemented (D2: 0-14, D3: 15-43, D6: 44-59)");
        check (familyWrong == 0, "family / kind: H {0,1,2,3,7,9,14}, V {4,5,6,11,12,13}, VT {8,10}");
        check (warpr::warpModeInfo (-5).name == warpr::kWarpModes[0].name && warpr::warpModeInfo (99).name == warpr::kWarpModes[59].name,
               "warpModeInfo clamps out-of-range indices");
        check (warpr::warpModeInfo (8).stepFlag && warpr::warpModeInfo (10).stepFlag && warpr::warpModeInfo (9).seamJump
               && ! warpr::warpModeInfo (0).stepFlag, "intended-discontinuity flags: Flip, Wave Skew (step), Mirror (seam jump)");
    }

    //==========================================================================
    // 2. FastMath + Adaa kernels
    //==========================================================================
    void testFastMathAndAdaa()
    {
        std::printf ("\n[D2] FastMath accuracy and ADAA kernel properties\n");

        double worstExp = 0.0, worstLog = 0.0, worstPow = 0.0, worstSin = 0.0;
        for (double x = -30.0; x <= 30.0; x += 0.0137)
            worstExp = std::max (worstExp, std::abs (warpr::fastmath::exp2 (x) / std::exp2 (x) - 1.0));
        for (double x = 1.0e-6; x < 1.0e6; x *= 1.0123)
            worstLog = std::max (worstLog, std::abs (warpr::fastmath::log2 (x) - std::log2 (x)));
        for (double x = 0.001; x <= 4.0; x += 0.0173)
            for (double y = 0.2; y <= 8.0; y += 0.37)
                worstPow = std::max (worstPow, std::abs (warpr::fastmath::pow (x, y) / std::pow (x, y) - 1.0));
        for (double c = -3.0; c <= 3.0; c += 0.00137)
            worstSin = std::max (worstSin, std::abs (warpr::fastmath::sin2pi (c) - std::sin (2.0 * kPi * c)));

        std::printf ("        (worst: exp2 %.2e rel, log2 %.2e abs, pow %.2e rel, sin2pi %.2e abs)\n", worstExp, worstLog, worstPow, worstSin);
        check (worstExp < 1.0e-8 && worstLog < 1.0e-8 && worstPow < 1.0e-7 && worstSin < 1.0e-8, "exp2 / log2 / pow / sin2pi within 1e-8 (pow 1e-7)");
        check (warpr::fastmath::exp2 (0.0) == 1.0 && warpr::fastmath::pow (0.0, 2.0) == 0.0 && warpr::fastmath::pow (-1.0, 2.0) == 0.0,
               "exp2(0) == 1 exactly; pow(<=0, y) == 0");

        // F1' == f for every closed-form pair (central difference, away from the cusp at 0)
        struct Probe { const char* name; int which; double p1, p2, k; };
        const Probe probes[] = {
            { "asym power (0.45, 1)", 0, 0.45, 1.0, 1.0 }, { "asym power (1, 0.4)", 0, 1.0, 0.4, 1.0 },
            { "asym power (0.7, 0.5)", 0, 0.7, 0.5, 1.0 }, { "power law p=2.2 k=0.6", 1, 2.2, 0.0, 0.6 },
            { "power law p=0.35 k=0.8", 1, 0.35, 0.0, 0.8 }, { "compander p=2.05 k=0.6", 2, 2.05, 0.0, 0.6 },
            { "compander p=4 k=1", 2, 4.0, 0.0, 1.0 }
        };

        int badPairs = 0;
        for (const auto& pr : probes)
        {
            double worst = 0.0;
            for (double x = -1.2; x <= 1.2; x += 0.0137)
            {
                if (std::abs (x) < 0.05 || std::abs (std::abs (x) - 1.0) < 0.01)
                    continue;

                constexpr double h = 1.0e-3;
                double f = 0.0, d = 0.0;
                if (pr.which == 0)
                {
                    const warpr::ampwarp::AsymPowerKernel k { pr.p1, pr.p2 };
                    f = k.f (x);  d = (k.F1 (x + h) - k.F1 (x - h)) / (2.0 * h);
                }
                else if (pr.which == 1)
                {
                    const warpr::ampwarp::PowerLawKernel k { pr.p1, pr.k };
                    f = k.f (x);  d = (k.F1 (x + h) - k.F1 (x - h)) / (2.0 * h);
                }
                else
                {
                    const warpr::ampwarp::CompanderKernel k { pr.p1, pr.k };
                    f = k.f (x);  d = (k.F1 (x + h) - k.F1 (x - h)) / (2.0 * h);
                }
                worst = std::max (worst, std::abs (f - d));
            }

            if (worst > 1.0e-4)
            {
                ++badPairs;
                DETAIL ("        F1' != f for %s: %.2e\n", pr.name, worst);
            }
        }
        check (badPairs == 0, "F1' == f (1e-4 central difference) for every V-family kernel pair");

        // Simpson mean is exact for cubics
        struct Cubic { double f (double x) const noexcept { return x * x * x - 0.5 * x; } double F1 (double x) const noexcept { return 0.25 * x * x * x * x - 0.25 * x * x; } };
        const Cubic cub;
        const double a = -0.3, b = 0.9;
        const double exact = (cub.F1 (b) - cub.F1 (a)) / (b - a);
        check (std::abs (warpr::adaa::simpsonMean (cub, a, b) - exact) < 1.0e-12, "Simpson mean is exact for a cubic");

        // below dxEps the fallback is used: finite and close to f
        const double fallback = warpr::adaa::firstOrder (cub, 0.4, 0.4 + 1.0e-7);
        check (std::isfinite (fallback) && std::abs (fallback - cub.f (0.4)) < 1.0e-6, "ADAA fallback (|dx| < 1e-5) returns the Simpson mean of f");

        // remainder form: with p == 1 the stage is the identity (no two-sample low-pass), within the fast-math accuracy
        warpr::adaa::State st;
        const warpr::ampwarp::AsymPowerKernel ident { 1.0, 1.0 };
        double worstId = 0.0;
        juce::Random rng (11);
        double x = 0.0;
        for (int i = 0; i < 2000; ++i)
        {
            x += 0.02 + 0.05 * (double) rng.nextFloat();
            if (x > 0.9) x = -0.9;
            const double y = warpr::ampwarp::runRemainder (st, ident, x);
            if (i > 0)
                worstId = std::max (worstId, std::abs (y - x));
        }
        check (worstId < 1.0e-5, "remainder-form ADAA of a unity power law is the identity (no half-sample averaging)");

        // coefficient selection: Identity at a == 0 for every mode, and a bit-exact pass-through
        bool identityExact = true;
        for (int mode : { 4, 5, 6, 11, 12, 13 })
        {
            warpr::adaa::State s2;
            const auto c = warpr::ampwarp::makeCoeffs (mode, 0.0, 0.5, 0.5);
            for (double v : { -0.8, -0.1, 0.0, 0.3, 0.77 })
                identityExact = identityExact && warpr::ampwarp::run (c, s2, v) == v;
        }
        check (identityExact, "V family: a == 0 selects the Identity kernel (bit-exact)");

        // DiscreteSwitchFade bookkeeping
        warpr::DiscreteSwitchFade fade;
        fade.reset();
        const bool first = fade.request (3, 100);
        const bool same = fade.request (3, 100);
        const bool second = fade.request (7, 4);
        float w = 0.0f, last = 0.0f;
        int steps = 0;
        bool monotone = true;
        while (fade.fading())
        {
            w = fade.advance();
            monotone = monotone && w >= last;
            last = w;
            ++steps;
        }
        check (! first && ! same && second && fade.current() == 7 && fade.outgoing() == 3 && steps == 4
               && w == 1.0f && monotone && fade.advance() == 1.0f,
               "DiscreteSwitchFade: first request adopts silently, a change fades in N steps ending at weight 1");
    }

    //==========================================================================
    // 3. phase maps
    //==========================================================================
    void testPhaseMaps()
    {
        std::printf ("\n[D2] phase maps g(theta) of the H family\n");

        int zeroBad = 0, endBad = 0, monoBad = 0, rangeBad = 0, identityBad = 0, cases = 0;
        const double dTheta = 100.0 / (48000.0 * 2.0);

        for (int mode : { 0, 1, 2, 3, 7, 9, 14 })
            for (double a : { 0.05, 0.25, 0.5, 1.0 })
                for (double A : { 0.0, 0.5, 1.0 })
                    for (double B : { 0.0, 0.5, 1.0 })
                    {
                        ++cases;
                        auto g = [&] (double t) { return warpr::phasewarp::phaseMap (mode, a, A, B, t, dTheta); };

                        if (std::abs (g (0.0)) > 1.0e-9) ++zeroBad;

                        const double expectEnd = mode == 9 ? 1.0 - a : 1.0;
                        if (std::abs (g (1.0) - expectEnd) > 1.0e-6)
                        {
                            ++endBad;
                            DETAIL ("        mode %d a=%.2f A=%.1f B=%.1f: g(1)=%.6f expected %.6f\n", mode, a, A, B, g (1.0), expectEnd);
                        }

                        double prev = g (0.0);
                        bool mono = true, inRange = true;
                        for (double t = 0.0005; t <= 1.0; t += 0.0005)
                        {
                            const double v = g (t);
                            if (mode != 9 && v < prev - 1.0e-9) mono = false;
                            if (v < -1.0e-9 || v > 1.0 + 1.0e-9) inRange = false;
                            prev = v;
                        }
                        if (! mono) ++monoBad;
                        if (! inRange) ++rangeBad;
                    }

        for (int mode : { 0, 1, 2, 3, 7, 9, 14 })
            for (double t : { 0.0, 0.2, 0.5, 0.77, 0.999 })
                if (warpr::phasewarp::delta (mode, 0.0, 0.5, 0.5, t, dTheta) != 0.0)
                    ++identityBad;

        std::printf ("        (%d parameter combinations)\n", cases);
        check (zeroBad == 0, "g(0) == 0 for every mode and parameter combination");
        check (endBad == 0, "g(1) == 1 (Mirror: 1 - a, the intended seam jump)");
        check (monoBad == 0, "g is monotone (reads never run backwards) for every mode except Mirror");
        check (rangeBad == 0, "g stays inside [0, 1]");
        check (identityBad == 0, "delta() == 0 exactly at warpAmount == 0 for every mode");
    }

    //==========================================================================
    // 4. T1 universal checks
    //==========================================================================
    void testUniversal (bool full)
    {
        std::printf ("\n[D2] T1 universal checks: 15 bending modes x 4 signals x OS {1,2,8} x (a, B)%s\n", full ? " x A" : "");
        constexpr double fs = 48000.0;

        auto sine = makeCycle (2048, 0.5f);
        auto saw = warpr::SampleData::createDefaultWave();
        auto noise = makeNoise();

        struct Sig { const warpr::SampleData* sd; double hz; const char* name; bool smooth; };
        const Sig sigs[] = {
            { sine.get(),  100.0,    "sine 100 Hz", true },
            { sine.get(),  5000.0,   "sine 5 kHz", false },
            { saw.get(),   110.0,    "saw cycle 110 Hz", false },
            { noise.get(), rootHz(), "noise", false }
        };

        const std::vector<double> aList { 0.0, 0.5, 1.0 };
        const std::vector<double> bList { 0.0, 1.0 };
        const std::vector<double> AList = full ? std::vector<double> { 0.0, 0.5, 1.0 } : std::vector<double> { 0.5 };

        int runs = 0, nonFinite = 0, unbounded = 0, nullBad = 0, discBad = 0;
        double worstPeak = 0.0;

        for (int mode = 0; mode < 15; ++mode)
        {
            const auto& info = warpr::warpModeInfo (mode);

            for (const auto& sig : sigs)
                for (int os : { 1, 2, 8 })
                {
                    const int totalBase = 7200;
                    const auto base = renderOsc (*sig.sd, sig.hz, fs, os, totalBase, noWarp());
                    const double nat = maxStepOf (base.L);

                    for (double A : AList)
                        for (double a : aList)
                            for (double B : bList)
                            {
                                ++runs;
                                const auto run = renderOsc (*sig.sd, sig.hz, fs, os, totalBase,
                                                            constSpan (mode, (float) a, (float) A, (float) B, 1.0f));

                                if (! allFinite (run.L) || ! allFinite (run.R))
                                {
                                    ++nonFinite;
                                    DETAIL ("        non-finite: mode %d (%s) %s OS %d a=%.1f B=%.1f\n", mode, info.name, sig.name, os, a, B);
                                    continue;
                                }

                                const double pk = std::max (peakOf (run.L), peakOf (run.R));
                                worstPeak = std::max (worstPeak, pk);
                                if (pk > 4.0)
                                {
                                    ++unbounded;
                                    DETAIL ("        unbounded %.2f: mode %d (%s) %s OS %d a=%.1f B=%.1f\n", pk, mode, info.name, sig.name, os, a, B);
                                }

                                if (a == 0.0)
                                {
                                    const double nd = nullDb (base.L, run.L);
                                    if (nd > -80.0)
                                    {
                                        ++nullBad;
                                        DETAIL ("        a=0 null %.1f dB: mode %d (%s) %s OS %d\n", nd, mode, info.name, sig.name, os);
                                    }
                                }

                                if (sig.smooth)
                                {
                                    double bound;
                                    if (info.stepFlag || info.seamJump)
                                        bound = 2.0;                                        // intended discontinuity: its own (full swing) bound
                                    else if (info.kind == warpr::WarpKind::Phase)
                                        bound = 2.0 * nat * maxSlope (mode, a, A, B, sig.hz / (fs * (double) os)) + 0.01;
                                    else
                                        bound = 1.0;                                        // V family: bounded by the full swing (ADAA through the cusp)

                                    const double step = maxStepOf (run.L);
                                    if (step > bound)
                                    {
                                        ++discBad;
                                        DETAIL ("        discontinuity %.4f > %.4f: mode %d (%s) OS %d a=%.1f A=%.1f B=%.1f\n", step, bound, mode, info.name, os, a, A, B);
                                    }
                                }
                            }
                }
        }

        std::printf ("        (%d renders, worst |y| %.3f)\n", runs, worstPeak);
        check (nonFinite == 0, "no NaN / inf for any mode, signal, OS factor or parameter combination");
        check (unbounded == 0, "|y| <= 4 FS before the master for every combination");
        check (nullBad == 0, "a == 0: -80 dB null against the unwarped read (all 15 modes)");
        check (discBad == 0, "smooth input: max step within the per-mode bound (flagged modes use their own bound)");
    }

    void testStereoSource()
    {
        std::printf ("\n[D2] stereo sources: shared read position, per-channel ADAA\n");
        constexpr double fs = 48000.0;

        auto same = makeStereoCycle (2048, true);
        auto diff = makeStereoCycle (2048, false);

        int asymmetric = 0, notIndependent = 0, nonFinite = 0;
        for (int mode = 0; mode < 15; ++mode)
        {
            const auto a = renderOsc (*same, 100.0, fs, 2, 4800, constSpan (mode, 0.6f, 0.5f, 0.5f, 1.0f));
            if (! bitEqual (a.L, a.R))
            {
                ++asymmetric;
                DETAIL ("        L != R for identical channels: mode %d\n", mode);
            }

            const auto b = renderOsc (*diff, 100.0, fs, 2, 4800, constSpan (mode, 0.6f, 0.5f, 0.5f, 1.0f));
            if (! allFinite (b.L) || ! allFinite (b.R))
                ++nonFinite;
            if (bitEqual (b.L, b.R))
                ++notIndependent;
        }

        check (asymmetric == 0, "identical L / R channels stay bit-identical through every mode (shared offset, symmetric ADAA)");
        check (notIndependent == 0 && nonFinite == 0, "different channels stay independent and finite");

        // mono source: the stage runs once and duplicates (L == R)
        auto mono = makeCycle (2048, 0.5f);
        bool monoExact = true;
        for (int mode = 0; mode < 15; ++mode)
        {
            const auto r = renderOsc (*mono, 100.0, fs, 2, 4800, constSpan (mode, 0.7f, 0.5f, 0.5f, 0.6f));
            monoExact = monoExact && bitEqual (r.L, r.R);
        }
        check (monoExact, "mono source: L == R exactly through every mode (also with warpMix < 1)");
    }

    //==========================================================================
    // 5. identity guarantees
    //==========================================================================
    void testIdentityGuarantees()
    {
        std::printf ("\n[D2] identity guarantees: bypass, unimplemented modes, warpMix = 0\n");
        constexpr double fs = 48000.0;
        auto sine = makeCycle (2048, 0.5f);
        auto saw = warpr::SampleData::createDefaultWave();

        for (const auto* sd : { sine.get(), saw.get() })
        {
            const auto base = renderOsc (*sd, 220.0, fs, 2, 6400, noWarp());

            int notIdentity = 0;
            for (int mode = warpr::kNumImplementedWarpModes; mode < 60; ++mode)
            {
                const auto run = renderOsc (*sd, 220.0, fs, 2, 6400, constSpan (mode, 1.0f, 0.5f, 0.5f, 1.0f));
                if (! bitEqual (base.L, run.L) || ! bitEqual (base.R, run.R))
                {
                    ++notIdentity;
                    DETAIL ("        mode %d is not the exact identity\n", mode);
                }
            }
            check (notIdentity == 0, "no mode is left unimplemented after D6 (the identity-by-flag path stays in the machinery)");

            // D6: a = 0 is the exact identity for the new rows too (Phase Mirror, FM) even at warpMix 0.4
            const auto mixHalf = renderOsc (*sd, 220.0, fs, 2, 6400, constSpan (47, 0.0f, 0.5f, 0.5f, 0.4f));
            const auto fmHalf = renderOsc (*sd, 220.0, fs, 2, 6400, constSpan (54, 0.0f, 0.5f, 0.5f, 0.4f));
            check (bitEqual (base.L, mixHalf.L) && bitEqual (base.L, fmHalf.L), "Phase Mirror / FM at warpAmount 0 with warpMix 0.4 are the exact identity");

            const auto inactive = renderOsc (*sd, 220.0, fs, 2, 6400, [] (int, warpr::WarpSpan& sp) { sp.active = false; sp.mode = 3; sp.a0 = sp.a1 = 1.0f; return true; });
            check (bitEqual (base.L, inactive.L) && bitEqual (base.R, inactive.R), "warpEnable off (span inactive) is bit-exact dry");

            int notDry = 0;
            for (int mode = 0; mode < 15; ++mode)
            {
                const auto run = renderOsc (*sd, 220.0, fs, 2, 6400, constSpan (mode, 0.0f, 0.5f, 0.5f, 1.0f));
                if (! bitEqual (base.L, run.L))
                {
                    ++notDry;
                    DETAIL ("        mode %d at a = 0 is not bit-exact dry (null %.1f dB)\n", mode, nullDb (base.L, run.L));
                }
            }
            check (notDry == 0, "a == 0 is bit-exact dry for all 15 implemented modes");

            // warpMix = 0 means full dry; mix 0.5 lies between dry and wet (equal-gain law)
            const auto wet = renderOsc (*sd, 220.0, fs, 2, 6400, constSpan (3, 0.8f, 0.5f, 0.5f, 1.0f));
            const auto m0 = renderOsc (*sd, 220.0, fs, 2, 6400, constSpan (3, 0.8f, 0.5f, 0.5f, 0.0f));
            const auto m5 = renderOsc (*sd, 220.0, fs, 2, 6400, constSpan (3, 0.8f, 0.5f, 0.5f, 0.5f));
            double worstMid = 0.0;
            for (size_t i = 0; i < base.L.size(); ++i)
                worstMid = std::max (worstMid, std::abs ((double) m5.L[i] - 0.5 * ((double) base.L[i] + (double) wet.L[i])));
            check (nullDb (base.L, m0.L) <= -140.0 && worstMid < 1.0e-5, "warpMix: 0 = dry, 0.5 = equal-gain midpoint of dry and wet");
        }
    }

    //==========================================================================
    // 6. mode switching
    //==========================================================================
    void testModeSwitch()
    {
        std::printf ("\n[D2] mode switching mid-note (DiscreteSwitchFade, ~10 ms)\n");
        constexpr double fs = 48000.0;
        auto sine = makeCycle (2048, 0.5f);

        const int pairs[][2] = { { 0, 13 }, { 4, 2 }, { 11, 14 }, { 6, 3 }, { 12, 5 }, { 7, 12 }, { 3, 30 }, { 17, 0 }, { 30, 17 }, { 13, 1 }, { 14, 6 }, { 2, 17 } };
        constexpr int totalBase = 9600, switchChunk = 150;

        int stepBad = 0, tailBad = 0;
        double worstStep = 0.0;

        for (const auto& pr : pairs)
        {
            const int from = pr[0], to = pr[1];
            auto sw = [&] (int ci, warpr::WarpSpan& sp)
            {
                sp.active = true;
                sp.mode = ci < switchChunk ? from : to;
                sp.a0 = sp.a1 = 0.35f;
                sp.A0 = sp.A1 = 0.5f;
                sp.B0 = sp.B1 = 0.5f;
                sp.mix0 = sp.mix1 = 1.0f;
                return true;
            };

            const auto run = renderOsc (*sine, 100.0, fs, 2, totalBase, sw);
            const auto pureTo = renderOsc (*sine, 100.0, fs, 2, totalBase, constSpan (to, 0.35f, 0.5f, 0.5f, 1.0f));

            const double step = maxStepOf (run.L);
            worstStep = std::max (worstStep, step);
            if (step > 0.1)
            {
                ++stepBad;
                DETAIL ("        switch %d -> %d: max step %.4f\n", from, to, step);
            }

            const size_t tail = run.L.size() * 3 / 4;                  // 50 ms after the switch (fade is 10 ms)
            if (nullDb (pureTo.L, run.L, tail) > -100.0)
            {
                ++tailBad;
                DETAIL ("        switch %d -> %d: output has not converged to the new mode (null %.1f dB)\n", from, to, nullDb (pureTo.L, run.L, tail));
            }
        }

        std::printf ("        (%d pairs, worst max step %.4f FS)\n", (int) (sizeof (pairs) / sizeof (pairs[0])), worstStep);
        check (stepBad == 0, "mode switch mid-note: max step <= 0.1 FS (incl. pairs with the D3 shapers)");
        check (tailBad == 0, "after the 10 ms fade the output equals the new mode rendered alone");
    }

    //==========================================================================
    // 7. smooth parameter sweep
    //==========================================================================
    void testSweep()
    {
        std::printf ("\n[D2] warpAmount sweep 0 -> 1 over 100 ms: no zipper\n");
        constexpr double fs = 48000.0;
        auto sine = makeCycle (2048, 0.5f);

        int bad = 0;
        for (int mode : { 0, 1, 2, 3, 4, 5, 6, 7, 11, 12, 13, 14 })
        {
            double staticMax = 0.0;
            for (double a : { 0.0, 0.25, 0.5, 0.75, 1.0 })
                staticMax = std::max (staticMax, maxStepOf (renderOsc (*sine, 100.0, fs, 2, 4800, constSpan (mode, (float) a, 0.5f, 0.5f, 1.0f)).L));

            constexpr int chunks = 150;
            auto sweep = [mode] (int ci, warpr::WarpSpan& sp)
            {
                sp.active = true;
                sp.mode = mode;
                sp.a0 = (float) ci / (float) chunks;
                sp.a1 = (float) (ci + 1) / (float) chunks;
                sp.A0 = sp.A1 = 0.5f;
                sp.B0 = sp.B1 = 0.5f;
                sp.mix0 = sp.mix1 = 1.0f;
                return true;
            };

            const double sweepMax = maxStepOf (renderOsc (*sine, 100.0, fs, 2, 4800, sweep).L);
            if (sweepMax > 1.5 * staticMax + 0.01)
            {
                ++bad;
                DETAIL ("        zipper: mode %d sweep max step %.4f vs static %.4f\n", mode, sweepMax, staticMax);
            }
        }
        check (bad == 0, "a-sweep max step <= 1.5 x the worst static step (+0.01) for all smooth modes");
    }

    //==========================================================================
    // 8. determinism + allocations (class level)
    //==========================================================================
    void testDeterminismAndAllocations()
    {
        std::printf ("\n[D2] determinism and zero allocations (class level)\n");
        constexpr double fs = 48000.0;
        auto saw = warpr::SampleData::createDefaultWave();

        int nondeterministic = 0;
        for (int mode = 0; mode < 15; ++mode)
        {
            const auto a = renderOsc (*saw, 110.0, fs, 2, 6400, constSpan (mode, 0.8f, 0.4f, 0.6f, 0.9f));
            const auto b = renderOsc (*saw, 110.0, fs, 2, 6400, constSpan (mode, 0.8f, 0.4f, 0.6f, 0.9f));
            if (! bitEqual (a.L, b.L) || ! bitEqual (a.R, b.R))
                ++nondeterministic;
        }
        check (nondeterministic == 0, "two identical renders are bit-equal for all 15 modes");

        // allocation proof: every mode, with mode switches and parameter ramps inside the counted region
        warpr::RegionFold rg;
        rg.configure (*saw, 0.0f, 1.0f, warpr::RegionFold::Mode::Forward);
        const double lc = saw->fileSampleRate / rootHz();
        warpr::UnisonOsc osc;
        osc.start (rg, false, 0, lc);
        std::vector<float> l (64), r (64);
        const double rho = 110.0 / rootHz() * saw->fileSampleRate / fs;

        std::size_t allocations = 0;
        {
            warpr::tests::ScopedAllocCount counter;
            for (int ci = 0; ci < 600; ++ci)
            {
                warpr::WarpSpan sp;
                sp.active = true;
                sp.mode = (ci / 7) % 50;                   // walks through all 44 implemented modes and 6 unimplemented ones
                sp.a0 = (float) (ci % 13) / 13.0f;  sp.a1 = (float) ((ci + 1) % 13) / 13.0f;
                sp.A0 = sp.A1 = 0.5f;
                sp.B0 = sp.B1 = 0.3f;
                sp.mix0 = 1.0f;  sp.mix1 = (ci % 5 == 0) ? 0.6f : 1.0f;
                sp.Lc = lc;
                sp.os = 2;
                sp.fadeLen = 960;
                osc.render (*saw, rg, 0, rho, rho, 2, 1.0f, 1.0f, 1.0f, 1.0f, l.data(), r.data(), 64, &sp);
            }
            allocations = counter.count();
        }
        check (allocations == 0, "UnisonOsc::render with the warp stage (all modes, switches, ramps) performs zero allocations");
    }

    //==========================================================================
    // 9. processor level
    //==========================================================================
    void testProcessorLevel()
    {
        std::printf ("\n[D2] processor level: DC after the blockers, bounds, enable bypass, smoothing\n");
        constexpr double fs = 48000.0;

        PluginProcessor proc;
        setParam (proc, "ampSustain", 1.0f);
        setParam (proc, "ampAttack", 1.0f);
        setParam (proc, "warpAmount", 1.0f);
        setParam (proc, "warpMix", 1.0f);                     // default mix is 0 (stage armed but silent)

        juce::MidiBuffer midi;
        midi.addEvent (juce::MidiMessage::noteOn (1, 60, 0.9f), 0);

        int dcBad = 0, finiteBad = 0, peakBad = 0;
        double worstDc = 0.0;
        for (float B : { 0.0f, 1.0f })
        {
            setParam (proc, "warpParamB", B);
            for (int mode = 0; mode < 15; ++mode)
            {
                setParam (proc, "warpMode", (float) mode);
                const auto out = warpr::tests::render (proc, midi, fs, 256, 1.3);
                const int start = (int) (0.9 * fs);

                for (int ch = 0; ch < 2; ++ch)
                {
                    const auto s = warpr::tests::scan (out.getReadPointer (ch) + start, out.getNumSamples() - start);
                    worstDc = std::max (worstDc, std::abs (s.mean));
                    if (! s.allFinite) ++finiteBad;
                    if (s.peak > 1.0f) ++peakBad;
                    if (std::abs (s.mean) > 0.02)
                    {
                        ++dcBad;
                        DETAIL ("        DC %.4f: mode %d B=%.0f ch %d\n", s.mean, mode, (double) B, ch);
                    }
                }
            }
        }
        std::printf ("        (worst |DC| %.5f)\n", worstDc);
        check (finiteBad == 0 && peakBad == 0, "full voice path: finite and master output <= 1.0 for all 15 modes (a = 1, B in {0, 1})");
        check (dcBad == 0, "|DC| <= 0.02 after the voice + master DC blockers for all 15 modes");

        // warpEnable off == warpEnable on with a = 0 (both are the unwarped read)
        setParam (proc, "warpParamB", 0.0f);
        setParam (proc, "warpMode", 0.0f);
        setParam (proc, "warpEnable", 0.0f);
        setParam (proc, "warpAmount", 1.0f);
        const auto off = warpr::tests::render (proc, midi, fs, 256, 0.4);
        setParam (proc, "warpEnable", 1.0f);
        setParam (proc, "warpAmount", 0.0f);
        const auto zero = warpr::tests::render (proc, midi, fs, 256, 0.4);
        setParam (proc, "warpMode", 47.0f);                          // D6 Phase Mirror at warpAmount 0 -> identity
        setParam (proc, "warpAmount", 0.0f);
        const auto ident = warpr::tests::render (proc, midi, fs, 256, 0.4);

        auto toVec = [] (const juce::AudioBuffer<float>& b, int ch) { return std::vector<float> (b.getReadPointer (ch), b.getReadPointer (ch) + b.getNumSamples()); };
        check (nullDb (toVec (off, 0), toVec (zero, 0)) <= -100.0 && nullDb (toVec (off, 1), toVec (zero, 1)) <= -100.0,
               "warpEnable off and (enable on, warpAmount 0) render the same sound (-100 dB null)");
        check (bitEqual (toVec (off, 0), toVec (ident, 0)) && bitEqual (toVec (off, 1), toVec (ident, 1)),
               "Phase Mirror (index 47) at warpAmount 0 is bit-exact dry through the whole voice path");

        // enabling the warp mid-note ramps in (no click)
        PluginProcessor p2;
        setParam (p2, "ampSustain", 1.0f);
        setParam (p2, "ampAttack", 1.0f);
        p2.getSampleStore().publish (makeCycle (2048, 0.5f));
        setParam (p2, "warpMode", 4.0f);
        setParam (p2, "warpAmount", 0.2f);
        setParam (p2, "warpMix", 1.0f);                       // default mix is 0 (stage armed but silent)
        setParam (p2, "warpEnable", 0.0f);
        p2.setPlayConfigDetails (0, 2, fs, 256);
        p2.prepareToPlay (fs, 256);
        juce::AudioBuffer<float> buf (2, 256);
        std::vector<float> trace;
        auto run = [&] (int blocks, juce::MidiBuffer& m)
        {
            for (int b = 0; b < blocks; ++b)
            {
                buf.clear();
                p2.processBlock (buf, m);
                trace.insert (trace.end(), buf.getReadPointer (0), buf.getReadPointer (0) + 256);
                m.clear();
            }
        };
        juce::MidiBuffer on;
        on.addEvent (juce::MidiMessage::noteOn (1, 36, 0.9f), 0);        // 65 Hz: slow, so a click would be visible
        run (60, on);
        const size_t toggleAt = trace.size();
        setParam (p2, "warpEnable", 1.0f);
        juce::MidiBuffer none;
        run (60, none);
        setParam (p2, "warpEnable", 0.0f);
        run (60, none);
        p2.releaseResources();

        const double natural = 2.0 * kPi * 65.4 / fs * 0.5;
        const double enableStep = maxStepOf (trace, toggleAt - 1);
        std::printf ("        (enable/disable toggling: max step %.4f, natural slope %.4f)\n", enableStep, natural);
        check (enableStep < 0.05, "switching warpEnable on / off mid-note ramps (max step < 0.05 FS)");
    }

    //==========================================================================
    // 10. viz feed
    //==========================================================================
    void feedViz (warpr::VizAnalyzer& va, WarprVizFifo& fifo, const std::vector<float>& l, const std::vector<float>& r,
                  std::vector<WarprVizFrame>& frames)
    {
        constexpr size_t block = 256;
        for (size_t pos = 0; pos < l.size(); pos += block)
        {
            const int n = (int) std::min (block, l.size() - pos);
            va.process (l.data() + pos, r.data() + pos, n, fifo, nullptr);

            WarprVizFrame f;
            if (fifo.popLatest (f))
                frames.push_back (f);
        }
    }

    void testVizAnalyzer()
    {
        std::printf ("\n[D2] VizAnalyzer: silence, steady tone, transients, rate, allocations\n");
        constexpr double fs = 48000.0;

        // ---- silence
        {
            WarprVizFifo fifo;
            warpr::VizAnalyzer va;
            va.prepare (fs);
            std::vector<WarprVizFrame> frames;
            const std::vector<float> zeros ((size_t) fs, 0.0f);
            feedViz (va, fifo, zeros, zeros, frames);

            bool allZero = true, tseqConst = true;
            for (const auto& f : frames)
            {
                allZero = allZero && f.rms == 0.0f && f.peakL == 0.0f && f.peakR == 0.0f && f.low == 0.0f && f.high == 0.0f
                          && f.transient == 0.0f && f.numVoices == 0 && f.r == 0.0f && f.theta == 0.0f;
                tseqConst = tseqConst && f.tseq == 0;
            }
            check (frames.size() <= 2 && allZero, "silence: rms == 0, no level, no voices; an idle session emits at most the first frame");
            check (tseqConst, "silence: tseq stays constant");
        }

        // ---- steady tone (L = R) and hard-panned tone
        {
            WarprVizFifo fifo;
            warpr::VizAnalyzer va;
            va.prepare (fs);
            std::vector<WarprVizFrame> frames;
            const int n = (int) (1.2 * fs);
            std::vector<float> tone ((size_t) n), zeros ((size_t) n, 0.0f);
            float peak = 0.0f;
            for (int i = 0; i < n; ++i)
            {
                tone[(size_t) i] = 0.5f * (float) std::sin (2.0 * kPi * 1000.0 * (double) i / fs);
                peak = std::max (peak, std::abs (tone[(size_t) i]));
            }
            feedViz (va, fifo, tone, tone, frames);

            std::printf ("        (%d frames in 1.2 s)\n", (int) frames.size());
            check (frames.size() >= 34 && frames.size() <= 38, "frames arrive rate-limited: ~30 per second (36 +- 2 in 1.2 s)");

            float maxPeakL = 0.0f, maxPeakR = 0.0f;
            for (const auto& f : frames) { maxPeakL = std::max (maxPeakL, f.peakL); maxPeakR = std::max (maxPeakR, f.peakR); }
            check (std::abs (maxPeakL - peak) < 1.0e-5f && std::abs (maxPeakR - peak) < 1.0e-5f, "peaks equal the measured output peak");

            const auto& last = frames.back();
            check (last.rms > 0.30f && last.rms < 0.40f, "rms follower settles near the 0.354 RMS of a 0.5 sine");
            check (last.theta < 1.0e-3f && last.r > 0.45f && last.r < 0.55f, "mono signal: theta == 0, r ~ 0.5");
            check (last.numVoices == 0 && last.tseq <= 1, "no synth attached: no voices; a steady tone is at most one onset");
            bool lfoOk = true;
            for (float p : last.lfoPhase) lfoOk = lfoOk && p == -1.0f;
            check (lfoOk, "lfoPhase slots are -1 (no LFOs yet)");
            check (last.tang > 2.2f && last.tang < 2.7f && last.low >= 0.0f && last.low <= 1.0f && last.high >= 0.0f && last.high <= 1.0f,
                   "tang tracks a 1 kHz tone (zero-crossing rate -> 2.47 rad); low / high within 0..1");

            // hard-panned: M = S -> theta = pi / 4
            WarprVizFifo fifo2;
            warpr::VizAnalyzer va2;
            va2.prepare (fs);
            std::vector<WarprVizFrame> frames2;
            feedViz (va2, fifo2, tone, zeros, frames2);
            check (std::abs (frames2.back().theta - 0.7853982f) < 0.01f, "hard-panned signal: theta == pi / 4");
        }

        // ---- transients: 4 noise bursts
        {
            WarprVizFifo fifo;
            warpr::VizAnalyzer va;
            va.prepare (fs);
            std::vector<WarprVizFrame> frames;

            juce::Random rng (99);
            std::vector<float> sig;
            auto silence = [&] (double seconds) { sig.insert (sig.end(), (size_t) (seconds * fs), 0.0f); };
            silence (0.5);
            for (int hit = 0; hit < 4; ++hit)
            {
                for (int i = 0; i < 240; ++i)
                    sig.push_back (0.5f * (rng.nextFloat() * 2.0f - 1.0f));
                silence (0.3);
            }
            silence (0.4);

            feedViz (va, fifo, sig, sig, frames);

            int framesWithTransient = 0, jumps = 0;
            uint32_t prev = 0;
            bool monotone = true;
            for (const auto& f : frames)
            {
                if (f.transient > 0.0f) ++framesWithTransient;
                if (f.tseq < prev) monotone = false;
                if (f.tseq > prev + 1) ++jumps;
                prev = f.tseq;
            }
            std::printf ("        (tseq %u after 4 bursts, %d frames with transient > 0)\n", (unsigned) frames.back().tseq, framesWithTransient);
            check (frames.back().tseq == 4 && monotone && jumps == 0, "transient test: tseq increments exactly once per hit (4 hits -> 4)");
            check (framesWithTransient == 4, "each hit shows up as `transient > 0` in exactly one frame");
            check (frames.front().tseq == 0 && frames.front().rms == 0.0f, "first frame before any hit: tseq 0, rms 0");
        }

        // ---- allocations
        {
            WarprVizFifo fifo;
            warpr::VizAnalyzer va;
            va.prepare (fs);
            std::vector<float> tone (256);
            for (size_t i = 0; i < tone.size(); ++i)
                tone[i] = 0.3f * (float) std::sin (2.0 * kPi * 440.0 * (double) i / fs);

            std::size_t allocations = 1;
            {
                warpr::tests::ScopedAllocCount counter;
                for (int b = 0; b < 2000; ++b)           // far more pushes than the FIFO holds: the full path is exercised
                    va.process (tone.data(), tone.data(), 256, fifo, nullptr);
                allocations = counter.count();
            }
            check (allocations == 0, "VizAnalyzer::process (including FIFO pushes and drops) performs zero allocations");
        }
    }

    void testVizThroughProcessor()
    {
        std::printf ("\n[D2] viz frames through the processor (real voices, peaks, idle)\n");
        constexpr double fs = 48000.0;

        PluginProcessor proc;
        proc.setPlayConfigDetails (0, 2, fs, 256);
        proc.prepareToPlay (fs, 256);
        warpr::tests::pinForwardLoop (proc);       // gated playback assumed (plugin default: One-Shot)

        juce::AudioBuffer<float> buf (2, 256);
        std::vector<WarprVizFrame> frames;
        float outPeakL = 0.0f, outPeakR = 0.0f;

        auto runBlocks = [&] (int blocks, juce::MidiBuffer m)
        {
            for (int b = 0; b < blocks; ++b)
            {
                buf.clear();
                proc.processBlock (buf, m);
                m.clear();
                outPeakL = std::max (outPeakL, warpr::tests::scan (buf, 0).peak);
                outPeakR = std::max (outPeakR, warpr::tests::scan (buf, 1).peak);

                WarprVizFrame f;
                if (proc.getUiBackend().vizFifo().popLatest (f))
                    frames.push_back (f);
            }
        };

        runBlocks (40, juce::MidiBuffer());                     // idle: at most the first frame
        const size_t idleFrames = frames.size();

        juce::MidiBuffer on;
        on.addEvent (juce::MidiMessage::noteOn (1, 60, 0.9f), 0);
        runBlocks (200, on);                                    // ~1.07 s

        float maxPeakL = 0.0f, maxPeakR = 0.0f;
        bool voiceSeen = false, voiceOk = true;
        for (size_t i = idleFrames; i < frames.size(); ++i)
        {
            const auto& f = frames[i];
            maxPeakL = std::max (maxPeakL, f.peakL);
            maxPeakR = std::max (maxPeakR, f.peakR);

            if (f.numVoices > 0)
            {
                voiceSeen = true;
                for (int v = 0; v < f.numVoices; ++v)
                    voiceOk = voiceOk && f.voices[(size_t) v].pos >= 0.0f && f.voices[(size_t) v].pos <= 1.0f && f.voices[(size_t) v].level >= 0.0f;
            }
        }

        std::printf ("        (%d idle frames, %d frames while playing, peak %.3f)\n", (int) idleFrames, (int) (frames.size() - idleFrames), (double) outPeakL);
        check (idleFrames <= 1, "idle processor emits no frames after the first");
        check (frames.size() - idleFrames >= 28 && frames.size() - idleFrames <= 36, "~30 frames per second while playing");
        check (voiceSeen && voiceOk, "frames carry the sounding voice's playhead (position 0..1, level >= 0)");
        check (std::abs (maxPeakL - outPeakL) < 1.0e-6f && std::abs (maxPeakR - outPeakR) < 1.0e-6f,
               "frame peaks equal the measured master output peaks");
        check (frames.back().rms > 0.01f, "rms is live while a note sounds");

        // release: the organism returns to rest (rms -> 0 needs seconds; just check the frames keep coming and stay finite)
        juce::MidiBuffer off;
        off.addEvent (juce::MidiMessage::noteOff (1, 60), 0);
        runBlocks (400, off);
        bool finite = true;
        for (const auto& f : frames)
            finite = finite && std::isfinite (f.low) && std::isfinite (f.high) && std::isfinite (f.rms) && std::isfinite (f.theta)
                     && std::isfinite (f.r) && std::isfinite (f.tang) && std::isfinite (f.peakL);
        check (finite, "every frame field is finite");
        proc.releaseResources();

        // zero allocations with warp + viz active in processBlock
        PluginProcessor p2;
        setParam (p2, "warpMode", 3.0f);
        setParam (p2, "warpAmount", 0.7f);
        setParam (p2, "warpMix", 1.0f);                       // default mix is 0 (stage armed but silent)
        setParam (p2, "unisonVoices", 4.0f);
        p2.setPlayConfigDetails (0, 2, fs, 256);
        p2.prepareToPlay (fs, 256);
        juce::AudioBuffer<float> b2 (2, 256);
        juce::MidiBuffer m2;
        m2.addEvent (juce::MidiMessage::noteOn (1, 64, 0.9f), 0);
        p2.processBlock (b2, m2);                               // warm-up
        std::size_t allocations = 1;
        {
            warpr::tests::ScopedAllocCount counter;
            for (int i = 0; i < 400; ++i)
                p2.processBlock (b2, m2);
            allocations = counter.count();
        }
        check (allocations == 0, "processBlock with a warp mode, unison 4 and the viz feed performs zero allocations");
        p2.releaseResources();
    }
}

JUCE_END_IGNORE_WARNINGS_GCC_LIKE

//==============================================================================
int runPhaseD2Tests (bool full)
{
    failures = 0;
    printedDetails = 0;
    std::printf ("\n=== Phase D2: warp framework + bending modes + viz feeds (%s) ===\n", full ? "full" : "quick");

    testModeTable();
    testFastMathAndAdaa();
    testPhaseMaps();
    testUniversal (full);
    testStereoSource();
    testIdentityGuarantees();
    testModeSwitch();
    testSweep();
    testDeterminismAndAllocations();
    testProcessorLevel();
    testVizAnalyzer();
    testVizThroughProcessor();

    return failures;
}
