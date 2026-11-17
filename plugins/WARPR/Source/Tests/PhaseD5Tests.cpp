/*
    WARPR - phase D5 test block (filters).  This file holds D5a: the standard filter family, types 0-19
    (TPT one-pole / SVF cascades, band-pass, notch, peak, shelves), the per-voice FilterStage and the response display.
    D5b (special linear types 20-27: combs, all-passes, formant / vowel banks, phaser) follows below the D5a sections
    ([D5b] headings): table, comb / all-pass / formant / phaser formulas, analytic vs measured IR (magnitude AND complex),
    measured comb teeth, stability, bypass + open-LP rule, D5a<->D5b type switching, sweeps + walk, key tracking, backend
    display, zero allocations + determinism.  D5c (nonlinear types 28-33: Res LP / Res HP / Dist LP / Scream / S&H /
    Ring Mod, local 2x) follows below the D5b sections ([D5c] headings): table, small-signal formulas, measured tiny-signal
    impulse response vs analytic, ladder self-oscillation, character (hold rate, ring-mod lines, AM/RM, carrier continuity),
    alias comparison local 2x on vs off, stability torture, bypass + L/R symmetry, 366 type-switch transitions, sweeps + walk,
    backend display, zero allocations (half-band buffers) + determinism.

    Called from RunTests.cpp main():   failures += runPhaseD5Tests (full);

    Contents (plan.md T6 + the D5 test criteria)
      1. type table          34 names == choices::filterType, 0-19 implemented / linear, 20-33 flagged for D5b / D5c
      2. design formulas     corner gains, asymptotic slopes (6 dB/oct x order), peak / shelf / notch gains, key-track law,
                             resonance / Q / gain mappings, identity design for unimplemented types
      3. response            analytic FilterResponse vs the impulse-response FFT measured THROUGH FilterStage, 20 types x
                             cutoffs x (res, morph) [x sample rates in --full], tolerance 0.5 dB (20 Hz .. 0.45 fs)
      4. stability sweep     cutoff {20, 1k, 20k} x res {0, .5, 1} x drive {0, 24} x morph {0, 1} [x fs 44.1 .. 192 in
                             --full] x 20 types: finite, |y| < 8
      5. bypass              filterEnable off / mix 0 / "open low-pass" are BIT-EXACT nulls (class and processor level),
                             enable ramp is click-free, L/R symmetry, drive: identity at 0 dB, unity small-signal gain,
                             saturates peaks
      6. type switching      all 20 x 19 transitions: no step above the crossfade bound, the new type is reached;
                             processor-level switch during a sustained note
      7. sweeps              fast cutoff sweeps: amplitude monotone, no step above the sine's own slope (zipper-free);
                             random modulation walk (10k+ parameter changes) stays finite and bounded
      8. key tracking        2 x per octave (pure law) and measured through the voice (note 60 vs 72)
      9. backend             computeFilterResponse: 256 points, equals FilterResponse, flat for open LP / unimplemented
                             types, < 1 ms; Revision::filterResponse bumps on every one of the 8 filter parameters
     10. processor           zero allocations with an active filter (incl. type changes), determinism with mid-note edits

    Measured numbers are always printed.
*/

#include "PluginProcessor.h"
#include "DSP/FilterDesign.h"
#include "DSP/FilterStage.h"
#include "DSP/FilterTypeTable.h"
#include "Parameters/ParamTable.h"
#include "Tests/AllocHook.h"
#include "Tests/OfflineRender.h"
#include "Tests/Scanners.h"
#include "Tests/WarpTestKit.h"

#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>
#include <functional>
#include <vector>

// Bit-exact float equality is the point of several assertions in this file (bypass nulls, symmetry, determinism), so the
// warning is suppressed here only.
JUCE_BEGIN_IGNORE_WARNINGS_GCC_LIKE ("-Wfloat-equal")

int runPhaseD5Tests (bool full);

namespace
{
    using namespace warpr::tests::kit;
    using warpr::FilterDesign;
    using warpr::FilterResponse;
    using warpr::FilterStage;
    using warpr::FilterTargets;
    namespace fd = warpr::filterdesign;

    int failures = 0;
    int printedDetails = 0;

    constexpr int kD5aTypes = 20;      // the standard family 0-19 (D5a loops); D5b adds 20-27 in its own sections

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

    //==========================================================================
    // signals and stage runners
    //==========================================================================
    std::vector<float> makeNoise (int n, float amp, int seed)
    {
        juce::Random rng (seed);
        std::vector<float> v (ix (n));
        for (auto& s : v)
            s = amp * (rng.nextFloat() * 2.0f - 1.0f);
        return v;
    }

    std::vector<float> makeSine (int n, double hz, double fs, float amp)
    {
        std::vector<float> v (ix (n));
        for (int i = 0; i < n; ++i)
            v[ix (i)] = amp * (float) std::sin (2.0 * kPi * hz * (double) i / fs);
        return v;
    }

    /** Naive (aliasing) saw: rich in harmonics, which is exactly what the filters should be exercised with. */
    std::vector<float> makeSaw (int n, double hz, double fs, float amp)
    {
        std::vector<float> v (ix (n));
        double phase = 0.0;
        for (int i = 0; i < n; ++i)
        {
            v[ix (i)] = amp * (float) (2.0 * phase - 1.0);
            phase += hz / fs;
            phase -= std::floor (phase);
        }
        return v;
    }

    /** Smooth multi-sine programme (220 Hz + 1.7 kHz + 5.3 kHz): broadband enough to separate filter types, but without
        edges, so the click tests measure the filters' own steps rather than the signal's. */
    std::vector<float> makeMix (int n, double fs)
    {
        std::vector<float> v (ix (n));
        for (int i = 0; i < n; ++i)
        {
            const double t = (double) i / fs;
            v[ix (i)] = (float) (0.3 * std::sin (2.0 * kPi * 220.0 * t) + 0.2 * std::sin (2.0 * kPi * 1700.0 * t + 0.7)
                                 + 0.1 * std::sin (2.0 * kPi * 5300.0 * t + 1.9));
        }
        return v;
    }

    FilterTargets mk (int type, double fc, double res = 0.0, double morph = 0.0, double driveDb = 0.0,
                      double mix = 1.0, bool enable = true)
    {
        FilterTargets t;
        t.enable = enable;
        t.type = type;
        t.cutoffHz = (float) fc;
        t.res = (float) res;
        t.morph = (float) morph;
        t.driveDb = (float) driveDb;
        t.mix = (float) mix;
        return t;
    }

    /** Runs `in` (both channels) through a fresh, snapped stage in 32-sample chunks.  `fn (chunkIndex, targets)` may
        edit the targets before every chunk.  Returns the left channel; the right channel goes to `rightOut`. */
    template <class Fn>
    std::vector<float> runStage (double fs, const FilterTargets& start, const std::vector<float>& in, Fn&& fn,
                                 std::vector<float>* rightOut = nullptr)
    {
        FilterStage st;
        st.prepare (fs);
        st.snap (start);

        FilterTargets t = start;
        std::vector<float> l = in, r = in;
        int ci = 0;

        for (size_t i = 0; i < in.size(); i += 32, ++ci)
        {
            const int n = (int) std::min<size_t> (32, in.size() - i);
            fn (ci, t);
            st.process (l.data() + i, r.data() + i, n, t);
        }

        if (rightOut != nullptr)
            *rightOut = r;

        return l;
    }

    std::vector<float> runStageConst (double fs, const FilterTargets& t, const std::vector<float>& in)
    {
        return runStage (fs, t, in, [] (int, FilterTargets&) {});
    }

    double maxStepIn (const std::vector<float>& x, size_t from, size_t to)
    {
        double m = 0.0;
        for (size_t i = from + 1; i < to && i < x.size(); ++i)
            m = std::max (m, (double) std::abs (x[i] - x[i - 1]));
        return m;
    }

    double magAt (int type, double fc, double res, double morph, double fs, double hz)
    {
        FilterDesign d;
        fd::make (type, fc, res, morph, fs, d);
        return std::abs (fd::response (d, hz, fs));
    }

    double dbOf (double mag) { return 20.0 * std::log10 (std::max (mag, 1.0e-12)); }

    //==========================================================================
    // 1. type table
    //==========================================================================
    void testTable()
    {
        std::printf ("\n[D5a] filter type table\n");

        int nameMismatch = 0, implWrong = 0, buildWrong = 0, linearWrong = 0;

        for (int i = 0; i < warpr::kNumFilterTypes; ++i)
        {
            const auto& info = warpr::kFilterTypes[ix (i)];

            if (std::strcmp (info.name, warpr::choices::filterType[i]) != 0)
                ++nameMismatch;
            if (info.implemented != (i < warpr::kNumImplementedFilterTypes))
                ++implWrong;

            const auto expectedBuild = i < kD5aTypes ? warpr::FilterBuild::D5a : (i < 28 ? warpr::FilterBuild::D5b : warpr::FilterBuild::D5c);
            if (info.build != expectedBuild)
                ++buildWrong;
            if (info.linear != (i < 28))
                ++linearWrong;
        }

        check (nameMismatch == 0, "all 34 table names equal choices::filterType (single source)");
        check (implWrong == 0 && warpr::kNumImplementedFilterTypes == 34, "all 34 types are implemented (D5a 0-19, D5b 20-27, D5c 28-33)");
        check (buildWrong == 0, "build phases: 0-19 D5a, 20-27 D5b, 28-33 D5c");
        check (linearWrong == 0, "0-27 are linear, 28-33 nonlinear");
        check (std::strcmp (warpr::kFilterTypes[3].name, "LP 24") == 0 && std::strcmp (warpr::kFilterTypes[19].name, "High Shelf") == 0
               && std::strcmp (warpr::kFilterTypes[33].name, "Ring Mod") == 0,
               "spot names: 3 = LP 24, 19 = High Shelf, 33 = Ring Mod");
        check (warpr::filterTypeInfo (-5).name == warpr::kFilterTypes[0].name && warpr::filterTypeInfo (999).name == warpr::kFilterTypes[33].name,
               "filterTypeInfo clamps out-of-range indices");
    }

    //==========================================================================
    // 2. design formulas
    //==========================================================================
    void testFormulas()
    {
        std::printf ("\n[D5a] design formulas (corner gains, slopes, peak / shelf / notch, mappings)\n");
        constexpr double fs = 48000.0;
        const int order[6] = { 1, 2, 3, 4, 6, 8 };

        // Butterworth: -3.01 dB at the cutoff for every LP / HP cascade (res 0, morph 0)
        int cornerBad = 0;
        for (int t = 0; t < 12; ++t)
        {
            const double m = magAt (t, 1000.0, 0.0, 0.0, fs, 1000.0);
            if (std::abs (m - 0.70710678) > 1.0e-3)
            {
                ++cornerBad;
                DETAIL ("        type %d: |H(fc)| = %.5f (expected 0.70711)\n", t, m);
            }
        }
        check (cornerBad == 0, "LP 6..48 and HP 6..48 are -3.01 dB at the cutoff (Butterworth)");

        // passbands
        int passBad = 0;
        for (int t = 0; t < 6; ++t)
            if (std::abs (magAt (t, 1000.0, 0.0, 0.0, fs, 10.0) - 1.0) > 1.0e-3) ++passBad;
        for (int t = 6; t < 12; ++t)
            if (std::abs (magAt (t, 1000.0, 0.0, 0.0, 192000.0, 40000.0) - 1.0) > 2.0e-2) ++passBad;
        check (passBad == 0, "LP passes DC with unity gain, HP passes the top of the band with unity gain");

        // asymptotic slope: 6.02 dB per octave per order.  Taken at 192 kHz so the bilinear warping stays below 0.02 dB/order.
        int slopeBad = 0;
        double worstSlope = 0.0;
        for (int i = 0; i < 6; ++i)
        {
            const double expected = 6.0206 * order[i];
            // The HP pair sits one octave at fc/16..fc/8: close enough to the corner that even the order-8
            // stopband magnitude ((1/16)^8 = 2.3e-10) stays above the dbOf floor of 1e-12 - at 100/200 Hz the
            // order-8 readings were BOTH floored and the slope measured 0.00.  The Butterworth correction at
            // f/fc = 1/8 is (f/fc)^(2n) relative, < 0.14 dB even for order 1.
            const double lp = dbOf (magAt (i, 200.0, 0.0, 0.0, 192000.0, 1600.0)) - dbOf (magAt (i, 200.0, 0.0, 0.0, 192000.0, 3200.0));
            const double hp = dbOf (magAt (6 + i, 6400.0, 0.0, 0.0, 192000.0, 800.0)) - dbOf (magAt (6 + i, 6400.0, 0.0, 0.0, 192000.0, 400.0));
            worstSlope = std::max ({ worstSlope, std::abs (lp - expected), std::abs (hp - expected) });
            if (std::abs (lp - expected) > 0.5 || std::abs (hp - expected) > 0.5)
            {
                ++slopeBad;
                DETAIL ("        order %d: LP slope %.2f dB/oct, HP slope %.2f dB/oct (expected %.2f)\n", order[i], lp, hp, expected);
            }
        }
        std::printf ("        (worst slope deviation %.3f dB/oct)\n", worstSlope);
        check (slopeBad == 0, "LP / HP slopes are 6.02 dB/oct x order (6, 12, 18, 24, 36, 48 dB/oct) within 0.5 dB");

        // resonance raises the peak at the cutoff: Q_top = Q0 (1 + 14 res^1.5)
        const double lp12Res1 = magAt (1, 1000.0, 1.0, 0.0, fs, 1000.0);
        check (std::abs (lp12Res1 - 0.70710678 * 15.0) < 0.05, "LP 12, res 1: |H(fc)| = Q0 x 15 = 10.6 (+20.5 dB)");
        const double lp12Bessel = magAt (1, 1000.0, 0.0, 1.0, fs, 1000.0);
        check (std::abs (lp12Bessel - 0.57735027) < 1.0e-3, "LP 12, morph 1: Bessel-like Q 0.5774 -> |H(fc)| = 0.577");

        // band-pass
        check (std::abs (magAt (13, 1000.0, 0.5, 0.0, fs, 1000.0) - 1.0) < 1.0e-6, "BP 12 morph 0: constant-peak, 0 dB at fc at any Q");
        check (std::abs (magAt (13, 1000.0, 0.5, 1.0, fs, 1000.0) - 0.5 * std::pow (40.0, 0.5)) < 1.0e-6,
               "BP 12 morph 1: constant-skirt, peak gain = Q (0.5 x 40^res)");
        bool bp6 = true;
        for (double mo : { 0.0, 0.5, 1.0 })
            bp6 = bp6 && std::abs (magAt (12, 1000.0, 0.0, mo, fs, 1000.0) - 1.0) < 1.0e-6;
        check (bp6, "BP 6: 0 dB at fc for every corner spacing (normalised)");
        check (std::abs (magAt (14, 1000.0, 0.5, 0.0, fs, 1000.0) - 1.0) < 1.0e-6, "BP 24 morph 0: both sections at fc, 0 dB peak");
        // Q = 0.5 x 40^0.5 = 3.16; each section sits half an octave... (x = sqrt2 from the centre): 1 / sqrt(1 + Q^2 (x - 1/x)^2) = 0.41 each, 0.17 together
        check (magAt (14, 1000.0, 0.5, 1.0, fs, 1000.0) < 0.3 && magAt (14, 1000.0, 0.5, 1.0, fs, 1000.0) > 0.08,
               "BP 24 morph 1: staggered by +-0.5 octave (the centre dips well below the 0 dB section peaks)");
        check (dbOf (magAt (13, 1000.0, 0.0, 0.0, fs, 10.0)) < -25.0 && dbOf (magAt (13, 1000.0, 0.0, 0.0, fs, 20000.0)) < -20.0,
               "BP 12 rejects both ends of the spectrum");

        // notch family
        check (magAt (15, 1000.0, 0.5, 0.0, fs, 1000.0) < 1.0e-9, "Notch: exactly zero at fc");
        check (std::abs (magAt (16, 1000.0, 0.5, 0.5, fs, 1000.0) - 0.5) < 1.0e-9 && std::abs (magAt (16, 1000.0, 0.5, 0.0, fs, 1000.0) - 1.0) < 1.0e-9
               && magAt (16, 1000.0, 0.5, 1.0, fs, 1000.0) < 1.0e-9,
               "Var Notch: depth = morph (0 bypass, 0.5 half, 1 full notch)");

        // peak and shelves: res is a bipolar gain
        check (std::abs (dbOf (magAt (17, 1000.0, 1.0, 0.0, fs, 1000.0)) - 18.0) < 0.01 && std::abs (dbOf (magAt (17, 1000.0, 0.0, 0.0, fs, 1000.0)) + 18.0) < 0.01,
               "Peak: res 1 = +18 dB, res 0 = -18 dB at fc");
        bool flat = true;
        for (double f : { 20.0, 300.0, 1000.0, 5000.0, 19000.0 })
            flat = flat && std::abs (magAt (17, 1000.0, 0.5, 0.2, fs, f) - 1.0) < 1.0e-12
                        && std::abs (magAt (18, 1000.0, 0.5, 0.2, fs, f) - 1.0) < 1.0e-12
                        && std::abs (magAt (19, 1000.0, 0.5, 0.2, fs, f) - 1.0) < 1.0e-12;
        check (flat, "Peak / shelves at res 0.5 (0 dB) are exactly flat");
        check (std::abs (dbOf (magAt (17, 1000.0, 1.0, 0.0, fs, 20.0))) < 0.3 && std::abs (dbOf (magAt (17, 1000.0, 1.0, 0.0, fs, 18000.0))) < 0.3,
               "Peak (Q 4) leaves the far skirts untouched");
        check (magAt (17, 1000.0, 1.0, 1.0, fs, 250.0) > magAt (17, 1000.0, 1.0, 0.0, fs, 250.0), "Peak morph widens the bell (Q 4 -> 0.3)");

        const double lsLow = dbOf (magAt (18, 1000.0, 1.0, 0.5, 192000.0, 10.0));
        const double lsMid = dbOf (magAt (18, 1000.0, 1.0, 0.5, 192000.0, 1000.0));
        const double lsHigh = dbOf (magAt (18, 1000.0, 1.0, 0.5, 192000.0, 40000.0));
        std::printf ("        (low shelf +18 dB: 10 Hz %.2f dB, 1 kHz %.2f dB, 40 kHz %.2f dB)\n", lsLow, lsMid, lsHigh);
        check (std::abs (lsLow - 18.0) < 0.1 && std::abs (lsMid - 9.0) < 0.2 && std::abs (lsHigh) < 0.2,
               "Low shelf +18 dB: +18 below, half the dB (+9) at the corner, 0 above");
        const double hsLow = dbOf (magAt (19, 1000.0, 1.0, 0.5, 192000.0, 10.0));
        const double hsMid = dbOf (magAt (19, 1000.0, 1.0, 0.5, 192000.0, 1000.0));
        const double hsHigh = dbOf (magAt (19, 1000.0, 1.0, 0.5, 192000.0, 40000.0));
        std::printf ("        (high shelf +18 dB: 10 Hz %.2f dB, 1 kHz %.2f dB, 40 kHz %.2f dB)\n", hsLow, hsMid, hsHigh);
        check (std::abs (hsLow) < 0.2 && std::abs (hsMid - 9.0) < 0.2 && std::abs (hsHigh - 18.0) < 0.1,
               "High shelf +18 dB: 0 below, +9 at the corner, +18 above");
        const double lsCut = dbOf (magAt (18, 1000.0, 0.0, 0.5, 192000.0, 10.0));
        check (std::abs (lsCut + 18.0) < 0.1, "Low shelf res 0 = -18 dB cut");

        // mappings
        check (std::abs (fd::resonanceBoost (0.0) - 1.0) < 1.0e-12 && std::abs (fd::resonanceBoost (1.0) - 15.0) < 1.0e-12,
               "resonanceBoost: 1 at res 0, 15 at res 1");
        check (std::abs (fd::bandQ (0.0) - 0.5) < 1.0e-12 && std::abs (fd::bandQ (1.0) - 20.0) < 1.0e-12, "bandQ: 0.5 .. 20");
        check (fd::shelfPeakGainDb (0.5) == 0.0 && fd::shelfPeakGainDb (0.0) == -18.0 && fd::shelfPeakGainDb (1.0) == 18.0,
               "shelfPeakGainDb: 36 (res - 0.5), clamped +-18");
        check (std::abs (fd::clampCutoff (1.0, fs) - 10.0) < 1.0e-12 && std::abs (fd::clampCutoff (1.0e6, fs) - 0.45 * fs) < 1.0e-9,
               "cutoff clamp: [10 Hz, 0.45 fs]");

        // key tracking law
        check (std::abs (fd::keyTrackedCutoff (1000.0, 1.0, 72.0) - 2000.0) < 1.0e-9 && std::abs (fd::keyTrackedCutoff (1000.0, 1.0, 48.0) - 500.0) < 1.0e-9
               && std::abs (fd::keyTrackedCutoff (1000.0, 0.5, 72.0) - 1000.0 * std::sqrt (2.0)) < 1.0e-9
               && std::abs (fd::keyTrackedCutoff (1000.0, 0.0, 100.0) - 1000.0) < 1.0e-12 && std::abs (fd::keyTrackedCutoff (1000.0, 1.0, 60.0) - 1000.0) < 1.0e-12,
               "key tracking: x2 per octave at 1.0, x sqrt2 at 0.5, none at 0, anchored at note 60");

        // open low-pass convention
        check (fd::isOpenLowpass (3, 20000.0, 0.0, 0.0) && fd::isOpenLowpass (0, 19999.6, 0.005, 0.005)
               && ! fd::isOpenLowpass (3, 19998.0, 0.0, 0.0) && ! fd::isOpenLowpass (3, 20000.0, 0.5, 0.0) && ! fd::isOpenLowpass (3, 20000.0, 0.0, 6.0)
               && ! fd::isOpenLowpass (6, 20000.0, 0.0, 0.0) && ! fd::isOpenLowpass (17, 20000.0, 0.0, 0.0),
               "open low-pass: LP types at >= 19999.5 Hz with res < 0.01 and drive < 0.01 dB only");

        // out-of-range indices: identity design + flat response
        int identityBad = 0;
        for (int t : { -1, warpr::kNumFilterTypes, 99 })
        {
            FilterDesign d;
            fd::make (t, 1000.0, 0.7, 0.3, fs, d);
            if (d.numSections != 0 || d.gain != 1.0 || std::abs (fd::response (d, 1234.0, fs) - std::complex<double> (1.0, 0.0)) > 1.0e-15
                || FilterResponse::magnitudeDbAt (t, 1000.0, 0.7, 0.0, 0.3, fs, 1234.0) != 0.0)
                ++identityBad;
        }
        check (identityBad == 0, "out-of-range type indices: identity design and flat 0 dB response");
    }

    //==========================================================================
    // 3. analytic response vs measured impulse response
    //==========================================================================
    struct IrError
    {
        double worstDb = 0.0, worstAbs = 0.0, worstHz = 0.0;
    };

