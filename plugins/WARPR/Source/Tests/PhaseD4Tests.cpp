/*
    WARPR - phase D4 test block (the A/B transfer-curve shaper: curve model, preset library, LUT + antiderivative
    engine, editor backend).

    Called from RunTests.cpp main():   failures += runPhaseD4Tests (full);

    Contents (plan.md T5 + the D4 test criteria)
      1. preset library      9 names == shaperPreset choices, point data bounds, odd / asymmetric flags, spot values
      2. interpolation       linear / smooth through the points; monotone cubic never overshoots (random sets);
                             symmetric mode EXACTLY odd (curve, LUT, bitwise); asymmetric halves are independent
      3. JSON                canonical round trip (parse -> serialise -> parse stable), tolerant of formatting,
                             strict about structure, clamping + sorting like the editor
      4. LUT                 LUT vs direct evaluation < 1e-4; Hermite F1 reconstruction < 1e-6 against an independent
                             exact integral; F1' == f; flat extension; kink metric -> minOS mapping
      5. stage               identity curve is bit-exact, bypass is bit-exact, mix 0 is bit-exact, morph is linear in f,
                             stereo / mono behaviour, determinism
      6. engine              LockFreeSwap publication + 10 ms crossfade bookkeeping (class level)
      7. backend             setShaperCurveJson validation + echo rule (no revision bump), RESET, NORMALIZE, preset
                             listener (bump), user edit -> Custom flip, asymmetric presets switch the symmetry parameter,
                             state load keeps the stored curves
      8. T2 alias gate A1    shaper hard-clip preset: ADAA improvement >= 12 dB at 1x, or the curve-hardness minOS floor
                             (R1(b), enforced at note-on) covers 1x operation; the full alias table is printed
      9. T1-style cube       signals x OS x morph x mix x curve sets through UnisonOsc::render
     10. processor level     minOS enforcement, LUT-crossfade click bound (edit mid-note), identity null, zero
                             allocations during a crossfade, determinism

    Measured numbers (alias table, step bounds) are always printed.
*/

#include "PluginProcessor.h"
#include "DSP/ShaperCurve.h"
#include "DSP/ShaperEngine.h"
#include "DSP/ShaperPresetLibrary.h"
#include "DSP/UnisonOsc.h"
#include "Parameters/ParamTable.h"
#include "Tests/AllocHook.h"
#include "Tests/OfflineRender.h"
#include "Tests/Scanners.h"
#include "Tests/WarpTestKit.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

// Bit-exact float equality is the point of several assertions in this file (identity, bypass, oddness, determinism),
// so the warning is suppressed here only.
JUCE_BEGIN_IGNORE_WARNINGS_GCC_LIKE ("-Wfloat-equal")

int runPhaseD4Tests (bool full);

namespace
{
    using namespace warpr::tests::kit;
    using warpr::ShaperChannelState;
    using warpr::ShaperCurve;
    using warpr::ShaperLutSet;
    using warpr::ShaperPoint;
    using warpr::ShaperSpan;
    namespace presets = warpr::shaperpresets;

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

    float getParam (PluginProcessor& p, const char* id)
    {
        auto* raw = p.apvts.getRawParameterValue (id);
        return raw != nullptr ? raw->load() : -9999.0f;
    }

    ShaperCurve makeCurve (ShaperCurve::Interp interp, std::vector<ShaperPoint> pts)
    {
        ShaperCurve c;
        c.interp = interp;
        c.points = std::move (pts);
        return c;
    }

    /** Random curve: 3..10 points, x from -1 to 1 (min gap 0.03, none within 0.05 of 0).  monotone: y non-decreasing from -1 to 1. */
    ShaperCurve randomCurve (juce::Random& rng, bool monotone, bool linear)
    {
        const int n = 3 + rng.nextInt (8);
        std::vector<double> xs { -1.0, 1.0 };

        while ((int) xs.size() < n)
        {
            const double x = -0.95 + 1.9 * (double) rng.nextDouble();
            bool ok = true;
            for (double v : xs)
                if (std::abs (v - x) < 0.03)
                    ok = false;
            if (std::abs (x) < 0.05)                 // keeps the symmetric half's prepended (0, 0) a sane distance from the next point
                ok = false;
            if (ok)
                xs.push_back (x);
        }

        std::sort (xs.begin(), xs.end());

        ShaperCurve c;
        c.interp = linear ? ShaperCurve::Interp::Linear : ShaperCurve::Interp::Smooth;
        c.points.clear();

        if (monotone)
        {
            std::vector<double> cum { 0.0 };
            for (int i = 1; i < n; ++i)
                cum.push_back (cum.back() + (i == 1 ? 0.2 : (rng.nextInt (6) == 0 ? 0.0 : 0.05 + (double) rng.nextDouble())));

            for (int i = 0; i < n; ++i)
                c.points.push_back ({ xs[(size_t) i], -1.0 + 2.0 * cum[(size_t) i] / cum.back() });
        }
        else
        {
            for (int i = 0; i < n; ++i)
                c.points.push_back ({ xs[(size_t) i], -1.0 + 2.0 * (double) rng.nextDouble() });
        }

        return c;
    }

    /** Independent reference: integral of (f(x) - x) from `from` to `to`, Simpson per polynomial segment
        (exact for cubics), split at every control point. */
    double refIntegral (const ShaperCurve::Evaluator& ev, double from, double to)
    {
        std::vector<double> cuts { from };
        for (double b : ev.breakpoints())
            if (b > from && b < to)
                cuts.push_back (b);
        cuts.push_back (to);

        auto r = [&ev] (double x) { return ev (x) - x; };
        double total = 0.0;

        for (size_t i = 0; i + 1 < cuts.size(); ++i)
        {
            const double a = cuts[i], b = cuts[i + 1];
            // evaluate just inside the segment so a jump on a control point cannot leak in
            const double eps = 1.0e-12 * (b - a);
            total += (b - a) / 6.0 * (r (a + eps) + 4.0 * r (0.5 * (a + b)) + r (b - eps));
        }

        return total;
    }

    //==========================================================================
    // 1. preset library
    //==========================================================================
    void testPresets()
    {
        std::printf ("\n[D4] ShaperPresetLibrary: 9 presets\n");

        int nameBad = 0, shapeBad = 0, boundsBad = 0, oddBad = 0, jsonBad = 0;
        const int expectedPoints[9] = { 9, 4, 11, 11, 13, 9, 9, 9, 3 };

        for (int i = 0; i < presets::kNumPresets; ++i)
        {
            if (std::string (presets::name (i)) != std::string (warpr::choices::shaperPreset[i]))
                ++nameBad;

            const auto c = presets::generate (i);

            if ((int) c.points.size() != expectedPoints[i])
            {
                ++shapeBad;
                DETAIL ("        preset %d (%s): %d points, expected %d\n", i, presets::name (i), (int) c.points.size(), expectedPoints[i]);
            }

            bool sorted = true, inRange = true;
            for (size_t k = 0; k < c.points.size(); ++k)
            {
                if (k > 0 && ! (c.points[k].x > c.points[k - 1].x))
                    sorted = false;
                if (std::abs (c.points[k].x) > 1.0 || std::abs (c.points[k].y) > 1.0)
                    inRange = false;
            }
            if (! sorted || c.points.front().x != -1.0 || c.points.back().x != 1.0)
                ++shapeBad;
            if (! inRange)
                ++boundsBad;

            // odd presets: every (x, y) has a bit-exact (-x, -y) twin
            bool isOdd = true;
            for (const auto& p : c.points)
            {
                bool twin = false;
                for (const auto& q : c.points)
                    if (q.x == -p.x && q.y == -p.y)
                        twin = true;
                isOdd = isOdd && twin;
            }
            if (isOdd == presets::isAsymmetric (i))
            {
                ++oddBad;
                DETAIL ("        preset %d (%s): odd = %d but isAsymmetric = %d\n", i, presets::name (i), (int) isOdd, (int) presets::isAsymmetric (i));
            }

            ShaperCurve back;
            if (! ShaperCurve::fromJson (c.toJson(), back) || back.toJson() != c.toJson())
                ++jsonBad;
        }

        check (nameBad == 0 && presets::kNumPresets == 9, "nine presets, names == the shaperPreset choice list (Custom last)");
        check (shapeBad == 0, "point counts (9, 4, 11, 11, 13, 9, 9, 9, Custom = identity), sorted, x spans -1 .. 1");
        check (boundsBad == 0, "every preset stays inside [-1, 1]");
        check (oddBad == 0, "all presets are exactly odd except Tube and Diode (which are flagged asymmetric)");
        check (jsonBad == 0, "preset curves survive the canonical JSON round trip");
        check (presets::isLoadable (0) && presets::isLoadable (7) && ! presets::isLoadable (presets::kCustomIndex),
               "Custom is the only non-loadable preset");

        auto at = [] (int preset, double x, bool sym) { return presets::generate (preset).eval (x, sym); };
        check (std::abs (at (1, 0.25, false) - 0.5) < 1.0e-12 && at (1, 0.9, false) == 1.0 && at (1, -0.9, false) == -1.0,
               "Hard Clip: slope 2 between +-0.5, flat 1 beyond");
        check (std::abs (at (5, 0.5, true) - std::sin (3.14159265358979 / 4.0)) < 0.01, "Sine: sin(pi x / 2) within 0.01 at x = 0.5");
        check (std::abs (at (0, 0.5, true) - std::tanh (1.25) / std::tanh (2.5)) < 0.02, "Soft Sat: tanh(2.5 x) / tanh(2.5) within 0.02 at x = 0.5");
        check (std::abs (at (7, 1.0, true) - 1.0) < 1.0e-12 && at (7, 0.5, true) < 0.2, "Expo: reaches 1 at x = 1 and stays low below (convex)");
        check (at (4, 0.25, false) == 1.0 && at (4, 0.5, false) == 0.0 && at (4, 0.75, false) == -1.0,
               "Fold: corners (.25, 1) (.5, 0) (.75, -1)");
        check (at (6, 0.25, false) == 1.0 && at (6, 0.75, false) == -1.0 && at (6, -0.25, false) == -1.0,
               "Triangle: zigzag (.25, 1) (.75, -1), odd");
        check (std::abs (at (2, -1.0, false) + 1.0) < 1.0e-12 && at (2, 1.0, false) > 0.45 && at (2, 1.0, false) < 0.6,
               "Tube: asymmetric, -1.0 at the negative end, about +0.53 at the positive end");
        check (std::abs (at (3, 1.0, false) - 1.0) < 1.0e-12 && std::abs (at (3, -1.0, false) + 0.6) < 1.0e-12,
               "Diode: +1 / -0.6 at the ends (asymmetric)");
        check (presets::generate (presets::kCustomIndex) == ShaperCurve::identity(), "Custom generates the identity (and the backend treats it as a no-op)");
    }

