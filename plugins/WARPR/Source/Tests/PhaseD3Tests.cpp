/*
    WARPR - phase D3 test block (folds, dynamics, distortions, rectifiers, quantisers: warp modes 15-43,
    plus the oversampling / ADAA alias validation).

    Called from RunTests.cpp main():   failures += runPhaseD3Tests (full);

    Contents
      1. mode table rows 15-43 (implemented, family, flags, minOS)
      2. new FastMath primitives and the fold / clip building blocks
      3. PROPERTY TEST  F1' == f  for every closed-form (f, F1) pair (central differences with an
         automatic non-smooth-point exclusion + an interval-quadrature continuity check + breakpoint continuity)
      4. per-mode behaviour (known values, monotone staircases, hold period, Dynamic Fold == Linear Fold at A = 0)
      5. T1 universal cube for the 29 modes through the shared class-level renderer (Tests/WarpTestKit.h)
      6. stereo / mono behaviour, identity guarantees, determinism, zero allocations, mode switching
      7. T2 alias gates A1 / A2 (+ informational alias table), Bitcrush / SR alias-by-design documentation
      8. processor level: DC after the blockers, bounds, zero allocations

    ALIAS GATES (plan T2).  The meter is Tests/Scanners.h aliasDb(): energy in non-harmonic bins relative to the
    harmonic energy, restricted to the band below the BASE Nyquist (what a perfect decimator keeps).  Operating point:
    a 3.0 kHz sine (an exact FFT bin centre, deliberately NOT a divisor of the sample rate so that aliases do not
    land on harmonics), "drive 16" = warpAmount 1, warpParamA 14/15 (D = 1 + a (1 + 15 A) = 16), input amplitude
    0.25 (a -12 dBFS sample: u-peak 4, i.e. strongly clipped / two reflections).  A full-scale (amplitude 1.0) line is
    printed for the record.
      A1  ADAA vs naive at 1x:          improvement >= 12 dB   (strict)
      A2  2x + ADAA vs naive 8x:        alias(2x ADAA) <= alias(naive 8x) + 6 dB, else the smallest OS in {4, 8}
          that meets it is the mode's required minOS (risk R1(b)); the test fails if the table's minOS hint is lower.
    Measured numbers are always printed.

    Quick tier is a few seconds; `--full` widens the parameter cube.
*/

#include "PluginProcessor.h"
#include "DSP/Adaa.h"
#include "DSP/AmpWarp.h"
#include "DSP/FastMath.h"
#include "DSP/ShapeWarp.h"
#include "DSP/UnisonOsc.h"
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
#include <functional>
#include <set>
#include <string>
#include <utility>
#include <vector>

// Bit-exact float equality is the point of several assertions in this file (a = 0 identity, determinism,
// L == R), so the warning is suppressed here only.
JUCE_BEGIN_IGNORE_WARNINGS_GCC_LIKE ("-Wfloat-equal")

int runPhaseD3Tests (bool full);

namespace
{
    using namespace warpr::tests::kit;
    namespace sw = warpr::shapewarp;

    int failures = 0;
    int printedDetails = 0;

    void check (bool cond, const char* name)
    {
        std::printf ("  %s  %s\n", cond ? "PASS" : "FAIL", name);
        if (! cond) ++failures;
    }

    /** Prints a failure detail line (bounded, so a systematic failure does not flood the log). */
   #define DETAIL(...) do { if (printedDetails++ < 60) std::printf (__VA_ARGS__); } while (false)

    constexpr int firstD3 = 15, lastD3 = 43;

    const char* modeName (int mode) { return warpr::kWarpModes[ix (mode)].name; }

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

    //==========================================================================
    // 1. mode table
    //==========================================================================
    void testModeTable()
    {
        std::printf ("\n[D3] WarpModeTable rows 15-43: implemented, family, flags, minOS\n");

        int notImplemented = 0, nameBad = 0, familyBad = 0, flagBad = 0, osBad = 0;

        for (int i = firstD3; i <= lastD3; ++i)
        {
            const auto& m = warpr::kWarpModes[ix (i)];

            if (! m.implemented || m.kind != warpr::WarpKind::Amp)
                ++notImplemented;

            if (std::string (m.name) != std::string (warpr::choices::warpMode[i]))
                ++nameBad;

            warpr::WarpFamily expected = warpr::WarpFamily::Clip;
            if (i <= 22 || i == 24) expected = warpr::WarpFamily::Fold;
            else if (i == 23)       expected = warpr::WarpFamily::Dyn;
            else if (i >= 38 && i <= 40) expected = warpr::WarpFamily::Rect;
            else if (i >= 41)       expected = warpr::WarpFamily::Qnt;
            if (m.family != expected)
                ++familyBad;

            if (m.stepFlag != (i >= 41) || m.seamJump || m.aliasByDesign != (i == 41 || i == 42))
                ++flagBad;

            if (m.minOS != 1 && m.minOS != 2 && m.minOS != 4 && m.minOS != 8)
                ++osBad;
        }

        int stillTodo = 0;
        for (int i = 44; i < 60; ++i)
            if (warpr::kWarpModes[ix (i)].implemented)
                ++stillTodo;

        check (notImplemented == 0, "rows 15-43 are implemented amplitude-domain modes");
        check (nameBad == 0, "names equal choices::warpMode[15..43] (parameter-spec spelling)");
        check (familyBad == 0, "families: FOLD 15-22 + 24, DYN 23, CLIP 25-37, RECT 38-40, QNT 41-43");
        check (flagBad == 0, "flags: stepFlag on the quantisers 41-43 only, aliasByDesign on Bitcrush + SR Reduction only, no seam jumps");
        check (osBad == 0, "minOS hints are 1, 2, 4 or 8");
        check (stillTodo == 0 && warpr::kNumImplementedWarpModes == 44, "rows 44-59 (D6) are still explicit identity; 44 implemented modes");
    }