    /** Impulse through FilterStage (float path, full process()) -> FFT -> compare with the analytic response of the SAME
        coefficients over 20 Hz .. 0.45 fs. */
    IrError irError (int type, double fc, double res, double morph, double fs, int order = 16)
    {
        // Constant TIME window, not constant sample count: at res 1 the top section reaches Q ~38, whose
        // ring at a 250 Hz cutoff needs ~0.34 s to decay 60 dB.  2^16 samples is 1.37 s at 48 kHz but only
        // 0.34 s at 192 kHz - exactly the truncation that produced ~5e-3 of spectral leakage here.
        while ((1 << order) < (int) (0.6 * fs))
            ++order;

        const int size = 1 << order;
        const FilterTargets t = mk (type, fc, res, morph);

        FilterStage st;
        st.prepare (fs);
        st.snap (t);

        std::vector<float> l (ix (size), 0.0f), r (ix (size), 0.0f);
        l[0] = r[0] = 1.0f;

        for (int i = 0; i < size; i += 32)
            st.process (l.data() + i, r.data() + i, 32, t);

        juce::dsp::FFT fft (order);
        std::vector<float> data (ix (size) * 2, 0.0f);
        std::copy (l.begin(), l.end(), data.begin());
        fft.performFrequencyOnlyForwardTransform (data.data());

        FilterDesign d;
        fd::make (type, (double) t.cutoffHz, (double) t.res, (double) t.morph, fs, d);

        IrError e;
        const double binHz = fs / (double) size;
        const int lo = (int) std::ceil (20.0 / binHz), hi = (int) std::floor (0.45 * fs / binHz);

        for (int k = lo; k <= hi; ++k)
        {
            const double f = (double) k * binHz;
            const double ma = std::abs (fd::response (d, f, fs));
            const double mm = (double) data[ix (k)];

            // the absolute gate is a STOPBAND gate (where dB is meaningless); on the passband and on
            // resonance peaks (|H| up to ~38 at res 1) the relative dB gate below is the right measure,
            // and absolute error there legitimately scales with the peak height
            if (ma <= 0.01)
                e.worstAbs = std::max (e.worstAbs, std::abs (mm - ma));

            if (ma > 0.01)
            {
                const double db = mm > 0.0 ? std::abs (20.0 * std::log10 (mm / ma)) : 200.0;
                if (db > e.worstDb)
                {
                    e.worstDb = db;
                    e.worstHz = f;
                }
            }
        }

        return e;
    }

    void testResponse (bool full)
    {
        std::printf ("\n[D5a] analytic response vs measured impulse-response FFT (through FilterStage), 0.5 dB gate\n");

        struct Rm { double res, morph; };
        const std::vector<Rm> combos = full ? std::vector<Rm> { { 0, 0 }, { 0, 0.35 }, { 0, 1 }, { 0.5, 0 }, { 0.5, 0.35 }, { 0.5, 1 }, { 1, 0 }, { 1, 0.35 }, { 1, 1 } }
                                            : std::vector<Rm> { { 0, 0 }, { 0.5, 0.35 }, { 1, 1 } };
        const std::vector<double> fsList = full ? std::vector<double> { 44100.0, 48000.0, 96000.0, 192000.0 } : std::vector<double> { 48000.0 };
        const std::vector<double> cutoffs { 250.0, 1000.0, 6000.0, 15000.0 };

        int cases = 0, bad = 0;
        double overallWorst = 0.0;
        std::array<double, 20> worstPerType {};

        for (const double fs : fsList)
        {
            for (int type = 0; type < kD5aTypes; ++type)
            {
                for (const double fc : cutoffs)
                {
                    for (const auto& rm : combos)
                    {
                        const auto e = irError (type, fc, rm.res, rm.morph, fs);
                        ++cases;
                        worstPerType[ix (type)] = std::max (worstPerType[ix (type)], e.worstDb);
                        overallWorst = std::max (overallWorst, e.worstDb);

                        if (e.worstDb > 0.5 || e.worstAbs > 5.0e-4)
                        {
                            ++bad;
                            DETAIL ("        FAIL type %d (%s) fs %.0f fc %.0f res %.2f morph %.2f: worst %.3f dB at %.0f Hz, abs %.2e\n",
                                    type, warpr::kFilterTypes[ix (type)].name, fs, fc, rm.res, rm.morph, e.worstDb, e.worstHz, e.worstAbs);
                        }
                    }
                }
            }
        }

        if (full)
        {
            // very low cutoffs need a longer window; moderate resonance only (the ring-down must fit in 2^17 samples)
            for (int type = 0; type < kD5aTypes; ++type)
                for (const double fc : { 40.0, 80.0 })
                    for (double res : { 0.0, 0.5 })
                    {
                        const auto e = irError (type, fc, res, 0.35, 48000.0, 17);
                        ++cases;
                        overallWorst = std::max (overallWorst, e.worstDb);
                        if (e.worstDb > 0.5 || e.worstAbs > 5.0e-4)
                        {
                            ++bad;
                            DETAIL ("        FAIL (low fc) type %d fc %.0f res %.2f: worst %.3f dB at %.0f Hz\n", type, fc, res, e.worstDb, e.worstHz);
                        }
                    }
        }

        std::printf ("        (%d cases; worst deviation per type, dB:", cases);
        for (int type = 0; type < 20; ++type)
            std::printf (" %d:%.3f", type, worstPerType[ix (type)]);
        std::printf (")\n");
        check (bad == 0, "all 20 linear types: stage impulse response matches the analytic response within 0.5 dB (20 Hz .. 0.45 fs)");
        std::printf ("        (overall worst %.4f dB)\n", overallWorst);
    }

    //==========================================================================
    // 4. stability sweep
    //==========================================================================
    void testStability (bool full)
    {
        std::printf ("\n[D5a] stability sweep (cutoff x res x drive x morph x fs, 20 types)\n");

        const std::vector<double> fsList = full ? std::vector<double> { 44100.0, 48000.0, 96000.0, 192000.0 } : std::vector<double> { 48000.0 };
        const int len = full ? 24000 : 6000;
        const auto input = makeNoise (len, 0.25f, 4242);

        int runs = 0, nonFinite = 0, tooLoud = 0;
        double worstPeak = 0.0;

        for (const double fs : fsList)
            for (int type = 0; type < kD5aTypes; ++type)
                for (const double fc : { 20.0, 1000.0, 20000.0 })
                    for (const double res : { 0.0, 0.5, 1.0 })
                        for (const double drv : { 0.0, 24.0 })
                            for (const double morph : { 0.0, 1.0 })
                            {
                                const auto y = runStageConst (fs, mk (type, fc, res, morph, drv), input);
                                ++runs;

                                if (! allFinite (y))
                                {
                                    ++nonFinite;
                                    DETAIL ("        non-finite: type %d fs %.0f fc %.0f res %.1f drive %.0f morph %.0f\n", type, fs, fc, res, drv, morph);
                                    continue;
                                }

                                const double pk = peakOf (y);
                                worstPeak = std::max (worstPeak, pk);

                                if (pk >= 8.0)
                                {
                                    ++tooLoud;
                                    DETAIL ("        |y| = %.2f: type %d fs %.0f fc %.0f res %.1f drive %.0f morph %.0f\n", pk, type, fs, fc, res, drv, morph);
                                }
                            }

        std::printf ("        (%d runs, noise at -12 dBFS, worst peak %.3f)\n", runs, worstPeak);
        check (nonFinite == 0, "no NaN / inf anywhere in the sweep");
        check (tooLoud == 0, "|y| < 8 everywhere (resonators bounded; constant-skirt BP 12 at Q 20 is the loudest case)");
    }

    //==========================================================================
    // 5. bypass, symmetry, drive
    //==========================================================================
    struct Render
    {
        std::vector<float> l, r;
    };

    Render renderBlocks (PluginProcessor& p, double fs, int nBlocks, int block, int note, const std::function<void (int)>& beforeBlock)
    {
        p.setPlayConfigDetails (0, 2, fs, block);
        p.prepareToPlay (fs, block);

        juce::AudioBuffer<float> buf (2, block);
        Render out;
        out.l.reserve (ix (nBlocks) * ix (block));
        out.r.reserve (ix (nBlocks) * ix (block));

        for (int b = 0; b < nBlocks; ++b)
        {
            if (beforeBlock)
                beforeBlock (b);

            juce::MidiBuffer midi;
            if (b == 0)
                midi.addEvent (juce::MidiMessage::noteOn (1, note, 0.9f), 0);

            p.processBlock (buf, midi);
            out.l.insert (out.l.end(), buf.getReadPointer (0), buf.getReadPointer (0) + block);
            out.r.insert (out.r.end(), buf.getReadPointer (1), buf.getReadPointer (1) + block);
        }

        p.releaseResources();
        return out;
    }

    void prepSustained (PluginProcessor& p)
    {
        warpr::tests::pinForwardLoop (p);     // the D5 corpus assumes GATED playback (plugin default: One-Shot)
        setParam (p, "ampAttack", 1.0f);
        setParam (p, "ampSustain", 1.0f);
    }

    void testBypass()
    {
        std::printf ("\n[D5a] bypass nulls, enable ramp, L/R symmetry, drive\n");
        constexpr double fs = 48000.0;
        const auto saw = makeSaw (9600, 110.0, fs, 0.5f);
        const auto noise = makeNoise (9600, 0.5f, 5);

        // ---- class level: untouched samples
        {
            FilterStage st;
            st.prepare (fs);
            const FilterTargets wild = mk (9, 120.0, 1.0, 1.0, 24.0, 1.0, false);          // enable OFF, HP 24 @ 120 Hz, res 1, drive 24
            st.snap (wild);

            std::vector<float> l = noise, r = noise;
            for (size_t i = 0; i < l.size(); i += 32)
                st.process (l.data() + i, r.data() + i, 32, wild);

            check (bitEqual (l, noise) && bitEqual (r, noise) && st.isParked(), "filterEnable off: output is bit-identical to the input (stage parked)");

            FilterStage st2;
            st2.prepare (fs);
            const FilterTargets mix0 = mk (13, 300.0, 1.0, 0.5, 12.0, 0.0, true);          // enabled but mix 0
            st2.snap (mix0);
            l = noise;
            r = noise;
            for (size_t i = 0; i < l.size(); i += 32)
                st2.process (l.data() + i, r.data() + i, 32, mix0);
            check (bitEqual (l, noise) && bitEqual (r, noise), "filterMix 0: bit-identical to the input");

            FilterStage st3;
            st3.prepare (fs);
            const FilterTargets open = mk (3, 20000.0);                                      // the default patch: LP 24 fully open
            st3.snap (open);
            l = noise;
            r = noise;
            for (size_t i = 0; i < l.size(); i += 32)
                st3.process (l.data() + i, r.data() + i, 32, open);
            check (bitEqual (l, noise) && bitEqual (r, noise) && st3.isParked(), "default patch (LP 24 @ 20 kHz, open): bit-exact transparent, zero cost");

            const auto driven = runStageConst (fs, mk (3, 20000.0, 0.0, 0.0, 12.0), noise);       // drive > 0 closes the open rule
            check (! bitEqual (driven, noise), "open low-pass with drive > 0 still processes (the saturator is audible)");

            const auto tone = makeSine (9600, 1000.0, fs, 0.5f);
            const auto almost = runStageConst (fs, mk (3, 19000.0), tone);
            double rt = 0.0, ra = 0.0;
            for (size_t i = 2400; i < tone.size(); ++i)
            {
                rt += (double) tone[i] * (double) tone[i];
                ra += (double) almost[i] * (double) almost[i];
            }
            check (! bitEqual (almost, tone) && std::abs (std::sqrt (ra / rt) - 1.0) < 1.0e-3,
                   "LP 24 at 19 kHz is a real filter (differs from the input) yet leaves a 1 kHz tone's level untouched");
        }

        // ---- class level: enable toggled mid-run -> 5 ms linear ramp, then exact
        {
            const FilterTargets on = mk (7, 800.0, 0.3);
            const auto smooth = makeMix (9600, fs);
            const auto ya = runStageConst (fs, on, smooth);
            const int offAt = 160;                                                          // chunk index (5120 samples)
            const auto y = runStage (fs, on, smooth, [&] (int ci, FilterTargets& t) { if (ci >= offAt) t.enable = false; });

            bool tailExact = true;
            for (size_t i = (size_t) offAt * 32 + 256; i < smooth.size(); ++i)
                tailExact = tailExact && y[i] == smooth[i];

            double diff = 0.0;
            for (size_t i = (size_t) offAt * 32; i < (size_t) offAt * 32 + 256; ++i)
                diff = std::max (diff, (double) std::abs (ya[i] - smooth[i]));

            const double natural = std::max (maxStepIn (ya, 4000, 6000), maxStepIn (smooth, 4000, 6000));
            const double step = maxStepIn (y, (size_t) offAt * 32 - 64, (size_t) offAt * 32 + 600);
            const double bound = natural + 2.0 * diff / 240.0 + 1.0e-3;
            std::printf ("        (enable off mid-run: max step %.5f, natural %.5f, wet/dry difference %.4f, bound %.5f)\n", step, natural, diff, bound);
            check (tailExact, "after the 5 ms ramp the disabled stage is bit-exact again");
            check (step <= bound && diff > 0.05, "disabling mid-note ramps the mix over 5 ms (no click)");
        }

        // ---- class level: left / right symmetry
        {
            int asym = 0;
            for (int type : { 1, 3, 5, 9, 12, 14, 16, 17, 18, 19, 21 })
            {
                std::vector<float> r;
                const auto l = runStage (fs, mk (type, 900.0, 0.4, 0.3, type == 5 ? 18.0 : 0.0), saw,
                                         [type] (int ci, FilterTargets& t) { if (ci == 100) t.type = (type + 5) % 20; }, &r);
                if (! bitEqual (l, r))
                    ++asym;
            }
            check (asym == 0, "identical L / R input gives bit-identical L / R output (incl. drive and a mid-run type switch)");
        }

        // ---- drive
        {
            const auto quiet = makeSine (9600, 1000.0, fs, 0.002f);
            const auto base = runStageConst (fs, mk (1, 12000.0), quiet);
            const auto driven = runStageConst (fs, mk (1, 12000.0, 0.0, 0.0, 24.0), quiet);
            double rb = 0.0, rd = 0.0;
            for (size_t i = 2400; i < quiet.size(); ++i)
            {
                rb += (double) base[i] * (double) base[i];
                rd += (double) driven[i] * (double) driven[i];
            }
            const double ratio = std::sqrt(rd / rb);
            std::printf ("        (drive 24 dB on a -54 dBFS sine: level ratio %.4f)\n", ratio);
            check (ratio > 0.985 && ratio < 1.015, "drive: unity small-signal gain (quiet material passes unchanged at +24 dB drive)");

            const auto loud = makeSine (9600, 200.0, fs, 0.8f);
            const auto sat = runStageConst (fs, mk (1, 12000.0, 0.0, 0.0, 24.0), loud);
            const double pk = peakOf (sat);
            std::printf ("        (drive 24 dB on a 0.8 sine: output peak %.4f, saturation level 1/P = %.4f)\n", pk, std::pow (10.0, -24.0 / 20.0));
            check (pk < 0.075 && pk > 0.04 && allFinite (sat), "drive: peaks are rounded off at 1/P (ADAA tanh), output finite");

            // drive 0 dB must be the plain filter: the stage vs the same design run through FilterCore in double
            FilterDesign d;
            fd::make (3, 2000.0, 0.2, 0.0, fs, d);
            warpr::FilterCore core;
            core.setDesign (d);
            std::vector<double> in (saw.begin(), saw.end()), ref (saw.size());
            core.process (0, in.data(), ref.data(), (int) in.size());
            const auto viaStage = runStageConst (fs, mk (3, 2000.0, 0.2), saw);
            double worst = 0.0;
            for (size_t i = 0; i < saw.size(); ++i)
                worst = std::max (worst, std::abs ((double) viaStage[i] - ref[i]));
            check (worst < 1.0e-6, "drive 0 dB: the stage output equals the bare filter (transparent, < 1e-6)");

            // constant input through the drive stage (ADAA midpoint fallback) must be finite and equal to tanh(P x)/P
            const std::vector<float> dc (2000, 0.5f);
            const auto yd = runStageConst (fs, mk (0, 19000.0, 0.0, 0.0, 12.0), dc);
            const double p = std::pow (10.0, 12.0 / 20.0);
            const double expected = std::tanh (p * 0.5) / p;
            check (allFinite (yd) && std::abs ((double) yd.back() - expected) < 2.0e-3, "drive: a constant input hits the ADAA fallback and settles at tanh(P x) / P");
        }

        // ---- processor level: the same nulls through the whole voice
        {
            auto make = [&] (const std::function<void (PluginProcessor&)>& cfg)
            {
                PluginProcessor p;
                prepSustained (p);
                cfg (p);
                return renderBlocks (p, fs, 60, 256, 48, nullptr);
            };

            const auto defaultR = make ([] (PluginProcessor&) {});
            const auto offWild = make ([] (PluginProcessor& p)
            {
                setParam (p, "filterEnable", 0.0f);  setParam (p, "filterType", 9.0f);   setParam (p, "filterCutoff", 120.0f);
                setParam (p, "filterResonance", 1.0f); setParam (p, "filterDrive", 24.0f); setParam (p, "filterMorph", 1.0f);
                setParam (p, "filterKeyTrack", 1.0f);
            });
            const auto mixZero = make ([] (PluginProcessor& p)
            {
                setParam (p, "filterType", 13.0f); setParam (p, "filterCutoff", 300.0f); setParam (p, "filterResonance", 1.0f);
                setParam (p, "filterDrive", 12.0f); setParam (p, "filterMix", 0.0f);
            });
            const auto active = make ([] (PluginProcessor& p)
            {
                setParam (p, "filterType", 3.0f); setParam (p, "filterCutoff", 600.0f); setParam (p, "filterResonance", 0.3f);
            });

            check (peakOf (defaultR.l) > 0.05, "processor: the default patch is audible");
            check (bitEqual (defaultR.l, offWild.l) && bitEqual (defaultR.r, offWild.r),
                   "processor: filterEnable off (any other settings) is bit-identical to the default patch");
            check (bitEqual (offWild.l, mixZero.l) && bitEqual (offWild.r, mixZero.r), "processor: filterMix 0 is bit-identical to filterEnable off");
            const double nullActive = nullDb (defaultR.l, active.l);
            std::printf ("        (LP 24 @ 600 Hz vs bypass: null %.1f dB)\n", nullActive);
            check (nullActive > -20.0 && allFinite (active.l) && allFinite (active.r), "processor: an active filter really changes the sound");
        }
    }

    //==========================================================================
    // 6. type switching
    //==========================================================================
    struct SwitchResult
    {
        bool stepOk = false, tailOk = false;
        double step = 0.0, bound = 0.0, tail = 0.0;
    };

    SwitchResult switchPair (int a, int b, double fs, const std::vector<float>& in)
    {
        constexpr int switchAt = 187 * 32;                                       // chunk aligned
        const FilterTargets ta = mk (a, 1200.0, 0.3, 0.2), tb = mk (b, 1200.0, 0.3, 0.2);

        const auto ya = runStageConst (fs, ta, in);
        const auto yb = runStageConst (fs, tb, in);
        const auto ysw = runStage (fs, ta, in, [b] (int ci, FilterTargets& t) { if (ci * 32 >= switchAt) t.type = b; });

        const size_t w0 = (size_t) switchAt - 64, w1 = (size_t) switchAt + 1200;
        const double natural = std::max (maxStepIn (ya, w0, w1), maxStepIn (yb, w0, w1));

        double diff = 0.0;
        for (size_t i = (size_t) switchAt; i < (size_t) switchAt + 528; ++i)
            diff = std::max (diff, (double) std::abs (ya[i] - yb[i]));

        double tail = 0.0;
        for (size_t i = (size_t) switchAt + 4000; i < in.size(); ++i)
            tail = std::max (tail, (double) std::abs (ysw[i] - yb[i]));

        SwitchResult r;
        r.step = maxStepIn (ysw, w0, w1);
        r.bound = natural + 2.0 * diff / 480.0 + 5.0e-3;
        r.tail = tail;
        r.stepOk = r.step <= r.bound && allFinite (ysw);
        r.tailOk = tail < 2.0e-3;
        return r;
    }

    void testSwitching (bool full)
    {
        std::printf ("\n[D5a] type-switch crossfade (DiscreteSwitchFade, 10 ms + 64 samples input priming)\n");
        constexpr double fs = 48000.0;
        const auto saw = makeMix (12000, fs);

        int pairs = 0, stepBad = 0, tailBad = 0;
        double worstRatio = 0.0;

        for (int a = 0; a < kD5aTypes; ++a)
            for (int b = 0; b < kD5aTypes; ++b)
            {
                if (a == b)
                    continue;

                const auto r = switchPair (a, b, fs, saw);
                ++pairs;
                worstRatio = std::max (worstRatio, r.step / std::max (r.bound, 1.0e-9));

                if (! r.stepOk)
                {
                    ++stepBad;
                    DETAIL ("        step: %d -> %d: max step %.5f > bound %.5f\n", a, b, r.step, r.bound);
                }
                if (! r.tailOk)
                {
                    ++tailBad;
                    DETAIL ("        tail: %d -> %d: |switched - new| = %.5f\n", a, b, r.tail);
                }
            }

        std::printf ("        (%d transitions; worst step / bound = %.3f)\n", pairs, worstRatio);
        check (stepBad == 0, "every transition: max step <= natural step + 2 x difference / 480 samples (click-free)");
        check (tailBad == 0, "every transition ends on the new type (|switched - new| < 2e-3 after the fade)");

        // (switches into / out of the nonlinear types 28-33 are covered by the [D5c] switching section)

        // stage bookkeeping: fade length, and no fade when the type does not change
        {
            FilterStage st;
            st.prepare (fs);
            const FilterTargets ta = mk (3, 900.0);
            st.snap (ta);
            std::vector<float> l (32, 0.1f), r (32, 0.1f);
            st.process (l.data(), r.data(), 32, ta);
            const bool idleBefore = ! st.isFading();
            FilterTargets tb = ta;
            tb.type = 9;
            int chunks = 0;
            st.process (l.data(), r.data(), 32, tb);
            const bool startedFade = st.isFading();
            while (st.isFading() && chunks < 100)
            {
                st.process (l.data(), r.data(), 32, tb);
                ++chunks;
            }
            std::printf ("        (fade of %d samples; ended after %d further chunks)\n", st.fadeLengthSamples(), chunks);
            check (idleBefore && startedFade && ! st.isFading() && st.currentType() == 9 && st.fadeLengthSamples() == 64 + 480,
                   "a type change starts one fade of 64 + 480 samples (48 kHz) and ends on the new type");
        }

        // processor level: switch during a sustained note, LUT-style click bound (as in D4)
        {
            constexpr int nBlocks = 150, block = 256, swBlock = 60;
            const int swSample = swBlock * block;

            auto prep = [] (PluginProcessor& p)
            {
                prepSustained (p);
                setParam (p, "filterType", 3.0f);
                setParam (p, "filterCutoff", 900.0f);
                setParam (p, "filterResonance", 0.2f);
            };

            PluginProcessor pOld, pNew, pSw;
            prep (pOld);  prep (pNew);  prep (pSw);
            setParam (pNew, "filterType", 9.0f);                                           // HP 24

            const auto yOld = renderBlocks (pOld, fs, nBlocks, block, 48, nullptr).l;
            const auto yNew = renderBlocks (pNew, fs, nBlocks, block, 48, nullptr).l;
            const auto ySw = renderBlocks (pSw, fs, nBlocks, block, 48, [&] (int b) { if (b == swBlock) setParam (pSw, "filterType", 9.0f); }).l;

            const size_t w0 = (size_t) (swSample - 64), w1 = (size_t) (swSample + 12 * block);
            double maxDiff = 0.0;
            for (size_t i = (size_t) swSample; i < (size_t) swSample + 544; ++i)
                maxDiff = std::max (maxDiff, (double) std::abs (yNew[i] - yOld[i]));

            const double natural = std::max (maxStepIn (yOld, w0, w1), maxStepIn (yNew, w0, w1));
            const double stepSw = maxStepIn (ySw, w0, w1);
            const double bound = natural + 2.0 * maxDiff / 480.0 + 1.0e-3;

            double tailErr = 0.0;
            for (size_t i = (size_t) (swSample + 0.3 * fs); i < ySw.size(); ++i)
                tailErr = std::max (tailErr, (double) std::abs (ySw[i] - yNew[i]));

            std::printf ("        (LP 24 -> HP 24 at block %d: natural max step %.5f, old/new difference %.4f, switched max step %.5f, bound %.5f)\n",
                         swBlock, natural, maxDiff, stepSw, bound);
            check (maxDiff > 0.02, "the two filter types sound different (the test is meaningful)");
            check (stepSw <= bound, "processor: type switch mid-note is click-free (crossfade bound)");
            check (tailErr < 2.0e-3, "processor: after the fade the switched render equals the render with the new type from the start");
        }

        (void) full;
    }