    //==========================================================================
    // 2. interpolation, symmetry
    //==========================================================================
    void testInterpolation()
    {
        std::printf("\n[D4] interpolation: smooth monotone cubic / linear, symmetric (odd) and asymmetric modes\n");
        juce::Random rng (4321);

        int throughBad = 0, linBad = 0, overshoot = 0, nonMono = 0, genBad = 0;

        for (int iter = 0; iter < 300; ++iter)
        {
            // ---- monotone data: never overshoots, stays monotone
            const auto m = randomCurve (rng, true, false);
            double prev = -2.0;
            for (size_t i = 0; i + 1 < m.points.size(); ++i)
                for (int k = 0; k <= 32; ++k)
                {
                    const double x = m.points[i].x + (m.points[i + 1].x - m.points[i].x) * (double) k / 32.0;
                    const double y = ShaperCurve::evalPoints (m.points, x, ShaperCurve::Interp::Smooth);
                    if (y < m.points[i].y - 1.0e-12 || y > m.points[i + 1].y + 1.0e-12) ++overshoot;
                    if (y < prev - 1.0e-12) ++nonMono;
                    prev = y;
                }

            // ---- arbitrary data: each segment stays between its own end values (no overshoot per segment)
            const auto g = randomCurve (rng, false, false);
            for (size_t i = 0; i + 1 < g.points.size(); ++i)
                for (int k = 0; k <= 32; ++k)
                {
                    const double x = g.points[i].x + (g.points[i + 1].x - g.points[i].x) * (double) k / 32.0;
                    const double y = ShaperCurve::evalPoints (g.points, x, ShaperCurve::Interp::Smooth);
                    const double lo = std::min (g.points[i].y, g.points[i + 1].y), hi = std::max (g.points[i].y, g.points[i + 1].y);
                    if (y < lo - 1.0e-12 || y > hi + 1.0e-12) ++genBad;
                }

            // ---- both interpolators pass through the control points
            const auto l = randomCurve (rng, false, true);
            for (const auto& p : g.points)
                if (std::abs (ShaperCurve::evalPoints (g.points, p.x, ShaperCurve::Interp::Smooth) - p.y) > 1.0e-12) ++throughBad;
            for (size_t i = 0; i + 1 < l.points.size(); ++i)
            {
                const double mid = 0.5 * (l.points[i].x + l.points[i + 1].x);
                const double expect = 0.5 * (l.points[i].y + l.points[i + 1].y);
                if (std::abs (ShaperCurve::evalPoints (l.points, mid, ShaperCurve::Interp::Linear) - expect) > 1.0e-12) ++linBad;
                if (std::abs (ShaperCurve::evalPoints (l.points, l.points[i].x, ShaperCurve::Interp::Linear) - l.points[i].y) > 1.0e-12) ++linBad;
            }
        }

        check (throughBad == 0, "smooth interpolation passes through every control point");
        check (linBad == 0, "linear interpolation: exact at the points and the midpoints");
        check (overshoot == 0 && nonMono == 0, "monotone cubic NEVER overshoots and stays monotone (300 random monotone point sets)");
        check (genBad == 0, "arbitrary data: no segment leaves the interval of its end values (300 random sets)");

        {
            auto c = makeCurve (ShaperCurve::Interp::Smooth, { { -0.5, 0.2 }, { 0.5, 0.6 } });
            check (c.eval (-0.9, false) == 0.2 && c.eval (0.9, false) == 0.6, "outside the first / last point the curve is held flat (like the editor)");
        }

        // ---- symmetric: exactly odd
        int oddCurve = 0, oddLut = 0;
        for (int iter = 0; iter < 100; ++iter)
        {
            auto c = randomCurve (rng, false, (iter & 1) != 0);
            const ShaperCurve::Evaluator ev (c, true);
            for (int k = 0; k < 400; ++k)
            {
                const double x = 2.0 * (double) rng.nextDouble() - 1.0;
                if (ev (x) != -ev (-x))
                    ++oddCurve;
            }

            auto set = ShaperLutSet::build (c, c, true);
            const auto& r = set->remainder (0);
            const auto& g = set->antiderivative (0);
            for (int i = 0; i < ShaperLutSet::kCells; ++i)
            {
                if (i != ShaperLutSet::kCells / 2 && r[(size_t) i] != -r[(size_t) (ShaperLutSet::kCells - i)]) ++oddLut;
                if (g[(size_t) i] != g[(size_t) (ShaperLutSet::kCells - i)]) ++oddLut;
            }
            for (int k = 0; k < 200; ++k)
            {
                const double x = 2.0 * (double) rng.nextDouble() - 1.0;
                if (set->fAt (0, x) != -set->fAt (0, -x)) ++oddLut;
                if (set->gAt (0, x) != set->gAt (0, -x)) ++oddLut;
            }
        }
        check (oddCurve == 0, "symmetric mode: the curve is EXACTLY odd, f(-x) == -f(x) bitwise (100 random curves)");
        check (oddLut == 0, "symmetric LUT: r odd and g even bitwise, fAt exactly odd, gAt exactly even");

        // ---- symmetric mode ignores the negative half entirely
        {
            auto p1 = makeCurve (ShaperCurve::Interp::Smooth, { { -1.0, -1.0 }, { -0.5, -0.3 }, { 0.0, 0.0 }, { 0.4, 0.7 }, { 1.0, 1.0 } });
            auto p2 = makeCurve (ShaperCurve::Interp::Smooth, { { -1.0, -0.2 }, { -0.5, 0.9 }, { 0.0, 0.0 }, { 0.4, 0.7 }, { 1.0, 1.0 } });
            auto a = ShaperLutSet::build (p1, p1, true), b = ShaperLutSet::build (p2, p2, true);
            check (std::memcmp (a->remainder (0).data(), b->remainder (0).data(), sizeof (double) * ShaperLutSet::kPoints) == 0
                   && std::memcmp (a->antiderivative (0).data(), b->antiderivative (0).data(), sizeof (double) * ShaperLutSet::kPoints) == 0,
                   "symmetric mode: the negative half of the point list has no influence (tables identical bit for bit)");

            // asymmetric: the halves are independent (smooth: only the segment next to x = 0 sees the other side's slope)
            auto sa = ShaperLutSet::build (p1, p1, false), sb = ShaperLutSet::build (p2, p2, false);
            int rightDiff = 0, leftDiff = 0;
            for (int i = 0; i <= ShaperLutSet::kCells; ++i)
            {
                const double x = (double) (i - ShaperLutSet::kCells / 2) * ShaperLutSet::kCell;
                const bool differs = sa->remainder (0)[(size_t) i] != sb->remainder (0)[(size_t) i];
                if (x > 0.4 && differs) ++rightDiff;
                if (x < -0.05 && differs) ++leftDiff;
            }
            check (rightDiff == 0 && leftDiff > 100, "asymmetric mode: editing the negative half leaves the positive half beyond the neighbouring segment untouched");
            check (std::abs (sa->fAt (0, 0.5) + sa->fAt (0, -0.5)) > 0.05, "asymmetric mode: f(-x) != -f(x) for an asymmetric point set");

            auto l1 = makeCurve (ShaperCurve::Interp::Linear, { { -1.0, -1.0 }, { -0.5, -0.3 }, { 0.0, 0.0 }, { 0.5, 0.7 }, { 1.0, 1.0 } });
            auto l2 = makeCurve (ShaperCurve::Interp::Linear, { { -1.0, -0.2 }, { -0.5, 0.9 }, { 0.0, 0.0 }, { 0.5, 0.7 }, { 1.0, 1.0 } });
            auto la = ShaperLutSet::build (l1, l1, false), lb = ShaperLutSet::build (l2, l2, false);
            int linRight = 0;
            for (int i = ShaperLutSet::kCells / 2; i <= ShaperLutSet::kCells; ++i)
                if (la->remainder (0)[(size_t) i] != lb->remainder (0)[(size_t) i]) ++linRight;
            check (linRight == 0, "asymmetric + linear: the positive half is completely independent of the negative half");
        }
    }