    //==========================================================================
    // 2. primitives
    //==========================================================================
    void testPrimitives()
    {
        std::printf ("\n[D3] FastMath additions and fold / clip building blocks\n");

        double worstAtan = 0.0, worstAcos = 0.0, worstCos = 0.0, worstLncosh = 0.0, worstTanh = 0.0;
        for (double x = -60.0; x <= 60.0; x += 0.0137)
            worstAtan = std::max (worstAtan, std::abs (warpr::fastmath::atan (x) - std::atan (x)));
        for (double x = -0.9999; x <= 0.9999; x += 0.00123)
            worstAcos = std::max (worstAcos, std::abs (sw::acosF (x) - std::acos (x)));
        for (double c = -3.0; c <= 3.0; c += 0.00137)
            worstCos = std::max (worstCos, std::abs (warpr::fastmath::cos2pi (c) - std::cos (2.0 * kPi * c)));
        for (double u = -30.0; u <= 30.0; u += 0.0171)
        {
            worstLncosh = std::max (worstLncosh, std::abs (sw::lncosh (u) - std::log (std::cosh (u))));
            worstTanh = std::max (worstTanh, std::abs (sw::tanhF (u) - std::tanh (u)));
        }

        std::printf ("        (worst: atan %.2e, acos %.2e, cos2pi %.2e, lncosh %.2e, tanh %.2e)\n",
                     worstAtan, worstAcos, worstCos, worstLncosh, worstTanh);
        check (worstAtan < 1.0e-8 && worstAcos < 1.0e-8 && worstCos < 1.0e-8 && worstLncosh < 1.0e-8 && worstTanh < 1.0e-8,
               "atan / acos / cos2pi / lncosh / tanh within 1e-8 of libm");

        bool floorOk = true;
        for (double x = -1000.0; x <= 1000.0; x += 0.0731)
            floorOk = floorOk && warpr::fastmath::floorFast (x) == std::floor (x);
        floorOk = floorOk && warpr::fastmath::floorFast (-3.0) == -3.0 && warpr::fastmath::floorFast (2.0) == 2.0;
        check (floorOk, "floorFast == floor for |x| <= 1000 (negative values included)");

        // triangle fold: identity inside [-1, 1], odd, period 4, reflects at +-1
        bool foldOk = true;
        for (double u = -9.0; u <= 9.0; u += 0.0137)
        {
            foldOk = foldOk && std::abs (sw::triFold (u) + sw::triFold (-u)) < 1.0e-12
                            && std::abs (sw::triFold (u) - sw::triFold (u + 4.0)) < 1.0e-12
                            && std::abs (sw::triFold (u)) <= 1.0 + 1.0e-12;
            if (std::abs (u) <= 1.0)
                foldOk = foldOk && std::abs (sw::triFold (u) - u) < 1.0e-12;
        }
        foldOk = foldOk && std::abs (sw::triFold (1.5) - 0.5) < 1.0e-12 && std::abs (sw::triFold (3.0) + 1.0) < 1.0e-12;
        check (foldOk, "triFold: identity on [-1, 1], odd, period 4, |y| <= 1, reflects at the walls");

        // hard clip knee: C1, exact in the linear and saturated parts
        bool clipOk = true;
        for (double w : { 0.0, 0.25, 0.6, 0.999 })
        {
            clipOk = clipOk && std::abs (sw::hardClip (0.3 * (1.0 - w), w) - 0.3 * (1.0 - w)) < 1.0e-12
                            && std::abs (sw::hardClip (1.0 + w, w) - 1.0) < 1.0e-12
                            && std::abs (sw::hardClip (4.0, w) - 1.0) < 1.0e-12
                            && std::abs (sw::hardClip (-4.0, w) + 1.0) < 1.0e-12;
        }
        clipOk = clipOk && std::abs (sw::hardClip (1.0, 0.5) - 0.875) < 1.0e-12;
        check (clipOk, "hardClip: linear below the knee, +-1 above it, quadratic knee value 0.875 at u = 1, w = 0.5");
    }

    //==========================================================================
    // 3. property test F1' == f
    //==========================================================================
    struct Pair
    {
        std::string name;
        std::function<double (double)> f, F1;
        double jump = 0.0;                    // largest step of f (staircases), widens the quadrature tolerance
        std::vector<double> breaks;           // x where f (or f') may be discontinuous
    };

    void addLattice (Pair& p, const sw::Kernel& k)
    {
        for (int m = -12; m <= 12; ++m)
        {
            const double x = ((double) m - k.b) / k.D;
            if (std::abs (x) <= 1.3)
                p.breaks.push_back (x);
        }
    }

    std::vector<Pair> buildPairs()
    {
        std::vector<Pair> out;

        const double sets[][3] = { { 0.4, 0.3, 0.2 }, { 0.7, 0.75, 0.6 }, { 1.0, 0.5, 1.0 }, { 0.55, 1.0, 0.0 } };
        const int modes[] = { 15, 17, 18, 19, 20, 21, 22, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 43 };

        for (int mode : modes)
        {
            int setIndex = 0;
            for (const auto& s : sets)
            {
                const sw::Kernel k = sw::makeKernel (mode, s[0], s[1], s[2]);

                Pair p;
                p.name = std::string (modeName (mode)) + " set " + std::to_string (setIndex++);
                p.f  = [k] (double x) { return k.f (x); };
                p.F1 = [k] (double x) { return k.F1 (x); };
                addLattice (p, k);

                if (mode == 18)                                           // skewed-triangle corners
                    for (int n = -4; n <= 4; ++n)
                        for (double corner : { 0.0, k.c[0] })
                        {
                            const double x = ((double) n * k.c[1] + corner - k.c[4] - k.b) / k.D;
                            if (std::abs (x) <= 1.3) p.breaks.push_back (x);
                        }

                if (mode == 25)                                           // knee edges
                    for (double edge : { 1.0 - k.c[0], 1.0 + k.c[0], -(1.0 - k.c[0]), -(1.0 + k.c[0]) })
                        p.breaks.push_back (edge / k.D);

                if (mode == 41)                                           // staircase risers at (m + 1/2) delta
                {
                    p.jump = k.c[0] * (1.0 - k.c[1]);
                    for (int m = -20; m <= 20; ++m)
                        p.breaks.push_back (((double) m + 0.5) * k.c[0]);
                }

                out.push_back (std::move (p));
            }
        }

        // the three Multi Fold stage kernels
        for (double t : { 1.0, 0.5, 0.25 })
        {
            sw::FoldStageKernel st;
            st.T = t;
            st.invT = 1.0 / t;

            Pair p;
            p.name = "fold stage T=" + std::to_string (t);
            p.f  = [st] (double x) { return st.f (x); };
            p.F1 = [st] (double x) { return st.F1 (x); };
            for (int m = -9; m <= 9; m += 2)
                p.breaks.push_back ((double) m * t);
            out.push_back (std::move (p));
        }

        return out;
    }