    //==========================================================================
    // 7. sweeps and modulation walk
    //==========================================================================
    void testSweeps (bool full)
    {
        std::printf ("\n[D5a] fast cutoff sweeps (zipper-free, monotone) and random modulation walk\n");
        constexpr double fs = 48000.0;
        const double hz = 1000.0;
        const float amp = 0.5f;
        const auto sine = makeSine (19200, hz, fs, amp);               // 400 ms
        const double sineSlope = (double) amp * 2.0 * kPi * hz / fs;   // largest |dy| of the untouched sine

        for (int type : { 1, 3, 4 })
        {
            const double a = std::log (200.0), b = std::log (8000.0);
            const int sweepChunks = 9600 / 32;                          // 200 ms exponential sweep up, then down

            // up
            const auto up = runStage (fs, mk (type, 200.0), sine, [&] (int ci, FilterTargets& t)
            {
                const double x = std::min (1.0, (double) ci / (double) sweepChunks);
                t.cutoffHz = (float) std::exp (a + (b - a) * x);
            });

            const auto down = runStage (fs, mk (type, 8000.0), sine, [&] (int ci, FilterTargets& t)
            {
                const double x = std::min (1.0, (double) ci / (double) sweepChunks);
                t.cutoffHz = (float) std::exp (b + (a - b) * x);
            });

            auto envelope = [&] (const std::vector<float>& y)
            {
                std::vector<double> env;
                for (size_t i = 0; i + 48 <= y.size(); i += 48)             // exactly one cycle per window
                {
                    double s = 0.0;
                    for (size_t k = 0; k < 48; ++k)
                        s += (double) y[i + k] * (double) y[i + k];
                    env.push_back (std::sqrt (2.0 * s / 48.0));
                }
                return env;
            };

            const auto envUp = envelope (up), envDown = envelope (down);
            double worstDrop = 0.0, worstRise = 0.0;
            for (size_t w = 8; w + 1 < envUp.size(); ++w)
            {
                worstDrop = std::max (worstDrop, envUp[w] - envUp[w + 1]);          // up-sweep: must not fall
                worstRise = std::max (worstRise, envDown[w + 1] - envDown[w]);      // down-sweep: must not rise
            }

            const double stepUp = maxStepIn (up, 400, up.size()), stepDown = maxStepIn (down, 400, down.size());
            const double bound = sineSlope * 1.05 + 5.0e-4;
            std::printf ("        (type %d: max step up %.5f, down %.5f, sine slope %.5f; envelope reversal up %.5f, down %.5f)\n",
                         type, stepUp, stepDown, sineSlope, worstDrop, worstRise);
            check (stepUp <= bound && stepDown <= bound, "sweep: no sample step above the untouched sine's own slope (zipper-free)");
            // The bound is set by transient ringing, not zipper noise: the cascade's top section (Q up to
            // 1.93 for LP 36) rings for a few cycles as the sweeping corner passes the probe tone, which
            // the 1 ms RMS windows see as a 0.1-0.35% wobble (measured: type 1 0.0, type 3 0.0011,
            // type 4 0.0035).  A coefficient-update artefact would be an order larger and is separately
            // bounded by the sample-level zipper check above.
            check (worstDrop < 6.0e-3 && worstRise < 6.0e-3, "sweep: the passed amplitude moves monotonically with the cutoff");
        }

        // ---- random modulation walk: new targets for every parameter on every chunk (>= 1500 changes/s each)
        {
            const int chunks = full ? 15000 : 3000;                     // 10 s / 2 s
            juce::Random rng (99);
            FilterStage st;
            st.prepare (fs);
            FilterTargets t = mk (3, 1000.0);
            st.snap (t);

            std::vector<float> l (32), r (32);
            bool finite = true;
            double peak = 0.0;
            int changes = 0;

            for (int c = 0; c < chunks; ++c)
            {
                if (rng.nextInt (30) == 0)
                    t.type = rng.nextInt (warpr::kNumFilterTypes);

                t.cutoffHz = (float) (20.0 * std::pow (1000.0, (double) rng.nextFloat()));
                t.res = rng.nextFloat();
                t.morph = rng.nextFloat();
                t.driveDb = rng.nextInt (3) == 0 ? 24.0f * rng.nextFloat() : 0.0f;
                t.mix = rng.nextFloat() > 0.2f ? 1.0f : rng.nextFloat();
                t.enable = rng.nextInt (200) != 0;
                changes += 6;

                for (size_t i = 0; i < 32; ++i)
                    l[i] = r[i] = 0.3f * (rng.nextFloat() * 2.0f - 1.0f);

                st.process (l.data(), r.data(), 32, t);

                for (size_t i = 0; i < 32; ++i)
                {
                    finite = finite && std::isfinite (l[i]) && std::isfinite (r[i]);
                    peak = std::max ({ peak, (double) std::abs (l[i]), (double) std::abs (r[i]) });
                }
            }

            std::printf ("        (%d parameter changes in %.1f s, peak %.3f)\n", changes, (double) chunks * 32.0 / fs, peak);
            check (finite, "random modulation walk: no NaN / inf");
            check (peak < 64.0, "random modulation walk: output stays below the voice guard (|y| < 64)");
        }
    }

    //==========================================================================
    // 8. key tracking through the voice
    //==========================================================================
    double tailRms (const std::vector<float>& v, size_t lastSamples)
    {
        double s = 0.0;
        for (size_t i = v.size() - lastSamples; i < v.size(); ++i)
            s += (double) v[i] * (double) v[i];
        return std::sqrt (s / (double) lastSamples);
    }

    double keyRms (int note, float keyTrack)
    {
        PluginProcessor p;
        prepSustained (p);
        setParam (p, "filterType", 1.0f);                          // LP 12
        setParam (p, "filterCutoff", 1000.0f);
        setParam (p, "filterKeyTrack", keyTrack);
        const auto r = renderBlocks (p, 48000.0, 200, 256, note, nullptr);
        return tailRms (r.l, 19200);
    }

    void testKeyTrack()
    {
        std::printf ("\n[D5a] key tracking through the voice\n");

        // The default saw has a 1/n spectrum: with key tracking 1 an octave-higher note sees the filter an octave higher,
        // i.e. the same relative shape, so the level stays put.  Without tracking the higher note loses its upper partials.
        const double r60 = keyRms (60, 1.0f), r72 = keyRms (72, 1.0f);
        const double f60 = keyRms (60, 0.0f), f72 = keyRms (72, 0.0f);
        std::printf ("        (LP 12 @ 1 kHz: key 1 -> RMS note 60 %.4f, note 72 %.4f (ratio %.3f); key 0 -> %.4f, %.4f (ratio %.3f))\n",
                     r60, r72, r72 / r60, f60, f72, f72 / f60);
        check (r60 > 0.05 && std::abs (r72 / r60 - 1.0) < 0.04, "key track 1: note 72 vs note 60 keep the same level (filter follows the pitch one-to-one)");
        check (f72 / f60 < 0.93, "key track 0: the octave-higher note loses level against the fixed cutoff");
    }

    //==========================================================================
    // 9. backend: response display
    //==========================================================================
    void testBackend()
    {
        std::printf ("\n[D5a] backend: computeFilterResponse and Revision::filterResponse\n");
        constexpr int n = 256;

        PluginProcessor p;
        auto& be = p.getBackendImpl();
        be.setSampleRate (48000.0);

        std::array<float, n> db {};
        auto freq = [] (int i) { return 20.0 * std::pow (1000.0, (double) i / (double) (n - 1)); };

        be.computeFilterResponse (db.data(), n);
        bool flat = true;
        for (float v : db)
            flat = flat && v == 0.0f;
        check (flat, "default patch (LP 24 @ 20 kHz, open) draws a flat 0 dB line");

        // LP 12 @ 1 kHz: equals FilterResponse point for point
        setParam (p, "filterType", 1.0f);
        setParam (p, "filterCutoff", 1000.0f);
        be.computeFilterResponse (db.data(), n);
        double worst = 0.0;
        for (int i = 0; i < n; ++i)
            worst = std::max (worst, std::abs ((double) db[ix (i)] - FilterResponse::magnitudeDbAt (1, 1000.0, 0.0, 0.0, 0.0, 48000.0, freq (i))));
        check (worst < 0.01, "LP 12 @ 1 kHz: all 256 points equal FilterResponse (the same coefficient code)");
        check (db[0] > -0.1f && db[n - 1] < -20.0f && db[144] > -3.6f && db[144] < -2.4f,
               "LP 12 shape: ~0 dB at 20 Hz, about -3 dB at the cutoff, steep fall at 20 kHz");

        // Peak +18 dB
        setParam (p, "filterType", 17.0f);
        setParam (p, "filterCutoff", 2000.0f);
        setParam (p, "filterResonance", 1.0f);
        setParam (p, "filterMorph", 0.0f);
        be.computeFilterResponse (db.data(), n);
        int peakIdx = 0;
        for (int i = 0; i < n; ++i)
            if (db[ix (i)] > db[ix (peakIdx)])
                peakIdx = i;
        check (db[ix (peakIdx)] > 17.5f && db[ix (peakIdx)] < 18.3f && std::abs (freq (peakIdx) / 2000.0 - 1.0) < 0.05,
               "Peak, res 1: +18 dB bell centred on the cutoff");

        // nonlinear type (D5c): the small-signal curve, not a flat line
        setParam (p, "filterType", 28.0f);
        be.computeFilterResponse (db.data(), n);
        flat = true;
        for (float v : db)
            flat = flat && v == 0.0f;
        check (! flat, "Res LP (D5c, small-signal ladder response) draws a real curve");

        // display range clamp
        setParam (p, "filterType", 5.0f);
        setParam (p, "filterCutoff", 100.0f);
        setParam (p, "filterResonance", 0.0f);
        be.computeFilterResponse (db.data(), n);
        bool inRange = true;
        for (float v : db)
            inRange = inRange && std::isfinite (v) && v >= -48.0f && v <= 24.0f;
        check (inRange && db[n - 1] == -48.0f, "LP 48 @ 100 Hz: finite and clamped to the [-48, +24] dB display range");

        // the display follows the host sample rate
        be.setSampleRate (96000.0);
        setParam (p, "filterType", 1.0f);
        setParam (p, "filterCutoff", 1000.0f);
        be.computeFilterResponse (db.data(), n);
        worst = 0.0;
        for (int i : { 20, 120, 200, 255 })
            worst = std::max (worst, std::abs ((double) db[ix (i)] - FilterResponse::magnitudeDbAt (1, 1000.0, 0.0, 0.0, 0.0, 96000.0, freq (i))));
        check (worst < 0.01, "the response is evaluated at the processor's sample rate (96 kHz)");

        // speed
        {
            setParam (p, "filterType", 5.0f);
            setParam (p, "filterResonance", 0.5f);
            double best = 1.0e9;
            for (int rep = 0; rep < 50; ++rep)
            {
                const auto t0 = std::chrono::steady_clock::now();
                be.computeFilterResponse (db.data(), n);
                const auto t1 = std::chrono::steady_clock::now();
                best = std::min (best, std::chrono::duration<double, std::milli> (t1 - t0).count());
            }
            std::printf ("        (computeFilterResponse, 256 points, LP 48: best of 50 = %.3f ms)\n", best);
            check (best < 1.0, "computeFilterResponse returns 256 points in < 1 ms");
        }

        // revision counter
        {
            PluginProcessor q;
            auto& qb = q.getBackendImpl();
            int notBumped = 0;
            const struct { const char* id; float value; } changes[] = {
                { "filterEnable", 0.0f }, { "filterType", 5.0f }, { "filterCutoff", 5000.0f }, { "filterResonance", 0.7f },
                { "filterDrive", 12.0f }, { "filterMorph", 0.4f }, { "filterKeyTrack", 0.3f }, { "filterMix", 0.5f } };

            for (const auto& c : changes)
            {
                const auto before = qb.revision (WarprUiBackend::Revision::filterResponse);
                setParam (q, c.id, c.value);
                if (qb.revision (WarprUiBackend::Revision::filterResponse) <= before)
                {
                    ++notBumped;
                    DETAIL ("        %s did not bump Revision::filterResponse\n", c.id);
                }
            }
            check (notBumped == 0, "Revision::filterResponse bumps on a change of each of the 8 filter parameters");

            const auto before = qb.revision (WarprUiBackend::Revision::filterResponse);
            setParam (q, "warpAmount", 0.5f);
            check (qb.revision (WarprUiBackend::Revision::filterResponse) == before, "a non-filter parameter does not bump it");
        }
    }

    //==========================================================================
    // 10. processor level: allocations, determinism
    //==========================================================================
    void testProcessor()
    {
        std::printf ("\n[D5a] processor level: allocations, determinism\n");
        constexpr double fs = 48000.0;

        {
            PluginProcessor p;
            prepSustained (p);
            setParam (p, "unisonVoices", 4.0f);
            setParam (p, "filterType", 3.0f);
            setParam (p, "filterCutoff", 800.0f);
            setParam (p, "filterResonance", 0.5f);
            setParam (p, "filterDrive", 12.0f);
            p.setPlayConfigDetails (0, 2, fs, 256);
            p.prepareToPlay (fs, 256);

            juce::AudioBuffer<float> buf (2, 256);
            juce::MidiBuffer on;
            on.addEvent (juce::MidiMessage::noteOn (1, 64, 0.9f), 0);
            on.addEvent (juce::MidiMessage::noteOn (1, 67, 0.9f), 0);
            p.processBlock (buf, on);
            juce::MidiBuffer none;

            std::size_t allocations = 0;
            bool sawFade = false;

            for (int round = 0; round < 12; ++round)
            {
                setParam (p, "filterType", (float) ((round * 7) % 20));                // allocates: uncounted
                setParam (p, "filterCutoff", 300.0f + 700.0f * (float) (round % 4));

                warpr::tests::ScopedAllocCount counter;
                for (int i = 0; i < 6; ++i)
                {
                    p.processBlock (buf, none);
                    sawFade = sawFade || p.getSynth().getNumActiveVoices() > 0;
                }
                allocations += counter.count();
            }

            check (sawFade, "the counted window contained sounding voices");
            check (allocations == 0, "processBlock performs zero allocations with an active filter (drive on, 12 type changes, 2 voices x unison 4)");
            p.releaseResources();
        }

        {
            auto run = [&]
            {
                PluginProcessor p;
                prepSustained (p);
                setParam (p, "unisonVoices", 3.0f);
                setParam (p, "filterType", 13.0f);
                setParam (p, "filterCutoff", 700.0f);
                setParam (p, "filterResonance", 0.6f);
                setParam (p, "filterDrive", 6.0f);
                return renderBlocks (p, fs, 80, 256, 50, [&p] (int b)
                {
                    if (b == 20) setParam (p, "filterType", 2.0f);
                    if (b == 35) setParam (p, "filterCutoff", 3000.0f);
                    if (b == 50) setParam (p, "filterEnable", 0.0f);
                    if (b == 65) setParam (p, "filterEnable", 1.0f);
                });
            };

            const auto a = run(), b = run();
            check (bitEqual (a.l, b.l) && bitEqual (a.r, b.r) && peakOf (a.l) > 0.05, "two identical sessions with mid-note filter edits are bit-equal");
        }
    }

    //==========================================================================
    //==========================================================================
    // D5b: special linear filters, types 20-27
    //==========================================================================
    //==========================================================================
    constexpr int kD5bFirst = 20, kD5bEnd = 28;           // [first, end)

    std::vector<double> logGrid (double lo, double hi, int n)
    {
        std::vector<double> f (ix (n));
        for (int i = 0; i < n; ++i)
            f[ix (i)] = lo * std::pow (hi / lo, (double) i / (double) (n - 1));
        return f;
    }

    std::vector<double> magCurve (int type, double fc, double res, double morph, double fs, const std::vector<double>& freqs)
    {
        FilterDesign d;
        fd::make (type, fc, res, morph, fs, d);
        std::vector<double> m (freqs.size());
        for (size_t i = 0; i < freqs.size(); ++i)
            m[i] = std::abs (fd::response (d, freqs[i], fs));
        return m;
    }

    size_t argMaxIn (const std::vector<double>& m, size_t lo, size_t hi)
    {
        size_t best = lo;
        for (size_t i = lo; i < hi && i < m.size(); ++i)
            if (m[i] > m[best])
                best = i;
        return best;
    }

    /** First index of the grid at or above `hz` (grids are ascending). */
    size_t indexAtOrAbove (const std::vector<double>& f, double hz)
    {
        size_t i = 0;
        while (i + 1 < f.size() && f[i] < hz)
            ++i;
        return i;
    }

    int countLocalMinima (const std::vector<double>& m, double below)
    {
        int n = 0;
        for (size_t i = 1; i + 1 < m.size(); ++i)
            if (m[i] < m[i - 1] && m[i] <= m[i + 1] && m[i] < below)
                ++n;
        return n;
    }

    /** Parabolic (in log magnitude) refinement of a peak on a UNIFORM log-frequency grid. */
    double refinePeakHz (const std::vector<double>& f, const std::vector<double>& m, size_t i)
    {
        if (i == 0 || i + 1 >= m.size())
            return f[i];

        const double a = std::log (std::max (m[i - 1], 1.0e-300)), b = std::log (std::max (m[i], 1.0e-300)), c = std::log (std::max (m[i + 1], 1.0e-300));
        const double den = a - 2.0 * b + c;
        const double off = std::abs (den) < 1.0e-18 ? 0.0 : 0.5 * (a - c) / den;
        return f[i] * std::pow (f[i + 1] / f[i], off);
    }

    //==========================================================================
    void testD5bTable()
    {
        std::printf ("\n[D5b] type table\n");

        bool ok = true;
        for (int i = kD5bFirst; i < kD5bEnd; ++i)
        {
            const auto& info = warpr::kFilterTypes[ix (i)];
            ok = ok && info.implemented && info.linear && info.build == warpr::FilterBuild::D5b;
            std::printf ("        %d: %s  [%s]\n", i, info.name, info.topology);
        }

        using FF = warpr::FilterFamily;
        const auto fam = [] (int i) { return warpr::kFilterTypes[ix (i)].family; };
        check (ok, "types 20-27 are flagged implemented, linear, build D5b");
        check (fam (20) == FF::Comb && fam (21) == FF::Comb && fam (22) == FF::AllPass && fam (23) == FF::AllPass
               && fam (24) == FF::Formant && fam (25) == FF::Formant && fam (26) == FF::Formant && fam (27) == FF::AllPass,
               "families: 20-21 comb, 22-23 / 27 all-pass, 24-26 formant");
        check (warpr::kFilterTypes[28].implemented && warpr::kNumImplementedFilterTypes == 34, "D5c has filled types 28-33 (34 implemented in total)");

        // every new type designs a non-trivial structure and is not subject to the open-LP rule
        int trivial = 0, openRule = 0;
        for (int t = kD5bFirst; t < kD5bEnd; ++t)
        {
            FilterDesign d;
            fd::make (t, 1000.0, 0.7, 0.3, 48000.0, d);
            if (d.topo != FilterDesign::Topo::Comb && d.numSections == 0)
                ++trivial;
            if (fd::isOpenLowpass (t, 20000.0, 0.0, 0.0))
                ++openRule;
        }
        check (trivial == 0 && openRule == 0, "types 20-27 design real structures; the open low-pass rule never applies to them");
    }