    //==========================================================================
    // 3. JSON
    //==========================================================================
    void testJson()
    {
        std::printf ("\n[D4] curve JSON: canonical round trip, tolerance and rejection\n");
        juce::Random rng (777);

        int unstable = 0, drift = 0;
        for (int iter = 0; iter < 300; ++iter)
        {
            auto c = randomCurve (rng, (iter % 3) == 0, (iter & 1) != 0);
            for (auto& p : c.points)
                p.y = std::round (p.y * 1.0e7) * 1.0e-7 + 1.234567891234e-9 * (double) rng.nextDouble();   // many digits

            const auto s1 = c.toJson();
            ShaperCurve c2, c3;
            if (! ShaperCurve::fromJson (s1, c2)) { ++unstable; continue; }
            const auto s2 = c2.toJson();
            if (! ShaperCurve::fromJson (s2, c3)) { ++unstable; continue; }
            const auto s3 = c3.toJson();

            if (s2 != s3 || ! (c2 == c3))
                ++unstable;
            for (size_t i = 0; i < c.points.size(); ++i)
                if (std::abs (c.points[i].y - c2.points[i].y) > 1.0e-8 || std::abs (c.points[i].x - c2.points[i].x) > 1.0e-8)
                    ++drift;
        }
        check (unstable == 0, "parse -> serialise -> parse is stable: the canonical text re-serialises to itself (300 random curves)");
        check (drift == 0, "%.9g round trip stays within 1e-8 of the source values");
        check (ShaperCurve::identity().toJson() == warpr::UiModel::defaultShaperCurveJson(),
               "the canonical identity text IS the model's default document");

        ShaperCurve c;
        const bool ok = ShaperCurve::fromJson (R"( { "points" : [ {"y": 1, "x": 1}, {"x": -1, "y": -1}, {"x":0.25,"y":0.125} ] , "interp" : "linear" } )", c);
        check (ok && c.toJson() == R"({"interp":"linear","points":[{"x":-1,"y":-1},{"x":0.25,"y":0.125},{"x":1,"y":1}]})",
               "tolerant of whitespace, key order and integer values; points are sorted; canonical text is fixed");

        ShaperCurve clamped;
        check (ShaperCurve::fromJson (R"({"interp":"smooth","points":[{"x":-5,"y":0},{"x":5,"y":3}]})", clamped)
               && clamped.toJson() == R"({"interp":"smooth","points":[{"x":-1,"y":0},{"x":1,"y":1}]})",
               "coordinates are clamped to [-1, 1]");

        ShaperCurve noInterp;
        check (ShaperCurve::fromJson (R"({"points":[{"x":-1,"y":-1},{"x":1,"y":1}]})", noInterp) && noInterp.interp == ShaperCurve::Interp::Smooth,
               "a missing \"interp\" means smooth");

        std::string sixtyFive = R"({"interp":"smooth","points":[)";
        for (int i = 0; i < 65; ++i)
            sixtyFive += std::string (i ? "," : "") + "{\"x\":" + std::to_string (-1.0 + 2.0 * i / 64.0) + ",\"y\":0}";
        sixtyFive += "]}";

        const juce::String bad[] = {
            "", "not json", "[]", "{}", R"({"interp":"smooth"})", R"({"interp":"smooth","points":[]})",
            R"({"interp":"smooth","points":[{"x":0,"y":0}]})", R"({"interp":"cubic","points":[{"x":-1,"y":-1},{"x":1,"y":1}]})",
            R"({"interp":5,"points":[{"x":-1,"y":-1},{"x":1,"y":1}]})",
            R"({"interp":"smooth","points":[{"x":"a","y":0},{"x":1,"y":1}]})", R"({"interp":"smooth","points":[1,2]})",
            R"({"interp":"smooth","points":{"x":0}})", R"({"interp":"smooth","points":[{"x":0},{"x":1,"y":1}]})",
            juce::String (sixtyFive)
        };

        int accepted = 0;
        ShaperCurve untouched = ShaperCurve::identity();
        for (const auto& b : bad)
        {
            ShaperCurve out = untouched;
            if (ShaperCurve::fromJson (b, out) || ! (out == untouched))
            {
                ++accepted;
                DETAIL ("        accepted / modified by a bad document: %s\n", b.substring (0, 60).toRawUTF8());
            }
        }
        check (accepted == 0, "malformed or out-of-contract documents are rejected and leave the target untouched (14 documents)");
    }

    //==========================================================================
    // 4. LUT accuracy
    //==========================================================================
    struct NamedSet
    {
        std::string name;
        ShaperCurve curve;
        bool sym;
    };

    std::vector<NamedSet> allTestSets (juce::Random& rng, int nRandom)
    {
        std::vector<NamedSet> sets;
        for (int i = 0; i < 8; ++i)
            sets.push_back ({ presets::name (i), presets::generate (i), ! presets::isAsymmetric (i) });
        for (int i : { 2, 3 })
            sets.push_back ({ std::string (presets::name (i)) + " (sym)", presets::generate (i), true });
        for (int i = 0; i < nRandom; ++i)
        {
            sets.push_back ({ "random smooth monotone " + std::to_string (i), randomCurve (rng, true, false), (i & 1) == 0 });
            sets.push_back ({ "random smooth " + std::to_string (i), randomCurve (rng, false, false), (i & 1) != 0 });
        }
        return sets;
    }

    void testLut (bool fullRun)
    {
        std::printf ("\n[D4] LUT: reconstruction vs direct evaluation, Hermite F1 vs exact integral, kink metric\n");
        juce::Random rng (99);
        const auto sets = allTestSets (rng, fullRun ? 40 : 14);

        double worstF = 0.0, worstG = 0.0, worstD = 0.0;
        int fBad = 0, gBad = 0, dBad = 0;

        for (const auto& s : sets)
        {
            const auto lut = ShaperLutSet::build (s.curve, s.curve, s.sym);
            const ShaperCurve::Evaluator ev (s.curve, s.sym);

            for (int k = 0; k < 3000; ++k)
            {
                const double x = 2.0 * (double) rng.nextDouble() - 1.0;
                const double e = std::abs (lut->fAt (0, x) - ev (x));
                worstF = std::max (worstF, e);
                if (e >= 1.0e-4) { ++fBad; DETAIL ("        LUT error %.3g at x=%.4f (%s)\n", e, x, s.name.c_str()); }
            }

            for (int k = 0; k < 24; ++k)
            {
                const double x = 2.0 * (double) rng.nextDouble() - 1.0;
                const double ref = s.sym ? refIntegral (ev, 0.0, std::abs (x)) : refIntegral (ev, -1.0, x);
                const double e = std::abs (lut->gAt (0, x) - ref);
                worstG = std::max (worstG, e);
                if (e >= 1.0e-6) { ++gBad; DETAIL ("        F1 error %.3g at x=%.4f (%s)\n", e, x, s.name.c_str()); }
            }

            // F1' == f_lut (the pair is exactly consistent)
            for (int k = 0; k < 40; ++k)
            {
                const double x = 1.8 * (double) rng.nextDouble() - 0.9;
                constexpr double h = 1.0e-6;
                const double d = std::abs ((lut->gAt (0, x + h) - lut->gAt (0, x - h)) / (2.0 * h) - lut->rAt (0, x));
                worstD = std::max (worstD, d);
                if (d > 1.0e-5) ++dBad;
            }
        }

        std::printf ("        (%d curves: worst LUT error %.3g, worst F1 error %.3g, worst |F1' - f| %.3g)\n",
                     (int) sets.size(), worstF, worstG, worstD);
        check (fBad == 0, "LUT vs direct evaluation < 1e-4 (8 presets, both modes, random smooth sets)");
        check (gBad == 0, "Hermite F1 reconstruction < 1e-6 against an independent exact (Simpson per segment) integral");
        check (dBad == 0, "F1' == f_lut: the ADAA kernel and the LUT are one consistent pair");

        // ---- flat extension + remainder form
        {
            auto hc = presets::generate (1);
            auto set = ShaperLutSet::build (hc, hc, true);
            check (set->fAt (0, 1.7) == 1.0 && set->fAt (0, -3.0) == -1.0 && std::abs (set->gAt (0, 2.0) - set->gAt (0, 1.0) - (1.0 * 1.0 - 0.5 * 3.0)) < 1.0e-12,
                   "beyond +-1 the curve is held flat and G extends analytically");

            auto id = ShaperLutSet::build (ShaperCurve::identity(), ShaperCurve::identity(), false);
            bool zero = true;
            for (int i = 0; i < ShaperLutSet::kPoints; ++i)
                zero = zero && id->remainder (0)[(size_t) i] == 0.0 && id->antiderivative (0)[(size_t) i] == 0.0;
            check (zero, "the identity curve stores r == 0 and g == 0 exactly (the remainder form behind the bit-exact null)");
        }

        // ---- hardness metric
        {
            auto hc = ShaperLutSet::build (presets::generate (1), presets::generate (1), true);
            auto id = ShaperLutSet::build (ShaperCurve::identity(), ShaperCurve::identity(), true);
            auto fold = ShaperLutSet::build (presets::generate (4), presets::generate (6), true);
            check (std::abs (hc->kink (0) - 2.0) < 1.0e-6, "kink metric of the Hard Clip preset == 2 (its slope jump)");
            check (id->kink (0) == 0.0 && id->minOs (0) == 1, "identity: kink 0, minOS 1");
            check (hc->minOs (0) == 4 && fold->minOs (0) == 4 && fold->minOs (1) == 4, "Hard Clip, Fold and Triangle are hard-clip-like: minOS 4");

            int smoothBad = 0;
            for (int i : { 0, 5, 7 })
            {
                auto s = ShaperLutSet::build (presets::generate (i), presets::generate (i), true);
                std::printf ("        kink(%-8s) = %.4f  -> minOS %d\n", presets::name (i), s->kink (0), s->minOs (0));
                if (s->minOs (0) != 1) ++smoothBad;
            }
            for (int i : { 2, 3 })
            {
                auto s = ShaperLutSet::build (presets::generate (i), presets::generate (i), false);
                std::printf ("        kink(%-8s) = %.4f  -> minOS %d\n", presets::name (i), s->kink (0), s->minOs (0));
                if (s->minOs (0) > 2) ++smoothBad;
            }
            check (smoothBad == 0, "smooth presets (Soft Sat, Sine, Expo) need no floor; Tube / Diode need at most 2");
            check (ShaperLutSet::minOsFromKink (0.3) == 4 && ShaperLutSet::minOsFromKink (0.1) == 2 && ShaperLutSet::minOsFromKink (0.005) == 1
                   && hc->minOsFor (0.0f) == 4 && fold->minOsFor (0.5f) == 4,
                   "kink -> minOS mapping (>= 0.25: 4, >= 0.02: 2, else 1); morph picks the curve(s) in use");
            auto mixed = ShaperLutSet::build (presets::generate (1), ShaperCurve::identity(), true);
            check (mixed->minOsFor (0.0f) == 4 && mixed->minOsFor (1.0f) == 1 && mixed->minOsFor (0.5f) == 4,
                   "A hard / B identity: morph 0 -> 4, morph 1 -> 1, in between -> 4");
        }
    }