    void testPairs()
    {
        std::printf ("\n[D3] property test: dF1/dx == f for every closed-form pair (and F1 continuity)\n");

        const auto pairs = buildPairs();

        constexpr double h = 5.0e-5;
        int badPairs = 0, quadBad = 0, contBad = 0, skipBad = 0;
        double worstAll = 0.0, worstQuadAll = 0.0;
        long long tested = 0, skipped = 0;

        for (const auto& p : pairs)
        {
            double worst = 0.0;
            int pairTested = 0, pairSkipped = 0;

            // (a) central differences; windows where the two step sizes disagree (kinks, jumps, steep curvature) are excluded
            auto probe = [&] (double x)
            {
                const double d1 = (p.F1 (x + h) - p.F1 (x - h)) / (2.0 * h);
                const double d2 = (p.F1 (x + 0.5 * h) - p.F1 (x - 0.5 * h)) / h;

                if (std::abs (d1 - d2) > 5.0e-6)
                {
                    ++pairSkipped;
                    return;
                }

                ++pairTested;
                worst = std::max (worst, std::abs (d1 - p.f (x)));
            };

            for (double x = -1.3 + 0.000031; x <= 1.3; x += 0.00137)
                probe (x);
            for (double bp : p.breaks)
            {
                probe (bp + 3.7e-4);
                probe (bp - 3.7e-4);
            }

            // (b) interval quadrature: F1 (x1) - F1 (x0) == integral of f (composite Simpson, 64 panels)
            double worstQuad = 0.0;
            constexpr double cell = 0.0037;
            constexpr int panels = 64;
            const double tol = 4.0e-6 + p.jump * (cell / panels);

            for (double x0 = -1.3; x0 < 1.3; x0 += cell)
            {
                const double dx = cell / panels;
                double acc = p.f (x0) + p.f (x0 + cell);
                for (int i = 1; i < panels; ++i)
                    acc += p.f (x0 + dx * (double) i) * (i % 2 == 1 ? 4.0 : 2.0);

                const double integral = acc * dx / 3.0;
                worstQuad = std::max (worstQuad, std::abs ((p.F1 (x0 + cell) - p.F1 (x0)) - integral) - tol);
            }

            // (c) F1 continuity at every breakpoint
            double worstCont = 0.0;
            for (double bp : p.breaks)
                worstCont = std::max (worstCont, std::abs (p.F1 (bp + 1.0e-7) - p.F1 (bp - 1.0e-7)));

            tested += pairTested;
            skipped += pairSkipped;
            worstAll = std::max (worstAll, worst);
            worstQuadAll = std::max (worstQuadAll, worstQuad);

            if (worst > 1.0e-5)
            {
                ++badPairs;
                DETAIL ("        F1' != f for %s: %.2e (%d points)\n", p.name.c_str(), worst, pairTested);
            }
            if (worstQuad > 0.0)
            {
                ++quadBad;
                DETAIL ("        interval mismatch for %s: %.2e over tolerance\n", p.name.c_str(), worstQuad);
            }
            if (worstCont > 1.0e-5)
            {
                ++contBad;
                DETAIL ("        F1 discontinuous at a breakpoint for %s: %.2e\n", p.name.c_str(), worstCont);
            }
            if (pairSkipped > (pairTested + pairSkipped) / 12)
            {
                ++skipBad;
                DETAIL ("        too many excluded points for %s: %d of %d\n", p.name.c_str(), pairSkipped, pairTested + pairSkipped);
            }
        }

        std::printf ("        (%d pairs, %lld points tested, %lld excluded as non-smooth, worst |F1' - f| %.2e)\n",
                     (int) pairs.size(), tested, skipped, worstAll);
        check (badPairs == 0, "dF1/dx == f to 1e-5 at every smooth sample point, for every closed-form pair (25 modes x 4 parameter sets + 3 fold stages)");
        check (quadBad == 0, "F1 differences equal the integral of f over 1400 cells per pair (no branch offset errors)");
        check (contBad == 0, "F1 is continuous at every breakpoint (kinks, clamps, staircase risers)");
        check (skipBad == 0, "the non-smooth exclusion never removes more than 8.3 % of the sample points of a pair");
    }