    //==========================================================================
    void testD5bCombFormulas()
    {
        std::printf ("\n[D5b] combs (20 / 21): tooth gains, tuning, damping, mapping, delay-line capacity\n");
        constexpr double fs = 48000.0;
        const double g = 0.97, c = std::sqrt (1.0 - g * g);
        const double peak = c / (1.0 - g), valley = c / (1.0 + g);
        auto mag = [&] (int type, double fc, double res, double morph, double hz) { return magAt (type, fc, res, morph, fs, hz); };

        FilterDesign d;
        fd::make (20, 1000.0, 1.0, 0.0, fs, d);
        check (d.topo == FilterDesign::Topo::Comb && std::abs (d.comb.delay - 48.0) < 1.0e-9 && d.comb.a == 1.0 && d.comb.sign > 0.0
               && std::abs (d.comb.g - g) < 1.0e-12 && std::abs (d.comb.c - c) < 1.0e-12,
               "Comb +: 1 kHz @ 48 kHz -> delay exactly 48 samples, g = 0.97 res, c = sqrt(1-g^2), transparent loop at morph 0");

        check (std::abs (mag (20, 1000.0, 1.0, 0.0, 1000.0) - peak) < 1.0e-6 && std::abs (mag (20, 1000.0, 1.0, 0.0, 2000.0) - peak) < 1.0e-6
               && std::abs (mag (20, 1000.0, 1.0, 0.0, 5000.0) - peak) < 1.0e-6
               && std::abs (mag (20, 1000.0, 1.0, 0.0, 500.0) - valley) < 1.0e-6 && std::abs (mag (20, 1000.0, 1.0, 0.0, 1500.0) - valley) < 1.0e-6,
               "Comb +: teeth at k fc have gain c/(1-g) = 8.1, valleys midway c/(1+g) = 0.12");

        check (std::abs (mag (21, 1000.0, 1.0, 0.0, 1000.0) - peak) < 1.0e-6 && std::abs (mag (21, 1000.0, 1.0, 0.0, 3000.0) - peak) < 1.0e-6
               && std::abs (mag (21, 1000.0, 1.0, 0.0, 2000.0) - valley) < 1.0e-6 && std::abs (mag (21, 1000.0, 1.0, 0.0, 20.0) / valley - 1.0) < 3.0e-3,
               "Comb -: teeth at the ODD multiples of fc (fc, 3 fc, ...), valleys at DC and the even multiples");

        bool flat = true;
        for (int type : { 20, 21 })
            for (double f : { 30.0, 440.0, 1000.0, 7777.0, 19000.0 })
                flat = flat && std::abs (mag (type, 1000.0, 0.0, 0.0, f) - 1.0) < 1.0e-12;
        check (flat, "res 0 (no feedback): both combs are exactly flat");

        // maximum over the whole band never exceeds the undamped peak (stability at the extreme feedback)
        {
            const auto f = logGrid (20.0, 21000.0, 6000);
            double worst = 0.0;
            for (int type : { 20, 21 })
                for (double fc : { 40.0, 173.0, 1000.0, 5300.0 })
                    for (double morph : { 0.0, 0.5, 1.0 })
                    {
                        const auto m = magCurve (type, fc, 1.0, morph, fs, f);
                        worst = std::max (worst, m[argMaxIn (m, 0, m.size())]);
                    }
            std::printf ("        (largest |H| over fc x morph at res 1: %.4f; ideal peak %.4f)\n", worst, peak);
            check (worst < peak * 1.01 && worst > 1.0, "res 1 (g = 0.97): the response stays bounded by c/(1-g) (Lagrange3 + damping only reduce it)");
        }

        // tuning: the tooth nearest fc sits at fc within 5 cents (loop phase delay solved incl. Lagrange3 and damping)
        {
            double worstCents = 0.0;
            int bad = 0;

            for (int type : { 20, 21 })
                for (double fc : { 55.0, 110.0, 440.0, 1234.5, 3100.0 })
                    for (double res : { 0.5, 1.0 })
                    {
                        const auto f = logGrid (fc * 0.97, fc * 1.03, 2001);
                        const auto m = magCurve (type, fc, res, 0.0, fs, f);
                        const size_t i = argMaxIn (m, 0, m.size());
                        const double cents = 1200.0 * std::log2 (refinePeakHz (f, m, i) / fc);
                        worstCents = std::max (worstCents, std::abs (cents));

                        if (std::abs (cents) > 5.0)
                        {
                            ++bad;
                            DETAIL ("        comb %d fc %.1f res %.1f: tooth %.2f cents off\n", type, fc, res, cents);
                        }
                    }

            // damped loops pull the tooth slightly (the loop gain slopes across it): 8 cents at morph 0.5
            for (int type : { 20, 21 })
                for (double fc : { 110.0, 440.0 })
                {
                    const auto f = logGrid (fc * 0.97, fc * 1.03, 2001);
                    const auto m = magCurve (type, fc, 1.0, 0.5, fs, f);
                    const double cents = 1200.0 * std::log2 (refinePeakHz (f, m, argMaxIn (m, 0, m.size())) / fc);
                    worstCents = std::max (worstCents, std::abs (cents));

                    if (std::abs (cents) > 8.0)
                    {
                        ++bad;
                        DETAIL ("        damped comb %d fc %.1f: tooth %.2f cents off\n", type, fc, cents);
                    }
                }

            std::printf ("        (worst tooth offset %.3f cents)\n", worstCents);
            check (bad == 0, "the fundamental tooth is at the cutoff within 5 cents (8 cents with loop damping), both polarities");
        }

        // damping: morph darkens the loop -> the 8th tooth loses level monotonically
        {
            double t8[3] = {};
            int k = 0;
            for (double morph : { 0.0, 0.5, 1.0 })
            {
                const auto f = logGrid (1568.0, 1632.0, 401);
                const auto m = magCurve (20, 200.0, 1.0, morph, fs, f);
                t8[k++] = m[argMaxIn (m, 0, m.size())];
            }
            std::printf ("        (8th tooth of a 200 Hz comb: %.3f / %.3f / %.3f at morph 0 / 0.5 / 1)\n", t8[0], t8[1], t8[2]);
            // full damping legitimately sinks the 8th tooth toward the valley floor (~0.12 at res 1):
            // dark means the teeth are GONE.  Measured 0.292; the floor only guards against a dead line.
            check (t8[0] > t8[1] && t8[1] > t8[2] && t8[2] > 0.1, "loop damping: morph 0 -> 1 lowers the upper teeth monotonically (bright -> dark)");
        }

        // delay-line capacity covers every reachable delay
        {
            bool fits = true;
            for (double rate : { 44100.0, 48000.0, 96000.0, 192000.0 })
                for (int type : { 20, 21 })
                    for (double fc : { 1.0, 20.0, 1000.0, 20000.0, 90000.0 })
                        for (double morph : { 0.0, 1.0 })
                        {
                            FilterDesign dd;
                            fd::make (type, fc, 1.0, morph, rate, dd);
                            fits = fits && dd.comb.delay >= 2.0 && dd.comb.delay + 2.0 <= (double) (fd::combBufferLength (rate) - 6);
                        }
            check (fits, "delay lines: every comb delay (20 Hz .. 0.45 fs, all fs) stays within the pre-allocated fs/20 + margin");
        }
    }

    //==========================================================================
    void testD5bAllPassFormulas()
    {
        std::printf ("\n[D5b] all-pass types (22 / 23): unit magnitude, blend, stage count, dispersion\n");
        constexpr double fs = 48000.0;
        const auto grid = logGrid (20.0, 21000.0, 400);

        // 22: pure all-pass at both ends of the morph; (1-m) AP2 + m AP1 in between
        bool unity = true;
        for (double res : { 0.0, 0.5, 1.0 })
            for (double morph : { 0.0, 1.0 })
                for (double v : magCurve (22, 1500.0, res, morph, fs, grid))
                    unity = unity && std::abs (v - 1.0) < 1.0e-9;
        check (unity, "All Pass: |H| = 1 at every frequency for morph 0 (2nd order) and morph 1 (1st order)");
        check (std::abs (magAt (22, 1500.0, 0.5, 0.5, fs, 1500.0) - std::sqrt (0.5)) < 1.0e-9
               && std::abs (magAt (22, 1500.0, 0.5, 0.25, fs, 1500.0) - std::sqrt (0.75 * 0.75 + 0.25 * 0.25)) < 1.0e-9
               && std::abs (magAt (22, 1500.0, 0.5, 0.5, fs, 20.0) - 1.0) < 1.0e-3,
               "All Pass blend: |H(fc)| = sqrt((1-m)^2 + m^2) (AP2 -180 deg vs AP1 -90 deg), and 1 at DC");

        // 23: unit magnitude for whole stage counts, group delay at DC = sum of 1/g_i
        bool unity23 = true;
        double worstTau = 0.0;
        for (int steps : { 0, 3, 7, 14 })
            for (double res : { 0.0, 0.6 })
            {
                const double morph = (double) steps / 14.0;
                for (double v : magCurve (23, 1000.0, res, morph, fs, grid))
                    unity23 = unity23 && std::abs (v - 1.0) < 1.0e-9;

                FilterDesign d;
                fd::make (23, 1000.0, res, morph, fs, d);
                const double w1 = 2.0 * kPi * 5.0 / fs, w2 = 2.0 * kPi * 10.0 / fs;
                const double tau = -(std::arg (fd::response (d, 10.0, fs)) - std::arg (fd::response (d, 5.0, fs))) / (w2 - w1);
                double expected = 0.0;
                for (int i = 0; i < d.numSections; ++i)
                    expected += 1.0 / d.sec[ix (i)].g;

                worstTau = std::max (worstTau, std::abs (tau / expected - 1.0));
                if (d.numSections != 2 + steps || std::abs (tau / expected - 1.0) > 0.01)
                    DETAIL ("        serial AP steps %d res %.1f: %d sections, group delay %.2f vs sum 1/g %.2f\n", steps, res, d.numSections, tau, expected);
                unity23 = unity23 && d.numSections == 2 + steps;
            }
        std::printf ("        (worst group-delay mismatch %.4f)\n", worstTau);
        check (unity23 && worstTau < 0.01, "Serial AP: N = 2 + round(14 morph) stages, |H| = 1, DC group delay = sum of the stage delays 1/g_i");

        // dispersion grows with the stage count
        {
            FilterDesign a, b;
            fd::make (23, 1000.0, 0.0, 0.0, fs, a);
            fd::make (23, 1000.0, 0.0, 1.0, fs, b);
            const double ta = -std::arg (fd::response (a, 50.0, fs)), tb = -std::arg (fd::response (b, 50.0, fs));      // 50 Hz: no phase wrap yet
            check (b.numSections == 16 && a.numSections == 2 && tb > 4.0 * ta && ta > 0.0, "Serial AP: 16 stages disperse far more than 2 (phase at 50 Hz)");

            FilterDesign s0, s1;
            fd::make (23, 1000.0, 0.0, 0.5, fs, s0);
            fd::make (23, 1000.0, 1.0, 0.5, fs, s1);
            check (s0.sec[3].g == s0.sec[0].g && s1.sec[3].g > 2.0 * s1.sec[0].g, "Serial AP: res = spread (0 stacks every stage at fc, 1 spreads them upward)");
        }
    }

    //==========================================================================
    void testD5bFormantFormulas()
    {
        std::printf ("\n[D5b] formant types (24 / 25 / 26): peak frequencies, transposition, stepped plateaus\n");
        constexpr double fs = 48000.0;
        const auto grid = logGrid (100.0, 9000.0, 8000);

        // Csound tenor F1 / F2 (independent copy of the table values); U's F2 is 20 dB down and sits on the F1 skirt
        const double f1[5] = { 650.0, 400.0, 290.0, 400.0, 350.0 };
        const double f2[5] = { 1080.0, 1700.0, 1870.0, 800.0, 600.0 };
        const bool   hasF2[5] = { true, true, true, true, false };

        int bad = 0;
        double worst = 0.0;

        for (int type : { 25, 26 })
            for (int v = 0; v < 5; ++v)
                for (double res : { 0.5, 0.8 })
                {
                    const auto m = magCurve (type, 1000.0, res, (double) v / 4.0, fs, grid);
                    const size_t i1 = argMaxIn (m, 0, m.size());
                    const double e1 = std::abs (refinePeakHz (grid, m, i1) / f1[v] - 1.0);
                    worst = std::max (worst, e1);

                    double e2 = 0.0;
                    if (hasF2[v])
                    {
                        const size_t lo = indexAtOrAbove (grid, f2[v] * 0.9), hi = indexAtOrAbove (grid, f2[v] * 1.1);
                        const size_t i2 = argMaxIn (m, lo, hi);
                        e2 = (i2 == lo || i2 + 1 >= hi) ? 1.0 : std::abs (refinePeakHz (grid, m, i2) / f2[v] - 1.0);   // an edge hit = no peak
                        worst = std::max (worst, e2);
                    }

                    if (e1 > 0.03 || e2 > 0.03)
                    {
                        ++bad;
                        DETAIL ("        type %d vowel %d res %.1f: F1 off %.2f%%, F2 off %.2f%%\n", type, v, res, 100.0 * e1, 100.0 * e2);
                    }
                }
        std::printf ("        (worst F1 / F2 offset %.2f%%)\n", 100.0 * worst);
        check (bad == 0, "Vowel / Morph Formant: F1 (global peak) and F2 (local peak) of A E I O within 3% of the table, U's F1 too");

        // interpolation: halfway between A and E the F1 peak is the geometric mean (log-frequency interpolation)
        {
            const auto m = magCurve (26, 1000.0, 0.5, 0.125, fs, grid);
            const double fpk = refinePeakHz (grid, m, argMaxIn (m, 0, m.size()));
            const double expected = std::sqrt (650.0 * 400.0);
            std::printf ("        (A-E midpoint F1: %.1f Hz, geometric mean %.1f Hz)\n", fpk, expected);
            check (std::abs (fpk / expected - 1.0) < 0.03, "Morph Formant: morph 0.125 (halfway A -> E) puts F1 at the geometric mean of the two vowels");
        }

        // transposition: cutoff 1 kHz = table, 20 kHz = +1 octave, 20 Hz = -1 octave
        {
            const auto wide = logGrid (60.0, 18000.0, 9000);
            auto peakAt = [&] (double cutoff)
            {
                const auto m = magCurve (26, cutoff, 0.5, 0.0, fs, wide);
                return refinePeakHz (wide, m, argMaxIn (m, 0, m.size()));
            };
            const double pLow = peakAt (20.0), pMid = peakAt (1000.0), pHigh = peakAt (20000.0);
            std::printf ("        (vowel A F1 at cutoff 20 Hz / 1 kHz / 20 kHz: %.1f / %.1f / %.1f Hz)\n", pLow, pMid, pHigh);
            check (std::abs (pLow / 325.0 - 1.0) < 0.03 && std::abs (pMid / 650.0 - 1.0) < 0.03 && std::abs (pHigh / 1300.0 - 1.0) < 0.03,
                   "global formant transposition: cutoff 20 Hz = -1 octave, 1 kHz = 0, 20 kHz = +1 octave");
        }

        // stepped Vowel: plateaus equal the interpolated bank at the vowel positions, midpoints agree, and it never overshoots
        {
            int diffs = 0;
            double worstDiff = 0.0;
            const auto probe = logGrid (80.0, 12000.0, 300);

            auto same = [&] (double morph25, double morph26)
            {
                const auto a = magCurve (25, 1000.0, 0.5, morph25, fs, probe);
                const auto b = magCurve (26, 1000.0, 0.5, morph26, fs, probe);
                double w = 0.0;
                for (size_t i = 0; i < a.size(); ++i)
                    w = std::max (w, std::abs (a[i] - b[i]));
                worstDiff = std::max (worstDiff, w);
                return w < 1.0e-12;
            };

            for (double m : { 0.0, 0.25, 0.5, 0.75, 1.0, 0.125, 0.375 })
                diffs += same (m, m) ? 0 : 1;                       // exact vowel positions and exact midpoints
            diffs += same (0.2, 0.25) ? 0 : 1;                      // inside the plateau of E
            diffs += same (0.06, 0.0) ? 0 : 1;                      // inside the plateau of A
            diffs += same (0.31, 0.25) ? 0 : 1;                     // inside the plateau of E (upper side)
            // U's plateau spans morph >= 0.925 (plateau halfwidth is 0.075 in morph units; the 0.4-wide
            // smoothstep window owns 0.85..0.925) - morph 0.9 sits in the transition, 0.95 on the plateau
            diffs += same (0.95, 1.0) ? 0 : 1;                      // plateau of U
            std::printf ("        (worst plateau mismatch %.2e)\n", worstDiff);
            check (diffs == 0, "Vowel (stepped): plateaus around every vowel are identical to the interpolated bank; the midpoint between two vowels is the 50% blend");

            // continuity: the stepped position function never jumps (largest change per 1e-3 of morph)
            double jump = 0.0;
            double prev = 0.0;
            for (int i = 0; i <= 1000; ++i)
            {
                FilterDesign d;
                fd::make (25, 1000.0, 0.5, (double) i / 1000.0, fs, d);
                const double f = d.sec[0].g;                         // first formant's g tracks its frequency
                if (i > 0)
                    jump = std::max (jump, std::abs (f / prev - 1.0));
                prev = f;
            }
            std::printf ("        (largest relative F1 change per 0.001 of morph: %.4f)\n", jump);
            check (jump < 0.08, "Vowel (stepped): the formants glide through the short transition window (no jump in the design)");
        }

        // 24: F1 = fc, F2 = F1 x (1.5 + 2 morph), fixed third formant at 2.8 kHz
        {
            int badF = 0;
            const auto wide = logGrid (60.0, 12000.0, 9000);

            for (double fc : { 300.0, 800.0 })
                for (double morph : { 0.0, 1.0 })
                {
                    const auto m = magCurve (24, fc, 0.5, morph, fs, wide);
                    const double p1 = refinePeakHz (wide, m, argMaxIn (m, 0, m.size()));
                    const double target2 = fc * (1.5 + 2.0 * morph);
                    const size_t lo = indexAtOrAbove (wide, target2 * 0.9), hi = indexAtOrAbove (wide, target2 * 1.1);
                    const size_t i2 = argMaxIn (m, lo, hi);
                    const double p2 = refinePeakHz (wide, m, i2);
                    const size_t l3 = indexAtOrAbove (wide, 2800.0 * 0.9), h3 = indexAtOrAbove (wide, 2800.0 * 1.1);
                    const size_t i3 = argMaxIn (m, l3, h3);
                    const double p3 = refinePeakHz (wide, m, i3);

                    if (std::abs (p1 / fc - 1.0) > 0.03 || i2 == lo || i2 + 1 >= hi || std::abs (p2 / target2 - 1.0) > 0.03
                        || i3 == l3 || i3 + 1 >= h3 || std::abs (p3 / 2800.0 - 1.0) > 0.03)
                    {
                        ++badF;
                        DETAIL ("        Formant fc %.0f morph %.0f: F1 %.1f F2 %.1f (want %.1f) F3 %.1f\n", fc, morph, p1, p2, target2, p3);
                    }
                }
            check (badF == 0, "Formant: peaks at F1 = fc, F2 = F1 x (1.5 + 2 morph) and the fixed F3 = 2.8 kHz (within 3%)");
        }

        // narrowness: res widens / narrows the bandwidth (-3 dB width of F1)
        {
            auto width = [&] (double res)
            {
                const auto f = logGrid (60.0, 12000.0, 12000);
                const auto m = magCurve (26, 1000.0, res, 0.0, fs, f);
                const size_t ip = argMaxIn (m, 0, m.size());
                size_t a = ip, b = ip;
                while (a > 0 && m[a] > m[ip] * 0.7071)
                    --a;
                while (b + 1 < m.size() && m[b] > m[ip] * 0.7071)
                    ++b;
                return f[b] / f[a];
            };
            const double w0 = width (0.1), w5 = width (0.5), w9 = width (0.9);
            std::printf ("        (F1 -3 dB width ratio at res 0.1 / 0.5 / 0.9: %.3f / %.3f / %.3f)\n", w0, w5, w9);
            check (w0 > w5 && w5 > w9, "res = narrowness: the formant peaks narrow monotonically with the knob");
        }
    }

    //==========================================================================
    void testD5bPhaserFormulas()
    {
        std::printf ("\n[D5b] phaser (27): notch count, positions, feedback\n");
        constexpr double fs = 48000.0;
        const auto grid = logGrid (20.0, 0.45 * fs, 20000);

        int countBad = 0;
        for (int k = 0; k <= 5; ++k)
        {
            const int stages = 2 + 2 * k;
            const double morph = (double) k / 5.0;
            FilterDesign d;
            fd::make (27, 1000.0, 0.5, morph, fs, d);
            const auto m = magCurve (27, 1000.0, 0.5, morph, fs, grid);
            const int notches = countLocalMinima (m, 0.1);

            if (d.numSections != stages || notches != stages / 2)
            {
                ++countBad;
                DETAIL ("        phaser morph %.1f: %d sections (want %d), %d notches (want %d)\n", morph, d.numSections, stages, notches, stages / 2);
            }
        }
        check (countBad == 0, "morph 0 .. 1 in fifths gives 2, 4, ..., 12 stages and exactly stages / 2 deep notches (res 0.5 = no feedback)");

        // symmetry: the centre is a notch for 2, 6, 10 stages and a peak (+0 dB sum) for 4, 8, 12
        {
            bool ok = true;
            for (int k = 0; k <= 5; ++k)
            {
                const double mc = magAt (27, 1000.0, 0.5, (double) k / 5.0, fs, 1000.0);
                const bool notch = (2 + 2 * k) % 4 == 2;
                ok = ok && (notch ? mc < 0.05 : mc > 0.99);          // the bilinear warp of the outer stages leaves a few 0.01 rad
            }
            check (ok, "the geometric centre fc is a notch for 2 / 6 / 10 stages and a peak for 4 / 8 / 12 (half-octave stage spacing)");
        }

        // notch spacing about one octave in the middle of the bank (12 stages)
        {
            const auto m = magCurve (27, 1000.0, 0.5, 1.0, fs, grid);
            std::vector<double> pos;
            for (size_t i = 1; i + 1 < m.size(); ++i)
                if (m[i] < m[i - 1] && m[i] <= m[i + 1] && m[i] < 0.1)
                    pos.push_back (grid[i]);

            // stage density (and so the notch density) falls off towards both ends of the bank; the inner notches are the
            // "one octave apart" ones (2 stages per notch, half-octave stage spacing)
            double r23 = 0.0, r34 = 0.0;
            if (pos.size() == 6)
            {
                r23 = pos[3] / pos[2];
                r34 = pos[4] / pos[3];
            }
            std::printf ("        (12 stages: %d notches, inner ratios %.3f / %.3f)\n", (int) pos.size(), r23, r34);
            check (pos.size() == 6 && r23 > 1.7 && r23 < 2.6 && r34 > 1.7 && r34 < 2.6, "12 stages: six notches, the inner ones about an octave apart");
        }

        // feedback: bipolar around res 0.5, bounded, deepens / narrows the notch structure
        {
            FilterDesign d0, dn, dp;
            fd::make (27, 1000.0, 0.5, 0.4, fs, d0);
            fd::make (27, 1000.0, 0.0, 0.4, fs, dn);
            fd::make (27, 1000.0, 1.0, 0.4, fs, dp);
            const auto m0 = magCurve (27, 1000.0, 0.5, 0.4, fs, grid);
            const auto mn = magCurve (27, 1000.0, 0.0, 0.4, fs, grid);
            const auto mp = magCurve (27, 1000.0, 1.0, 0.4, fs, grid);
            const double max0 = m0[argMaxIn (m0, 0, m0.size())], maxN = mn[argMaxIn (mn, 0, mn.size())], maxP = mp[argMaxIn (mp, 0, mp.size())];
            std::printf ("        (peak |H| for feedback -0.95 / 0 / +0.95: %.3f / %.3f / %.3f)\n", maxN, max0, maxP);
            check (d0.fb == 0.0 && std::abs (dn.fb + 0.95) < 1.0e-12 && std::abs (dp.fb - 0.95) < 1.0e-12, "res 0.5 = no feedback, res 0 / 1 = -0.95 / +0.95");
            check (maxN > 2.0 && maxN < 3.4 && maxP > 2.0 && maxP < 3.4 && max0 < 1.0 + 1.0e-9,
                   "strong feedback adds resonant peaks (bounded: gain trim keeps |H| < 3.4), no feedback is a flat-topped notch comb");
        }
    }