    //==========================================================================
    // 5. the stage
    //==========================================================================
    /** One-oscillator renderer with a shaper span (and no warp stage). */
    template <class SpanFn>
    OscRun renderOscShaper (const warpr::SampleData& sd, double hz, double fs, int os, int totalBase, SpanFn&& spanFn)
    {
        warpr::RegionFold rg;
        rg.configure (sd, 0.0f, 1.0f, warpr::RegionFold::Mode::Forward);

        const double lc = sd.fileSampleRate / rootHz();
        warpr::UnisonOsc osc;
        osc.start (rg, false, 0, lc);

        const double rho = hz / rootHz() * sd.fileSampleRate / fs;
        constexpr int chunk = 32;
        const int total = (totalBase / chunk) * chunk;

        OscRun out;
        out.L.assign (ix (total) * ix (os), 0.0f);
        out.R.assign (ix (total) * ix (os), 0.0f);

        int ci = 0;
        for (int i = 0; i < total; i += chunk, ++ci)
        {
            ShaperSpan sp;
            const bool use = spanFn (ci, sp);

            osc.render (sd, rg, 0, rho, rho, os, 1.0f, 1.0f, 1.0f, 1.0f,
                        out.L.data() + ix (i) * ix (os), out.R.data() + ix (i) * ix (os), chunk * os,
                        nullptr, use ? &sp : nullptr);
        }

        return out;
    }

    auto constShaper (const ShaperLutSet* set, float morph, float mix)
    {
        return [=] (int, ShaperSpan& s)
        {
            s.active = true;
            s.cur = set;
            s.morph0 = s.morph1 = morph;
            s.mix0 = s.mix1 = mix;
            return true;
        };
    }

    auto noShaper()
    {
        return [] (int, ShaperSpan&) { return false; };
    }

    void testStage()
    {
        std::printf ("\n[D4] shaper stage: identity null, bypass, mix 0, morph linearity, stereo / mono, determinism\n");
        constexpr double fs = 48000.0;
        juce::Random rng (2468);

        // ---- identity curves: bit-exact for inputs inside [-1, 1]
        {
            int bad = 0;
            for (bool sym : { true, false })
            {
                auto id = ShaperLutSet::build (ShaperCurve::identity(), ShaperCurve::identity(), sym);
                ShaperSpan sp;
                sp.active = true;
                sp.cur = id.get();

                warpr::ShaperOscState st;
                for (float mix : { 1.0f, 0.6f })
                    for (double morph : { 0.0, 0.37, 1.0 })
                        for (int k = 0; k < 2000; ++k)
                        {
                            float l = rng.nextFloat() * 2.0f - 1.0f, r = rng.nextFloat() * 2.0f - 1.0f;
                            const float l0 = l, r0 = r;
                            st.process (sp, morph, (double) mix, 1.0, l, r, true);
                            if (l != l0 || r != r0) ++bad;
                        }
            }
            check (bad == 0, "identity curves: the stage returns its input BIT-EXACTLY (symmetric + asymmetric, morph 0 / 0.37 / 1, mix 1 / 0.6)");

            auto id = ShaperLutSet::build (ShaperCurve::identity(), ShaperCurve::identity(), true);
            ShaperSpan sp;
            sp.active = true;
            sp.cur = id.get();
            warpr::ShaperOscState st;
            float l = 1.5f, r = 1.5f;
            st.process (sp, 0.0, 1.0, 1.0, l, r, true);
            st.process (sp, 0.0, 1.0, 1.0, l = 1.5f, r = 1.5f, true);
            check (l == 1.0f && r == 1.0f, "an overshooting input (1.5) is held at the curve end value (1.0): the clamp is part of the curve");
        }

        auto saw = warpr::SampleData::createDefaultWave();
        auto sine = makeCycle (2048, 0.5f);
        auto hardSet = ShaperLutSet::build (presets::generate (1), presets::generate (4), true);
        auto idSet = ShaperLutSet::build (ShaperCurve::identity(), ShaperCurve::identity(), true);

        {
            int bypassBad = 0, idBad = 0, mixBad = 0;
            for (const auto* sd : { sine.get(), saw.get() })
                for (int os : { 1, 2, 4 })
                {
                    const auto base = renderOscShaper (*sd, 220.0, fs, os, 3200, noShaper());

                    const auto inactive = renderOscShaper (*sd, 220.0, fs, os, 3200, [&] (int, ShaperSpan& s) { s.active = false; s.cur = hardSet.get(); return true; });
                    if (! bitEqual (base.L, inactive.L) || ! bitEqual (base.R, inactive.R)) ++bypassBad;

                    const auto ident = renderOscShaper (*sd, 220.0, fs, os, 3200, constShaper (idSet.get(), 0.4f, 1.0f));
                    if (! bitEqual (base.L, ident.L) || ! bitEqual (base.R, ident.R)) ++idBad;

                    const auto mix0 = renderOscShaper (*sd, 220.0, fs, os, 3200, constShaper (hardSet.get(), 0.3f, 0.0f));
                    if (! bitEqual (base.L, mix0.L) || ! bitEqual (base.R, mix0.R)) ++mixBad;
                }
            check (bypassBad == 0, "bypass (span inactive) is bit-exact: the oscillator output equals the no-shaper render");
            check (idBad == 0, "identity curve through UnisonOsc::render: bit-exact against the no-shaper render (null = -inf dB)");
            check (mixBad == 0, "mix = 0 with a hard curve: bit-exact dry");

            const auto sh = renderOscShaper (*saw, 220.0, fs, 2, 3200, constShaper (hardSet.get(), 0.0f, 1.0f));
            const auto base = renderOscShaper (*saw, 220.0, fs, 2, 3200, noShaper());
            check (! bitEqual (base.L, sh.L) && allFinite (sh.L), "sanity: an active hard curve does change the signal");
        }

        // ---- morph is linear in f (and in the antiderivative / the ADAA output)
        {
            auto A = presets::generate (5), B = presets::generate (7);
            auto set = ShaperLutSet::build (A, B, true);
            double worstF = 0.0, worstG = 0.0, worstY = 0.0;
            bool endpoints = true;

            for (double m : { 0.1, 0.25, 0.5, 0.75, 0.9 })
            {
                for (int k = 0; k < 200; ++k)
                {
                    const double x = 2.4 * (double) rng.nextDouble() - 1.2;
                    worstF = std::max (worstF, std::abs (set->fBlend (m, x) - ((1.0 - m) * set->fAt (0, x) + m * set->fAt (1, x))));
                    worstG = std::max (worstG, std::abs (set->gBlend (m, x) - ((1.0 - m) * set->gAt (0, x) + m * set->gAt (1, x))));
                }

                ShaperSpan sp;
                sp.active = true;
                sp.cur = set.get();
                ShaperChannelState sA, sB, sM;
                double x = 0.0;
                for (int k = 0; k < 500; ++k)
                {
                    x = juce::jlimit (-1.2, 1.2, x + 0.4 * ((double) rng.nextDouble() - 0.5));
                    const double yA = sA.step (sp, 0.0, 1.0, x), yB = sB.step (sp, 1.0, 1.0, x), yM = sM.step (sp, m, 1.0, x);
                    worstY = std::max (worstY, std::abs (yM - ((1.0 - m) * yA + m * yB)));
                }
            }

            for (int k = 0; k < 100; ++k)
            {
                const double x = 2.0 * (double) rng.nextDouble() - 1.0;
                endpoints = endpoints && set->fBlend (0.0, x) == set->fAt (0, x) && set->fBlend (1.0, x) == set->fAt (1, x);
            }

            std::printf ("        (worst morph deviation: f %.3g, F1 %.3g, ADAA output %.3g)\n", worstF, worstG, worstY);
            check (worstF < 1.0e-13 && worstG < 1.0e-13, "morph is linear in f and in F1: y = (1-m) A + m B (Sine -> Expo)");
            check (worstY < 1.0e-9, "morph is linear at the ADAA output level (same input stream, blended kernels; 1e-9 allows for the 1/dx amplified rounding)");
            check (endpoints, "morph 0 / 1 read exactly curve A / B");
        }

        // ---- stereo / mono
        {
            auto mono = makeCycle (2048, 0.5f);
            auto same = makeStereoCycle (2048, true);
            auto diff = makeStereoCycle (2048, false);
            const auto m = renderOscShaper (*mono, 110.0, fs, 2, 4800, constShaper (hardSet.get(), 0.5f, 0.7f));
            const auto s = renderOscShaper (*same, 110.0, fs, 2, 4800, constShaper (hardSet.get(), 0.5f, 0.7f));
            const auto d = renderOscShaper (*diff, 110.0, fs, 2, 4800, constShaper (hardSet.get(), 0.5f, 0.7f));
            check (bitEqual (m.L, m.R), "mono source: L == R exactly through the shaper");
            check (bitEqual (s.L, s.R), "identical stereo channels stay bit-identical (per-channel ADAA state is symmetric)");
            check (! bitEqual (d.L, d.R) && allFinite (d.L) && allFinite (d.R), "different channels stay independent and finite");
        }

        // ---- determinism
        {
            const auto a = renderOscShaper (*saw, 110.0, fs, 4, 6400, constShaper (hardSet.get(), 0.5f, 0.8f));
            const auto b = renderOscShaper (*saw, 110.0, fs, 4, 6400, constShaper (hardSet.get(), 0.5f, 0.8f));
            check (bitEqual (a.L, b.L) && bitEqual (a.R, b.R), "two identical renders are bit-equal");
        }
    }