    //==========================================================================
    // 4. per-mode behaviour
    //==========================================================================
    void testBehaviour()
    {
        std::printf ("\n[D3] per-mode behaviour: known values, monotone staircases, hold period, Dynamic Fold, stage counts\n");

        auto kern = [] (int mode, double a, double A, double B) { return sw::makeKernel (mode, a, A, B); };
        const double aD3 = 1.0, AD3 = 1.0 / 15.0;                  // D = 1 + 1 * (1 + 15/15) = 3

        // Linear Fold at D = 3
        {
            const auto k = kern (15, aD3, AD3, 0.0);
            check (std::abs (k.D - 3.0) < 1.0e-12 && std::abs (k.f (0.2) - 0.6) < 1.0e-9 && std::abs (k.f (0.5) - 0.5) < 1.0e-9
                   && std::abs (k.f (2.0 / 3.0)) < 1.0e-9 && std::abs (k.f (1.0) + 1.0) < 1.0e-9 && std::abs (k.f (-1.0) - 1.0) < 1.0e-9,
                   "Linear Fold D=3: f(0.2) = 0.6, f(0.5) = 0.5, f(2/3) = 0, f(+-1) = -+1");
        }

        // Hard Clip at D = 3
        {
            const auto k = kern (25, aD3, AD3, 0.0);
            const auto soft = kern (25, aD3, AD3, 0.5);
            check (std::abs (k.f (0.2) - 0.6) < 1.0e-9 && std::abs (k.f (0.5) - 1.0) < 1.0e-9 && std::abs (k.f (-0.5) + 1.0) < 1.0e-9
                   && std::abs (soft.f (1.0 / 3.0) - 0.875) < 1.0e-9,
                   "Hard Clip D=3: linear below, +-1 above, quadratic knee value at B = 0.5");
        }

        // closed forms against libm
        {
            double worst = 0.0;
            for (double x = -1.0; x <= 1.0; x += 0.0173)
            {
                const double d = 3.0;
                worst = std::max (worst, std::abs (kern (27, aD3, AD3, 0.0).f (x) - std::tanh (d * x)));
                worst = std::max (worst, std::abs (kern (26, aD3, AD3, 0.0).f (x) - d * x / (1.0 + std::abs (d * x))));
                worst = std::max (worst, std::abs (kern (28, aD3, AD3, 0.0).f (x) - (2.0 / kPi) * std::atan (d * x)));
                worst = std::max (worst, std::abs (kern (30, aD3, AD3, 0.0).f (x) - (x < 0.0 ? -1.0 : 1.0) * (1.0 - std::exp (-std::abs (d * x)))));
                worst = std::max (worst, std::abs (kern (17, aD3, AD3, 0.0).f (x) - std::sin (0.5 * kPi * d * x)));
            }
            check (worst < 1.0e-8, "Tanh / Soft Clip / Arctangent / Diode 1 / Sine Fold equal their libm definitions (1e-8)");
        }

        // Cubic peak, Sinusoidal saturation, Polynomial identity at a -> 0 shape
        {
            const auto cubic = kern (32, aD3, AD3, 0.0);
            const auto sine = kern (33, aD3, AD3, 0.0);
            check (std::abs (cubic.f (1.0 / 3.0) - 1.0) < 1.0e-9 && std::abs (cubic.f (0.9) - 1.0) < 1.0e-9
                   && std::abs (cubic.f (0.1) - 1.5 * (0.3 - 0.009)) < 1.0e-9 && std::abs (sine.f (0.5) - 1.0) < 1.0e-9,
                   "Cubic peaks at exactly 1 (normalised x1.5), Sinusoidal saturates at 1 without folding");
        }

        // Symmetric Fold is even at B = 0; Asymmetric Fold's negative half is bounded by T-
        {
            const auto sym = kern (21, 0.7, 0.5, 0.0);
            double evenWorst = 0.0;
            for (double x = 0.01; x <= 1.0; x += 0.0137)
                evenWorst = std::max (evenWorst, std::abs (sym.f (x) - sym.f (-x)));

            const double B = 0.6;
            const auto asym = kern (22, 0.9, 0.5, B);
            const double tNeg = 1.0 / (1.0 + 3.0 * B);
            double minNeg = 0.0, maxPos = 0.0;
            for (double x = -1.0; x <= 0.0; x += 0.0031)             // negative input half: bounded by T-
                minNeg = std::min (minNeg, asym.f (x));
            for (double x = 0.0; x <= 1.0; x += 0.0031)              // positive input half: reflects at 1
                maxPos = std::max (maxPos, asym.f (x));
            check (evenWorst < 1.0e-12 && std::abs (sym.f (0.0)) < 1.0e-12, "Symmetric Fold (B = 0) is an even function with f(0) = 0");
            check (minNeg >= -tNeg - 1.0e-9 && minNeg < -0.9 * tNeg && maxPos > 0.9 && maxPos <= 1.0 + 1.0e-9,
                   "Asymmetric Fold: positive side reflects at 1, negative side at T- = 1 / (1 + 3B)");
        }

        // Rectifiers
        {
            const auto full = kern (38, 1.0, 0.5, 0.0);
            const auto half = kern (39, 1.0, 0.5, 0.0);
            const auto soft = kern (40, 1.0, 0.5, 0.0);
            bool ok = true;
            for (double x = -1.0; x <= 1.0; x += 0.0173)
            {
                ok = ok && full.f (x) >= -1.0e-12 && std::abs (full.f (x) - std::abs (x)) < 1.0e-9;
                ok = ok && (x >= 0.0 ? std::abs (half.f (x) - x) < 1.0e-9 : std::abs (half.f (x)) < 1.0e-9);
                ok = ok && soft.f (x) >= -1.0e-12 && std::abs (soft.f (x) - (std::sqrt (x * x + 0.0 + soft.c[0] * soft.c[0]) - soft.c[0])) < 1.0e-9;
            }
            check (ok && full.g == 1.0 && kern (38, 0.5, 0.5, 0.0).g == 0.5, "rectifiers: |x|, max(x, 0), sqrt(x^2 + eps^2) - eps; blend weight = warpAmount (cross-fade from identity)");
        }

        // Staircases: levels, monotone
        {
            const auto crush = kern (41, 0.5, 0.0, 0.0);
            const double delta = crush.c[0];
            bool levels = true, monotone = true;
            double prev = -1.0e9;
            for (double x = -1.0; x <= 1.0; x += 0.00071)
            {
                const double y = crush.f (x);
                levels = levels && std::abs (y / delta - std::round (y / delta)) < 1.0e-8;
                monotone = monotone && y >= prev - 1.0e-12;
                prev = y;
            }

            const auto quant = kern (43, 0.5, 0.0, 0.0);
            double prevQ = -1.0e9;
            bool quantMono = true;
            std::set<long long> distinct;
            for (double x = -1.0; x <= 1.0; x += 0.00071)
            {
                const double y = quant.f (x);
                quantMono = quantMono && y >= prevQ - 1.0e-9;
                prevQ = y;
                distinct.insert (std::llround (y / quant.c[0] * 20.0));
            }

            std::printf ("        (Bitcrush a=0.5: step %.5f = %.2f bits; Quantization a=0.5: step %.4f, %d distinct plateaus/transitions)\n",
                         delta, 1.0 - std::log2 (delta), quant.c[0], (int) distinct.size());
            check (levels && monotone, "Bitcrush: every output is an exact multiple of the step, staircase is monotone");
            check (quantMono && (int) distinct.size() > 8, "Quantization: soft staircase is monotone and has several steps at a = 0.5");
            check (std::abs (kern (41, 1.0, 0.0, 0.0).c[0] - 0.5) < 1.0e-9 && std::abs (kern (43, 1.0, 0.5, 0.0).c[0] - 1.0) < 1.0e-9,
                   "Bitcrush a=1: 2 bits (step 0.5); Quantization a=1: 2 steps (step 1)");
        }

        // SR Reduction: hold period
        {
            sw::ChannelState cs;
            const sw::RunInfo ri { 1, 48000.0 };
            const auto k = kern (42, 0.2, 0.0, 0.0);                       // period 1 + 127 * 0.04 = 6.08 base samples
            int changes = 0;
            double prev = 0.0;
            for (int n = 0; n < 600; ++n)
            {
                const double y = sw::run (k, cs, ri, 1.0e-4 * (double) n);
                if (n > 0 && std::abs (y - prev) > 1.0e-6) ++changes;
                prev = y;
            }
            std::printf ("        (SR Reduction a=0.2: %d hold captures in 600 samples, expected 98.7)\n", changes);
            check (changes >= 96 && changes <= 101, "SR Reduction: the hold period is 1 + 127 a^2 base samples");

            // at OS 4 the same hold lasts 4x as many oscillator samples
            sw::ChannelState cs4;
            const sw::RunInfo ri4 { 4, 192000.0 };
            int changes4 = 0;
            prev = 0.0;
            for (int n = 0; n < 2400; ++n)
            {
                const double y = sw::run (k, cs4, ri4, 1.0e-4 * (double) n);
                if (n > 0 && std::abs (y - prev) > 1.0e-6) ++changes4;
                prev = y;
            }
            check (changes4 >= 96 && changes4 <= 101, "SR Reduction: the hold period scales with the oversampling factor (same rate in Hz)");
        }

        // Dynamic Fold == Linear Fold at A = 0 (no envelope depth), and the follower rises
        {
            sw::ChannelState c23, c15;
            const sw::RunInfo ri { 2, 96000.0 };
            const auto k23 = kern (23, 0.8, 0.0, 0.3);
            const auto k15 = kern (15, 0.8, 0.0, 0.0);
            bool same = true;
            for (int n = 0; n < 2000; ++n)
            {
                const double x = 0.7 * std::sin (0.013 * (double) n);
                same = same && sw::run (k23, c23, ri, x) == sw::run (k15, c15, ri, x);
            }
            sw::ChannelState cEnv;
            const auto kEnv = kern (23, 0.8, 1.0, 0.0);
            for (int n = 0; n < 4000; ++n)
                sw::run (kEnv, cEnv, ri, 0.9);
            check (same, "Dynamic Fold with A = 0 equals Linear Fold bit-exactly (D_eff = D)");
            check (cEnv.env > 0.85 && cEnv.env <= 0.9 + 1.0e-9, "Dynamic Fold follower converges to the input level");
        }

        // Multi Fold stage count and Chebyshev order
        {
            check (static_cast<int> (kern (16, 1.0, 0.5, 0.0).c[0]) == 2 && static_cast<int> (kern (16, 1.0, 0.5, 1.0).c[0]) == 5
                   && static_cast<int> (kern (16, 1.0, 0.5, 0.5).c[0]) == 4,
                   "Multi Fold: K = 2 + round(3B) -> 2 / 4 / 5 stages at B = 0 / 0.5 / 1");
            check (std::abs (kern (24, 0.0, 0.5, 0.0).c[0] - 1.0) < 1.0e-12 && std::abs (kern (24, 1.0, 0.5, 0.0).c[0] - 13.0) < 1.0e-12,
                   "Chebyshev Fold: real order n = 1 at a = 0 and 13 at a = 1 (B = 0)");

            // Chebyshev Harmonic keeps the fundamental: f(x) - x is the harmonic sum only
            const auto ch = kern (37, 1.0, 0.5, 0.5);
            double meanOdd = 0.0;
            for (double x = 0.0; x <= 0.5; x += 0.01)
                meanOdd += ch.f (x) - x;
            check (std::abs (ch.f (0.0)) < 1.0e-12 && std::isfinite (meanOdd), "Chebyshev Harmonic: s(0) = 0 after the offset removal of the even terms");
        }
    }