    //==========================================================================
    // measured impulse response (through FilterStage) -> complex spectrum
    //==========================================================================
    struct IrSpectrum
    {
        std::vector<std::complex<float>> h;
        double binHz = 0.0;
        int size = 0, order = 0;
        double tail = 0.0;
    };

    /** Window = constant time (>= 0.6 s) and grown (up to 2^21) until the last 1/16 of the impulse response is below 1e-6:
        comb and phaser rings at strong feedback / low cutoff need seconds. */
    IrSpectrum measureIr (int type, double fc, double res, double morph, double fs, int maxOrder = 21)
    {
        int order = 14;
        while ((1 << order) < (int) (0.6 * fs))
            ++order;

        const FilterTargets t = mk (type, fc, res, morph);
        std::vector<float> l;
        double tail = 0.0;

        for (;;)
        {
            const int size = 1 << order;
            FilterStage st;
            st.prepare (fs);
            st.snap (t);

            l.assign (ix (size), 0.0f);
            std::vector<float> r (ix (size), 0.0f);
            l[0] = r[0] = 1.0f;

            for (int i = 0; i < size; i += 32)
                st.process (l.data() + i, r.data() + i, 32, t);

            tail = 0.0;
            for (int i = size - size / 16; i < size; ++i)
                tail = std::max (tail, (double) std::abs (l[ix (i)]));

            if (tail < 1.0e-6 || order >= maxOrder)
                break;

            ++order;
        }

        const int size = 1 << order;
        juce::dsp::FFT fft (order);
        std::vector<std::complex<float>> in (ix (size)), out (ix (size));
        for (int i = 0; i < size; ++i)
            in[ix (i)] = std::complex<float> (l[ix (i)], 0.0f);

        fft.perform (in.data(), out.data(), false);

        IrSpectrum s;
        s.h = std::move (out);
        s.binHz = fs / (double) size;
        s.size = size;
        s.order = order;
        s.tail = tail;
        return s;
    }

    struct IrErrorC
    {
        double worstDb = 0.0, worstAbs = 0.0, worstRel = 0.0, worstHz = 0.0;
    };

    IrErrorC compareIr (const IrSpectrum& s, int type, double fc, double res, double morph, double fs)
    {
        const FilterTargets t = mk (type, fc, res, morph);
        FilterDesign d;
        fd::make (type, (double) t.cutoffHz, (double) t.res, (double) t.morph, fs, d);

        const int lo = (int) std::ceil (20.0 / s.binHz), hi = (int) std::floor (0.45 * fs / s.binHz);
        const int stride = std::max (1, (hi - lo) / 90000);
        IrErrorC e;

        for (int k = lo; k <= hi; k += stride)
        {
            const double f = (double) k * s.binHz;
            const std::complex<double> ha = fd::response (d, f, fs);
            const std::complex<double> hm ((double) s.h[ix (k)].real(), (double) s.h[ix (k)].imag());
            const double ma = std::abs (ha), mm = std::abs (hm);

            if (ma <= 0.01)
                e.worstAbs = std::max (e.worstAbs, std::abs (mm - ma));
            else
            {
                const double db = mm > 0.0 ? std::abs (20.0 * std::log10 (mm / ma)) : 200.0;
                if (db > e.worstDb)
                {
                    e.worstDb = db;
                    e.worstHz = f;
                }
            }

            if (ma > 0.05)
                e.worstRel = std::max (e.worstRel, std::abs (hm - ha) / ma);
        }

        return e;
    }

    void testD5bResponse (bool full)
    {
        std::printf ("\n[D5b] analytic response vs measured impulse-response FFT (through FilterStage), 0.5 dB gate + complex gate\n");

        struct Rm { double res, morph; };
        const std::vector<Rm> combos = full ? std::vector<Rm> { { 0, 0 }, { 0, 1 }, { 0.5, 0.35 }, { 1, 0 }, { 1, 1 } }
                                            : std::vector<Rm> { { 0, 0 }, { 0.5, 0.35 }, { 1, 1 } };
        const std::vector<double> fsList = full ? std::vector<double> { 44100.0, 48000.0, 96000.0, 192000.0 } : std::vector<double> { 48000.0 };

        int cases = 0, bad = 0, capped = 0;
        double overall = 0.0, overallRel = 0.0;
        std::array<double, 8> worstPerType {};

        for (const double fs : fsList)
        {
            const std::vector<double> cutoffs = fs > 50000.0 ? std::vector<double> { 1000.0, 15000.0 } : std::vector<double> { 250.0, 1000.0, 6000.0, 15000.0 };

            for (int type = kD5bFirst; type < kD5bEnd; ++type)
                for (const double fc : cutoffs)
                    for (const auto& rm : combos)
                    {
                        const auto s = measureIr (type, fc, rm.res, rm.morph, fs);
                        const auto e = compareIr (s, type, fc, rm.res, rm.morph, fs);
                        ++cases;
                        capped += s.tail > 1.0e-4 ? 1 : 0;
                        worstPerType[ix (type - kD5bFirst)] = std::max (worstPerType[ix (type - kD5bFirst)], e.worstDb);
                        overall = std::max (overall, e.worstDb);
                        overallRel = std::max (overallRel, e.worstRel);

                        if (e.worstDb > 0.5 || e.worstAbs > 5.0e-4 || e.worstRel > 0.06)
                        {
                            ++bad;
                            DETAIL ("        FAIL type %d (%s) fs %.0f fc %.0f res %.2f morph %.2f: %.3f dB at %.0f Hz, abs %.2e, complex rel %.3f (order %d, tail %.1e)\n",
                                    type, warpr::kFilterTypes[ix (type)].name, fs, fc, rm.res, rm.morph, e.worstDb, e.worstHz, e.worstAbs,
                                    e.worstRel, s.order, s.tail);
                        }
                    }
        }

        std::printf ("        (%d cases; worst magnitude deviation per type 20..27, dB:", cases);
        for (int i = 0; i < 8; ++i)
            std::printf (" %d:%.3f", kD5bFirst + i, worstPerType[ix (i)]);
        std::printf (")\n        (overall worst %.4f dB, worst complex relative error %.4f, %d windows hit the 2^21 cap)\n", overall, overallRel, capped);
        check (bad == 0, "all 8 special linear types: stage impulse response matches the analytic response (|H| within 0.5 dB, complex within 6%)");
    }

    /** Teeth of the MEASURED comb sit at k fc (or the odd multiples for Comb -). */
    void testD5bCombMeasured()
    {
        std::printf ("\n[D5b] comb tooth positions in the measured spectrum\n");
        constexpr double fs = 48000.0;

        int bad = 0;
        double worst = 0.0;

        for (int type : { 20, 21 })
            for (double fc : { 440.0, 1234.0 })
            {
                const auto s = measureIr (type, fc, 1.0, 0.0, fs);
                std::vector<double> teeth;

                for (int k = 1; k <= 4; ++k)
                {
                    const double expected = type == 20 ? fc * k : fc * (2 * k - 1);
                    const int b0 = (int) std::floor (expected * 0.98 / s.binHz), b1 = (int) std::ceil (expected * 1.02 / s.binHz);
                    int best = b0;
                    for (int b = b0; b <= b1; ++b)
                        if (std::abs (s.h[ix (b)]) > std::abs (s.h[ix (best)]))
                            best = b;

                    const double a = std::abs (s.h[ix (best - 1)]), m = std::abs (s.h[ix (best)]), c = std::abs (s.h[ix (best + 1)]);
                    const double den = a - 2.0 * m + c;
                    const double off = std::abs (den) < 1.0e-12 ? 0.0 : 0.5 * (a - c) / den;
                    const double pos = ((double) best + off) * s.binHz;
                    teeth.push_back (pos);
                    const double err = std::abs (pos / expected - 1.0);
                    worst = std::max (worst, err);

                    if (err > 0.004)
                    {
                        ++bad;
                        DETAIL ("        comb %d fc %.0f tooth %d: %.2f Hz (expected %.2f)\n", type, fc, k, pos, expected);
                    }
                }

                // spacing: consecutive teeth are fc apart (Comb +) / 2 fc apart (Comb -)
                const double spacing = (teeth[3] - teeth[0]) / 3.0;
                const double want = type == 20 ? fc : 2.0 * fc;
                if (std::abs (spacing / want - 1.0) > 0.004)
                {
                    ++bad;
                    DETAIL ("        comb %d fc %.0f: tooth spacing %.2f Hz (expected %.2f)\n", type, fc, spacing, want);
                }
            }

        std::printf ("        (worst tooth position error %.3f%%)\n", 100.0 * worst);
        check (bad == 0, "measured teeth: Comb + at k fc, Comb - at the odd multiples, spacing = fc / 2 fc, all within 0.4% (~7 cents)");
    }

    //==========================================================================
    void testD5bStability (bool full)
    {
        std::printf ("\n[D5b] stability sweep (cutoff x res x drive x morph x fs, types 20-27)\n");

        const std::vector<double> fsList = full ? std::vector<double> { 44100.0, 48000.0, 96000.0, 192000.0 } : std::vector<double> { 48000.0 };
        const int len = full ? 24000 : 6000;
        const auto input = makeNoise (len, 0.25f, 4243);

        int runs = 0, nonFinite = 0, tooLoud = 0;
        double worstPeak = 0.0;

        for (const double fs : fsList)
            for (int type = kD5bFirst; type < kD5bEnd; ++type)
                for (const double fc : { 20.0, 1000.0, 20000.0 })
                    for (const double res : { 0.0, 0.5, 1.0 })
                        for (const double drv : { 0.0, 24.0 })
                            for (const double morph : { 0.0, 1.0 })
                            {
                                const auto y = runStageConst (fs, mk (type, fc, res, morph, drv), input);
                                ++runs;

                                if (! allFinite (y))
                                {
                                    ++nonFinite;
                                    DETAIL ("        non-finite: type %d fs %.0f fc %.0f res %.1f drive %.0f morph %.0f\n", type, fs, fc, res, drv, morph);
                                    continue;
                                }

                                const double pk = peakOf (y);
                                worstPeak = std::max (worstPeak, pk);

                                if (pk >= 8.0)
                                {
                                    ++tooLoud;
                                    DETAIL ("        |y| = %.2f: type %d fs %.0f fc %.0f res %.1f drive %.0f morph %.0f\n", pk, type, fs, fc, res, drv, morph);
                                }
                            }

        std::printf ("        (%d runs, noise at -12 dBFS, worst peak %.3f)\n", runs, worstPeak);
        check (nonFinite == 0, "no NaN / inf anywhere in the sweep (comb feedback 0.97, phaser feedback +-0.95, 4x-narrow formants)");
        check (tooLoud == 0, "|y| < 8 everywhere");

        // a long, loud, tonal burst at the extreme feedback: the in-loop limiter keeps the comb bounded
        {
            const auto tone = makeSine (48000, 1000.0, 48000.0, 0.9f);               // exactly on a tooth of the 1 kHz comb
            const auto y20 = runStageConst (48000.0, mk (20, 1000.0, 1.0, 0.0), tone);
            const auto y21 = runStageConst (48000.0, mk (21, 1000.0, 1.0, 0.0), tone);
            std::printf ("        (0.9 tone on a g = 0.97 tooth: peak %.3f / %.3f)\n", peakOf (y20), peakOf (y21));
            check (allFinite (y20) && allFinite (y21) && peakOf (y20) < 9.0 && peakOf (y21) < 9.0,
                   "resonating a loud tone at feedback 0.97: finite and held below the in-loop limiter's ceiling (7.5 + input)");
        }
    }

    //==========================================================================
    void testD5bBypass()
    {
        std::printf ("\n[D5b] bypass nulls and the open-LP rule (types 20-27)\n");
        constexpr double fs = 48000.0;
        const auto noise = makeNoise (9600, 0.5f, 6);
        const auto smooth = makeMix (9600, fs);

        int offBad = 0, mixBad = 0, openBad = 0, rampBad = 0;

        for (int type = kD5bFirst; type < kD5bEnd; ++type)
        {
            {
                FilterStage st;
                st.prepare (fs);
                const FilterTargets wild = mk (type, 120.0, 1.0, 1.0, 24.0, 1.0, false);        // enable OFF
                st.snap (wild);
                std::vector<float> l = noise, r = noise;
                for (size_t i = 0; i < l.size(); i += 32)
                    st.process (l.data() + i, r.data() + i, 32, wild);
                if (! (bitEqual (l, noise) && bitEqual (r, noise) && st.isParked()))
                    ++offBad;
            }
            {
                FilterStage st;
                st.prepare (fs);
                const FilterTargets zero = mk (type, 300.0, 1.0, 0.5, 12.0, 0.0, true);         // mix 0
                st.snap (zero);
                std::vector<float> l = noise, r = noise;
                for (size_t i = 0; i < l.size(); i += 32)
                    st.process (l.data() + i, r.data() + i, 32, zero);
                if (! (bitEqual (l, noise) && bitEqual (r, noise)))
                    ++mixBad;
            }
            {
                // the open-LP rule must NOT apply: cutoff 20 kHz, res 0, drive 0, enabled -> the stage is live (not parked)
                FilterStage st;
                st.prepare (fs);
                const FilterTargets open = mk (type, 20000.0, 0.0, 0.0, 0.0, 1.0, true);
                st.snap (open);
                std::vector<float> l = noise, r = noise;
                for (size_t i = 0; i < l.size(); i += 32)
                    st.process (l.data() + i, r.data() + i, 32, open);
                if (st.isParked())
                    ++openBad;
            }
            {
                // enable toggled mid-run: 5 ms ramp, then bit-exact again
                const FilterTargets on = mk (type, 800.0, 0.5, 0.4);
                const int offAt = 160;
                const auto y = runStage (fs, on, smooth, [&] (int ci, FilterTargets& t) { if (ci >= offAt) t.enable = false; });
                bool tailExact = true;
                for (size_t i = (size_t) offAt * 32 + 256; i < smooth.size(); ++i)
                    tailExact = tailExact && y[i] == smooth[i];
                if (! tailExact)
                    ++rampBad;
            }
        }

        check (offBad == 0, "filterEnable off: bit-identical output and a parked stage for every type 20-27");
        check (mixBad == 0, "filterMix 0: bit-identical output for every type 20-27");
        check (openBad == 0, "the open low-pass rule does not apply: a comb / all-pass / formant / phaser at 20 kHz, res 0 stays live");
        check (rampBad == 0, "disabling mid-note ramps over 5 ms and is bit-exact afterwards (delay lines are parked and wiped)");

        // re-enabling after a parked period starts from a clean (wiped) comb: output equals a fresh stage
        {
            const FilterTargets on = mk (20, 700.0, 0.9, 0.2);
            const auto ref = runStageConst (fs, on, noise);
            std::vector<float> l = noise, r = noise;
            FilterStage st;
            st.prepare (fs);
            FilterTargets t = on;
            st.snap (t);
            // run enabled (fills the delay line), then park, then re-enable on the same input
            for (size_t i = 0; i < 3200; i += 32)
                st.process (l.data() + i, r.data() + i, 32, t);
            t.enable = false;
            for (size_t i = 3200; i < 6400; i += 32)
                st.process (l.data() + i, r.data() + i, 32, t);
            const bool parked = st.isParked();
            t.enable = true;
            std::vector<float> a (noise.begin(), noise.begin() + 3200), b = a;
            for (size_t i = 0; i < a.size(); i += 32)
                st.process (a.data() + i, b.data() + i, 32, t);
            // the first 5 ms ramp in; afterwards the output is determined by the (clean) state: compare the tail to the fresh run's tail
            double worst = 0.0;
            for (size_t i = 2000; i < a.size(); ++i)
                worst = std::max (worst, (double) std::abs (a[i] - ref[i]));
            std::printf ("        (re-enabled comb vs a fresh comb after 2000 samples: %.2e)\n", worst);
            check (parked && worst < 1.0e-3, "a parked comb forgets its history: after re-enable it matches a fresh run (delay line wiped)");
        }
    }

    //==========================================================================
    void testD5bSwitching()
    {
        std::printf ("\n[D5b] type-switch crossfade, D5a <-> D5b and D5b <-> D5b (DiscreteSwitchFade, 10 ms + 64 samples priming)\n");
        constexpr double fs = 48000.0;
        const auto sig = makeMix (12000, fs);

        int pairs = 0, stepBad = 0, tailBad = 0;
        double worstRatio = 0.0;

        for (int a = 0; a < kD5bEnd; ++a)
            for (int b = 0; b < kD5bEnd; ++b)
            {
                if (a == b || (a < kD5aTypes && b < kD5aTypes))
                    continue;                                       // D5a pairs are covered by the D5a section

                const auto r = switchPair (a, b, fs, sig);
                ++pairs;
                worstRatio = std::max (worstRatio, r.step / std::max (r.bound, 1.0e-9));

                if (! r.stepOk)
                {
                    ++stepBad;
                    DETAIL ("        step: %d -> %d: max step %.5f > bound %.5f\n", a, b, r.step, r.bound);
                }
                if (! r.tailOk)
                {
                    ++tailBad;
                    DETAIL ("        tail: %d -> %d: |switched - new| = %.5f\n", a, b, r.tail);
                }
            }

        std::printf ("        (%d transitions; worst step / bound = %.3f)\n", pairs, worstRatio);
        check (stepBad == 0, "every transition into / out of / among types 20-27: max step <= natural step + 2 x difference / 480 samples");
        check (tailBad == 0, "every transition ends on the new type (|switched - new| < 2e-3 after the fade; comb / phaser memory has died away)");

        // a switch from a running comb to another comb: the old line keeps ringing through the fade, the new line starts silent
        {
            const auto sq = makeSaw (12000, 110.0, fs, 0.4f);
            const auto r1 = switchPair (20, 21, fs, sq);
            const auto r2 = switchPair (21, 20, fs, sq);
            check (r1.stepOk && r1.tailOk && r2.stepOk && r2.tailOk, "Comb + <-> Comb - on a saw: click-free");
        }
    }

    //==========================================================================
    void testD5bSweepsAndWalk (bool full)
    {
        std::printf ("\n[D5b] comb cutoff sweep (per-sample delay interpolation) and random modulation walk\n");
        constexpr double fs = 48000.0;
        const auto sine = makeSine (19200, 1000.0, fs, 0.25f);
        const int sweepChunks = 9600 / 32;
        const double a = std::log (400.0), b = std::log (1600.0);

        for (int type : { 20, 21 })
        {
            const auto up = runStage (fs, mk (type, 400.0, 0.5), sine, [&] (int ci, FilterTargets& t)
            {
                t.cutoffHz = (float) std::exp (a + (b - a) * std::min (1.0, (double) ci / (double) sweepChunks));
            });
            const auto down = runStage (fs, mk (type, 1600.0, 0.5), sine, [&] (int ci, FilterTargets& t)
            {
                t.cutoffHz = (float) std::exp (b + (a - b) * std::min (1.0, (double) ci / (double) sweepChunks));
            });

            const double su = maxStepIn (up, 400, up.size()), sd = maxStepIn (down, 400, down.size());
            std::printf ("        (comb %d: max step up %.5f, down %.5f, peak %.3f / %.3f)\n", type, su, sd, peakOf (up), peakOf (down));
            check (allFinite (up) && allFinite (down) && su < 0.10 && sd < 0.10, "swept comb: finite and no sample step above 0.10 (a 1 kHz tone at 0.25 through g = 0.49 steps at most ~0.06)");
        }

        const int chunks = full ? 15000 : 3000;
        juce::Random rng (123);
        FilterStage st;
        st.prepare (fs);
        FilterTargets t = mk (20, 1000.0);
        st.snap (t);

        std::vector<float> l (32), r (32);
        bool finite = true;
        double peak = 0.0;

        for (int c = 0; c < chunks; ++c)
        {
            if (rng.nextInt (25) == 0)
                t.type = kD5bFirst + rng.nextInt (kD5bEnd - kD5bFirst);

            t.cutoffHz = (float) (20.0 * std::pow (1000.0, (double) rng.nextFloat()));
            t.res = rng.nextFloat();
            t.morph = rng.nextFloat();
            t.driveDb = rng.nextInt (3) == 0 ? 24.0f * rng.nextFloat() : 0.0f;
            t.mix = rng.nextFloat() > 0.2f ? 1.0f : rng.nextFloat();
            t.enable = rng.nextInt (200) != 0;

            for (size_t i = 0; i < 32; ++i)
                l[i] = r[i] = 0.3f * (rng.nextFloat() * 2.0f - 1.0f);

            st.process (l.data(), r.data(), 32, t);

            for (size_t i = 0; i < 32; ++i)
            {
                finite = finite && std::isfinite (l[i]) && std::isfinite (r[i]);
                peak = std::max ({ peak, (double) std::abs (l[i]), (double) std::abs (r[i]) });
            }
        }

        std::printf ("        (%d chunks of fully random cutoff / res / morph / drive / mix / type 20-27, peak %.3f)\n", chunks, peak);
        check (finite, "random modulation walk over types 20-27: no NaN / inf");
        check (peak < 64.0, "random modulation walk over types 20-27: output below the voice guard (|y| < 64)");
    }

    //==========================================================================
    double combTrackRms (int note, float keyTrack, float cutoff)
    {
        PluginProcessor p;
        prepSustained (p);
        setParam (p, "ampSustain", 0.4f);
        setParam (p, "filterType", 20.0f);                         // Comb +
        setParam (p, "filterCutoff", cutoff);
        setParam (p, "filterResonance", 0.5f);
        setParam (p, "filterKeyTrack", keyTrack);
        const auto r = renderBlocks (p, 48000.0, 200, 256, note, nullptr);
        return tailRms (r.l, 19200);
    }

    void testD5bKeyTrack()
    {
        std::printf ("\n[D5b] comb key tracking through the voice\n");

        const double c4 = 261.6256f;
        // key 1: the comb follows the note (cutoff anchored at note 60), so G4 (67) and C5 (72) both sit on the teeth
        const double tuned67 = combTrackRms (67, 1.0f, (float) c4);
        const double free67 = combTrackRms (67, 0.0f, (float) c4);               // fixed 261.6 Hz comb vs a 392 Hz saw: every other harmonic misses
        const double t60 = combTrackRms (60, 1.0f, (float) c4), t72 = combTrackRms (72, 1.0f, (float) c4);
        const double db = 20.0 * std::log10 (tuned67 / free67);
        std::printf ("        (note 67: tuned %.4f vs fixed comb %.4f = %.2f dB; tuned note 60 %.4f / note 72 %.4f)\n", tuned67, free67, db, t60, t72);
        check (tuned67 > 0.02 && db > 1.2, "key track 1: the comb tuned to the played note (every harmonic on a tooth) is louder than the same comb left at 261.6 Hz");
        check (std::abs (t72 / t60 - 1.0) < 0.06, "key track 1: note 72 and note 60 both sit on the teeth -> the same level (the comb follows the pitch one-to-one)");

        // analytic: key-tracked cutoff for the voice is the comb fundamental
        FilterDesign d;
        fd::make (20, fd::keyTrackedCutoff (c4, 1.0, 67.0), 1.0, 0.0, 48000.0, d);
        const double f0 = 48000.0 / d.comb.delay;
        check (std::abs (1200.0 * std::log2 (f0 / (440.0 * std::exp2 ((67.0 - 69.0) / 12.0)))) < 5.0,
               "key track 1 at note 67: the comb's loop period equals the note's period within 5 cents");
    }