    //==========================================================================
    // 6. engine bookkeeping (class level)
    //==========================================================================
    void testEngine()
    {
        std::printf ("\n[D4] ShaperEngine: publication, 10 ms crossfade bookkeeping, retire-list hand-back\n");
        warpr::RetireList retire;
        warpr::ShaperEngine engine (retire);
        engine.prepare (48000.0);

        check (engine.current() == nullptr && ! engine.isFading() && engine.minOsFor (0.0f) == 1, "before the first publication there is no set (stage inactive, minOS 1)");

        auto s1 = ShaperLutSet::build (ShaperCurve::identity(), ShaperCurve::identity(), true);
        auto s2 = ShaperLutSet::build (presets::generate (1), presets::generate (1), true);
        auto s3 = ShaperLutSet::build (presets::generate (5), presets::generate (5), true);

        engine.publish (s1);
        engine.beginBlock();
        check (engine.current() == s1.get() && ! engine.isFading(), "the first set is adopted without a fade");

        engine.publish (s2);
        engine.beginBlock();
        check (engine.current() == s2.get() && engine.previous() == s1.get() && engine.isFading() && engine.fadeBase() == 0 && engine.fadeLength() == 480,
               "a replaced set is kept for a 480-sample (10 ms @ 48 kHz) crossfade");

        engine.publish (s3);                              // arrives mid-fade
        engine.advance (240);
        engine.beginBlock();
        check (engine.current() == s2.get() && engine.isFading() && engine.fadeBase() == 240, "a publication during a fade waits for it to finish");

        engine.advance (240);
        check (! engine.isFading() && engine.previous() == nullptr, "the fade ends after exactly fadeLength samples");
        check (retire.numPending() == 2, "both the swap and the engine hand their reference of the old set to the retire list (never freed on the audio thread)");

        engine.beginBlock();
        check (engine.current() == s3.get() && engine.isFading() && engine.previous() == s2.get(), "then the waiting set is adopted (a new fade starts)");
        check (engine.minOsFor (0.0f) == s3->minOsFor (0.0f), "minOsFor reads the current set");

        engine.advance (480);
        engine.beginBlock();
        retire.drain();
        check (retire.numPending() == 0, "the retire list drains on the message thread");
        check (engine.latest() == s3, "latest() is the most recent publication (message thread view)");
    }

    //==========================================================================
    // 7. backend
    //==========================================================================
    void testBackendStandalone()
    {
        std::printf ("\n[D4] backend (standalone): validation, echo rule, RESET, NORMALIZE\n");
        UiBackendImpl be;
        const auto rev = [&be] { return be.revision (WarprUiBackend::Revision::shaperCurves); };

        const juce::String good = R"({"interp":"smooth","points":[{"x":-1,"y":-0.5},{"x":0,"y":0},{"x":0.5,"y":0.3},{"x":1,"y":0.4}]})";
        const auto before = rev();
        be.setShaperCurveJson ("A", good);
        check (rev() == before, "setShaperCurveJson does NOT bump Revision::shaperCurves (echo avoidance)");
        check (be.getShaperCurveJson ("A") == good, "getShaperCurveJson returns the canonical text of the edit");
        check (be.getShaperCurveJson ("B") == warpr::UiModel::defaultShaperCurveJson() && be.getShaperCurveJson ("C") == warpr::UiModel::defaultShaperCurveJson(),
               "the untouched curve is the identity document; an unknown id reads as the identity");

        be.setShaperCurveJson ("A", R"( {"points":[ {"y":-0.5,"x":-1}, {"x":0,"y":0}, {"x":0.5,"y":0.3}, {"x":1,"y":0.4} ], "interp":"smooth"} )");
        check (be.getShaperCurveJson ("A") == good, "a differently formatted but equal document canonicalises to the same text");

        be.setShaperCurveJson ("A", "{}");
        be.setShaperCurveJson ("A", R"({"interp":"smooth","points":[{"x":0,"y":0}]})");
        be.setShaperCurveJson ("A", "garbage");
        be.setShaperCurveJson ("Z", good);
        check (be.getShaperCurveJson ("A") == good && rev() == before, "invalid documents / unknown curve ids change nothing and bump nothing");

        auto lut = be.getShaperLut();
        check (lut != nullptr && lut->isOdd() && std::abs (lut->fAt (0, 0.5) - 0.3) < 1.0e-4, "the edit was built into the LUT set (symmetric by default)");

        // ---- NORMALIZE
        be.shaperNormalize ("A");
        {
            ShaperCurve c;
            const bool ok = ShaperCurve::fromJson (be.getShaperCurveJson ("A"), c);
            const auto set = ShaperLutSet::build (c, c, true);
            check (ok && rev() == before + 1, "NORMALIZE bumps Revision::shaperCurves once");
            std::printf ("        (peak after NORMALIZE: curve %.9f, LUT %.9f)\n", set->peak (0), be.getShaperLut()->peak (0));
            check (std::abs (set->peak (0) - 1.0) < 1.0e-6 && std::abs (be.getShaperLut()->peak (0) - 1.0) < 1.0e-6,
                   "NORMALIZE: max |y| of the effective (symmetric) curve is 1 (the x >= 0 half peaked at 0.4)");
            check (c.points.back().y > 0.99999 && c.points.front().y == -1.0, "NORMALIZE scaled the control points (-0.5 -> clamped -1, 0.4 -> 1)");
        }

        const juce::String flat = R"({"interp":"linear","points":[{"x":-1,"y":0},{"x":1,"y":0}]})";
        be.setShaperCurveJson ("B", flat);
        const auto beforeFlat = rev();
        be.shaperNormalize ("B");
        check (be.getShaperCurveJson ("B") == flat && rev() == beforeFlat, "NORMALIZE of a flat zero curve changes nothing (no bump)");

        // ---- RESET
        be.setShaperCurveJson ("B", good);
        const auto beforeReset = rev();
        be.shaperReset ("B");
        bool zero = true;
        auto set = be.getShaperLut();
        for (int i = 0; i < ShaperLutSet::kPoints; ++i)
            zero = zero && set->remainder (1)[(size_t) i] == 0.0 && set->antiderivative (1)[(size_t) i] == 0.0;
        check (be.getShaperCurveJson ("B") == warpr::UiModel::defaultShaperCurveJson() && rev() == beforeReset + 1, "RESET restores the identity document and bumps the revision once");
        check (zero, "RESET: the rebuilt curve is the exact identity (r == 0, g == 0)");
        be.shaperReset ("Q");
        check (rev() == beforeReset + 1, "RESET of an unknown curve is ignored");

        // ---- state adoption canonicalises / falls back
        warpr::UiModel m;
        m.shaperCurveJson[0] = R"({ "interp":"linear", "points":[{"x":-1,"y":-1},{"x":1,"y":1.5}] })";
        m.shaperCurveJson[1] = "not a curve";
        be.adoptModel (m);
        check (be.getShaperCurveJson ("A") == R"({"interp":"linear","points":[{"x":-1,"y":-1},{"x":1,"y":1}]})"
               && be.getShaperCurveJson ("B") == warpr::UiModel::defaultShaperCurveJson(),
               "adoptModel: a stored curve is re-parsed (canonical, clamped), an invalid one falls back to the identity");
    }