    //==========================================================================
    // 5. T1 universal cube
    //==========================================================================
    /** sup |s'(x)| of a mode's kernel over [-1, 1], at least 1 (the blend (1-g) + g s' is bounded by it). */
    double supSlope (int mode, double a, double A, double B)
    {
        auto k = sw::makeKernel (mode, a, A, B);
        if (mode == 23)
            k.D *= (1.0 + A);                                   // largest dynamic drive

        double m = 1.0;
        constexpr double h = 2.0e-4;
        for (double x = -1.0; x <= 1.0; x += 0.001)
            m = std::max (m, std::abs (k.f (x + h) - k.f (x - h)) / (2.0 * h));
        return m;
    }

    void testUniversal (bool full)
    {
        std::printf ("\n[D3] T1 universal checks: 29 modes x 4 signals x OS x (a, A, B)%s\n", full ? "  [full cube]" : "  [quick cube]");
        constexpr double fs = 48000.0;

        auto sine = makeCycle (2048, 0.5f);
        auto saw = warpr::SampleData::createDefaultWave();
        auto noise = makeNoise();

        struct Sig { const warpr::SampleData* sd; double hz; const char* name; bool smooth; std::vector<int> osQuick, osFull; };
        const std::vector<Sig> sigs {
            { sine.get(),  100.0,    "sine 100 Hz",      true,  { 1, 2, 8 }, { 1, 2, 8 } },
            { sine.get(),  5000.0,   "sine 5 kHz",       false, { 1, 8 },    { 1, 2, 8 } },
            { saw.get(),   110.0,    "saw cycle 110 Hz", false, { 2 },       { 1, 2, 8 } },
            { noise.get(), rootHz(), "noise",            false, { 1, 8 },    { 1, 2, 8 } }
        };

        const std::vector<double> aList = full ? std::vector<double> { 0.0, 0.5, 1.0 } : std::vector<double> { 0.0, 1.0 };
        const std::vector<double> AList = full ? std::vector<double> { 0.0, 0.5, 1.0 } : std::vector<double> { 0.5 };
        const std::vector<double> bList { 0.0, 1.0 };
        const int totalBase = full ? 7200 : 3200;

        int runs = 0, nonFinite = 0, unbounded = 0, identityBad = 0, discBad = 0;
        double worstPeak = 0.0;

        for (int mode = firstD3; mode <= lastD3; ++mode)
        {
            const auto& info = warpr::warpModeInfo (mode);

            for (const auto& sig : sigs)
                for (int os : (full ? sig.osFull : sig.osQuick))
                {
                    const auto base = renderOsc (*sig.sd, sig.hz, fs, os, totalBase, noWarp());
                    const double nat = maxStepOf (base.L);

                    for (double A : AList)
                        for (double a : aList)
                            for (double B : bList)
                            {
                                if (a == 0.0 && B > 0.0 && ! full)
                                    continue;

                                ++runs;
                                const auto run = renderOsc (*sig.sd, sig.hz, fs, os, totalBase,
                                                            constSpan (mode, (float) a, (float) A, (float) B, 1.0f));

                                if (! allFinite (run.L) || ! allFinite (run.R))
                                {
                                    ++nonFinite;
                                    DETAIL ("        non-finite: mode %d (%s) %s OS %d a=%.1f A=%.1f B=%.1f\n", mode, info.name, sig.name, os, a, A, B);
                                    continue;
                                }

                                const double pk = std::max (peakOf (run.L), peakOf (run.R));
                                worstPeak = std::max (worstPeak, pk);
                                if (pk > 4.0)
                                {
                                    ++unbounded;
                                    DETAIL ("        unbounded %.2f: mode %d (%s) %s OS %d a=%.1f A=%.1f B=%.1f\n", pk, mode, info.name, sig.name, os, a, A, B);
                                }

                                if (a == 0.0 && ! (bitEqual (base.L, run.L) && bitEqual (base.R, run.R)))
                                {
                                    ++identityBad;
                                    DETAIL ("        a=0 not bit-exact dry (null %.1f dB): mode %d (%s) %s OS %d\n", nullDb (base.L, run.L), mode, info.name, sig.name, os);
                                }

                                if (sig.smooth)
                                {
                                    const double bound = info.stepFlag ? 2.0
                                                                       : std::min (2.5, 2.0 * supSlope (mode, a, A, B) * nat + 0.01);
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
        check (identityBad == 0, "a == 0: bit-exact dry for all 29 modes (stronger than the -80 dB null)");
        check (discBad == 0, "smooth input: max step within 2 x (sup |s'|) x natural step (flagged quantisers use the full-swing bound)");
    }

    //==========================================================================
    // 6. stereo / mono, identity, determinism, allocations, mode switching
    //==========================================================================
    void testStereoMono()
    {
        std::printf ("\n[D3] stereo sources: shared read position, per-channel state; mono duplicate\n");
        constexpr double fs = 48000.0;

        auto same = makeStereoCycle (2048, true);
        auto diff = makeStereoCycle (2048, false);
        auto mono = makeCycle (2048, 0.5f);

        int asymmetric = 0, notIndependent = 0, nonFinite = 0, monoBad = 0;
        for (int mode = firstD3; mode <= lastD3; ++mode)
        {
            const auto a = renderOsc (*same, 100.0, fs, 2, 4800, constSpan (mode, 0.6f, 0.5f, 0.5f, 1.0f));
            if (! bitEqual (a.L, a.R))
            {
                ++asymmetric;
                DETAIL ("        L != R for identical channels: mode %d (%s)\n", mode, modeName (mode));
            }

            const auto b = renderOsc (*diff, 100.0, fs, 2, 4800, constSpan (mode, 0.3f, 0.5f, 0.5f, 1.0f));
            if (! allFinite (b.L) || ! allFinite (b.R))
                ++nonFinite;
            if (bitEqual (b.L, b.R))
            {
                ++notIndependent;
                DETAIL ("        L == R for different channels: mode %d (%s)\n", mode, modeName (mode));
            }

            const auto m = renderOsc (*mono, 100.0, fs, 2, 4800, constSpan (mode, 0.7f, 0.5f, 0.5f, 0.6f));
            if (! bitEqual (m.L, m.R))
                ++monoBad;
        }

        check (asymmetric == 0, "identical L / R channels stay bit-identical through every D3 mode (symmetric state and dither seed)");
        check (notIndependent == 0 && nonFinite == 0, "different channels stay independent and finite");
        check (monoBad == 0, "mono source: L == R exactly through every D3 mode (also with warpMix < 1)");
    }

    void testIdentity()
    {
        std::printf ("\n[D3] identity guarantees: a = 0, warpMix = 0, modes 44-59\n");
        constexpr double fs = 48000.0;
        auto sine = makeCycle (2048, 0.5f);
        auto saw = warpr::SampleData::createDefaultWave();

        for (const auto* sd : { sine.get(), saw.get() })
        {
            const auto base = renderOsc (*sd, 220.0, fs, 2, 6400, noWarp());

            int zeroBad = 0, mixBad = 0, todoBad = 0;
            for (int mode = firstD3; mode <= lastD3; ++mode)
            {
                for (float A : { 0.0f, 0.5f, 1.0f })
                    for (float B : { 0.0f, 1.0f })
                    {
                        const auto z = renderOsc (*sd, 220.0, fs, 2, 6400, constSpan (mode, 0.0f, A, B, 1.0f));
                        if (! bitEqual (base.L, z.L) || ! bitEqual (base.R, z.R))
                        {
                            ++zeroBad;
                            DETAIL ("        mode %d (%s) at a = 0 (A=%.1f B=%.1f) is not bit-exact dry\n", mode, modeName (mode), (double) A, (double) B);
                        }
                    }

                const auto m0 = renderOsc (*sd, 220.0, fs, 2, 6400, constSpan (mode, 1.0f, 0.5f, 0.5f, 0.0f));
                if (nullDb (base.L, m0.L) > -140.0)
                    ++mixBad;
            }

            for (int mode = 44; mode < 60; ++mode)
            {
                const auto t = renderOsc (*sd, 220.0, fs, 2, 6400, constSpan (mode, 1.0f, 0.5f, 0.5f, 1.0f));
                if (! bitEqual (base.L, t.L))
                    ++todoBad;
            }

            check (zeroBad == 0, "a == 0 is bit-exact dry for all 29 D3 modes (A in {0, 0.5, 1}, B in {0, 1}); Bitcrush / Quantization / SR Reduction included");
            check (mixBad == 0, "warpMix = 0 is the dry signal for all 29 D3 modes (-140 dB null)");
            check (todoBad == 0, "modes 44-59 are still the exact identity");
        }
    }

    void testDeterminismAndAllocations()
    {
        std::printf ("\n[D3] determinism and zero allocations (class level)\n");
        constexpr double fs = 48000.0;
        auto saw = warpr::SampleData::createDefaultWave();

        int nondeterministic = 0;
        for (int mode = firstD3; mode <= lastD3; ++mode)
        {
            const auto a = renderOsc (*saw, 110.0, fs, 2, 6400, constSpan (mode, 0.8f, 0.4f, 0.6f, 0.9f));
            const auto b = renderOsc (*saw, 110.0, fs, 2, 6400, constSpan (mode, 0.8f, 0.4f, 0.6f, 0.9f));
            if (! bitEqual (a.L, b.L) || ! bitEqual (a.R, b.R))
            {
                ++nondeterministic;
                DETAIL ("        non-deterministic: mode %d (%s)\n", mode, modeName (mode));
            }
        }
        check (nondeterministic == 0, "two identical renders are bit-equal for all 29 modes (dither / jitter PRNG is seeded per note)");

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
            for (int ci = 0; ci < 900; ++ci)
            {
                warpr::WarpSpan sp;
                sp.active = true;
                sp.mode = firstD3 + (ci / 4) % 29;           // walks through all 29 modes, switching every 4 chunks
                sp.a0 = (float) (ci % 13) / 13.0f;  sp.a1 = (float) ((ci + 1) % 13) / 13.0f;
                sp.A0 = sp.A1 = (float) (ci % 7) / 6.0f;
                sp.B0 = sp.B1 = (float) (ci % 5) / 4.0f;
                sp.mix0 = 1.0f;  sp.mix1 = (ci % 5 == 0) ? 0.6f : 1.0f;
                sp.Lc = lc;
                sp.os = 2;
                sp.osRate = fs * 2.0;
                sp.fadeLen = 960;
                osc.render (*saw, rg, 0, rho, rho, 2, 1.0f, 1.0f, 1.0f, 1.0f, l.data(), r.data(), 64, &sp);
            }
            allocations = counter.count();
        }
        check (allocations == 0, "UnisonOsc::render with the D3 modes (all 29, switches, ramps, stateful kernels) performs zero allocations");
    }

    void testModeSwitch()
    {
        std::printf ("\n[D3] mode switching mid-note between D3 modes (10 ms crossfade)\n");
        constexpr double fs = 48000.0;
        auto sine = makeCycle (2048, 0.5f);

        struct Case { int from, to; double bound; bool tail; };
        const Case cases[] = {
            { 17, 25, 0.1, true }, { 27, 15, 0.1, true }, { 29, 38, 0.1, true }, { 35, 26, 0.1, true }, { 19, 30, 0.1, true },
            { 22, 3, 0.1, true },  { 32, 21, 0.1, true }, { 16, 34, 0.1, true }, { 24, 25, 0.1, true }, { 20, 40, 0.1, true },
            { 36, 28, 0.1, true }, { 31, 37, 0.1, true }, { 23, 17, 0.1, false },
            { 35, 41, 0.3, false }, { 42, 27, 0.3, false }, { 43, 15, 0.3, true }, { 41, 17, 0.3, false }, { 44, 33, 0.1, true }
        };
        constexpr int totalBase = 9600, switchChunk = 150;

        int stepBad = 0, tailBad = 0;
        double worstStep = 0.0;

        for (const auto& cs : cases)
        {
            auto sw2 = [&] (int ci, warpr::WarpSpan& sp)
            {
                sp.active = true;
                sp.mode = ci < switchChunk ? cs.from : cs.to;
                sp.a0 = sp.a1 = 0.35f;
                sp.A0 = sp.A1 = 0.5f;
                sp.B0 = sp.B1 = 0.5f;
                sp.mix0 = sp.mix1 = 1.0f;
                return true;
            };

            const auto run = renderOsc (*sine, 100.0, fs, 2, totalBase, sw2);
            const auto pureTo = renderOsc (*sine, 100.0, fs, 2, totalBase, constSpan (cs.to, 0.35f, 0.5f, 0.5f, 1.0f));

            const double step = maxStepOf (run.L);
            worstStep = std::max (worstStep, step);
            if (step > cs.bound)
            {
                ++stepBad;
                DETAIL ("        switch %d -> %d: max step %.4f > %.2f\n", cs.from, cs.to, step, cs.bound);
            }

            if (cs.tail)
            {
                const size_t tail = run.L.size() * 3 / 4;
                if (nullDb (pureTo.L, run.L, tail) > -100.0)
                {
                    ++tailBad;
                    DETAIL ("        switch %d -> %d: not converged to the new mode (null %.1f dB)\n", cs.from, cs.to, nullDb (pureTo.L, run.L, tail));
                }
            }
        }

        std::printf ("        (%d pairs, worst max step %.4f FS)\n", (int) (sizeof (cases) / sizeof (cases[0])), worstStep);
        check (stepBad == 0, "mode switch mid-note: max step <= 0.1 FS (0.3 FS where a quantiser / hold is involved: its own step is the bound)");
        check (tailBad == 0, "after the 10 ms fade the output equals the new mode rendered alone (stateless target modes)");
    }

    //==========================================================================
    // 7. T2 alias gates
    //==========================================================================
    constexpr double kBaseFs = 48000.0;
    constexpr int kBin = 2053;                                      // f0 = fs * 2053 / 32768 = 3007.4 Hz
    const double kF0 = kBaseFs * (double) kBin / 32768.0;

    /** Alias level (dB) of mode `mode` driven by a sine of amplitude `amp` at kF0, run at os * 48 kHz, in-band only. */
    double aliasLevel (int mode, double a, double A, double B, double amp, int os, bool useAdaa)
    {
        const double rate = kBaseFs * (double) os;
        const int order = 15 + (os == 1 ? 0 : (os == 2 ? 1 : (os == 4 ? 2 : 3)));
        const int n = 1 << order;
        const int total = n + 2048;

        const auto k = sw::makeKernel (mode, a, A, B);
        sw::ChannelState cs;
        const sw::RunInfo ri { os, rate };

        std::vector<float> y ((size_t) total);
        for (int i = 0; i < total; ++i)
        {
            const double x = amp * std::sin (2.0 * kPi * kF0 * (double) i / rate);
            y[(size_t) i] = (float) (useAdaa ? sw::run (k, cs, ri, x) : sw::naive (k, x));
        }

        return warpr::tests::aliasDb (y.data(), total, rate, kF0, order, 8, 0.5 * kBaseFs);
    }

    void testAliasGates()
    {
        std::printf ("\n[D3] T2 alias gates A1 / A2 (3.0 kHz sine, drive 16 = a 1, A 14/15; amplitude 0.25 = u-peak 4)\n");

        struct Gated { int mode; const char* name; };
        const Gated gated[] = { { 25, "Hard Clip" }, { 27, "Tanh" }, { 15, "Linear Fold" }, { 17, "Sine Fold" } };
        const double a = 1.0, A = 14.0 / 15.0, B = 0.0;
        const double amp = 0.25;

        int a1Bad = 0, a2Bad = 0;

        for (const auto& g : gated)
        {
            const double naive1 = aliasLevel (g.mode, a, A, B, amp, 1, false);
            const double adaa1  = aliasLevel (g.mode, a, A, B, amp, 1, true);
            const double improvement = naive1 - adaa1;

            std::printf ("        A1 %-12s 1x: naive %7.1f dB, ADAA %7.1f dB  -> improvement %5.1f dB (gate 12)\n",
                         g.name, naive1, adaa1, improvement);
            const int minOsHint = warpr::warpModeInfo (g.mode).minOS;
            if (improvement >= 12.0)
            {
                std::printf ("        A1 %s: %.1f dB >= 12 -> PASS\n", g.name, improvement);
            }
            else if (minOsHint >= 2)
            {
                // R1(b) fallback: minOS is enforced by WarprSynth (latched OS = max(user, minOS)), so the naive-1x
                // operating point never occurs for this mode.
                std::printf ("        A1 %s: %.1f dB < 12 -> covered by minOS %d (R1(b) fallback, enforced at note-on)\n",
                             g.name, improvement, minOsHint);
            }
            else
            {
                ++a1Bad;
                DETAIL ("        A1 FAILED for %s: %.1f dB < 12 dB and minOS %d does not cover 1x operation\n", g.name, improvement, minOsHint);
            }

            const double adaa2  = aliasLevel (g.mode, a, A, B, amp, 2, true);
            const double adaa4  = aliasLevel (g.mode, a, A, B, amp, 4, true);
            const double naive8 = aliasLevel (g.mode, a, A, B, amp, 8, false);
            const double limit = naive8 + 6.0;

            int need = 8;
            if (adaa2 <= limit) need = 2;
            else if (adaa4 <= limit) need = 4;

            const int table = warpr::warpModeInfo (g.mode).minOS;
            std::printf ("        A2 %-12s naive 8x %7.1f dB (+6 -> %7.1f), ADAA 2x %7.1f dB, ADAA 4x %7.1f dB -> required minOS %d, table minOS %d%s\n",
                         g.name, naive8, limit, adaa2, adaa4, need, table, adaa2 <= limit ? "  [2x meets the gate]" : "  [R1(b): 2x misses the gate]");
            if (table < need)
            {
                ++a2Bad;
                DETAIL ("        A2: raise minOS of %s from %d to %d\n", g.name, table, need);
            }

            // the same shape at full scale, for the record (not gated: u-peak 16 pushes the bandwidth far beyond 2x)
            std::printf ("        (full-scale amplitude 1.0 for the record: 1x naive %.1f / ADAA %.1f dB, 2x ADAA %.1f dB, naive 8x %.1f dB)\n",
                         aliasLevel (g.mode, a, A, B, 1.0, 1, false), aliasLevel (g.mode, a, A, B, 1.0, 1, true),
                         aliasLevel (g.mode, a, A, B, 1.0, 2, true), aliasLevel (g.mode, a, A, B, 1.0, 8, false));
        }

        check (a1Bad == 0, "A1: ADAA lowers the 1x alias power by >= 12 dB, or the mode's enforced minOS (>= 2) removes 1x operation (R1(b))");
        check (a2Bad == 0, "A2: every gated mode meets '2x + ADAA <= naive 8x + 6 dB' or carries the OS requirement it needs as its minOS hint (R1(b))");

        // informational table over every other closed-form mode at 1x
        std::printf ("        alias table at 1x (a 1, A 14/15, B 0, amplitude 0.25; improvement = naive - ADAA):\n");
        for (int mode : { 18, 19, 20, 21, 22, 26, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 43 })
        {
            const double naive1 = aliasLevel (mode, a, A, B, amp, 1, false);
            const double adaa1 = aliasLevel (mode, a, A, B, amp, 1, true);
            std::printf ("          %-20s naive %7.1f dB  ADAA %7.1f dB  improvement %5.1f dB\n", modeName (mode), naive1, adaa1, naive1 - adaa1);
        }

        // quantisers: aliasing is present by design
        const double crush = aliasLevel (41, 0.5, 0.0, 0.0, 0.5, 1, true);
        const double hold = aliasLevel (42, 0.5, 0.0, 0.0, 0.5, 1, true);
        std::printf ("        by design: Bitcrush (a 0.5) alias %.1f dB, SR Reduction (a 0.5) alias %.1f dB at 1x  (EXPECTED: not subject to A1 / A2)\n", crush, hold);
        check (crush > -55.0 && hold > -55.0, "Bitcrush and SR Reduction alias above -55 dB at 1x: aliasing is present by design (documented, exempt from the gates)");
    }

    //==========================================================================
    // 8. processor level
    //==========================================================================
    void testProcessorLevel()
    {
        std::printf ("\n[D3] processor level: DC after the blockers, bounds, zero allocations\n");
        constexpr double fs = 48000.0;

        PluginProcessor proc;
        setParam (proc, "ampSustain", 1.0f);
        setParam (proc, "ampAttack", 1.0f);
        setParam (proc, "warpAmount", 1.0f);

        juce::MidiBuffer midi;
        midi.addEvent (juce::MidiMessage::noteOn (1, 60, 0.9f), 0);

        int dcBad = 0, finiteBad = 0, peakBad = 0;
        double worstDc = 0.0;
        for (float B : { 0.0f, 1.0f })
        {
            setParam (proc, "warpParamB", B);
            for (int mode = firstD3; mode <= lastD3; ++mode)
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
                        DETAIL ("        DC %.4f: mode %d (%s) B=%.0f ch %d\n", s.mean, mode, modeName (mode), (double) B, ch);
                    }
                }
            }
        }

        std::printf ("        (worst |DC| %.5f)\n", worstDc);
        check (finiteBad == 0 && peakBad == 0, "full voice path: finite and master output <= 1.0 for all 29 modes (a = 1, B in {0, 1})");
        check (dcBad == 0, "|DC| <= 0.02 after the voice + master DC blockers for all 29 modes (asymmetric / rectifying modes included)");
        proc.releaseResources();

        // minOS enforcement: latched OS = max(user oversampling, mode minOS) while the warp stage is active
        {
            auto latchedOs = [] (const char* modeId, float mode, float amount, float userOsIndex, float laterMode)
            {
                juce::ignoreUnused (modeId);
                PluginProcessor p;
                setParam (p, "oversampling", userOsIndex);
                setParam (p, "warpMode", mode);
                setParam (p, "warpAmount", amount);
                p.setPlayConfigDetails (0, 2, fs, 256);
                p.prepareToPlay (fs, 256);
                juce::AudioBuffer<float> buf (2, 256);
                juce::MidiBuffer on;
                on.addEvent (juce::MidiMessage::noteOn (1, 60, 0.9f), 0);
                p.processBlock (buf, on);

                setParam (p, "warpMode", laterMode);              // mid-note change must not re-latch
                juce::MidiBuffer none;
                p.processBlock (buf, none);

                int os = 0;
                for (int slot = 0; slot < 40; ++slot)
                {
                    const auto info = p.getSynth().getVoiceInfo (slot);
                    if (info.active && ! info.ghost)
                        os = info.os;
                }
                p.releaseResources();
                return os;
            };

            const int tanh1x = latchedOs ("Tanh", 27.0f, 1.0f, 0.0f, 27.0f);
            const int tanh8x = latchedOs ("Tanh", 27.0f, 1.0f, 3.0f, 27.0f);
            const int idle1x = latchedOs ("Tanh a=0", 27.0f, 0.0f, 0.0f, 27.0f);
            const int latched = latchedOs ("Tanh latch", 27.0f, 1.0f, 0.0f, 33.0f);
            const int crush1x = latchedOs ("Bitcrush", 41.0f, 1.0f, 0.0f, 41.0f);
            std::printf ("        (user 1x + Tanh -> %dx, user 8x + Tanh -> %dx, Tanh at amount 0 -> %dx, mode change mid-note -> %dx, Bitcrush -> %dx)\n",
                         tanh1x, tanh8x, idle1x, latched, crush1x);
            check (tanh1x == 4, "minOS is enforced: user 1x + Tanh (minOS 4) -> the voice runs at 4x");
            check (tanh8x == 8 && crush1x == 1, "enforcement is a max(): user 8x stays 8x; Bitcrush (minOS 1) stays 1x");
            check (idle1x == 1, "no enforcement while the warp amount is 0 (stage inactive)");
            check (latched == 4, "the factor is latched at note-on: a mid-note mode change keeps it");
        }

        // zero allocations with a D3 mode + unison in processBlock
        PluginProcessor p2;
        setParam (p2, "warpMode", 25.0f);
        setParam (p2, "warpAmount", 0.8f);
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
        check (allocations == 0, "processBlock with Hard Clip, unison 4 and the viz feed performs zero allocations");

        for (int mode : { 16, 23, 41, 42, 43 })
        {
            setParam (p2, "warpMode", (float) mode);
            juce::MidiBuffer none;
            p2.processBlock (b2, none);                         // parameter change outside the counted region
            std::size_t n = 1;
            {
                warpr::tests::ScopedAllocCount counter;
                for (int i = 0; i < 100; ++i)
                    p2.processBlock (b2, none);
                n = counter.count();
            }
            allocations += n;
        }
        check (allocations == 0, "processBlock with the stateful modes (Multi Fold, Dynamic Fold, Bitcrush, SR Reduction, Quantization) performs zero allocations");
        p2.releaseResources();
    }
}

JUCE_END_IGNORE_WARNINGS_GCC_LIKE

//==============================================================================
int runPhaseD3Tests (bool full)
{
    failures = 0;
    printedDetails = 0;
    std::printf ("\n=== Phase D3: folds + distortions + quantisers + oversampling validation (%s) ===\n", full ? "full" : "quick");

    testModeTable();
    testPrimitives();
    testPairs();
    testBehaviour();
    testUniversal (full);
    testStereoMono();
    testIdentity();
    testDeterminismAndAllocations();
    testModeSwitch();
    testAliasGates();
    testProcessorLevel();

    return failures;
}