    //==========================================================================
    void testD5bBackend()
    {
        std::printf ("\n[D5b] backend: computeFilterResponse for the special linear types\n");
        constexpr int n = 256;

        PluginProcessor p;
        auto& be = p.getBackendImpl();
        be.setSampleRate (48000.0);

        std::array<float, n> db {};
        auto freq = [] (int i) { return 20.0 * std::pow (1000.0, (double) i / (double) (n - 1)); };

        auto expectEqual = [&] (int type, double cutoff, double res, double morph)
        {
            double worst = 0.0;
            for (int i = 0; i < n; ++i)
                worst = std::max (worst, std::abs ((double) db[ix (i)] - FilterResponse::magnitudeDbAt (type, cutoff, res, 0.0, morph, 48000.0, freq (i))));
            return worst < 0.01;
        };

        // comb: teeth
        setParam (p, "filterType", 20.0f);
        setParam (p, "filterCutoff", 1000.0f);
        setParam (p, "filterResonance", 0.4f);                     // g = 0.39: teeth ~10% wide, resolved by the 256-point grid
        setParam (p, "filterMorph", 0.0f);
        be.computeFilterResponse (db.data(), n);
        const float mx = *std::max_element (db.begin(), db.end()), mn = *std::min_element (db.begin(), db.end());
        std::printf ("        (Comb +, 1 kHz, res 0.4: %.1f .. %.1f dB over the 256 points; ideal +-3.55 dB)\n", (double) mn, (double) mx);
        check (expectEqual (20, 1000.0, 0.4, 0.0) && mx > 2.5 && mn < -2.5, "Comb +: 256 points equal FilterResponse and show the teeth (about +3.5 / -3.5 dB)");

        // formant: A vowel peak
        setParam (p, "filterType", 26.0f);
        setParam (p, "filterResonance", 0.5f);
        be.computeFilterResponse (db.data(), n);
        int pk = 0;
        for (int i = 0; i < n; ++i)
            if (db[ix (i)] > db[ix (pk)])
                pk = i;
        check (expectEqual (26, 1000.0, 0.5, 0.0) && std::abs (freq (pk) / 650.0 - 1.0) < 0.06 && db[ix (pk)] > -1.0 && db[ix (pk)] < 6.0,
               "Morph Formant, vowel A: the drawn peak sits at F1 = 650 Hz at about 0 dB");

        // phaser: notches
        setParam (p, "filterType", 27.0f);
        setParam (p, "filterResonance", 0.5f);
        setParam (p, "filterMorph", 0.2f);
        be.computeFilterResponse (db.data(), n);
        const float pmin = *std::min_element (db.begin(), db.end());
        check (expectEqual (27, 1000.0, 0.5, 0.2) && pmin < -10.0, "Phaser (4 stages): the drawn curve carries deep notches");

        // all-pass: a flat line (magnitude is 1; the effect is the phase, audible through filterMix)
        setParam (p, "filterType", 23.0f);
        setParam (p, "filterMorph", 0.5f);
        be.computeFilterResponse (db.data(), n);
        bool flat = true;
        for (float v : db)
            flat = flat && std::abs (v) < 0.01f;
        check (flat, "Serial AP: |H| = 1 -> a flat 0 dB line");

        // the other new types respond too: not flat, equal to FilterResponse, inside the display range
        bool allOk = true;
        for (int type : { 21, 22, 24, 25, 27 })
        {
            setParam (p, "filterType", (float) type);
            setParam (p, "filterCutoff", 2000.0f);
            setParam (p, "filterResonance", 0.7f);
            setParam (p, "filterMorph", 0.3f);
            be.computeFilterResponse (db.data(), n);
            bool inRange = true;
            for (float v : db)
                inRange = inRange && std::isfinite (v) && v >= -48.0f && v <= 24.0f;
            allOk = allOk && inRange && expectEqual (type, 2000.0, 0.7, 0.3);
        }
        check (allOk, "types 21 / 22 / 24 / 25 / 27: backend curve equals FilterResponse, finite, inside [-48, +24] dB");

        // timing (comb needs complex exponentials per point)
        {
            setParam (p, "filterType", 20.0f);
            setParam (p, "filterCutoff", 200.0f);
            setParam (p, "filterResonance", 0.9f);
            double best = 1.0e9;
            for (int rep = 0; rep < 50; ++rep)
            {
                const auto t0 = std::chrono::steady_clock::now();
                be.computeFilterResponse (db.data(), n);
                const auto t1 = std::chrono::steady_clock::now();
                best = std::min (best, std::chrono::duration<double, std::milli> (t1 - t0).count());
            }
            std::printf ("        (computeFilterResponse, 256 points, comb: best of 50 = %.3f ms)\n", best);
            check (best < 1.0, "computeFilterResponse returns 256 comb points in < 1 ms");
        }
    }

    //==========================================================================
    void testD5bProcessor()
    {
        std::printf ("\n[D5b] allocations, determinism, processor-level nulls\n");
        constexpr double fs = 48000.0;

        // class level: delay lines are allocated in prepare(); nothing allocates while combs run and change type
        {
            FilterStage st;
            st.prepare (fs);
            FilterTargets t = mk (20, 400.0, 0.9, 0.3);
            st.snap (t);
            std::vector<float> l (32, 0.1f), r (32, 0.1f);
            juce::Random rng (7);

            std::size_t allocations = 0;
            {
                warpr::tests::ScopedAllocCount counter;
                for (int round = 0; round < 400; ++round)
                {
                    if (round % 7 == 0)
                        t.type = kD5bFirst + (round / 7) % 8;
                    t.cutoffHz = 50.0f + 4000.0f * rng.nextFloat();
                    t.res = rng.nextFloat();
                    t.morph = rng.nextFloat();
                    for (size_t i = 0; i < 32; ++i)
                        l[i] = r[i] = 0.2f * (rng.nextFloat() * 2.0f - 1.0f);
                    st.process (l.data(), r.data(), 32, t);
                }
                allocations = counter.count();
            }
            check (allocations == 0, "FilterStage: zero allocations while running and switching all of types 20-27 (400 chunks)");
        }

        // processor level
        {
            PluginProcessor p;
            prepSustained (p);
            setParam (p, "unisonVoices", 4.0f);
            setParam (p, "filterType", 20.0f);
            setParam (p, "filterCutoff", 400.0f);
            setParam (p, "filterResonance", 0.9f);
            setParam (p, "filterDrive", 6.0f);
            p.setPlayConfigDetails (0, 2, fs, 256);
            p.prepareToPlay (fs, 256);

            juce::AudioBuffer<float> buf (2, 256);
            juce::MidiBuffer on;
            on.addEvent (juce::MidiMessage::noteOn (1, 52, 0.9f), 0);
            on.addEvent (juce::MidiMessage::noteOn (1, 59, 0.9f), 0);
            p.processBlock (buf, on);
            juce::MidiBuffer none;

            std::size_t allocations = 0;
            bool sounding = false;
            for (int round = 0; round < 16; ++round)
            {
                setParam (p, "filterType", (float) (kD5bFirst + round % 8));        // allocates (APVTS): uncounted
                setParam (p, "filterCutoff", 200.0f + 900.0f * (float) (round % 4));
                setParam (p, "filterMorph", 0.15f * (float) (round % 7));

                warpr::tests::ScopedAllocCount counter;
                for (int i = 0; i < 6; ++i)
                {
                    p.processBlock (buf, none);
                    sounding = sounding || p.getSynth().getNumActiveVoices() > 0;
                }
                allocations += counter.count();
            }

            check (sounding, "the counted window contained sounding voices");
            check (allocations == 0, "processBlock performs zero allocations with active combs / formants / phasers (2 voices x unison 4, 16 type changes)");
            p.releaseResources();
        }

        // determinism
        {
            auto run = [&]
            {
                PluginProcessor p;
                prepSustained (p);
                setParam (p, "unisonVoices", 3.0f);
                setParam (p, "filterType", 20.0f);
                setParam (p, "filterCutoff", 330.0f);
                setParam (p, "filterResonance", 0.8f);
                setParam (p, "filterMorph", 0.3f);
                return renderBlocks (p, fs, 80, 256, 50, [&p] (int b)
                {
                    if (b == 15) setParam (p, "filterType", 26.0f);
                    if (b == 30) setParam (p, "filterType", 27.0f);
                    if (b == 45) setParam (p, "filterType", 21.0f);
                    if (b == 55) setParam (p, "filterCutoff", 2500.0f);
                    if (b == 65) setParam (p, "filterType", 23.0f);
                });
            };

            const auto a = run(), b = run();
            check (bitEqual (a.l, b.l) && bitEqual (a.r, b.r) && peakOf (a.l) > 0.05, "two identical sessions with mid-note changes among the D5b types are bit-equal");
        }

        // processor-level nulls: comb with mix 0 / filterEnable off equal the default patch exactly
        {
            auto make = [&] (const std::function<void (PluginProcessor&)>& cfg)
            {
                PluginProcessor p;
                prepSustained (p);
                cfg (p);
                return renderBlocks (p, fs, 40, 256, 48, nullptr);
            };

            const auto defaultR = make ([] (PluginProcessor&) {});
            const auto mixZero = make ([] (PluginProcessor& p)
            {
                setParam (p, "filterType", 20.0f); setParam (p, "filterCutoff", 200.0f); setParam (p, "filterResonance", 1.0f); setParam (p, "filterMix", 0.0f);
            });
            const auto offComb = make ([] (PluginProcessor& p)
            {
                setParam (p, "filterType", 27.0f); setParam (p, "filterEnable", 0.0f); setParam (p, "filterDrive", 24.0f);
            });
            const auto active = make ([] (PluginProcessor& p)
            {
                setParam (p, "filterType", 21.0f); setParam (p, "filterCutoff", 130.0f); setParam (p, "filterResonance", 0.8f);
            });

            check (bitEqual (defaultR.l, mixZero.l) && bitEqual (defaultR.r, mixZero.r), "processor: Comb + with filterMix 0 is bit-identical to the default patch");
            check (bitEqual (defaultR.l, offComb.l) && bitEqual (defaultR.r, offComb.r), "processor: Phaser with filterEnable off is bit-identical to the default patch");
            const double nl = nullDb (defaultR.l, active.l);
            std::printf ("        (Comb - @ 130 Hz, res 0.8 vs bypass: null %.1f dB)\n", nl);
            check (nl > -20.0 && allFinite (active.l) && allFinite (active.r), "processor: an active Comb - really changes the sound");
        }
    }

    //==========================================================================
    //==========================================================================
    // D5c: nonlinear filters, types 28-33 (Res LP, Res HP, Dist LP, Scream, S&H, Ring Mod)
    //==========================================================================
    //==========================================================================
    constexpr int kD5cFirst = 28, kD5cEnd = 34;                  // [first, end)
    constexpr double kNlBandFrac = 0.38;                         // measured-vs-analytic band edge (x fs): the half-band pair droops < 0.2 dB below 0.4 fs

    /** Amplitude of the sinusoid at exactly `hz` in the last `n` samples: a Hann-windowed single-frequency DFT, which is
        exact for a line AT that frequency whatever its bin alignment. */
    double lineAmp (const std::vector<float>& y, double fs, double hz, size_t n = 32768)
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

    std::vector<float> makeSquare (int n, double hz, double fs, float amp)
    {
        std::vector<float> v (ix (n));
        double phase = 0.0;
        for (int i = 0; i < n; ++i)
        {
            v[ix (i)] = phase < 0.5 ? amp : -amp;
            phase += hz / fs;
            phase -= std::floor (phase);
        }
        return v;
    }

    double rmsOf (const std::vector<float>& v, size_t from, size_t to)
    {
        double s = 0.0;
        for (size_t i = from; i < to && i < v.size(); ++i)
            s += (double) v[i] * (double) v[i];
        return std::sqrt (s / (double) std::max<size_t> (1, to - from));
    }

    /** A bare FilterCore (no stage, no drive stage, no fade) run over `in` in 32-sample chunks. */
    std::vector<float> runCoreDirect (const FilterDesign& d, const std::vector<float>& in)
    {
        warpr::FilterCore core;
        core.setDesign (d);

        std::vector<double> x (in.begin(), in.end()), y (in.size(), 0.0);
        for (size_t i = 0; i < x.size(); i += 32)
        {
            const int n = (int) std::min<size_t> (32, x.size() - i);
            core.process (0, x.data() + i, y.data() + i, n);
        }

        std::vector<float> out (y.size());
        for (size_t i = 0; i < y.size(); ++i)
            out[i] = (float) y[i];
        return out;
    }

    //==========================================================================
    void testD5cTable()
    {
        std::printf ("\n[D5c] type table\n");

        bool ok = true;
        for (int i = kD5cFirst; i < kD5cEnd; ++i)
        {
            const auto& info = warpr::kFilterTypes[ix (i)];
            ok = ok && info.implemented && ! info.linear && info.build == warpr::FilterBuild::D5c;
            std::printf ("        %d: %s  [%s]%s%s\n", i, info.name, info.topology,
                         info.localOversampling ? "  (local 2x)" : "", info.aliasByDesign ? "  (aliasByDesign)" : "");
        }

        using FF = warpr::FilterFamily;
        const auto fam = [] (int i) { return warpr::kFilterTypes[ix (i)].family; };
        check (ok && warpr::kNumImplementedFilterTypes == 34, "types 28-33 are implemented, nonlinear (not LTI), build D5c; all 34 types implemented");
        check (fam (28) == FF::Ladder && fam (29) == FF::Ladder && fam (30) == FF::Ladder && fam (31) == FF::Scream
               && fam (32) == FF::SampleHold && fam (33) == FF::RingMod, "families: 28-30 ladder, 31 scream, 32 S&H, 33 ring mod");
        check (std::strcmp (warpr::kFilterTypes[28].name, "Res LP") == 0 && std::strcmp (warpr::kFilterTypes[29].name, "Res HP") == 0
               && std::strcmp (warpr::kFilterTypes[30].name, "Dist LP") == 0 && std::strcmp (warpr::kFilterTypes[31].name, "Scream") == 0
               && std::strcmp (warpr::kFilterTypes[32].name, "S&H") == 0 && std::strcmp (warpr::kFilterTypes[33].name, "Ring Mod") == 0,
               "names (from the spec's choice list): Res LP, Res HP, Dist LP, Scream, S&H, Ring Mod");

        bool flagsOk = true;
        for (int i = 0; i < warpr::kNumFilterTypes; ++i)
        {
            const auto& info = warpr::kFilterTypes[ix (i)];
            const bool wantOs = i == 28 || i == 29 || i == 30 || i == 31 || i == 33;
            flagsOk = flagsOk && info.localOversampling == wantOs && info.aliasByDesign == (i == 32);
        }
        check (flagsOk, "local 2x on 28 / 29 / 30 / 31 / 33, aliasByDesign only on S&H (32); no linear type carries either flag");

        using K = warpr::NonlinearSpec::Kind;
        const K kinds[6] = { K::Ladder, K::Ladder, K::Ladder, K::Scream, K::SampleHold, K::RingMod };
        const bool wantOs[6] = { true, true, true, true, false, true };
        int bad = 0;

        for (int t = kD5cFirst; t < kD5cEnd; ++t)
        {
            FilterDesign d, dOff;
            fd::make (t, 1000.0, 0.5, 0.5, 48000.0, d);
            fd::make (t, 1000.0, 0.5, 0.5, 48000.0, dOff, 0.0, false);
            const auto k = ix (t - kD5cFirst);

            if (d.topo != FilterDesign::Topo::Nonlinear || d.nl.kind != kinds[k] || d.nl.os2x != wantOs[k] || dOff.nl.os2x
                || fd::isOpenLowpass (t, 20000.0, 0.0, 0.0))
                ++bad;
        }
        check (bad == 0, "designs are Topo::Nonlinear with the right kind; localOversampling = false builds the base-rate core; the open-LP rule never applies");
    }

    //==========================================================================
    void testD5cFormulas()
    {
        std::printf ("\n[D5c] small-signal response formulas (ladder closed loop, scream band-pass, S&H pre-LP, ring mod)\n");
        constexpr double fs = 48000.0;
        auto mag = [&] (int type, double fc, double res, double morph, double hz) { return magAt (type, fc, res, morph, fs, hz); };

        // ---- ladder low-pass: four equal poles; cutoff = the pole frequency
        check (std::abs (mag (28, 1000.0, 0.0, 1.0, 1000.0) - 0.25) < 1.0e-9 && std::abs (mag (28, 1000.0, 0.0, 0.0, 1000.0) - 0.5) < 1.0e-9,
               "Res LP, res 0: |H(fc)| = 1/4 with the 4-pole taps (morph 1) and 1/2 with the 2-pole taps (morph 0)");

        const double k5 = 4.2 * 0.5;
        check (std::abs (mag (28, 1000.0, 0.5, 1.0, 10.0) - (1.0 + 0.5 * k5) / (1.0 + k5)) < 2.0e-3,
               "Res LP: DC gain = (1 + k/2) / (1 + k), k = 4.2 res (the bass loss of the feedback, half compensated)");

        {
            const double k = 4.2 * 0.9, comp = 1.0 + 0.5 * k;
            const double expected = comp * 0.25 / (1.0 - k / 4.0);          // loop phase -180 deg, |H1^4| = 1/4 at fc
            const double got = mag (28, 1000.0, 0.9, 1.0, 1000.0);
            std::printf ("        (res 0.9: |H(fc)| = %.4f, expected %.4f)\n", got, expected);
            check (std::abs (got / expected - 1.0) < 1.0e-5, "Res LP, res 0.9: resonance peak at fc = comp x 0.25 / (1 - k/4)");
        }

        // ---- ladder high-pass
        check (std::abs (mag (29, 1000.0, 0.0, 1.0, 1000.0) - 0.25) < 1.0e-9 && std::abs (mag (29, 1000.0, 0.0, 0.0, 1000.0) - 0.5) < 1.0e-9,
               "Res HP, res 0: |H(fc)| = 1/4 (4-pole taps 1,-4,6,-4,1) and 1/2 (2-pole taps)");
        check (mag (29, 1000.0, 0.0, 1.0, 10.0) < 1.0e-6 && std::abs (mag (29, 1000.0, 0.0, 1.0, 20000.0) - 1.0) < 0.01,
               "Res HP: blocks DC, passes the top of the band at unity");
        {
            const double slope = dbOf (mag (29, 1000.0, 0.0, 1.0, 100.0)) - dbOf (mag (29, 1000.0, 0.0, 1.0, 50.0));
            std::printf ("        (Res HP 4-pole slope 50 -> 100 Hz: %.2f dB/oct)\n", slope);
            check (std::abs (slope - 24.08) < 0.5, "Res HP, 4-pole taps: 24 dB/oct");
        }

        // ---- Dist LP: morph (the in-loop distortion) never moves the small-signal curve; it equals the 4-pole Res LP
        {
            bool same = true;
            for (double f : { 100.0, 800.0, 1000.0, 1300.0, 5000.0, 15000.0 })
                same = same && std::abs (mag (30, 1000.0, 0.5, 0.0, f) - mag (30, 1000.0, 0.5, 1.0, f)) < 1.0e-12
                            && std::abs (mag (30, 1000.0, 0.5, 0.3, f) - mag (28, 1000.0, 0.5, 1.0, f)) < 1.0e-12;
            check (same, "Dist LP: small-signal response = 4-pole Res LP whatever the morph (the distortion is a large-signal effect)");
        }

        // ---- Scream: the peak-normalised band-pass
        check (std::abs (mag (31, 1000.0, 0.5, 0.7, 1000.0) - 1.0) < 1.0e-9 && std::abs (mag (31, 1000.0, 0.0, 0.0, 1000.0) - 1.0) < 1.0e-9,
               "Scream: 0 dB peak at fc at any res / morph (peak-normalised band-pass)");
        check (mag (31, 1000.0, 1.0, 0.0, 1200.0) < 0.2 && mag (31, 1000.0, 0.0, 0.0, 1200.0) > 0.8
               && std::abs (mag (31, 1000.0, 0.5, 0.0, 800.0) - mag (31, 1000.0, 0.5, 1.0, 800.0)) < 1.0e-12,
               "Scream: res narrows the band (Q 0.7 .. 28); morph (scream amount) does not change the drawn curve");

        // ---- S&H: the pre-LP
        check (std::abs (mag (32, 1000.0, 1.0, 0.0, 400.0) - 0.5) < 1.0e-9 && std::abs (mag (32, 1000.0, 0.0, 0.0, 10000.0) - 1.0) < 0.01,
               "S&H: res 1 puts two poles at 0.4 x the hold rate (|H| = 1/2 there), res 0 leaves the pre-LP open");

        // ---- Ring mod: no magnitude response
        {
            bool flat = true;
            for (double f : { 30.0, 440.0, 5000.0, 19000.0 })
                flat = flat && mag (33, 3000.0, 0.7, 0.4, f) == 1.0 && FilterResponse::magnitudeDbAt (33, 3000.0, 0.7, 0.0, 0.4, fs, f) == 0.0;
            check (flat, "Ring Mod: flat 0 dB display (the cutoff guide marks the carrier)");
        }

        // ---- drive never changes the drawn curve
        {
            bool same = true;
            for (int t = kD5cFirst; t < kD5cEnd; ++t)
                for (double f : { 200.0, 2000.0, 9000.0 })
                    same = same && FilterResponse::magnitudeDbAt (t, 2000.0, 0.6, 0.0, 0.4, fs, f) == FilterResponse::magnitudeDbAt (t, 2000.0, 0.6, 24.0, 0.4, fs, f);
            check (same, "FilterResponse for 28-33 is independent of the drive (normalised saturators: unity small-signal gain)");
        }
    }

    //==========================================================================
    // measured small-signal impulse response (through FilterStage) vs the analytic curve
    //==========================================================================
    struct IrScaled
    {
        std::vector<std::complex<float>> h;
        double binHz = 0.0, tail = 0.0;
        int order = 0;
    };