    void testBackendWithProcessor()
    {
        std::printf ("\n[D4] backend + APVTS: preset listener, Custom flip, symmetry switch, state load\n");
        constexpr double fs = 48000.0;
        PluginProcessor p;
        auto& be = p.getBackendImpl();
        const auto rev = [&be] { return be.revision (WarprUiBackend::Revision::shaperCurves); };

        check (getParam (p, "shaperPreset") == 8.0f, "shaperPreset defaults to Custom");

        // ---- preset change -> loads + bumps; applied via servicePending (message thread)
        auto r0 = rev();
        setParam (p, "shaperPreset", 1.0f);                 // Hard Clip
        check (rev() == r0, "the listener itself only flags work (no bump on the host / audio thread)");
        be.servicePending();
        check (rev() == r0 + 1, "a preset change bumps Revision::shaperCurves (the editor must re-emit)");
        const auto hardJson = presets::generate (1).toJson();
        check (be.getShaperCurveJson ("A") == hardJson, "the Hard Clip preset was loaded into the selected curve (A)");
        check (be.getShaperCurveJson ("B") == warpr::UiModel::defaultShaperCurveJson(), "the other curve is untouched");
        check (getParam (p, "shaperPreset") == 1.0f, "the preset parameter keeps its value after loading");
        be.servicePending();
        check (rev() == r0 + 1, "servicePending is idempotent");

        // ---- user edit -> Custom, no bump
        const juce::String edit = R"({"interp":"linear","points":[{"x":-1,"y":-1},{"x":0.3,"y":0.2},{"x":1,"y":1}]})";
        be.setShaperCurveJson ("A", edit);
        check (getParam (p, "shaperPreset") == 8.0f, "a UI edit flips shaperPreset to Custom");
        check (rev() == r0 + 1, "... without bumping Revision::shaperCurves (echo avoidance)");
        be.servicePending();
        ShaperCurve editCurve;
        ShaperCurve::fromJson (edit, editCurve);
        check (be.getShaperCurveJson ("A") == editCurve.toJson() && rev() == r0 + 1,
               "the Custom flip does not reload anything (the edited curve survives servicePending)");

        // ---- curve select B + Sine preset
        setParam (p, "shaperCurveSelect", 1.0f);
        setParam (p, "shaperPreset", 5.0f);                 // Sine
        be.servicePending();
        check (be.getShaperCurveJson ("B") == presets::generate (5).toJson() && be.getShaperCurveJson ("A") != presets::generate (5).toJson()
               && rev() == r0 + 2, "with curve B selected a preset loads into B and leaves A alone");

        // ---- asymmetric preset flips the symmetry parameter, one rebuild, one bump
        setParam (p, "shaperCurveSelect", 0.0f);
        setParam (p, "shaperPreset", 2.0f);                 // Tube
        be.servicePending();
        check (getParam (p, "shaperSymmetry") == 1.0f && be.getShaperLut() != nullptr && ! be.getShaperLut()->isOdd() && rev() == r0 + 3,
               "an asymmetric preset (Tube) switches shaperSymmetry to Asymmetric, rebuilds once, bumps once");

        setParam (p, "shaperSymmetry", 0.0f);
        const auto rSym = rev();
        be.servicePending();
        check (be.getShaperLut()->isOdd() && rev() == rSym, "a symmetry change rebuilds the LUT set (odd again) without touching the revision");

        setParam (p, "shaperPreset", 8.0f);                 // Custom: no-op
        const auto jsonA = be.getShaperCurveJson ("A");
        be.servicePending();
        check (be.getShaperCurveJson ("A") == jsonA && rev() == rSym, "selecting Custom changes nothing");

        // ---- NORMALIZE / RESET flip to Custom, asymmetric normalise uses the whole curve
        setParam (p, "shaperSymmetry", 1.0f);
        be.servicePending();
        be.setShaperCurveJson ("A", R"({"interp":"smooth","points":[{"x":-1,"y":-0.8},{"x":0,"y":0},{"x":1,"y":0.3}]})");
        setParam (p, "shaperPreset", 4.0f);                 // a preset value is active (not yet serviced) ...
        be.shaperNormalize ("A");                           // ... NORMALIZE makes the curve user-defined again
        {
            ShaperCurve c;
            ShaperCurve::fromJson (be.getShaperCurveJson ("A"), c);
            check (c.points.front().y == -1.0 && std::abs (c.points.back().y - 0.375) < 1.0e-8 && getParam (p, "shaperPreset") == 8.0f,
                   "asymmetric NORMALIZE scales by the largest |y| on EITHER side (-0.8 -> -1, 0.3 -> 0.375) and flips to Custom");
        }
        setParam (p, "shaperPreset", 3.0f);
        be.shaperReset ("A");
        be.servicePending();
        check (getParam (p, "shaperPreset") == 8.0f && be.getShaperCurveJson ("A") == warpr::UiModel::defaultShaperCurveJson(),
               "RESET also flips the preset to Custom (a pending Diode preset does not reload over it)");

        // ---- engine publication reaches the audio thread's view
        p.setPlayConfigDetails (0, 2, fs, 256);
        p.prepareToPlay (fs, 256);
        be.setShaperCurveJson ("A", edit);
        juce::AudioBuffer<float> buf (2, 256);
        juce::MidiBuffer none;
        p.processBlock (buf, none);
        check (p.getShaperEngine().current() != nullptr && p.getShaperEngine().current() == p.getShaperEngine().latest().get(),
               "after a block the audio side has adopted the latest published LUT set");
        p.releaseResources();

        // ---- state load: the stored curves win over a preset change raised by the load itself
        {
            PluginProcessor a;
            setParam (a, "shaperPreset", 1.0f);
            a.getBackendImpl().servicePending();               // A = Hard Clip, preset = Hard Clip
            a.getBackendImpl().setShaperCurveJson ("B", edit); // B = edit -> preset flips to Custom
            setParam (a, "shaperPreset", 4.0f);                // a preset value without servicePending: the stored curves stay
            juce::MemoryBlock blob;
            a.getStateInformation (blob);

            PluginProcessor b;
            b.setStateInformation (blob.getData(), (int) blob.getSize());
            const auto rb = b.getUiBackend().revision (WarprUiBackend::Revision::shaperCurves);
            b.getBackendImpl().servicePending();
            check (getParam (b, "shaperPreset") == 4.0f && b.getUiBackend().getShaperCurveJson ("A") == hardJson
                   && b.getUiBackend().getShaperCurveJson ("B") == a.getUiBackend().getShaperCurveJson ("B")
                   && b.getUiBackend().revision (WarprUiBackend::Revision::shaperCurves) == rb,
                   "state load: the stored curves are kept (a preset value restored from state does not regenerate them)");
        }
    }

    //==========================================================================
    // 8. alias gates (T2 / A1)
    //==========================================================================
    constexpr double kBaseFs = 48000.0;
    constexpr int kBin = 2053;                                      // f0 = fs * 2053 / 32768 = 3007.4 Hz
    const double kF0 = kBaseFs * (double) kBin / 32768.0;

    /** In-band alias level (dB) of a sine of amplitude `amp` at kF0 through curve A of `set`, run at os * 48 kHz. */
    double aliasLevel (const ShaperLutSet& set, int os, bool useAdaa, double amp)
    {
        const double rate = kBaseFs * (double) os;
        const int order = 15 + (os == 1 ? 0 : (os == 2 ? 1 : (os == 4 ? 2 : 3)));
        const int n = 1 << order;
        const int total = n + 2048;

        ShaperSpan sp;
        sp.active = true;
        sp.cur = &set;
        ShaperChannelState ch;

        std::vector<float> y ((size_t) total);
        for (int i = 0; i < total; ++i)
        {
            const double x = amp * std::sin (2.0 * kPi * kF0 * (double) i / rate);
            y[(size_t) i] = (float) (useAdaa ? x + ch.step (sp, 0.0, 1.0, x) : set.fAt (0, x));
        }

        return warpr::tests::aliasDb (y.data(), total, rate, kF0, order, 8, 0.5 * kBaseFs);
    }

