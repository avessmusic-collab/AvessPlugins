/*
    WARPR - phase D5 test block (filters).  This file holds D5a: the standard filter family, types 0-19
    (TPT one-pole / SVF cascades, band-pass, notch, peak, shelves), the per-voice FilterStage and the response display.
    D5b / D5c add their own sections here.

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

            const auto expectedBuild = i < 20 ? warpr::FilterBuild::D5a : (i < 28 ? warpr::FilterBuild::D5b : warpr::FilterBuild::D5c);
            if (info.build != expectedBuild)
                ++buildWrong;
            if (info.linear != (i < 28))
                ++linearWrong;
        }

        check (nameMismatch == 0, "all 34 table names equal choices::filterType (single source)");
        check (implWrong == 0 && warpr::kNumImplementedFilterTypes == 20, "types 0-19 are implemented, 20-33 are not");
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

        // unimplemented types: identity design + flat response
        int identityBad = 0;
        for (int t = 20; t < warpr::kNumFilterTypes; ++t)
        {
            FilterDesign d;
            fd::make (t, 1000.0, 0.7, 0.3, fs, d);
            if (d.numSections != 0 || d.gain != 1.0 || std::abs (fd::response (d, 1234.0, fs) - std::complex<double> (1.0, 0.0)) > 1.0e-15
                || FilterResponse::magnitudeDbAt (t, 1000.0, 0.7, 0.0, 0.3, fs, 1234.0) != 0.0)
                ++identityBad;
        }
        check (identityBad == 0, "types 20-33: identity design and flat 0 dB response (until D5b / D5c)");
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
            for (int type = 0; type < warpr::kNumImplementedFilterTypes; ++type)
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
            for (int type = 0; type < warpr::kNumImplementedFilterTypes; ++type)
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
            for (int type = 0; type < warpr::kNumImplementedFilterTypes; ++type)
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

        for (int a = 0; a < warpr::kNumImplementedFilterTypes; ++a)
            for (int b = 0; b < warpr::kNumImplementedFilterTypes; ++b)
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

        // a switch into / out of an unimplemented (identity) type is just as smooth
        {
            const auto r1 = switchPair (3, 25, fs, saw);
            const auto r2 = switchPair (25, 3, fs, saw);
            check (r1.stepOk && r1.tailOk && r2.stepOk && r2.tailOk, "switching to / from a not-yet-implemented (pass-through) type crossfades too");
        }

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

        // unimplemented type: flat
        setParam (p, "filterType", 25.0f);
        be.computeFilterResponse (db.data(), n);
        flat = true;
        for (float v : db)
            flat = flat && v == 0.0f;
        check (flat, "a not-yet-implemented type (Vowel) draws a flat line");

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

    return failures;
}