    /** Impulse of amplitude `amp` (small: the saturators then act as their unit slope) -> FFT, divided by `amp`.  The window
        is constant time (>= 0.6 s) and grows until the last 1/16 of the response is below 1e-5 x amp. */
    IrScaled measureIrScaled (int type, double fc, double res, double morph, double fs, float amp, int maxOrder = 20)
    {
        int order = 14;
        while ((1 << order) < (int) (0.6 * fs))
            ++order;

        const FilterTargets t = mk (type, fc, res, morph);
        std::vector<float> l;
        double tail = 0.0;

        for (;;)
        {
            const int size = 1 << order;
            FilterStage st;
            st.prepare (fs);
            st.snap (t);

            l.assign (ix (size), 0.0f);
            std::vector<float> r (ix (size), 0.0f);
            l[0] = r[0] = amp;

            for (int i = 0; i < size; i += 32)
                st.process (l.data() + i, r.data() + i, 32, t);

            tail = 0.0;
            for (int i = size - size / 16; i < size; ++i)
                tail = std::max (tail, (double) std::abs (l[ix (i)]));
            tail /= (double) amp;

            if (tail < 1.0e-5 || order >= maxOrder)
                break;

            ++order;
        }

        const int size = 1 << order;
        juce::dsp::FFT fft (order);
        std::vector<std::complex<float>> in (ix (size)), out (ix (size));
        for (int i = 0; i < size; ++i)
            in[ix (i)] = std::complex<float> (l[ix (i)], 0.0f);

        fft.perform (in.data(), out.data(), false);

        IrScaled s;
        for (auto& v : out)
            v /= amp;
        s.h = std::move (out);
        s.binHz = fs / (double) size;
        s.order = order;
        s.tail = tail;
        return s;
    }

    struct NlIrError
    {
        double worstDb = 0.0, worstAbs = 0.0, worstHz = 0.0;
    };

    template <class H>
    NlIrError compareNl (const IrScaled& s, double fs, H&& analytic)
    {
        const int lo = (int) std::ceil (20.0 / s.binHz), hi = (int) std::floor (kNlBandFrac * fs / s.binHz);
        const int stride = std::max (1, (hi - lo) / 60000);
        NlIrError e;

        for (int k = lo; k <= hi; k += stride)
        {
            const double f = (double) k * s.binHz;
            const double ma = std::abs (analytic (f));
            const double mm = (double) std::abs (s.h[ix (k)]);

            if (ma <= 0.01)
                e.worstAbs = std::max (e.worstAbs, std::abs (mm - ma));
            else
            {
                const double db = mm > 0.0 ? std::abs (20.0 * std::log10 (mm / ma)) : 200.0;
                if (db > e.worstDb)
                {
                    e.worstDb = db;
                    e.worstHz = f;
                }
            }
        }

        return e;
    }

    void testD5cResponse (bool full)
    {
        std::printf ("\n[D5c] small-signal response: measured impulse response (amplitude 1e-4, through FilterStage incl. the local 2x) vs analytic, 0.5 dB gate\n");

        struct Rm { double res, morph; };
        const std::vector<Rm> combos = full ? std::vector<Rm> { { 0, 0 }, { 0, 1 }, { 0.5, 0.6 }, { 0.9, 0 }, { 0.9, 1 } }
                                            : std::vector<Rm> { { 0, 1 }, { 0.5, 0.6 }, { 0.9, 0 } };
        const std::vector<double> fsList = full ? std::vector<double> { 44100.0, 48000.0, 96000.0, 192000.0 } : std::vector<double> { 48000.0 };
        const std::vector<double> cutoffs { 250.0, 1000.0, 6000.0 };

        int cases = 0, bad = 0;
        std::array<double, 4> worstPerType {};

        for (const double fs : fsList)
            for (int type = 28; type <= 31; ++type)
                for (const double fc : cutoffs)
                    for (const auto& rm : combos)
                    {
                        // Scream: the open-loop band-pass is the small-signal response only without gain / feedback (morph 0)
                        const double morph = type == 31 ? 0.0 : rm.morph;
                        const FilterTargets t = mk (type, fc, rm.res, morph);
                        FilterDesign d;
                        fd::make (type, (double) t.cutoffHz, (double) t.res, (double) t.morph, fs, d);

                        const auto s = measureIrScaled (type, fc, rm.res, morph, fs, 1.0e-4f);
                        const auto e = compareNl (s, fs, [&] (double hz) { return fd::response (d, hz, fs); });
                        ++cases;
                        worstPerType[ix (type - 28)] = std::max (worstPerType[ix (type - 28)], e.worstDb);

                        if (e.worstDb > 0.5 || e.worstAbs > 5.0e-4)
                        {
                            ++bad;
                            DETAIL ("        FAIL type %d (%s) fs %.0f fc %.0f res %.2f morph %.2f: %.3f dB at %.0f Hz, abs %.2e (order %d, tail %.1e)\n",
                                    type, warpr::kFilterTypes[ix (type)].name, fs, fc, rm.res, morph, e.worstDb, e.worstHz, e.worstAbs, s.order, s.tail);
                        }
                    }

        std::printf ("        (%d cases; worst deviation per type 28..31, dB: %.3f %.3f %.3f %.3f)\n", cases,
                     worstPerType[0], worstPerType[1], worstPerType[2], worstPerType[3]);
        check (bad == 0, "Res LP / Res HP / Dist LP / Scream (morph 0): measured small-signal response matches the analytic curve within 0.5 dB (20 Hz .. 0.38 fs)");

        // Scream with gain and feedback: tiny signals see  H = inGain Hbp / (1 - fb z^-1 Hbp)  at the 2x core rate
        {
            constexpr double fs = 48000.0;
            int closedBad = 0;
            double worst = 0.0;

            for (const double morph : { 0.3, 0.8 })
                for (const double res : { 0.2, 0.5 })
                {
                    const FilterTargets t = mk (31, 1000.0, res, morph);
                    FilterDesign d;
                    fd::make (31, (double) t.cutoffHz, (double) t.res, (double) t.morph, fs, d);
                    const double inGain = std::pow (10.0, 2.0 * (double) t.morph), fb = 0.95 * (double) t.morph;

                    const auto s = measureIrScaled (31, 1000.0, res, morph, fs, 1.0e-6f);
                    const auto e = compareNl (s, fs, [&] (double hz)
                    {
                        const std::complex<double> hbp = fd::response (d, hz, fs);
                        const std::complex<double> zInv = std::polar (1.0, -2.0 * kPi * hz / (2.0 * fs));
                        return inGain * hbp / (1.0 - fb * zInv * hbp);
                    });
                    worst = std::max (worst, e.worstDb);

                    if (e.worstDb > 0.5 || e.worstAbs > 5.0e-3)
                    {
                        ++closedBad;
                        DETAIL ("        FAIL scream closed loop res %.1f morph %.1f: %.3f dB at %.0f Hz, abs %.2e\n", res, morph, e.worstDb, e.worstHz, e.worstAbs);
                    }
                }

            std::printf ("        (Scream with input gain + clipped feedback, tiny signal: worst deviation %.3f dB)\n", worst);
            check (closedBad == 0, "Scream morph > 0, tiny signals: H = inGain Hbp / (1 - fb z^-1 Hbp) (the unit-delay feedback and the gain behave as designed)");
        }
    }

    //==========================================================================
    void testD5cSelfOscillation()
    {
        std::printf ("\n[D5c] ladder self-oscillation stays bounded at the oscillation frequency; below the threshold it decays\n");
        constexpr double fs = 48000.0;
        std::vector<float> kick (96000, 0.0f);
        kick[0] = 0.1f;

        for (int type : { 28, 29 })
        {
            const auto y = runStageConst (fs, mk (type, 1000.0, 1.0, 1.0), kick);
            const double r1 = rmsOf (y, 48000, 72000), r2 = rmsOf (y, 72000, 96000);

            int crossings = 0;
            for (size_t i = 72001; i < 96000; ++i)
                if ((y[i - 1] < 0.0f) != (y[i] < 0.0f))
                    ++crossings;
            const double freq = (double) crossings / (2.0 * 0.5);

            std::printf ("        (%s, res 1: RMS %.4f -> %.4f, peak %.3f, oscillation %.1f Hz)\n", warpr::kFilterTypes[ix (type)].name, r1, r2, peakOf (y), freq);
            check (allFinite (y) && r2 > 0.02 && r2 < 4.0 && r2 / r1 > 0.9 && r2 / r1 < 1.1 && peakOf (y) < 8.0,
                   "res 1: the ladder self-oscillates with a steady, bounded amplitude");
            check (std::abs (freq / 1000.0 - 1.0) < 0.08, "res 1: the oscillation sits at the cutoff within 8%");

            const auto d = runStageConst (fs, mk (type, 1000.0, 0.8, 1.0), kick);
            check (rmsOf (d, 72000, 96000) < 1.0e-5, "res 0.8 (below the 0.95 threshold): the ring dies away");
        }
    }

    //==========================================================================
    void testD5cCharacter()
    {
        std::printf ("\n[D5c] character: S&H hold rate, interpolation, ring-mod lines (RM / AM), product LP, carrier continuity, scream bound\n");
        constexpr double fs = 48000.0;

        // ---- S&H: the output changes exactly fc times per second
        {
            const auto sine = makeSine (60000, 100.0, fs, 0.5f);
            int bad = 0;

            for (const double fh : { 777.0, 2000.0, 5000.0 })
            {
                const auto y = runStageConst (fs, mk (32, fh, 0.0, 0.0), sine);
                int changes = 0;
                for (size_t i = 6000; i < 54000; ++i)                      // exactly one second
                    if (y[i] != y[i - 1])
                        ++changes;

                std::printf ("        (S&H hold rate %.0f Hz: %d value changes in 1 s)\n", fh, changes);
                if (std::abs ((double) changes - fh) > std::max (2.0, 0.005 * fh))
                    ++bad;
            }
            check (bad == 0, "S&H: the output steps exactly fc times per second (hold rate = cutoff)");

            const auto held = runStageConst (fs, mk (32, 2000.0, 0.0, 0.0), sine);
            const auto lerped = runStageConst (fs, mk (32, 2000.0, 0.0, 1.0), sine);
            const double stepHold = maxStepIn (held, 6000, 54000), stepLerp = maxStepIn (lerped, 6000, 54000);
            std::printf ("        (S&H 2 kHz: largest step hold %.4f, interpolated %.4f)\n", stepHold, stepLerp);
            check (stepLerp < 0.25 * stepHold && stepHold > 0.05, "S&H morph 1 interpolates between the held values (no steps), morph 0 holds");

            // the pre-LP (res) removes content above the hold Nyquist: a 9 kHz sine held at 4 kHz is much quieter with res 1
            const auto hi = makeSine (24000, 9000.0, fs, 0.5f);
            const double open = rmsOf (runStageConst (fs, mk (32, 4000.0, 0.0, 0.0), hi), 4800, 24000);
            const double closed = rmsOf (runStageConst (fs, mk (32, 4000.0, 1.0, 0.0), hi), 4800, 24000);
            std::printf ("        (9 kHz sine held at 4 kHz: RMS %.4f with the pre-LP open, %.4f with res 1)\n", open, closed);
            check (closed < 0.3 * open, "S&H res = anti-alias pre-LP: res 1 cuts content above the hold Nyquist");
        }

        // ---- Ring mod: carrier line math
        {
            const auto sine = makeSine (65536, 1000.0, fs, 0.5f);
            const auto rm = runStageConst (fs, mk (33, 5000.0, 0.0, 1.0), sine);
            const double d4 = lineAmp (rm, fs, 4000.0), s6 = lineAmp (rm, fs, 6000.0), f1 = lineAmp (rm, fs, 1000.0), c5 = lineAmp (rm, fs, 5000.0);
            std::printf ("        (RM, 1 kHz x 5 kHz carrier: lines 4k %.4f, 6k %.4f; input 1k %.5f, carrier 5k %.5f)\n", d4, s6, f1, c5);
            check (std::abs (d4 / 0.25 - 1.0) < 0.03 && std::abs (s6 / 0.25 - 1.0) < 0.03 && f1 < 1.0e-3 && c5 < 1.0e-3,
                   "Ring Mod morph 1 (RM): sum and difference lines at fc +- f (0.25 each), input and carrier cancelled");

            const auto am = runStageConst (fs, mk (33, 5000.0, 0.0, 0.0), sine);
            const double a1 = lineAmp (am, fs, 1000.0), a4 = lineAmp (am, fs, 4000.0), a6 = lineAmp (am, fs, 6000.0), a5 = lineAmp (am, fs, 5000.0);
            std::printf ("        (AM: input 1k %.4f, sidebands 4k %.4f, 6k %.4f, carrier 5k %.5f)\n", a1, a4, a6, a5);
            check (std::abs (a1 / 0.25 - 1.0) < 0.03 && std::abs (a4 / 0.125 - 1.0) < 0.03 && std::abs (a6 / 0.125 - 1.0) < 0.03 && a5 < 1.0e-3,
                   "Ring Mod morph 0 (AM, unipolar carrier): the input survives at half level, sidebands at a quarter, no carrier line");

            const auto smooth = runStageConst (fs, mk (33, 5000.0, 1.0, 1.0), sine);
            const double m4 = lineAmp (smooth, fs, 4000.0), m6 = lineAmp (smooth, fs, 6000.0);
            std::printf ("        (RM with the product LP closed (res 1): 4k %.4f, 6k %.4f)\n", m4, m6);
            check (m6 < 0.8 * m4 && m4 < 0.2, "Ring Mod res = smoothing LP of the product: res 1 pulls the upper sideband down");
        }

        // ---- Ring mod: phase-continuous carrier while the cutoff sweeps
        {
            const auto sine = makeSine (19200, 1000.0, fs, 0.5f);
            const int sweepChunks = 4800 / 32;
            const double a = std::log (1000.0), b = std::log (4000.0);
            const auto y = runStage (fs, mk (33, 1000.0, 0.0, 1.0), sine, [&] (int ci, FilterTargets& t)
            {
                t.cutoffHz = (float) std::exp (a + (b - a) * std::min (1.0, (double) ci / (double) sweepChunks));
            });
            const double step = maxStepIn (y, 400, y.size());
            std::printf ("        (RM carrier sweep 1 -> 4 kHz: max step %.4f, product slope bound %.4f)\n", step, 0.5 * 2.0 * kPi * 5000.0 / fs);
            check (allFinite (y) && step < 0.45, "Ring Mod: the carrier is phase continuous across a fast cutoff sweep (no steps)");
        }

        // ---- Scream: bounded and DC free under a loud tone at the cutoff
        {
            const auto tone = makeSine (48000, 1000.0, fs, 0.9f);
            bool ok = true;
            double worstMean = 0.0, worstPeak = 0.0;

            for (const double morph : { 0.0, 0.5, 1.0 })
                for (const double res : { 0.0, 0.5, 1.0 })
                {
                    const auto y = runStageConst (fs, mk (31, 1000.0, res, morph, 12.0), tone);
                    double mean = 0.0;
                    for (size_t i = 24000; i < 48000; ++i)
                        mean += (double) y[i];
                    mean /= 24000.0;
                    worstMean = std::max (worstMean, std::abs (mean));
                    worstPeak = std::max (worstPeak, peakOf (y));
                    ok = ok && allFinite (y);
                }
            std::printf ("        (Scream, 0.9 tone at fc, drive 12 dB, all morph x res: worst peak %.3f, worst |mean| %.5f)\n", worstPeak, worstMean);
            check (ok && worstPeak < 8.0 && worstMean < 0.02, "Scream: bounded (< 8) and free of DC offset although the clip is asymmetric (a band-pass has no DC gain)");
        }

        // ---- drive: unity small-signal through the ladders, more harmonics when loud
        {
            const auto quiet = makeSine (14400, 1000.0, fs, 0.0001f);
            bool unity = true;
            double worst = 0.0;

            for (int type : { 28, 29, 30 })
            {
                const double r0 = rmsOf (runStageConst (fs, mk (type, 4000.0, 0.4, 1.0, 0.0), quiet), 9600, 14400);
                const double r24 = rmsOf (runStageConst (fs, mk (type, 4000.0, 0.4, 1.0, 24.0), quiet), 9600, 14400);
                worst = std::max (worst, std::abs (r24 / r0 - 1.0));
                unity = unity && std::abs (r24 / r0 - 1.0) < 0.02;
            }
            std::printf ("        (ladders, 1 kHz at -80 dBFS: drive 24 dB vs 0 dB level change %.4f)\n", worst);
            check (unity, "ladders: drive 0 -> 24 dB leaves the small-signal level unchanged (normalised saturators)");

            const auto loud = makeSine (48000, 200.0, fs, 0.8f);
            const auto y0 = runStageConst (fs, mk (28, 4000.0, 0.2, 1.0, 0.0), loud);
            const auto y24 = runStageConst (fs, mk (28, 4000.0, 0.2, 1.0, 24.0), loud);
            // relative to the fundamental: the shared input saturator limits the driven signal to 1/P, so absolute levels fall
            const double r0 = lineAmp (y0, fs, 600.0) / std::max (lineAmp (y0, fs, 200.0), 1.0e-9);
            const double r24 = lineAmp (y24, fs, 600.0) / std::max (lineAmp (y24, fs, 200.0), 1.0e-9);
            std::printf ("        (Res LP, 0.8 sine at 200 Hz: 3rd / 1st harmonic %.4f at 0 dB drive, %.4f at 24 dB)\n", r0, r24);
            check (r24 > 1.5 * r0 && allFinite (y24), "ladder: drive pushes the loop into saturation (relative 3rd harmonic clearly up)");
        }
    }

    //==========================================================================
    void testD5cAlias()
    {
        std::printf ("\n[D5c] aliasing: local 2x on vs off (core-level), sine at the T2 operating points 1.1 / 3.3 / 8.7 kHz, 0.8 amplitude\n");
        constexpr double fs = 48000.0;
        constexpr int order = 15;
        const int total = (1 << order) + 8192;
        auto bin = [&] (double hz) { return fs * std::round (hz / fs * (double) (1 << order)) / (double) (1 << order); };

        struct Case { int type; double fc, res, morph, drive; const char* label; };
        const Case cases[] = {
            { 28, 16000.0, 0.3, 1.0, 18.0, "Res LP  (fc 16k, res 0.3, drive 18 dB)" },
            { 30, 16000.0, 0.2, 0.8, 12.0, "Dist LP (fc 16k, res 0.2, morph 0.8, drive 12 dB)" },
            { 31, 0.0,     0.3, 0.6, 0.0,  "Scream  (fc = f0, res 0.3, morph 0.6)" }
        };

        int gateBad = 0;

        for (const auto& c : cases)
        {
            for (const double f0Nominal : { 1100.0, 3300.0, 8700.0 })
            {
                const double f0 = bin (f0Nominal);
                const auto sine = makeSine (total, f0, fs, 0.8f);
                const double fc = c.fc > 0.0 ? c.fc : f0;

                FilterDesign on, off;
                fd::make (c.type, fc, c.res, c.morph, fs, on, c.drive, true);
                fd::make (c.type, fc, c.res, c.morph, fs, off, c.drive, false);
                const auto yOn = runCoreDirect (on, sine), yOff = runCoreDirect (off, sine);

                const double aOn = warpr::tests::aliasDb (yOn.data(), (int) yOn.size(), fs, f0, order);
                const double aOff = warpr::tests::aliasDb (yOff.data(), (int) yOff.size(), fs, f0, order);
                const double gain = aOff - aOn;
                std::printf ("        %s  f0 %6.0f Hz: alias %7.1f dB (2x) vs %7.1f dB (base rate)  improvement %5.1f dB\n", c.label, f0, aOn, aOff, gain);

                if (! (allFinite (yOn) && allFinite (yOff)))
                    ++gateBad;

                // gates for the ladders; Scream is printed only (its band-pass skirt shapes the folded energy unpredictably)
                if (c.type != 31)
                {
                    const double need = f0Nominal > 3000.0 ? 3.0 : -0.5;
                    if (gain < need)
                    {
                        ++gateBad;
                        DETAIL ("        GATE: %s f0 %.0f: improvement %.1f dB < %.1f dB\n", c.label, f0, gain, need);
                    }
                }
            }
        }
        check (gateBad == 0, "ladders: the local 2x lowers the in-band alias level by >= 3 dB at 3.3 / 8.7 kHz and never worsens it at 1.1 kHz");

        // ---- Ring mod: the sum line of 15 kHz x 14 kHz is at 29 kHz, i.e. an alias at 19 kHz without oversampling, removed with it
        {
            const auto sine = makeSine (65536, 15000.0, fs, 0.5f);
            FilterDesign on, off;
            fd::make (33, 14000.0, 0.0, 1.0, fs, on, 0.0, true);
            fd::make (33, 14000.0, 0.0, 1.0, fs, off, 0.0, false);
            const auto yOn = runCoreDirect (on, sine), yOff = runCoreDirect (off, sine);
            const double wanted = lineAmp (yOn, fs, 1000.0), aliasOn = lineAmp (yOn, fs, 19000.0), aliasOff = lineAmp (yOff, fs, 19000.0);
            std::printf ("        (Ring Mod 15 kHz x 14 kHz carrier: difference line 1 kHz %.4f; alias line at 19 kHz %.2e with 2x vs %.4f without)\n", wanted, aliasOn, aliasOff);
            check (aliasOff > 0.2 && aliasOn < 1.0e-3 && std::abs (wanted / 0.25 - 1.0) < 0.03,
                   "Ring Mod: the folded sum line (0.25 without the local 2x) is removed by it (< 1e-3) while the difference line is untouched");
        }

        // ---- S&H: aliasing is the effect (aliasByDesign): printed, never gated
        {
            const double f0 = bin (3300.0);
            const auto sine = makeSine (total, f0, fs, 0.8f);
            const auto y = runStageConst (fs, mk (32, 5000.0, 0.0, 0.0), sine);
            std::printf ("        (S&H hold 5 kHz on a %.0f Hz sine: alias level %.1f dB - intended, flagged aliasByDesign)\n",
                         f0, warpr::tests::aliasDb (y.data(), (int) y.size(), fs, f0, order));
            check (warpr::kFilterTypes[32].aliasByDesign && ! warpr::kFilterTypes[32].localOversampling, "S&H runs at the base rate and is flagged aliasByDesign");
        }
    }

    //==========================================================================
    void testD5cStability (bool full)
    {
        std::printf ("\n[D5c] stability torture (cutoff x res x drive x morph x fs, types 28-33; noise and a hot square)\n");

        const std::vector<double> fsList = full ? std::vector<double> { 44100.0, 48000.0, 96000.0, 192000.0 } : std::vector<double> { 48000.0 };
        const int len = full ? 24000 : 6000;
        const auto noise = makeNoise (len, 0.25f, 4244);
        const auto hot = makeSquare (len, 220.0, 48000.0, 0.95f);

        int runs = 0, nonFinite = 0, tooLoud = 0;
        double worstPeak = 0.0;
        int worstType = -1;

        for (const double fs : fsList)
            for (int type = kD5cFirst; type < kD5cEnd; ++type)
                for (const double fc : { 20.0, 1000.0, 20000.0 })
                    for (const double res : { 0.0, 0.5, 1.0 })
                        for (const double drv : { 0.0, 24.0 })
                            for (const double morph : { 0.0, 0.5, 1.0 })
                                for (const auto* sig : { &noise, &hot })
                                {
                                    const auto y = runStageConst (fs, mk (type, fc, res, morph, drv), *sig);
                                    ++runs;

                                    if (! allFinite (y))
                                    {
                                        ++nonFinite;
                                        DETAIL ("        non-finite: type %d fs %.0f fc %.0f res %.1f drive %.0f morph %.1f\n", type, fs, fc, res, drv, morph);
                                        continue;
                                    }

                                    const double pk = peakOf (y);
                                    if (pk > worstPeak)
                                    {
                                        worstPeak = pk;
                                        worstType = type;
                                    }

                                    if (pk >= 8.0)
                                    {
                                        ++tooLoud;
                                        DETAIL ("        |y| = %.2f: type %d fs %.0f fc %.0f res %.1f drive %.0f morph %.1f\n", pk, type, fs, fc, res, drv, morph);
                                    }
                                }

        std::printf ("        (%d runs, noise at -12 dBFS and a 0.95 square, worst peak %.3f (type %d))\n", runs, worstPeak, worstType);
        check (nonFinite == 0, "no NaN / inf anywhere in the sweep (self-oscillating ladders, 40 dB scream gain, 192 kHz, 20 Hz and 20 kHz cutoffs)");
        check (tooLoud == 0, "|y| < 8 everywhere (the saturators and the small-gain feedback of Scream bound every type)");
    }