    void testAliasGate()
    {
        std::printf ("\n[D4] T2 alias gate A1 for the shaper (3.0 kHz sine, amplitude 0.9, level measured below the base Nyquist)\n");
        constexpr double amp = 0.9;

        // --- the gated curve: the Hard Clip preset
        const auto hard = ShaperLutSet::build (presets::generate (1), presets::generate (1), true);
        const double naive1 = aliasLevel (*hard, 1, false, amp);
        const double adaa1 = aliasLevel (*hard, 1, true, amp);
        const double improvement = naive1 - adaa1;
        const int floorOs = hard->minOs (0);

        std::printf ("        A1 Hard Clip 1x: naive %7.1f dB, ADAA %7.1f dB  -> improvement %5.1f dB (gate 12)\n", naive1, adaa1, improvement);

        bool a1Ok;
        if (improvement >= 12.0)
        {
            std::printf ("        A1 Hard Clip: %.1f dB >= 12 -> PASS\n", improvement);
            a1Ok = true;
        }
        else if (floorOs >= 2)
        {
            std::printf ("        A1 Hard Clip: %.1f dB < 12 -> covered by minOS %d (R1(b) fallback, kink %.2f, enforced at note-on)\n",
                         improvement, floorOs, hard->kink (0));
            a1Ok = true;
        }
        else
        {
            DETAIL ("        A1 FAILED for the Hard Clip preset: %.1f dB < 12 dB and the hardness floor (%d) does not cover 1x operation\n", improvement, floorOs);
            a1Ok = false;
        }
        check (a1Ok, "A1: shaper hard clip - ADAA lowers the 1x alias power by >= 12 dB, or the curve-hardness minOS floor (>= 2) removes 1x operation (R1(b))");
        check (floorOs == 4, "the Hard Clip preset maps to the 4x floor");

        const double adaa2 = aliasLevel (*hard, 2, true, amp);
        const double adaa4 = aliasLevel (*hard, 4, true, amp);
        const double adaa8 = aliasLevel (*hard, 8, true, amp);
        const double naive8 = aliasLevel (*hard, 8, false, amp);
        const double limit = naive8 + 6.0;
        const int need = adaa2 <= limit ? 2 : (adaa4 <= limit ? 4 : 8);
        std::printf ("        A2 (informational for the shaper, plan T5 gates A1 only) Hard Clip: naive 8x %7.1f dB (+6 -> %7.1f), ADAA 2x %7.1f, 4x %7.1f, 8x %7.1f dB"
                     " -> OS meeting 'ADAA <= naive 8x + 6 dB' = %d, enforced floor = %d%s\n",
                     naive8, limit, adaa2, adaa4, adaa8, need, floorOs, need > floorOs ? "  [floor below the A2 requirement: raise the 'hard' class]" : "");
        check (adaa2 < adaa1 && adaa4 < adaa2, "alias power falls with the oversampling factor (ADAA 1x > 2x > 4x)");

        // --- the whole library, for the record (and the hardness map)
        std::printf ("        alias table, shaper presets (amplitude 0.9): 1x naive / 1x ADAA / improvement / 2x ADAA / 4x ADAA, kink -> minOS\n");
        for (int i = 0; i < 8; ++i)
        {
            const auto s = ShaperLutSet::build (presets::generate (i), presets::generate (i), ! presets::isAsymmetric (i));
            const double n1 = aliasLevel (*s, 1, false, amp), a1 = aliasLevel (*s, 1, true, amp);
            std::printf ("          %-10s naive %7.1f dB  ADAA %7.1f dB  improvement %5.1f dB   2x ADAA %7.1f dB   4x ADAA %7.1f dB   kink %.4f -> minOS %d%s\n",
                         presets::name (i), n1, a1, n1 - a1, aliasLevel (*s, 2, true, amp), aliasLevel (*s, 4, true, amp),
                         s->kink (0), s->minOs (0), (n1 - a1 < 12.0 && s->minOs (0) == 1) ? "   [<12 dB and no floor: informational, not gated]" : "");
        }
    }

    //==========================================================================
    // 9. T1-style cube
    //==========================================================================
    double slopeSup (const ShaperLutSet& s)
    {
        double m = 1.0;
        for (int c = 0; c < 2; ++c)
        {
            const auto& r = s.remainder (c);
            for (int i = 0; i + 1 < ShaperLutSet::kPoints; ++i)
                m = std::max (m, std::abs (1.0 + (r[(size_t) (i + 1)] - r[(size_t) i]) * ShaperLutSet::kPerUnit));
        }
        return m;
    }

    void testCube (bool fullRun)
    {
        std::printf ("\n[D4] T1-style cube: signals x OS x morph x mix x curve sets through UnisonOsc::render%s\n", fullRun ? "  [full]" : "");
        constexpr double fs = 48000.0;

        auto sine = makeCycle (2048, 0.5f);
        auto saw = warpr::SampleData::createDefaultWave();
        auto noise = makeNoise();

        struct Sig { const warpr::SampleData* sd; double hz; const char* name; bool smooth; };
        const std::vector<Sig> sigs {
            { sine.get(),  100.0,    "sine 100 Hz",      true },
            { sine.get(),  5000.0,   "sine 5 kHz",       false },
            { saw.get(),   110.0,    "saw cycle 110 Hz", false },
            { noise.get(), rootHz(), "noise",            false }
        };

        struct Pair { const char* name; ShaperCurve a, b; bool sym; };
        const std::vector<Pair> pairs {
            { "identity / identity", ShaperCurve::identity(), ShaperCurve::identity(), true },
            { "Hard Clip / Expo",    presets::generate (1), presets::generate (7), true },
            { "Tube / Diode (asym)", presets::generate (2), presets::generate (3), false },
            { "Fold / Triangle",     presets::generate (4), presets::generate (6), true }
        };

        const std::vector<int> osList = fullRun ? std::vector<int> { 1, 2, 4, 8 } : std::vector<int> { 1, 2, 8 };
        const int totalBase = fullRun ? 7200 : 3200;
        int runs = 0, nonFinite = 0, unbounded = 0, discBad = 0;
        double worstPeak = 0.0;

        for (const auto& pr : pairs)
        {
            const auto set = ShaperLutSet::build (pr.a, pr.b, pr.sym);
            const double sup = slopeSup (*set);

            for (const auto& sig : sigs)
                for (int os : osList)
                {
                    const auto base = renderOscShaper (*sig.sd, sig.hz, fs, os, totalBase, noShaper());
                    const double nat = maxStepOf (base.L);

                    for (float morph : { 0.0f, 0.5f, 1.0f })
                        for (float mix : { 0.5f, 1.0f })
                        {
                            ++runs;
                            const auto run = renderOscShaper (*sig.sd, sig.hz, fs, os, totalBase, constShaper (set.get(), morph, mix));

                            if (! allFinite (run.L) || ! allFinite (run.R))
                            {
                                ++nonFinite;
                                DETAIL ("        non-finite: %s, %s, OS %d, morph %.1f mix %.1f\n", pr.name, sig.name, os, (double) morph, (double) mix);
                                continue;
                            }

                            const double pk = std::max (peakOf (run.L), peakOf (run.R));
                            worstPeak = std::max (worstPeak, pk);
                            if (pk > 4.0)
                                ++unbounded;

                            if (sig.smooth)
                            {
                                // ADAA output is a box average of f over each input step: |dy| <= sup|f'| x natural step
                                const double bound = sup * nat * 1.02 + 0.01;
                                const double step = maxStepOf (run.L);
                                if (step > bound)
                                {
                                    ++discBad;
                                    DETAIL ("        discontinuity %.4f > %.4f: %s, OS %d, morph %.1f mix %.1f\n", step, bound, pr.name, os, (double) morph, (double) mix);
                                }
                            }
                        }
                }
        }

        std::printf ("        (%d renders, worst |y| %.3f)\n", runs, worstPeak);
        check (nonFinite == 0, "no NaN / inf for any signal, OS factor, morph, mix or curve set");
        check (unbounded == 0, "|y| <= 4 FS for every combination");
        check (discBad == 0, "smooth input: max step <= sup|f'| x the natural step (no clicks from the shaper itself)");
    }

    //==========================================================================
    // 10. processor level
    //==========================================================================
    int latchedOs (int preset, bool enable, float mix, float morph, float userOsIndex)
    {
        constexpr double fs = 48000.0;
        PluginProcessor p;

        if (preset >= 0)
        {
            setParam (p, "shaperPreset", (float) preset);
            p.getBackendImpl().servicePending();
        }

        setParam (p, "shaperEnable", enable ? 1.0f : 0.0f);
        setParam (p, "shaperMix", mix);
        setParam (p, "shaperMorph", morph);
        setParam (p, "oversampling", userOsIndex);
        p.setPlayConfigDetails (0, 2, fs, 256);
        p.prepareToPlay (fs, 256);

        juce::AudioBuffer<float> buf (2, 256);
        juce::MidiBuffer on;
        on.addEvent (juce::MidiMessage::noteOn (1, 60, 0.9f), 0);
        p.processBlock (buf, on);

        int os = 0;
        for (int slot = 0; slot < 40; ++slot)
        {
            const auto info = p.getSynth().getVoiceInfo (slot);
            if (info.active && ! info.ghost)
                os = info.os;
        }

        p.releaseResources();
        return os;
    }

    std::vector<float> renderBlocks (PluginProcessor& p, double fs, int nBlocks, int block, const std::function<void (int)>& beforeBlock)
    {
        p.setPlayConfigDetails (0, 2, fs, block);
        p.prepareToPlay (fs, block);

        juce::AudioBuffer<float> buf (2, block);
        std::vector<float> out;
        out.reserve ((size_t) nBlocks * (size_t) block);

        for (int b = 0; b < nBlocks; ++b)
        {
            if (beforeBlock)
                beforeBlock (b);

            juce::MidiBuffer midi;
            if (b == 0)
                midi.addEvent (juce::MidiMessage::noteOn (1, 48, 0.9f), 0);

            p.processBlock (buf, midi);
            out.insert (out.end(), buf.getReadPointer (0), buf.getReadPointer (0) + block);
        }

        p.releaseResources();
        return out;
    }

    void prepSustained (PluginProcessor& p)
    {
        setParam (p, "ampAttack", 1.0f);
        setParam (p, "ampSustain", 1.0f);
        setParam (p, "shaperEnable", 1.0f);
        setParam (p, "shaperMix", 1.0f);
        setParam (p, "shaperMorph", 0.0f);
        setParam (p, "oversampling", 1.0f);                  // 2x
    }

    void testProcessor()
    {
        std::printf ("\n[D4] processor level: minOS floor, identity null, LUT-crossfade click bound, allocations, determinism\n");
        constexpr double fs = 48000.0;

        // ---- minOS enforcement (curve-hardness floor, same latch path as the warp stage)
        {
            const int hard1x = latchedOs (1, true, 1.0f, 0.0f, 0.0f);
            const int hard8x = latchedOs (1, true, 1.0f, 0.0f, 3.0f);
            const int softPreset = latchedOs (5, true, 1.0f, 0.0f, 0.0f);         // Sine
            const int off = latchedOs (1, false, 1.0f, 0.0f, 0.0f);
            const int mix0 = latchedOs (1, true, 0.0f, 0.0f, 0.0f);
            const int morphB = latchedOs (1, true, 1.0f, 1.0f, 0.0f);              // B is the identity
            const int morphMid = latchedOs (1, true, 1.0f, 0.5f, 0.0f);
            const int identity1x = latchedOs (-1, true, 1.0f, 0.0f, 0.0f);
            std::printf ("        (user 1x: Hard Clip -> %dx, Sine -> %dx, disabled -> %dx, mix 0 -> %dx, morph 1 (B identity) -> %dx, morph .5 -> %dx, identity -> %dx; user 8x + Hard Clip -> %dx)\n",
                         hard1x, softPreset, off, mix0, morphB, morphMid, identity1x, hard8x);
            check (hard1x == 4, "shaper hardness floor is enforced: user 1x + Hard Clip preset -> the voice runs at 4x");
            check (hard8x == 8, "the floor is a max(): user 8x stays 8x");
            check (softPreset == 1 && identity1x == 1, "smooth curves and the identity need no floor (1x stays 1x)");
            check (off == 1 && mix0 == 1, "no floor while the shaper is disabled or its mix is 0");
            check (morphB == 1 && morphMid == 4, "the floor follows the curve(s) the morph uses (B identity -> 1, in between -> 4)");
        }

        // ---- identity curve through the whole voice path: null against the shaper-off render
        {
            PluginProcessor off, on;
            setParam (off, "ampAttack", 1.0f); setParam (off, "ampSustain", 1.0f);
            prepSustained (on);
            setParam (on, "shaperMorph", 0.3f);
            const auto a = renderBlocks (off, fs, 60, 256, nullptr);
            const auto b = renderBlocks (on, fs, 60, 256, nullptr);
            const double nullLevel = nullDb (a, b);
            std::printf ("        (identity curves, shaper on vs off: null %.1f dB, bit-exact %s)\n", nullLevel, bitEqual (a, b) ? "yes" : "no");
            check (nullLevel < -80.0 && peakOf (a) > 0.05, "identity curve: the whole voice path nulls against the shaper-off render (< -80 dB)");
        }

        // ---- LUT crossfade click bound: edit mid-note
        {
            const juce::String expo = presets::generate (7).toJson();
            constexpr int nBlocks = 150, block = 256, swBlock = 60;
            const int swSample = swBlock * block;

            PluginProcessor pOld, pNew, pSw;
            prepSustained (pOld); prepSustained (pNew); prepSustained (pSw);
            pNew.getBackendImpl().setShaperCurveJson ("A", expo);

            const auto yOld = renderBlocks (pOld, fs, nBlocks, block, nullptr);
            const auto yNew = renderBlocks (pNew, fs, nBlocks, block, nullptr);
            const auto ySw = renderBlocks (pSw, fs, nBlocks, block, [&] (int b)
            {
                if (b == swBlock)
                    pSw.getBackendImpl().setShaperCurveJson ("A", expo);
            });

            const size_t w0 = (size_t) (swSample - 64), w1 = (size_t) (swSample + 12 * block);
            auto stepIn = [&] (const std::vector<float>& v)
            {
                double m = 0.0;
                for (size_t i = w0 + 1; i < w1; ++i)
                    m = std::max (m, (double) std::abs (v[i] - v[i - 1]));
                return m;
            };

            double maxDiff = 0.0;
            for (size_t i = (size_t) swSample; i < (size_t) swSample + 480; ++i)
                maxDiff = std::max (maxDiff, (double) std::abs (yNew[i] - yOld[i]));

            const double natural = std::max (stepIn (yOld), stepIn (yNew));
            const double stepSw = stepIn (ySw);
            const double bound = natural + 1.5 * maxDiff / 480.0 + 1.0e-3;

            double tailErr = 0.0;
            for (size_t i = (size_t) (swSample + 0.3 * fs); i < ySw.size(); ++i)
                tailErr = std::max (tailErr, (double) std::abs (ySw[i] - yNew[i]));

            std::printf ("        (identity -> Expo at block %d: natural max step %.5f, output max difference old/new %.4f, switched max step %.5f, bound %.5f; a hard switch would step by up to %.4f)\n",
                         swBlock, natural, maxDiff, stepSw, bound, maxDiff);
            check (maxDiff > 0.02, "the two curves are audibly different (the test is meaningful)");
            check (stepSw <= bound, "LUT edit mid-note: max step <= natural step + difference / 480 samples (10 ms crossfade, click-free)");
            check (tailErr < 1.0e-3, "after the fade the switched render equals the render with the new curve from the start (< 1e-3 once the DC blockers settled)");
            check (peakOf (ySw) > 0.05 && allFinite (ySw), "switched render is audible and finite");
        }

        // ---- zero allocations, including the crossfade windows
        {
            PluginProcessor p;
            prepSustained (p);
            setParam (p, "unisonVoices", 4.0f);
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
            for (int round = 0; round < 8; ++round)
            {
                p.getBackendImpl().setShaperCurveJson ("A", presets::generate (round % 8).toJson());   // allocates: uncounted
                p.drainRetired();

                warpr::tests::ScopedAllocCount counter;
                for (int i = 0; i < 12; ++i)
                {
                    p.processBlock (buf, none);
                    sawFade = sawFade || p.getShaperEngine().isFading();
                }
                allocations += counter.count();
            }
            check (sawFade, "the counted window really contained crossfades");
            check (allocations == 0, "processBlock performs zero allocations while LUT sets are swapped and crossfaded (unison 4, 8 curve changes)");
            p.releaseResources();
        }

        // ---- determinism with a mid-note edit
        {
            auto run = [&]
            {
                PluginProcessor p;
                prepSustained (p);
                setParam (p, "unisonVoices", 3.0f);
                setParam (p, "shaperMorph", 0.4f);
                return renderBlocks (p, fs, 80, 256, [&p] (int b)
                {
                    if (b == 30)
                        p.getBackendImpl().setShaperCurveJson ("B", presets::generate (6).toJson());
                    if (b == 45)
                        p.getBackendImpl().setShaperCurveJson ("A", presets::generate (0).toJson());
                });
            };

            const auto a = run(), b = run();
            check (bitEqual (a, b) && peakOf (a) > 0.05, "two identical sessions with mid-note edits are bit-equal");
        }
    }
}

JUCE_END_IGNORE_WARNINGS_GCC_LIKE

//==============================================================================
int runPhaseD4Tests (bool full)
{
    failures = 0;
    printedDetails = 0;
    std::printf ("\n=== Phase D4: A/B transfer-curve shaper (LUT + antiderivative ADAA) + editor backend (%s) ===\n", full ? "full" : "quick");

    testPresets();
    testInterpolation();
    testJson();
    testLut (full);
    testStage();
    testEngine();
    testBackendStandalone();
    testBackendWithProcessor();
    testAliasGate();
    testCube (full);
    testProcessor();

    return failures;
}