    //==========================================================================
    void testD5cBypass()
    {
        std::printf ("\n[D5c] bypass nulls, open-LP rule, L/R symmetry (types 28-33)\n");
        constexpr double fs = 48000.0;
        const auto noise = makeNoise (9600, 0.5f, 7);
        const auto smooth = makeMix (9600, fs);

        int offBad = 0, mixBad = 0, openBad = 0, rampBad = 0, asym = 0;

        for (int type = kD5cFirst; type < kD5cEnd; ++type)
        {
            {
                FilterStage st;
                st.prepare (fs);
                const FilterTargets wild = mk (type, 120.0, 1.0, 1.0, 24.0, 1.0, false);          // enable OFF
                st.snap (wild);
                std::vector<float> l = noise, r = noise;
                for (size_t i = 0; i < l.size(); i += 32)
                    st.process (l.data() + i, r.data() + i, 32, wild);
                if (! (bitEqual (l, noise) && bitEqual (r, noise) && st.isParked()))
                    ++offBad;
            }
            {
                FilterStage st;
                st.prepare (fs);
                const FilterTargets zero = mk (type, 300.0, 1.0, 0.5, 12.0, 0.0, true);           // mix 0
                st.snap (zero);
                std::vector<float> l = noise, r = noise;
                for (size_t i = 0; i < l.size(); i += 32)
                    st.process (l.data() + i, r.data() + i, 32, zero);
                if (! (bitEqual (l, noise) && bitEqual (r, noise)))
                    ++mixBad;
            }
            {
                FilterStage st;
                st.prepare (fs);
                const FilterTargets open = mk (type, 20000.0, 0.0, 0.0, 0.0, 1.0, true);
                st.snap (open);
                std::vector<float> l = noise, r = noise;
                for (size_t i = 0; i < l.size(); i += 32)
                    st.process (l.data() + i, r.data() + i, 32, open);
                if (st.isParked())
                    ++openBad;
            }
            {
                const FilterTargets on = mk (type, 800.0, 0.5, 0.4);
                const int offAt = 160;
                const auto y = runStage (fs, on, smooth, [&] (int ci, FilterTargets& t) { if (ci >= offAt) t.enable = false; });
                bool tailExact = true;
                for (size_t i = (size_t) offAt * 32 + 256; i < smooth.size(); ++i)
                    tailExact = tailExact && y[i] == smooth[i];
                if (! tailExact)
                    ++rampBad;
            }
            {
                std::vector<float> r;
                const int other = kD5cFirst + (type - kD5cFirst + 1) % (kD5cEnd - kD5cFirst);
                const auto l = runStage (fs, mk (type, 900.0, 0.4, 0.3, 6.0), noise,
                                         [other] (int ci, FilterTargets& t) { if (ci == 100) t.type = other; }, &r);
                if (! bitEqual (l, r))
                    ++asym;
            }
        }

        check (offBad == 0, "filterEnable off: bit-identical output and a parked stage for every type 28-33");
        check (mixBad == 0, "filterMix 0: bit-identical output for every type 28-33");
        check (openBad == 0, "the open low-pass rule does not apply: a ladder / scream / S&H / ring mod at 20 kHz, res 0 stays live");
        check (rampBad == 0, "disabling mid-note ramps over 5 ms and is bit-exact afterwards (ladder, hold and half-band memory are parked)");
        check (asym == 0, "identical L / R input gives bit-identical L / R output (incl. drive and a mid-run switch to the next nonlinear type)");

        // re-enabling after a parked period starts clean: equals a fresh stage once the ramp is over
        {
            const FilterTargets on = mk (30, 700.0, 0.5, 0.5, 6.0);          // res 0.5: the ring (~1.5 ms/e-fold) is gone well before sample 2000
            const auto ref = runStageConst (fs, on, noise);
            std::vector<float> l = noise, r = noise;
            FilterStage st;
            st.prepare (fs);
            FilterTargets t = on;
            st.snap (t);
            for (size_t i = 0; i < 3200; i += 32)
                st.process (l.data() + i, r.data() + i, 32, t);
            t.enable = false;
            for (size_t i = 3200; i < 6400; i += 32)
                st.process (l.data() + i, r.data() + i, 32, t);
            const bool parked = st.isParked();
            t.enable = true;
            std::vector<float> a (noise.begin(), noise.begin() + 3200), b = a;
            for (size_t i = 0; i < a.size(); i += 32)
                st.process (a.data() + i, b.data() + i, 32, t);
            double worst = 0.0;
            for (size_t i = 2000; i < a.size(); ++i)
                worst = std::max (worst, (double) std::abs (a[i] - ref[i]));
            std::printf ("        (re-enabled Dist LP vs a fresh one after 2000 samples: %.2e)\n", worst);
            check (parked && worst < 2.0e-3, "a parked nonlinear core forgets its history: after re-enable it matches a fresh run");
        }
    }

    //==========================================================================
    void testD5cSwitching()
    {
        std::printf ("\n[D5c] type-switch crossfade into / out of / among types 28-33 (DiscreteSwitchFade, 10 ms + 64 samples priming)\n");
        constexpr double fs = 48000.0;
        const auto sig = makeMix (12000, fs);

        int pairs = 0, stepBad = 0, tailBad = 0;
        double worstRatio = 0.0;

        for (int a = 0; a < kD5cEnd; ++a)
            for (int b = 0; b < kD5cEnd; ++b)
            {
                if (a == b || (a < kD5cFirst && b < kD5cFirst))
                    continue;                                       // pairs among the linear types are covered by D5a / D5b

                const auto r = switchPair (a, b, fs, sig);
                ++pairs;

                // S&H strobes fall at different instants in the reference and in the switched render: a little slack
                const double slack = (a == 32 || b == 32) ? 1.5 : 1.0;
                const bool stepOk = r.step <= r.bound * slack;
                worstRatio = std::max (worstRatio, r.step / std::max (r.bound * slack, 1.0e-9));

                if (! stepOk)
                {
                    ++stepBad;
                    DETAIL ("        step: %d -> %d: max step %.5f > bound %.5f\n", a, b, r.step, r.bound * slack);
                }

                // S&H and Ring Mod keep time-dependent state (hold clock, carrier phase): the switched core starts at phase 0,
                // the reference run is already mid-cycle, so only the level (RMS) is compared for those targets
                if (b < 32 && ! r.tailOk)
                {
                    ++tailBad;
                    DETAIL ("        tail: %d -> %d: |switched - new| = %.5f\n", a, b, r.tail);
                }
            }

        std::printf ("        (%d transitions; worst step / bound = %.3f)\n", pairs, worstRatio);
        check (stepBad == 0, "every transition: max step <= natural step + 2 x difference / 480 samples (S&H pairs 1.5x) - click-free");
        check (tailBad == 0, "every transition into Res LP / Res HP / Dist LP / Scream and the linear types ends on the new type (< 2e-3)");

        int levelBad = 0;
        double worstLevel = 0.0;
        for (int src : { 3, 20, 28, 31 })
            for (int dst : { 32, 33 })
            {
                const FilterTargets ta = mk (src, 1200.0, 0.3, 0.2), tb = mk (dst, 1200.0, 0.3, 0.2);
                const auto yb = runStageConst (fs, tb, sig);
                const auto ysw = runStage (fs, ta, sig, [dst] (int ci, FilterTargets& t) { if (ci * 32 >= 187 * 32) t.type = dst; });
                const double rb = rmsOf (yb, 8000, 12000), rs = rmsOf (ysw, 8000, 12000);
                const double rel = std::abs (rs / std::max (rb, 1.0e-9) - 1.0);
                worstLevel = std::max (worstLevel, rel);
                if (rel > 0.15 || ! allFinite (ysw))
                {
                    ++levelBad;
                    DETAIL ("        level: %d -> %d: RMS %.4f vs %.4f\n", src, dst, rs, rb);
                }
            }
        std::printf ("        (switches into S&H / Ring Mod: worst RMS difference %.3f)\n", worstLevel);
        check (levelBad == 0, "switches into S&H / Ring Mod settle to the same level as a fresh run (RMS within 15%)");
    }

    //==========================================================================
    void testD5cSweepsAndWalk (bool full)
    {
        std::printf ("\n[D5c] ladder cutoff sweep and random modulation walk over types 28-33\n");
        constexpr double fs = 48000.0;

        {
            const auto sine = makeSine (19200, 1000.0, fs, 0.25f);
            const int sweepChunks = 9600 / 32;
            const double a = std::log (400.0), b = std::log (4000.0);
            double worst = 0.0;
            bool finite = true;

            for (int type : { 28, 29, 30 })
            {
                const auto up = runStage (fs, mk (type, 400.0, 0.5, 1.0), sine, [&] (int ci, FilterTargets& t)
                {
                    t.cutoffHz = (float) std::exp (a + (b - a) * std::min (1.0, (double) ci / (double) sweepChunks));
                });
                const auto down = runStage (fs, mk (type, 4000.0, 0.5, 1.0), sine, [&] (int ci, FilterTargets& t)
                {
                    t.cutoffHz = (float) std::exp (b + (a - b) * std::min (1.0, (double) ci / (double) sweepChunks));
                });
                worst = std::max ({ worst, maxStepIn (up, 400, up.size()), maxStepIn (down, 400, down.size()) });
                finite = finite && allFinite (up) && allFinite (down);
            }
            std::printf ("        (ladder sweeps 400 <-> 4000 Hz through a 1 kHz tone: largest sample step %.4f)\n", worst);
            check (finite && worst < 0.2, "swept ladders: finite and no sample step above 0.2 (the tone itself steps 0.033 at most)");
        }

        const int chunks = full ? 15000 : 3000;
        juce::Random rng (321);
        FilterStage st;
        st.prepare (fs);
        FilterTargets t = mk (28, 1000.0);
        st.snap (t);

        std::vector<float> l (32), r (32);
        bool finite = true;
        double peak = 0.0;

        for (int c = 0; c < chunks; ++c)
        {
            if (rng.nextInt (25) == 0)
                t.type = kD5cFirst + rng.nextInt (kD5cEnd - kD5cFirst);

            t.cutoffHz = (float) (20.0 * std::pow (1000.0, (double) rng.nextFloat()));
            t.res = rng.nextFloat();
            t.morph = rng.nextFloat();
            t.driveDb = rng.nextInt (3) == 0 ? 24.0f * rng.nextFloat() : 0.0f;
            t.mix = rng.nextFloat() > 0.2f ? 1.0f : rng.nextFloat();
            t.enable = rng.nextInt (200) != 0;

            for (size_t i = 0; i < 32; ++i)
                l[i] = r[i] = 0.3f * (rng.nextFloat() * 2.0f - 1.0f);

            st.process (l.data(), r.data(), 32, t);

            for (size_t i = 0; i < 32; ++i)
            {
                finite = finite && std::isfinite (l[i]) && std::isfinite (r[i]);
                peak = std::max ({ peak, (double) std::abs (l[i]), (double) std::abs (r[i]) });
            }
        }

        std::printf ("        (%d chunks of fully random cutoff / res / morph / drive / mix / type 28-33, peak %.3f)\n", chunks, peak);
        check (finite, "random modulation walk over types 28-33: no NaN / inf");
        check (peak < 64.0, "random modulation walk over types 28-33: output below the voice guard (|y| < 64)");
    }

    //==========================================================================
    void testD5cBackend()
    {
        std::printf ("\n[D5c] backend: computeFilterResponse for the nonlinear types\n");
        constexpr int n = 256;

        PluginProcessor p;
        auto& be = p.getBackendImpl();
        be.setSampleRate (48000.0);

        std::array<float, n> db {};
        auto freq = [] (int i) { return 20.0 * std::pow (1000.0, (double) i / (double) (n - 1)); };

        bool allOk = true;
        int notEqual = 0;

        for (int type = kD5cFirst; type < kD5cEnd; ++type)
        {
            setParam (p, "filterType", (float) type);
            setParam (p, "filterCutoff", 2000.0f);
            setParam (p, "filterResonance", 0.6f);
            setParam (p, "filterMorph", 0.4f);
            setParam (p, "filterDrive", 0.0f);
            be.computeFilterResponse (db.data(), n);

            bool inRange = true;
            double worst = 0.0;
            for (int i = 0; i < n; ++i)
            {
                inRange = inRange && std::isfinite (db[ix (i)]) && db[ix (i)] >= -48.0f && db[ix (i)] <= 24.0f;
                worst = std::max (worst, std::abs ((double) db[ix (i)] - FilterResponse::magnitudeDbAt (type, 2000.0, 0.6, 0.0, 0.4, 48000.0, freq (i))));
            }
            allOk = allOk && inRange;
            if (worst > 0.01)
                ++notEqual;

            const float mx = *std::max_element (db.begin(), db.end()), mn = *std::min_element (db.begin(), db.end());
            std::printf ("        %d %s: %.1f .. %.1f dB\n", type, warpr::kFilterTypes[ix (type)].name, (double) mn, (double) mx);

            if (type == 33)
                check (mx == 0.0f && mn == 0.0f, "Ring Mod: flat 0 dB line");
            else if (type == 32)
                check (db[0] > -0.5f && db[n - 1] < -20.0f, "S&H: the drawn pre-LP is open at 20 Hz and falls away towards 20 kHz");
            else
                check (mx - mn > 3.0f, "ladders / Scream: a real curve (not flat)");
        }
        check (allOk && notEqual == 0, "types 28-33: the backend curve equals FilterResponse point for point, finite, inside [-48, +24] dB");

        // drive does not move the curve
        {
            setParam (p, "filterType", 28.0f);
            be.computeFilterResponse (db.data(), n);
            const auto before = db;
            setParam (p, "filterDrive", 18.0f);
            be.computeFilterResponse (db.data(), n);
            check (before == db, "the drawn curve ignores the drive (small-signal)");
        }

        // timing
        {
            setParam (p, "filterType", 28.0f);
            setParam (p, "filterResonance", 0.9f);
            double best = 1.0e9;
            for (int rep = 0; rep < 50; ++rep)
            {
                const auto t0 = std::chrono::steady_clock::now();
                be.computeFilterResponse (db.data(), n);
                const auto t1 = std::chrono::steady_clock::now();
                best = std::min (best, std::chrono::duration<double, std::milli> (t1 - t0).count());
            }
            std::printf ("        (computeFilterResponse, 256 points, Res LP: best of 50 = %.3f ms)\n", best);
            check (best < 1.0, "computeFilterResponse returns 256 ladder points in < 1 ms");
        }
    }

    //==========================================================================
    void testD5cProcessor()
    {
        std::printf ("\n[D5c] allocations (half-band buffers!), determinism, processor-level nulls\n");
        constexpr double fs = 48000.0;

        // class level
        {
            FilterStage st;
            st.prepare (fs);
            FilterTargets t = mk (28, 400.0, 0.9, 0.3, 6.0);
            st.snap (t);
            std::vector<float> l (32, 0.1f), r (32, 0.1f);
            juce::Random rng (11);

            std::size_t allocations = 0;
            {
                warpr::tests::ScopedAllocCount counter;
                for (int round = 0; round < 600; ++round)
                {
                    if (round % 5 == 0)
                        t.type = kD5cFirst + (round / 5) % 6;
                    t.cutoffHz = 50.0f + 8000.0f * rng.nextFloat();
                    t.res = rng.nextFloat();
                    t.morph = rng.nextFloat();
                    t.driveDb = 24.0f * rng.nextFloat();
                    for (size_t i = 0; i < 32; ++i)
                        l[i] = r[i] = 0.4f * (rng.nextFloat() * 2.0f - 1.0f);
                    st.process (l.data(), r.data(), 32, t);
                }
                allocations = counter.count();
            }
            check (allocations == 0, "FilterStage: zero allocations while running and switching all of types 28-33 (600 chunks, local 2x active)");
        }

        // processor level
        {
            PluginProcessor p;
            prepSustained (p);
            setParam (p, "unisonVoices", 4.0f);
            setParam (p, "filterType", 28.0f);
            setParam (p, "filterCutoff", 700.0f);
            setParam (p, "filterResonance", 0.8f);
            setParam (p, "filterDrive", 6.0f);
            p.setPlayConfigDetails (0, 2, fs, 256);
            p.prepareToPlay (fs, 256);

            juce::AudioBuffer<float> buf (2, 256);
            juce::MidiBuffer on;
            on.addEvent (juce::MidiMessage::noteOn (1, 52, 0.9f), 0);
            on.addEvent (juce::MidiMessage::noteOn (1, 59, 0.9f), 0);
            p.processBlock (buf, on);
            juce::MidiBuffer none;

            std::size_t allocations = 0;
            bool sounding = false;
            for (int round = 0; round < 18; ++round)
            {
                setParam (p, "filterType", (float) (kD5cFirst + round % 6));        // allocates (APVTS): uncounted
                setParam (p, "filterCutoff", 200.0f + 900.0f * (float) (round % 4));
                setParam (p, "filterMorph", 0.15f * (float) (round % 7));

                warpr::tests::ScopedAllocCount counter;
                for (int i = 0; i < 6; ++i)
                {
                    p.processBlock (buf, none);
                    sounding = sounding || p.getSynth().getNumActiveVoices() > 0;
                }
                allocations += counter.count();
            }

            check (sounding, "the counted window contained sounding voices");
            check (allocations == 0, "processBlock performs zero allocations with active nonlinear filters (2 voices x unison 4, 18 type changes)");
            p.releaseResources();
        }

        // determinism
        {
            auto run = [&]
            {
                PluginProcessor p;
                prepSustained (p);
                setParam (p, "unisonVoices", 3.0f);
                setParam (p, "filterType", 28.0f);
                setParam (p, "filterCutoff", 900.0f);
                setParam (p, "filterResonance", 0.7f);
                setParam (p, "filterMorph", 0.5f);
                return renderBlocks (p, fs, 90, 256, 50, [&p] (int b)
                {
                    if (b == 12) setParam (p, "filterType", 31.0f);
                    if (b == 24) setParam (p, "filterType", 33.0f);
                    if (b == 36) setParam (p, "filterType", 32.0f);
                    if (b == 48) setParam (p, "filterType", 30.0f);
                    if (b == 60) setParam (p, "filterCutoff", 2500.0f);
                    if (b == 72) setParam (p, "filterType", 29.0f);
                });
            };

            const auto a = run(), b = run();
            check (bitEqual (a.l, b.l) && bitEqual (a.r, b.r) && peakOf (a.l) > 0.02, "two identical sessions with mid-note changes among the D5c types are bit-equal");
        }

        // processor-level nulls
        {
            auto make = [&] (const std::function<void (PluginProcessor&)>& cfg)
            {
                PluginProcessor p;
                prepSustained (p);
                cfg (p);
                return renderBlocks (p, fs, 40, 256, 48, nullptr);
            };

            const auto defaultR = make ([] (PluginProcessor&) {});
            const auto mixZero = make ([] (PluginProcessor& p)
            {
                setParam (p, "filterType", 31.0f); setParam (p, "filterCutoff", 200.0f); setParam (p, "filterResonance", 1.0f);
                setParam (p, "filterMorph", 1.0f); setParam (p, "filterMix", 0.0f);
            });
            const auto offRing = make ([] (PluginProcessor& p)
            {
                setParam (p, "filterType", 33.0f); setParam (p, "filterEnable", 0.0f); setParam (p, "filterDrive", 24.0f);
            });
            const auto active = make ([] (PluginProcessor& p)
            {
                setParam (p, "filterType", 28.0f); setParam (p, "filterCutoff", 400.0f); setParam (p, "filterResonance", 0.7f);
            });

            check (bitEqual (defaultR.l, mixZero.l) && bitEqual (defaultR.r, mixZero.r), "processor: Scream with filterMix 0 is bit-identical to the default patch");
            check (bitEqual (defaultR.l, offRing.l) && bitEqual (defaultR.r, offRing.r), "processor: Ring Mod with filterEnable off is bit-identical to the default patch");
            const double nl = nullDb (defaultR.l, active.l);
            std::printf ("        (Res LP @ 400 Hz, res 0.7 vs bypass: null %.1f dB)\n", nl);
            check (nl > -20.0 && allFinite (active.l) && allFinite (active.r), "processor: an active Res LP really changes the sound");
        }
    }
}

JUCE_END_IGNORE_WARNINGS_GCC_LIKE

//==============================================================================
int runPhaseD5Tests (bool full)
{
    failures = 0;
    printedDetails = 0;
    std::printf ("\n=== Phase D5a: standard filters (types 0-19), response display, type switching (%s) ===\n", full ? "full" : "quick");

    testTable();
    testFormulas();
    testResponse (full);
    testStability (full);
    testBypass();
    testSwitching (full);
    testSweeps (full);
    testKeyTrack();
    testBackend();
    testProcessor();

    std::printf ("\n=== Phase D5b: special linear filters (types 20-27: combs, all-passes, formants, phaser) (%s) ===\n", full ? "full" : "quick");

    testD5bTable();
    testD5bCombFormulas();
    testD5bAllPassFormulas();
    testD5bFormantFormulas();
    testD5bPhaserFormulas();
    testD5bResponse (full);
    testD5bCombMeasured();
    testD5bStability (full);
    testD5bBypass();
    testD5bSwitching();
    testD5bSweepsAndWalk (full);
    testD5bKeyTrack();
    testD5bBackend();
    testD5bProcessor();

    std::printf ("\n=== Phase D5c: nonlinear filters (types 28-33: Res LP / Res HP / Dist LP / Scream / S&H / Ring Mod, local 2x) (%s) ===\n", full ? "full" : "quick");

    testD5cTable();
    testD5cFormulas();
    testD5cResponse (full);
    testD5cSelfOscillation();
    testD5cCharacter();
    testD5cAlias();
    testD5cStability (full);
    testD5cBypass();
    testD5cSwitching();
    testD5cSweepsAndWalk (full);
    testD5cBackend();
    testD5cProcessor();

    return failures;
}
