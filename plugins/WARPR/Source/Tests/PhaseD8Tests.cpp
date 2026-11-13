/*
    WARPR - phase D8 test block: LFO system + modulation matrix.
    Called from RunTests.cpp main():   failures += runPhaseD8Tests (full);

    Contents (plan.md "Phase D8" test criteria + T8)
      1. golden vectors     13 presets x {no bends, 1, 2, 3 bends} + CUSTOM curves: C++ tables vs a line-by-line transliteration of
                            index.html presetFn / bend anchors / segment law (UI y coordinates, v = 1 - y), tolerance 1e-4, plus
                            hand-derived literal anchors that do not go through the oracle
      2. engine             Hz mapping, directions, phase offset, ENV hold, 2 ms slew, SMOOTH time constant
      3. sync lock          40..300 bpm x 17 divisions (+ time signatures in --full): measured period vs beatsPerCycle (0.01 %),
                            phase-based and output-based; relock after host jumps / loops / stop-start; no-playhead clock
      4. matrix math        norm' = clamp(norm + depth (2v-1), 0, 1) in the skewed domain == juce::NormalisableRange for every
                            Float / Int parameter; Int rounding; clamps; summed routes; compile rules; destination classes
      5. LFO-on-LFO         index ordering with the one-chunk delay, demonstrated numerically
      6. zipper bound       max step of a modulated value per chunk, and the processor-level step bound
      7. mode semantics     FREE / RETRIG / ENV / MONO through the processor (A1 assumptions), viz phases, lfoCount idle
      8. structural latching unisonVoices / polyphony are sampled at note-on only
      9. backend            setLfoCount / setLfoShapeJson / setModMatrixJson publication, getUiStateJson round trip, state load
     10. hook-up            every modulatable parameter changes the sound when an LFO drives it
     11. host transport     a real juce::AudioPlayHead drives a grid-locked LFO; jump / loop / stop-start relock
     12. zero allocations   processBlock with a loaded matrix
     13. CPU                10 LFOs x 16 voices x 3 routes at 8x OS vs the same patch without modulation

    Hand-derived gates carry a comment saying so; measured numbers are printed next to the assertion.
    STACK NOTE: sections 7-13 construct whole PluginProcessors (34 voices x 8 oscillators + grain pools, ~1 MB each) on the
    HEAP (std::make_unique), but WARPR_Tests still needs its 64 MB main stack for the earlier phases (see CMakeLists.txt).
*/

#include "PluginProcessor.h"
#include "DSP/LfoEngine.h"
#include "DSP/LfoTable.h"
#include "DSP/ModMatrix.h"
#include "DSP/TransportClock.h"
#include "Parameters/ParamTable.h"
#include "Tests/AllocHook.h"
#include "Tests/OfflineRender.h"
#include "Tests/Scanners.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// Bit-exact float equality is the point of several assertions in this file (bypass, determinism), so the warning is
// suppressed here only.
JUCE_BEGIN_IGNORE_WARNINGS_GCC_LIKE ("-Wfloat-equal")

int runPhaseD8Tests (bool full);

namespace
{
    int failures = 0;
    int printedDetails = 0;

    constexpr double kFs = 48000.0;
    constexpr double kPi = 3.14159265358979323846;

    void check (bool cond, const char* name)
    {
        std::printf ("  %s  %s\n", cond ? "PASS" : "FAIL", name);
        if (! cond) ++failures;
    }

    /** Prints a failure detail line (bounded, so a systematic failure does not flood the log). */
   #define DETAIL(...) do { if (printedDetails++ < 80) std::printf (__VA_ARGS__); } while (false)

    //==========================================================================
    // shape documents
    //==========================================================================
    juce::String constShapeJson (double v)
    {
        return juce::String::formatted (R"({"preset":"CUSTOM","points":[{"x":0,"v":%.6f,"c":0},{"x":1,"v":%.6f,"c":0}],"bends":[],"table":null,"drift":null})", v, v);
    }

    juce::String presetShapeJson (const char* preset)
    {
        return juce::String::formatted (R"({"preset":"%s","points":[],"bends":[],"table":null,"drift":null})", preset);
    }

    std::unique_ptr<warpr::LfoShapeTable> tableOf (const juce::String& wire)
    {
        warpr::LfoShapeDesc d;
        if (! warpr::LfoShapeDesc::fromJson (wire, d))
            d = warpr::LfoShapeDesc::defaultShape();
        return warpr::LfoTableBuilder::build (d);
    }

    //==========================================================================
    // 1. golden vectors: JS oracle (index.html, UI y coordinates; y = 0 is the top, wire v = 1 - y)
    //==========================================================================
    struct JsPoint { double x = 0, y = 0, c = 0; };
    struct JsBend  { double x = 0, y = 0; };

    struct JsShape
    {
        std::string preset = "CUSTOM";
        std::vector<JsPoint> points;
        std::vector<JsBend> bends;
        std::vector<double> table;
        bool hasTable = false;
        double da = 0.0, db = 0.0;
        bool hasDrift = false;
    };

    /** index.html presetFn(s)(t): returns false when the preset is not analytic (the UI then draws the points). */
    bool jsPresetY (const JsShape& s, double t, double& y)
    {
        const double TAU = 2.0 * kPi;
        auto steps = [&] (int n) { return 0.1 + 0.8 * ((double) std::min (n - 1, (int) std::floor (t * n)) / (double) (n - 1)); };

        if (s.preset == "SINE")           y = 0.5 - 0.4 * std::sin (TAU * t);
        else if (s.preset == "TRIANGLE")  y = 0.1 + 0.8 * std::abs (2.0 * t - 1.0);
        else if (s.preset == "SAW UP")    y = 0.9 - 0.8 * std::fmod (t, 1.0);
        else if (s.preset == "SAW DOWN")  y = 0.1 + 0.8 * std::fmod (t, 1.0);
        else if (s.preset == "SQUARE")    y = t < 0.5 ? 0.1 : 0.9;
        else if (s.preset == "PUMP")      y = 0.9 - 0.8 * std::exp (-4.5 * t);
        else if (s.preset == "STEPS 4")   y = steps (4);
        else if (s.preset == "STEPS 8")   y = steps (8);
        else if (s.preset == "STEPS 16")  y = steps (16);
        else if (s.preset == "SWING")
        {
            const double u = t < 0.62 ? std::pow (t / 0.62, 1.5) : 1.0 - 0.75 * ((t - 0.62) / 0.38);
            y = 0.9 - 0.8 * u;
        }
        else if (s.preset == "S&H NOISE")
        {
            const std::vector<double> L = s.hasTable ? s.table : std::vector<double> { 0.5 };
            y = L[(size_t) std::min ((int) L.size() - 1, (int) std::floor (t * (double) L.size()))];
        }
        else if (s.preset == "DRIFT")
        {
            const double a = s.hasDrift ? s.da : 0.0, b = s.hasDrift ? s.db : 0.0;
            y = 0.5 + 0.26 * std::sin (TAU * t + a) + 0.14 * std::sin (2.0 * TAU * t + b);
        }
        else
            return false;

        return true;
    }

    /** index.html lfoRender(): the drawn curve's y at t (analytic + bend anchors, or the custom polyline). */
    double jsCurveY (const JsShape& s, double t)
    {
        double fy = 0.0;

        if (jsPresetY (s, t, fy))
        {
            struct Anchor { double x, dy; };
            std::vector<JsBend> fp = s.bends;
            std::stable_sort (fp.begin(), fp.end(), [] (const JsBend& l, const JsBend& r) { return l.x < r.x; });

            std::vector<Anchor> anchors;
            anchors.push_back ({ 0.0, 0.0 });

            for (const auto& b : fp)
            {
                double bf = 0.0;
                jsPresetY (s, b.x, bf);
                anchors.push_back ({ b.x, b.y - bf });
            }

            anchors.push_back ({ 1.0, 0.0 });

            size_t i = 0;
            while (i < anchors.size() - 2 && t > anchors[i + 1].x)
                ++i;

            const Anchor& a0 = anchors[i];
            const Anchor& a1 = anchors[i + 1];
            const double u = (t - a0.x) / std::max (a1.x - a0.x, 0.001);
            const double uu = u * u * (3.0 - 2.0 * u);
            return fy + a0.dy * (1.0 - uu) + a1.dy * uu;
        }

        std::vector<JsPoint> pts = s.points;
        std::stable_sort (pts.begin(), pts.end(), [] (const JsPoint& l, const JsPoint& r) { return l.x < r.x; });

        if (t < pts.front().x)
            return pts.front().y;

        for (size_t i = 1; i < pts.size(); ++i)
        {
            if (t < pts[i].x)
            {
                const JsPoint& p0 = pts[i - 1];
                const JsPoint& p1 = pts[i];
                const double u = (t - p0.x) / (p1.x - p0.x);
                return p0.y + (p1.y - p0.y) * std::pow (u, std::pow (2.0, p0.c * 3.0));     // seg(): exponent 2^(3c)
            }
        }

        return pts.back().y;
    }

    /** index.html toWire(): v = 1 - y for points and bends, table / drift as stored. */
    juce::String wireJson (const JsShape& s)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("preset", juce::String (s.preset));

        juce::Array<juce::var> pts, bends;

        for (const auto& p : s.points)
        {
            auto* po = new juce::DynamicObject();
            po->setProperty ("x", p.x);
            po->setProperty ("v", 1.0 - p.y);
            po->setProperty ("c", p.c);
            pts.add (juce::var (po));
        }

        for (const auto& b : s.bends)
        {
            auto* bo = new juce::DynamicObject();
            bo->setProperty ("x", b.x);
            bo->setProperty ("v", 1.0 - b.y);
            bends.add (juce::var (bo));
        }

        o->setProperty ("points", pts);
        o->setProperty ("bends", bends);

        if (s.hasTable)
        {
            juce::Array<juce::var> tb;
            for (double v : s.table)
                tb.add (v);
            o->setProperty ("table", tb);
        }
        else
            o->setProperty ("table", juce::var());

        if (s.hasDrift)
        {
            auto* d = new juce::DynamicObject();
            d->setProperty ("a", s.da);
            d->setProperty ("b", s.db);
            o->setProperty ("drift", juce::var (d));
        }
        else
            o->setProperty ("drift", juce::var());

        return juce::JSON::toString (juce::var (o), true);
    }

    struct GoldenStat { double worst = 0.0, worstAt = 0.0; int probes = 0; };

    /** Compares table.eval(t) with 1 - jsCurveY(t) on a dense grid + the exact 1/16 grid points.  `skip(t)` excludes a
        probe (documented limits only). */
    template <class Skip>
    GoldenStat compareGolden (const JsShape& s, Skip&& skip)
    {
        GoldenStat st;
        warpr::LfoShapeDesc d;

        if (! warpr::LfoShapeDesc::fromJson (wireJson (s), d))
        {
            st.worst = 1.0e9;
            return st;
        }

        const auto table = warpr::LfoTableBuilder::build (d);

        auto probe = [&] (double t)
        {
            if (skip (t))
                return;

            const double want = 1.0 - jsCurveY (s, t);
            const double got = (double) table->eval (t);
            const double err = std::abs (got - std::min (1.0, std::max (0.0, want)));
            ++st.probes;

            if (err > st.worst)
            {
                st.worst = err;
                st.worstAt = t;
            }
        };

        for (int k = 0; k < 4001; ++k)
            probe (((double) k + 0.37) / 4001.0);

        for (int k = 0; k < 16; ++k)
            probe ((double) k / 16.0);                       // exact cell edges: the right-continuous value

        return st;
    }

    /** The bend sets (UI y coordinates) shared with Source/Tests/lfo_golden_export.js. */
    const std::vector<std::vector<JsBend>>& goldenBendSets()
    {
        static const std::vector<std::vector<JsBend>> sets = {
            {},
            { { 0.5, 0.2 } },
            { { 0.3, 0.8 }, { 0.7, 0.15 } },
            { { 0.25, 0.9 }, { 0.4, 0.1 }, { 0.9, 0.5 } }
        };
        return sets;
    }

    JsShape goldenShape (const std::string& preset, int bendSet)
    {
        JsShape s;
        s.preset = preset;
        s.bends = goldenBendSets()[(size_t) bendSet];

        if (s.preset == "S&H NOISE")
        {
            s.table = { 0.31, 0.74, 0.12, 0.55, 0.9, 0.2, 0.66, 0.43 };
            s.hasTable = true;
        }

        if (s.preset == "DRIFT")
        {
            s.da = 1.3;
            s.db = 4.1;
            s.hasDrift = true;
        }

        return s;
    }

    /** Rows exported from the REAL index.html by Source/Tests/lfo_golden_export.js (node).  Paste its output between the markers;
        until then the transliterated oracle above and the hand-derived anchors carry the gate. */
    struct GoldenRow { const char* preset; int bendSet; double t, y; };

    const std::vector<GoldenRow>& nodeGoldenRows()
    {
        static const std::vector<GoldenRow> rows = {
            // BEGIN GOLDEN
            // END GOLDEN
        };
        return rows;
    }

    void testNodeGoldenRows()
    {
        const auto& rows = nodeGoldenRows();

        if (rows.empty())
        {
            std::printf ("        (no node-exported rows pasted yet - run Source/Tests/lfo_golden_export.js and paste its output into nodeGoldenRows())\n");
            return;
        }

        int bad = 0;
        double worst = 0.0;

        for (const auto& row : rows)
        {
            warpr::LfoShapeDesc d;
            const JsShape s = goldenShape (row.preset, row.bendSet);

            if (! warpr::LfoShapeDesc::fromJson (wireJson (s), d))
            {
                ++bad;
                continue;
            }

            const auto table = warpr::LfoTableBuilder::build (d);
            const double err = std::abs ((double) table->eval (row.t) - std::min (1.0, std::max (0.0, 1.0 - row.y)));
            worst = std::max (worst, err);

            if (err > 1.0e-4)
                ++bad;
        }

        std::printf ("        (%d node-exported rows, worst error %.3g)\n", (int) rows.size(), worst);
        check (bad == 0, "rows exported from the real index.html JS match the C++ tables within 1e-4");
    }

    void testGoldenVectors()
    {
        std::printf ("\n[D8] golden vectors: C++ LFO tables vs the index.html shape math (1e-4)\n");

        const char* presets[] = { "SINE", "TRIANGLE", "SAW UP", "SAW DOWN", "SQUARE", "PUMP", "STEPS 4", "STEPS 8", "STEPS 16",
                                  "SWING", "S&H NOISE", "DRIFT" };

        testNodeGoldenRows();

        double worstAll = 0.0;
        int bad = 0, probes = 0;

        for (const char* name : presets)
        {
            double worstPreset = 0.0;

            for (int bendSet = 0; bendSet < (int) goldenBendSets().size(); ++bendSet)
            {
                const JsShape s = goldenShape (name, bendSet);
                const auto& bends = s.bends;
                const auto st = compareGolden (s, [] (double) { return false; });
                worstPreset = std::max (worstPreset, st.worst);
                probes += st.probes;

                if (st.worst > 1.0e-4)
                {
                    ++bad;
                    DETAIL ("        %s with %d bend(s): error %.3g at t = %.5f\n", name, (int) bends.size(), st.worst, st.worstAt);
                }
            }

            std::printf ("        %-10s worst |C++ - JS| over 4 bend sets = %.3g\n", name, worstPreset);
            worstAll = std::max (worstAll, worstPreset);
        }

        check (bad == 0, "12 analytic presets x {0, 1, 2, 3 bends}: every probe within 1e-4 of the JS curve");
        std::printf ("        (%d probes, worst %.3g; SWING is the binding case - its slope kink sits inside a cell)\n", probes, worstAll);

        // CUSTOM: segment exponent 2^(3c), including a vertical step; the first 8 cells after a point with c < 0 (infinite
        // slope there, documented limit of any uniform table) are excluded.
        struct CustomCase { const char* name; std::vector<JsPoint> pts; };
        const std::vector<CustomCase> customs = {
            { "default 5 points (c = 0)", { { 0, 0.9, 0 }, { 0.25, 0.1, 0 }, { 0.5, 0.65, 0 }, { 0.75, 0.25, 0 }, { 1, 0.9, 0 } } },
            { "mixed exponents", { { 0, 0.9, 0.6 }, { 0.3, 0.1, -0.3 }, { 0.55, 0.8, 1.0 }, { 0.8, 0.3, 0.2 }, { 1, 0.6, 0 } } },
            { "vertical step", { { 0, 0.8, 0 }, { 0.5, 0.8, 0 }, { 0.5, 0.1, 0 }, { 1, 0.1, 0 } } },
            { "unsorted input", { { 1, 0.4, 0 }, { 0.2, 0.7, 0.4 }, { 0, 0.2, 0 }, { 0.6, 0.9, -0.2 } } }
        };

        for (const auto& cc : customs)
        {
            JsShape s;
            s.preset = "CUSTOM";
            s.points = cc.pts;
            std::vector<JsPoint> sorted = cc.pts;
            std::stable_sort (sorted.begin(), sorted.end(), [] (const JsPoint& l, const JsPoint& r) { return l.x < r.x; });

            const auto st = compareGolden (s, [&sorted] (double t)
            {
                for (const auto& p : sorted)
                    if (p.c < 0.0 && t >= p.x && t < p.x + 8.0 / (double) warpr::LfoShapeTable::kCells)
                        return true;
                return false;
            });

            std::printf ("        CUSTOM '%s': worst %.3g (%d probes)\n", cc.name, st.worst, st.probes);
            check (st.worst <= 1.0e-4, cc.name);
        }
    }

    /** Literal, hand-derived values (no oracle code in the path): they pin the oracle AND the builder to the formulas. */
    void testHandDerivedAnchors()
    {
        std::printf ("\n[D8] hand-derived anchors (literal values, wire coordinates)\n");

        struct Anchor { juce::String wire; double t, expect; const char* what; };
        const auto presetWire = [] (const char* p, const char* bends) {
            return juce::String::formatted (R"({"preset":"%s","points":[],"bends":[%s],"table":null,"drift":null})", p, bends);
        };

        const std::vector<Anchor> anchors = {
            { presetWire ("SINE", ""), 0.0, 0.5, "SINE t=0" },
            { presetWire ("SINE", ""), 0.25, 0.9, "SINE t=1/4 (0.5 + 0.4)" },
            { presetWire ("SINE", ""), 0.75, 0.1, "SINE t=3/4" },
            { presetWire ("TRIANGLE", ""), 0.0, 0.1, "TRIANGLE t=0" },
            { presetWire ("TRIANGLE", ""), 0.25, 0.5, "TRIANGLE t=1/4" },
            { presetWire ("TRIANGLE", ""), 0.5, 0.9, "TRIANGLE t=1/2" },
            { presetWire ("SAW UP", ""), 0.25, 0.3, "SAW UP t=1/4 (0.1 + 0.8 t)" },
            { presetWire ("SAW UP", ""), 0.75, 0.7, "SAW UP t=3/4" },
            { presetWire ("SAW DOWN", ""), 0.25, 0.7, "SAW DOWN t=1/4" },
            { presetWire ("SQUARE", ""), 0.25, 0.9, "SQUARE first half is high" },
            { presetWire ("SQUARE", ""), 0.75, 0.1, "SQUARE second half is low" },
            { presetWire ("SQUARE", ""), 0.5, 0.1, "SQUARE t=1/2 (right-continuous)" },
            { presetWire ("PUMP", ""), 0.0, 0.9, "PUMP t=0" },
            { presetWire ("PUMP", ""), 0.5, 0.1 + 0.8 * 0.10539922456186433, "PUMP t=1/2 (0.1 + 0.8 e^-2.25)" },
            { presetWire ("STEPS 4", ""), 0.1, 0.9, "STEPS 4 step 0" },
            { presetWire ("STEPS 4", ""), 0.3, 0.9 - 0.8 / 3.0, "STEPS 4 step 1" },
            { presetWire ("STEPS 4", ""), 0.9, 0.1, "STEPS 4 step 3" },
            { presetWire ("STEPS 8", ""), 0.5, 0.9 - 0.8 * 4.0 / 7.0, "STEPS 8 step 4" },
            { presetWire ("STEPS 16", ""), 0.5, 0.9 - 0.8 * 8.0 / 15.0, "STEPS 16 step 8" },
            { presetWire ("SWING", ""), 0.0, 0.1, "SWING t=0" },
            { presetWire ("SWING", ""), 0.62, 0.9, "SWING peak at t=0.62" },
            { presetWire ("SWING", ""), 0.31, 0.1 + 0.8 * 0.35355339059327373, "SWING t=0.31 ((1/2)^1.5)" },
            { presetWire ("SWING", ""), 0.81, 0.6, "SWING t=0.81 (u = 0.625)" },
            { R"({"preset":"S&H NOISE","points":[],"bends":[],"table":[0.2,0.4,0.6,0.8],"drift":null})", 0.1, 0.8, "S&H: v = 1 - table[0]" },
            { R"({"preset":"S&H NOISE","points":[],"bends":[],"table":[0.2,0.4,0.6,0.8],"drift":null})", 0.6, 0.4, "S&H: v = 1 - table[2]" },
            { R"({"preset":"DRIFT","points":[],"bends":[],"table":null,"drift":{"a":0,"b":0}})", 0.25, 0.24, "DRIFT t=1/4 (v = 1 - (0.5 + 0.26))" },
            { R"({"preset":"DRIFT","points":[],"bends":[],"table":null,"drift":{"a":0,"b":0}})", 0.75, 0.76, "DRIFT t=3/4" },
            { R"({"preset":"DRIFT","points":[],"bends":[],"table":null,"drift":{"a":0,"b":0}})", 0.125, 1.0 - (0.5 + 0.26 * 0.7071067811865476 + 0.14), "DRIFT t=1/8" },
            // bends: SAW UP + bend (0.5, v 0.8): fn(0.5) = 0.5, dv = 0.3, anchors {0,0} {0.5,0.3} {1,0}
            { presetWire ("SAW UP", R"({"x":0.5,"v":0.8})"), 0.25, 0.3 + 0.3 * 0.5, "SAW UP + bend: t=1/4, smoothstep(0.5) = 0.5" },
            { presetWire ("SAW UP", R"({"x":0.5,"v":0.8})"), 0.5, 0.8, "SAW UP + bend: the bend point itself" },
            { presetWire ("SAW UP", R"({"x":0.5,"v":0.8})"), 0.75, 0.7 + 0.3 * 0.5, "SAW UP + bend: t=3/4" },
            // TRIANGLE + bend (0.25, v 0.9): fn(0.25) = 0.5, dv = 0.4; t = 1/8: fn = 0.3, u = 0.5
            { presetWire ("TRIANGLE", R"({"x":0.25,"v":0.9})"), 0.125, 0.3 + 0.4 * 0.5, "TRIANGLE + bend: t=1/8" },
            // CUSTOM exponent 2^(3c): one segment (0, v 0) -> (1, v 1) with c = 1/3 -> exponent 2 -> v = t^2
            { R"({"preset":"CUSTOM","points":[{"x":0,"v":0,"c":0.3333333333333333},{"x":1,"v":1,"c":0}],"bends":[],"table":null,"drift":null})",
              0.5, 0.25, "CUSTOM c = 1/3: exponent 2, v(1/2) = 1/4" },
            { R"({"preset":"CUSTOM","points":[{"x":0,"v":0,"c":0},{"x":0.5,"v":1,"c":0},{"x":1,"v":0,"c":0}],"bends":[],"table":null,"drift":null})",
              0.25, 0.5, "CUSTOM c = 0: straight segments" }
        };

        int bad = 0;

        for (const auto& a : anchors)
        {
            const auto table = tableOf (a.wire);
            const double got = (double) table->eval (a.t);

            if (std::abs (got - a.expect) > 1.0e-4)
            {
                ++bad;
                DETAIL ("        %s: got %.7f, expected %.7f\n", a.what, got, a.expect);
            }
        }

        check (bad == 0, "hand-derived anchors (34 values) match within 1e-4");

        // robustness of the parser
        warpr::LfoShapeDesc d;
        check (! warpr::LfoShapeDesc::fromJson ("", d) && ! warpr::LfoShapeDesc::fromJson ("[1,2]", d) && ! warpr::LfoShapeDesc::fromJson ("not json", d),
               "parser: empty / array / garbage documents are rejected");
        check (warpr::LfoShapeDesc::fromJson (R"({"preset":"WOBBLE","points":[{"x":0,"v":0.2},{"x":1,"v":0.8}]})", d)
                   && d.preset == warpr::LfoShapeDesc::Preset::Custom && d.points.size() == 2,
               "parser: an unknown preset id is drawn through the points (CUSTOM) like the UI does");
        check (warpr::LfoShapeDesc::fromJson (R"({"preset":"CUSTOM","points":[{"x":0.5,"v":0.5}]})", d) && d.points.size() == 5,
               "parser: a CUSTOM shape with fewer than two points gets the default points (index.html loadUiState)");
        check (warpr::LfoShapeDesc::fromJson (R"({"preset":"SINE","bends":[{"x":2,"v":-3}],"points":[{"x":"a"}]})", d)
                   && d.bends.size() == 1 && d.bends[0].x == 1.0 && d.bends[0].v == 0.0,
               "parser: out-of-range numbers are clamped, malformed points dropped");
    }

    //==========================================================================
    // 2. engine
    //==========================================================================
    struct Stepper
    {
        warpr::LfoInstance inst;
        warpr::LfoBlockParams p;
        const warpr::LfoShapeTable* table = nullptr;
        double fs = kFs, ppq = 0.0, bpm = 120.0, barBeats = 4.0;

        float step (int n)
        {
            warpr::LfoChunkTiming t;
            t.n = n;
            t.fs = fs;
            t.ppq0 = ppq;
            ppq += (double) n * bpm / (60.0 * fs);
            t.ppq1 = ppq;
            t.bpm = bpm;
            t.barBeats = barBeats;
            return warpr::advanceLfo (inst, p, table, t);
        }

        void run (int chunks, int n)
        {
            for (int i = 0; i < chunks; ++i)
                step (n);
        }
    };

    void testEngine()
    {
        std::printf ("\n[D8] LFO engine: rate mapping, directions, phase offset, ENV, slew, smooth\n");

        check (std::abs (warpr::lfoRateHz (0.0f) - 0.01) < 1.0e-12 && std::abs (warpr::lfoRateHz (1.0f) - 50.0) < 1.0e-9
                   && std::abs (warpr::lfoRateHz (0.5f) - 0.7071067811865476) < 1.0e-9,
               "Hz mapping: 0.01 * 5000^rate (0.01 Hz, 50 Hz, 0.7071 Hz at 0.5)");

        const double expectBeats[17] = { 32, 16, 8, 4, 2, 1.5, 1, 2.0 / 3.0, 0.75, 0.5, 1.0 / 3.0, 0.375, 0.25, 1.0 / 6.0, 0.125, 1.0 / 12.0, 0.0625 };
        int badDiv = 0;
        for (int i = 0; i < 17; ++i)
            if (std::abs (warpr::lfoBeatsPerCycle (i, 4.0) - expectBeats[i]) > 1.0e-12 || warpr::lfoDivisionIndex ((float) i / 16.0f) != i)
                ++badDiv;
        check (badDiv == 0 && std::abs (warpr::lfoBeatsPerCycle (0, 3.0) - 24.0) < 1.0e-12 && std::abs (warpr::lfoBeatsPerCycle (6, 3.0) - 1.0) < 1.0e-12,
               "17 sync divisions: beatsPerCycle table; the bar entries scale with the bar length (3/4: 8 bar = 24 beats)");

        const auto saw = tableOf (presetShapeJson ("SAW UP"));

        // Hz accumulation: 1 s of 32-sample chunks
        {
            Stepper s;
            s.table = saw.get();
            s.p.mode = 0; s.p.sync = 0; s.p.rate = 0.5f;
            s.run (1500, 32);
            check (std::abs (s.inst.progress - warpr::lfoRateHz (0.5f)) < 1.0e-9, "Hz mode: progress after 1 s = rate in Hz (cycles)");
        }

        // directions with an exact tempo-scaled step: 120 bpm, 1/4 (1 beat = 0.5 s); 24-sample chunks
        auto phaseAfter = [&] (int dir, double seconds, int mode = 1, float phaseDeg = 0.0f)
        {
            Stepper s;
            s.table = saw.get();
            s.p.mode = mode; s.p.dir = dir; s.p.sync = 1; s.p.rate = 6.0f / 16.0f; s.p.phaseDeg = phaseDeg;
            s.run ((int) std::lround (seconds * kFs / 24.0), 24);
            return (double) s.inst.phase;
        };

        check (std::abs (phaseAfter (0, 0.375) - 0.75) < 1.0e-6 && std::abs (phaseAfter (1, 0.375) - 0.25) < 1.0e-6,
               "Forward p / Reverse 1 - p (cycle 0.75 -> phase 0.75 / 0.25)");
        check (std::abs (phaseAfter (2, 0.375) - 0.75) < 1.0e-6 && std::abs (phaseAfter (2, 0.625) - 0.75) < 1.0e-6
                   && std::abs (phaseAfter (0, 0.625) - 0.25) < 1.0e-6 && std::abs (phaseAfter (1, 0.625) - 0.75) < 1.0e-6,
               "Ping-Pong: cycle 0 forward, cycle 1 backward (phase 0.75 at progress 1.25) - Forward / Reverse at 1.25 for contrast");
        check (std::abs (phaseAfter (0, 0.25, 1, 90.0f) - 0.75) < 1.0e-6, "phase offset 90 deg: start phase 0.25 (progress 0.5 -> 0.75)");

        // ENV: plays the shape once (rate = duration), then holds; Reverse ends at phase 0
        {
            Stepper f, r;
            f.table = r.table = saw.get();
            f.p.mode = r.p.mode = 2; f.p.sync = r.p.sync = 0; f.p.rate = r.p.rate = 1.0f;       // 50 Hz = 20 ms
            r.p.dir = 1;
            f.run (400, 32);
            r.run (400, 32);                                                                     // 267 ms
            const float f1 = f.inst.out, r1 = r.inst.out;
            f.run (400, 32);
            check (f.inst.envDone && std::abs (f.inst.phase - 1.0f) < 1.0e-6f && std::abs (f1 - 0.9f) < 1.0e-3f && std::abs (f.inst.out - f1) < 1.0e-6f,
                   "ENV Forward: runs to phase 1 and holds the last value (0.9) indefinitely");
            check (std::abs (r.inst.phase - 0.0f) < 1.0e-6f && std::abs (r1 - 0.1f) < 1.0e-3f, "ENV Reverse: ends at phase 0 (value 0.1)");

            Stepper o;
            o.table = saw.get();
            o.p.mode = 2; o.p.sync = 0; o.p.rate = 1.0f; o.p.phaseDeg = 180.0f;
            o.step (32);
            check (std::abs (o.inst.phase - (0.5f + 32.0f * 50.0f / 48000.0f)) < 1.0e-4f, "ENV with a phase offset starts at the offset");
        }

        // 2 ms slew: a step in the table value reaches 1 - 1/e after exactly 2 ms (3 chunks of 32 at 48 kHz)
        {
            const auto lo = tableOf (constShapeJson (0.0)), hi = tableOf (constShapeJson (1.0));
            Stepper s;
            s.p.mode = 0; s.p.sync = 0; s.p.rate = 0.5f;
            s.table = lo.get();
            s.step (32);                                        // fresh: snaps to 0
            s.table = hi.get();
            s.run (3, 32);
            check (std::abs (s.inst.out - (1.0f - std::exp (-1.0f))) < 1.0e-5f, "output slew: fixed 2 ms one-pole (0.632 after 2 ms)");
        }

        // SMOOTH: tau = smooth^2 * min(1 s, period / 2).  smooth 1, period 100 s -> tau 1 s.  The 2 ms slew is in series.
        {
            const auto lo = tableOf (constShapeJson (0.0)), hi = tableOf (constShapeJson (1.0));
            Stepper s;
            s.p.mode = 0; s.p.sync = 0; s.p.rate = 0.0f; s.p.smooth = 1.0f;     // 0.01 Hz = 100 s period
            s.table = lo.get();
            s.step (32);
            s.table = hi.get();
            s.run (1500, 32);                                   // 1 s
            check (std::abs (s.inst.out - 0.632f) < 0.01f, "SMOOTH 1: time constant min(1 s, period / 2) = 1 s (0.632 after 1 s)");

            Stepper q;
            q.p.mode = 0; q.p.sync = 0; q.p.rate = 0.0f; q.p.smooth = 0.5f;     // tau = 0.25 s
            q.table = lo.get();
            q.step (32);
            q.table = hi.get();
            q.run (375, 32);                                    // 0.25 s
            check (std::abs (q.inst.out - 0.632f) < 0.01f, "SMOOTH 0.5: tau = smooth^2 * 1 s = 0.25 s");
        }
    }

    //==========================================================================
    // 3. sync lock
    //==========================================================================
    struct LockResult { double relErr = 1.0; int wraps = 0; };

    /** FREE + Sync LFO driven by a TransportClock fed like a playing host; measures the cycle wraps of the shape phase. */
    LockResult measurePhasePeriod (double bpm, int div, int tsNum, int tsDen, double minSeconds)
    {
        warpr::TransportClock clock;
        clock.prepare (kFs);

        warpr::TransportInfo host;
        host.hasPpq = host.hasBpm = host.playing = true;
        host.bpm = bpm;
        host.timeSigNumerator = tsNum;
        host.timeSigDenominator = tsDen;

        warpr::LfoBlockParams p;
        p.mode = 0; p.dir = 0; p.sync = 1; p.rate = (float) div / 16.0f;
        warpr::LfoInstance inst;

        const double expected = warpr::lfoBeatsPerCycle (div, (double) tsNum * 4.0 / (double) tsDen) * 60.0 / bpm;
        const double totalSamples = std::max (minSeconds, 3.5 * expected) * kFs;
        double hostPpq = 0.0, prevPhase = -1.0, firstWrap = -1.0, lastWrap = -1.0;
        long long sample = 0;
        LockResult res;
        constexpr int block = 512;

        while ((double) sample < totalSamples)
        {
            host.ppq = hostPpq;
            clock.beginBlock (host, block);

            for (int off = 0; off < block; off += 32)
            {
                warpr::LfoChunkTiming t;
                t.n = 32; t.fs = kFs; t.ppq0 = clock.ppqAt (off); t.ppq1 = clock.ppqAt (off + 32);
                t.bpm = clock.bpm(); t.barBeats = clock.barBeats();
                warpr::advanceLfo (inst, p, nullptr, t);
                const double ph = (double) inst.phase;

                if (prevPhase >= 0.0 && ph < prevPhase)
                {
                    const double frac = (1.0 - prevPhase) / ((1.0 - prevPhase) + ph);
                    const double tw = ((double) (sample + off) + frac * 32.0) / kFs;
                    if (res.wraps == 0) firstWrap = tw;
                    lastWrap = tw;
                    ++res.wraps;
                }

                prevPhase = ph;
            }

            sample += block;
            hostPpq += (double) block * bpm / (60.0 * kFs);
        }

        if (res.wraps >= 3)
            res.relErr = std::abs ((lastWrap - firstWrap) / (double) (res.wraps - 1) / expected - 1.0);

        return res;
    }

    /** Same, but measured on the OUTPUT (SAW UP through the 2 ms slew): falling crossings of 0.5. */
    LockResult measureOutputPeriod (double bpm, int div, double minSeconds)
    {
        const auto saw = tableOf (presetShapeJson ("SAW UP"));
        warpr::TransportClock clock;
        clock.prepare (kFs);
        warpr::TransportInfo host;
        host.hasPpq = host.hasBpm = host.playing = true;
        host.bpm = bpm;

        warpr::LfoBlockParams p;
        p.mode = 0; p.dir = 0; p.sync = 1; p.rate = (float) div / 16.0f;
        warpr::LfoInstance inst;

        const double expected = warpr::lfoBeatsPerCycle (div, 4.0) * 60.0 / bpm;
        const double totalSamples = std::max (minSeconds, 3.5 * expected) * kFs;
        double hostPpq = 0.0, prev = -1.0, firstX = -1.0, lastX = -1.0;
        long long sample = 0;
        LockResult res;

        while ((double) sample < totalSamples)
        {
            host.ppq = hostPpq;
            clock.beginBlock (host, 512);

            for (int off = 0; off < 512; off += 32)
            {
                warpr::LfoChunkTiming t;
                t.n = 32; t.fs = kFs; t.ppq0 = clock.ppqAt (off); t.ppq1 = clock.ppqAt (off + 32);
                t.bpm = clock.bpm(); t.barBeats = clock.barBeats();
                const double y = (double) warpr::advanceLfo (inst, p, saw.get(), t);

                if (prev >= 0.5 && y < 0.5)                  // the shape only falls at the cycle wrap
                {
                    const double tx = ((double) (sample + off) + 32.0 * (prev - 0.5) / (prev - y)) / kFs;
                    if (res.wraps == 0) firstX = tx;
                    lastX = tx;
                    ++res.wraps;
                }

                prev = y;
            }

            sample += 512;
            hostPpq += 512.0 * bpm / (60.0 * kFs);
        }

        if (res.wraps >= 3)
            res.relErr = std::abs ((lastX - firstX) / (double) (res.wraps - 1) / expected - 1.0);

        return res;
    }

    void testSyncLock (bool full)
    {
        std::printf ("\n[D8] sync lock: measured LFO period vs beatsPerCycle (gate 0.01 %%)\n");

        std::vector<double> bpms = { 40.0, 97.3, 120.0, 300.0 };
        if (full)
            for (double b = 50.0; b < 300.0; b += 10.0)
                bpms.push_back (b);

        // HAND-DERIVED gate: 0.01 % = 1e-4.  The phase is an exact double function of the host ppq, so the wrap times are
        // exact to ~1e-9; the gate is the plan's number, not a measured floor.
        double worstPhase = 0.0;
        int bad = 0, cases = 0;

        for (double bpm : bpms)
            for (int div = 0; div < 17; ++div)
            {
                const auto r = measurePhasePeriod (bpm, div, 4, 4, 8.0);
                ++cases;
                worstPhase = std::max (worstPhase, r.relErr);

                if (! (r.relErr <= 1.0e-4) || r.wraps < 3)
                {
                    ++bad;
                    DETAIL ("        bpm %.1f div %d: relErr %.3g, %d wraps\n", bpm, div, r.relErr, r.wraps);
                }
            }

        std::printf ("        (%d bpm x division cases, worst phase-period error %.3g)\n", cases, worstPhase);
        check (bad == 0, "40-300 bpm x all 17 divisions: phase period within 0.01 % of beatsPerCycle");

        if (full)
        {
            int badSig = 0;
            for (int sig = 0; sig < 3; ++sig)
            {
                const int num = sig == 0 ? 3 : (sig == 1 ? 7 : 6), den = sig == 0 ? 4 : 8;
                for (int div = 0; div < 4; ++div)           // the bar-based divisions
                {
                    const auto r = measurePhasePeriod (120.0, div, num, den, 8.0);
                    if (! (r.relErr <= 1.0e-4) || r.wraps < 3) ++badSig;
                }
            }
            check (badSig == 0, "bar divisions follow the host time signature (3/4, 7/8, 6/8)");
        }

        // output-based: the same claim measured on the slewed table output; jitter <= 1 chunk (0.67 ms) per crossing,
        // so >= 20 s of signal keeps it below 1e-4 (HAND-DERIVED: 2 x 0.67 ms / 20 s = 6.7e-5)
        double worstOut = 0.0;
        int badOut = 0;
        for (double bpm : { 40.0, 120.0, 300.0 })
            for (int div : { 3, 6, 9, 12, 16 })
            {
                const auto r = measureOutputPeriod (bpm, div, 20.0);
                worstOut = std::max (worstOut, r.relErr);
                if (! (r.relErr <= 1.0e-4) || r.wraps < 3)
                {
                    ++badOut;
                    DETAIL ("        output: bpm %.0f div %d: relErr %.3g, %d crossings\n", bpm, div, r.relErr, r.wraps);
                }
            }
        std::printf ("        (output-based worst %.3g)\n", worstOut);
        check (badOut == 0, "output-based period (SAW UP through the slew) within 0.01 % at 40 / 120 / 300 bpm");
    }

    //==========================================================================
    // transport clock + relock
    //==========================================================================
    void testTransportClock()
    {
        std::printf ("\n[D8] TransportClock: relock after jumps / loops / stop-start, internal clock\n");

        warpr::TransportClock clock;
        clock.prepare (kFs);
        warpr::TransportInfo host;
        host.hasPpq = host.hasBpm = host.playing = true;
        host.bpm = 120.0;

        warpr::LfoBlockParams p;
        p.mode = 0; p.sync = 1; p.rate = 6.0f / 16.0f;            // FREE, 1/4 = 1 beat
        warpr::LfoInstance inst;
        const auto saw = tableOf (presetShapeJson ("SAW UP"));

        auto blockAt = [&] (double ppq, bool playing, int numSamples)
        {
            host.ppq = ppq;
            host.playing = playing;
            clock.beginBlock (host, numSamples);
            warpr::LfoChunkTiming t;
            t.n = 32; t.fs = kFs; t.ppq0 = clock.ppqAt (numSamples - 32); t.ppq1 = clock.ppqAt (numSamples);
            t.bpm = clock.bpm(); t.barBeats = clock.barBeats();
            warpr::advanceLfo (inst, p, saw.get(), t);
            return std::pair<double, double> { t.ppq1, (double) inst.phase };
        };

        // grid lock: phase == frac(ppq) at the end of the block, for arbitrary host positions
        double worst = 0.0;
        for (double ppq : { 0.0, 3.3, 1234.567, 7.1, 16.0, 15.999, 100000.25 })
        {
            const auto r = blockAt (ppq, true, 512);
            worst = std::max (worst, std::abs (r.second - (r.first - std::floor (r.first))));
        }
        std::printf ("        (worst |phase - frac(ppq)| after jumps / loops: %.3g)\n", worst);
        check (worst < 1.0e-6, "FREE + Sync relocks to the host ppq after every jump (forward, backward, loop start)");

        // stop: the internal clock continues from the last position at the last tempo; start relocks
        const auto a = blockAt (50.0, true, 512);
        const auto b = blockAt(50.0 + 512.0 / 24000.0, false, 512);           // stopped: host ppq frozen at some value
        const auto c = blockAt(50.0 + 512.0 / 24000.0, false, 512);
        check (std::abs (b.first - (a.first + 512.0 / 24000.0)) < 1.0e-9 && std::abs (c.first - (b.first + 512.0 / 24000.0)) < 1.0e-9,
               "stopped transport: the internal clock keeps running at the last tempo (continuous ppq)");
        const auto d = blockAt (200.0, true, 512);
        check (std::abs (d.first - (200.0 + 512.0 / 24000.0)) < 1.0e-9, "start: the next block relocks to the host position");

        // no playhead at all: 120 bpm default, from ppq 0
        warpr::TransportClock bare;
        bare.prepare (kFs);
        bare.beginBlock (warpr::TransportInfo {}, 480);
        bare.beginBlock (warpr::TransportInfo {}, 480);
        check (std::abs (bare.ppqAt (0) - 480.0 / 24000.0) < 1.0e-12 && std::abs (bare.bpm() - 120.0) < 1.0e-12 && std::abs (bare.barBeats() - 4.0) < 1.0e-12,
               "no playhead: internal clock at 120 bpm, 4/4");

        // tempo change between blocks
        host.bpm = 90.0;
        const auto e = blockAt (10.0, true, 480);
        check (std::abs (clock.bpm() - 90.0) < 1.0e-12 && std::abs (e.first - (10.0 + 480.0 * 90.0 / (60.0 * kFs))) < 1.0e-9,
               "tempo changes are taken at the next block");
    }

    //==========================================================================
    // 4. matrix math
    //==========================================================================
    void testMatrixMath()
    {
        std::printf ("\n[D8] matrix math: norm' = clamp(norm + depth (2v-1), 0, 1) in the skewed domain\n");

        auto proc = std::make_unique<PluginProcessor>();
        int mismatches = 0, checked = 0;
        double worst = 0.0;

        for (std::size_t i = 0; i < warpr::kNumParams; ++i)
        {
            const auto& m = warpr::kParamMeta[i];

            if (m.kind != warpr::ParamKind::Float && m.kind != warpr::ParamKind::Int)
                continue;

            auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (proc->getParameters()[(int) i]);
            if (ranged == nullptr)
            {
                ++mismatches;
                continue;
            }

            const auto& nr = ranged->getNormalisableRange();
            const auto& r = warpr::paramRanges()[i];

            for (int k = 0; k <= 20; ++k)
            {
                const float v = r.lo + (r.hi - r.lo) * (float) k / 20.0f;

                for (float delta : { -0.7f, -0.2f, 0.05f, 0.3f, 0.9f })
                {
                    float expect = nr.convertFrom0to1 (std::min (1.0f, std::max (0.0f, nr.convertTo0to1 (v) + delta)));
                    if (m.kind == warpr::ParamKind::Int)
                        expect = std::min (r.hi, std::max (r.lo, std::floor (expect + 0.5f)));

                    const float got = warpr::applyModulation (r, v, delta);
                    const double err = std::abs ((double) got - (double) expect) / (double) (r.hi - r.lo);
                    worst = std::max (worst, err);
                    ++checked;

                    if (err > 1.0e-5)
                    {
                        ++mismatches;
                        DETAIL ("        %s v=%g delta=%g: got %g expect %g\n", m.id, (double) v, (double) delta, (double) got, (double) expect);
                    }
                }
            }
        }

        std::printf ("        (%d values over 67 Float/Int parameters, worst relative-to-range error %.3g)\n", checked, worst);
        check (mismatches == 0, "applyModulation == juce::NormalisableRange convertFrom0to1(clamp(convertTo0to1 + delta)) for every Float / Int parameter");

        // closed-form spot checks (skewed domain): cutoff centre 1000 Hz at norm 0.5
        const auto& cut = warpr::paramRange (warpr::ParamId::filterCutoff);
        check (std::abs (cut.to01 (1000.0f) - 0.5f) < 1.0e-4f, "filterCutoff: 1 kHz sits at norm 0.5 (skew centre)");

        const std::vector<warpr::ModMatrixSnapshot::RouteIn> routes = {
            { 1, warpr::ParamId::filterCutoff, 0.5f },
            { 2, warpr::ParamId::filterCutoff, 0.25f },
            { 1, warpr::ParamId::transpose, 0.3f }
        };
        const auto mm = warpr::ModMatrixSnapshot::compile (routes.data(), (int) routes.size());
        float d0[warpr::kNumParams], d1[warpr::kNumParams];
        std::array<float, 10> s {}, e {};
        s.fill (0.5f);
        e.fill (0.5f);
        s[0] = 1.0f; e[0] = 0.0f;              // LFO 1: +1 -> -1
        s[1] = 0.0f; e[1] = 0.75f;             // LFO 2: -1 -> +0.5
        mm->evaluate (s.data(), e.data(), 10, d0, d1);

        const int iCut = (int) warpr::toIndex (warpr::ParamId::filterCutoff), iTr = (int) warpr::toIndex (warpr::ParamId::transpose);
        check (std::abs (d0[iCut] - (0.5f * 1.0f + 0.25f * -1.0f)) < 1.0e-6f && std::abs (d1[iCut] - (0.5f * -1.0f + 0.25f * 0.5f)) < 1.0e-6f
                   && std::abs (d0[iTr] - 0.3f) < 1.0e-6f && std::abs (d1[iTr] + 0.3f) < 1.0e-6f,
               "routes to one destination sum their depth * (2v - 1) before the clamp; start / end deltas are separate");

        const float up = warpr::applyModulation (warpr::ParamId::filterCutoff, 1000.0f, 0.25f);
        check (std::abs (up - cut.from01 (0.75f)) < 1.0e-2f && up > 4000.0f && up < 6000.0f,
               "filterCutoff 1 kHz + depth 0.25 -> norm 0.75 -> the skewed value (about 5.7 kHz), NOT 1 kHz + 25 % of the Hz range");
        check (warpr::applyModulation (warpr::ParamId::filterCutoff, 20000.0f, 0.5f) == 20000.0f
                   && warpr::applyModulation (warpr::ParamId::filterCutoff, 20.0f, -0.5f) == 20.0f
                   && warpr::applyModulation (warpr::ParamId::filterCutoff, 1000.0f, 0.0f) == 1000.0f,
               "clamp at both ends of the range; delta 0 is a bit-exact bypass");

        // Int rounding: transpose -24..24, base 0 (norm 0.5)
        check (warpr::applyModulation (warpr::ParamId::transpose, 0.0f, 0.3f) == 14.0f && warpr::applyModulation (warpr::ParamId::transpose, 0.0f, -0.3f) == -14.0f
                   && warpr::applyModulation (warpr::ParamId::transpose, 0.0f, 0.005f) == 0.0f,
               "Int destinations round to nearest after the conversion (14.4 -> 14, -14.4 -> -14, 0.24 -> 0)");
        check (warpr::applyModulation (warpr::ParamId::unisonVoices, 4.0f, 0.4f) == 7.0f && warpr::applyModulation (warpr::ParamId::unisonVoices, 4.0f, -0.4f) == 1.0f,
               "unisonVoices 4 +- 0.4: 6.8 -> 7, 1.2 -> 1 (the value can step - documented)");
    }

    void testCompileAndClasses()
    {
        std::printf ("\n[D8] matrix compile rules and destination classes\n");

        const auto mm = warpr::ModMatrixSnapshot::fromJson (
            R"([{"src":1,"dst":"filterCutoff","depth":0.5},
                {"src":2,"dst":"warpMode","depth":0.5},
                {"src":3,"dst":"warpEnable","depth":0.5},
                {"src":4,"dst":"doesNotExist","depth":0.5},
                {"src":0,"dst":"filterMix","depth":0.5},
                {"src":11,"dst":"filterMix","depth":0.5},
                {"src":5,"dst":"filterMix","depth":0},
                {"src":6,"dst":"filterMix","depth":-0.5},
                {"src":7,"dst":"masterGain","depth":4},
                {"src":1,"dst":"filterCutoff","depth":0.25},
                {"src":2.5,"dst":"filterMix","depth":0.5},
                {"src":"1","dst":"filterMix","depth":0.5},
                {"dst":"filterMix","depth":0.5},
                42,
                {"src":8,"dst":"lfo3Rate","depth":1}])");

        check (mm->getNumRoutes() == 4, "compile drops Choice / Bool / unknown destinations, src outside 1..10, depth <= 0 and malformed entries");
        check (mm->hasDestination (warpr::ParamId::filterCutoff) && mm->hasDestination (warpr::ParamId::masterGain) && mm->hasDestination (warpr::ParamId::lfo3Rate)
                   && ! mm->hasDestination (warpr::ParamId::warpMode) && ! mm->hasDestination (warpr::ParamId::filterMix),
               "kept destinations: filterCutoff (x2), masterGain, lfo3Rate");
        check (mm->usedSourceMask() == ((1u << 0) | (1u << 6) | (1u << 7)) && mm->lfoDestinationMask() == (1u << 2),
               "used-source mask {1, 7, 8}; LFO-on-LFO destination mask {LFO 3}");

        float srcMod[10] = {};
        srcMod[6] = 1.0f;
        check (std::abs (mm->delta (warpr::ParamId::masterGain, srcMod) - 1.0f) < 1.0e-6f, "depth > 1 is clamped to 1");

        // duplicate routes are kept and summed; capped at 256
        std::vector<warpr::ModMatrixSnapshot::RouteIn> many (300, { 1, warpr::ParamId::fineTune, 0.01f });
        const auto capped = warpr::ModMatrixSnapshot::compile (many.data(), (int) many.size());
        check (capped->getNumRoutes() == 256, "at most 256 routes are kept");
        check (warpr::ModMatrixSnapshot::fromJson ("")->empty() && warpr::ModMatrixSnapshot::fromJson ("{}")->empty()
                   && warpr::ModMatrixSnapshot::fromJson ("garbage")->empty(),
               "empty / non-array / garbage documents compile to an empty matrix");

        using warpr::ModClass;
        using warpr::ParamId;
        check (warpr::modClassOf (ParamId::polyphony) == ModClass::Latched && warpr::modClassOf (ParamId::unisonVoices) == ModClass::Latched
                   && warpr::modClassOf (ParamId::glideTime) == ModClass::Latched && warpr::modClassOf (ParamId::masterGain) == ModClass::Global
                   && warpr::modClassOf (ParamId::filterCutoff) == ModClass::Voice && warpr::modClassOf (ParamId::lfo4Smooth) == ModClass::Voice
                   && warpr::modClassOf (ParamId::warpMode) == ModClass::None && warpr::modClassOf (ParamId::lfo1GridX) == ModClass::None
                   && warpr::modClassOf (ParamId::grainScanLoop) == ModClass::None,
               "destination classes: V (voice), G (masterGain), N (latched: polyphony, unisonVoices, glideTime), none (Choice / Bool)");

        int nV = 0, nG = 0, nN = 0, nNone = 0;
        for (std::size_t i = 0; i < warpr::kNumParams; ++i)
            switch (warpr::modClassOf ((ParamId) i))
            {
                case ModClass::Voice:   ++nV; break;
                case ModClass::Global:  ++nG; break;
                case ModClass::Latched: ++nN; break;
                case ModClass::None:
                default:                ++nNone; break;
            }
        std::printf ("        (classes over the 135 parameters: V %d, G %d, N %d, none %d)\n", nV, nG, nN, nNone);
        check (nV + nG + nN == 62 + 5 && nG == 1 && nN == 3, "every Float (62) and Int (5) parameter is a destination: 63 V + 1 G + 3 N");
    }

    //==========================================================================
    // 5. LFO-on-LFO
    //==========================================================================
    void testLfoOnLfo()
    {
        std::printf ("\n[D8] LFO-on-LFO: index order, one chunk of delay for sources at or above the destination index\n");

        const auto hiTable = tableOf (constShapeJson (1.0));
        const auto loTable = tableOf (constShapeJson (0.0));

        // LFO1 = constant +1, LFO2 = constant -1; LFO1 -> lfo2Rate (src 1 < dest 2: same chunk), LFO2 -> lfo1Rate (src 2 > dest 1: delayed)
        const std::vector<warpr::ModMatrixSnapshot::RouteIn> routes = {
            { 1, warpr::ParamId::lfo2Rate, 1.0f },
            { 2, warpr::ParamId::lfo1Rate, 1.0f }
        };
        const auto mm = warpr::ModMatrixSnapshot::compile (routes.data(), 2);

        std::array<warpr::LfoBlockParams, 10> params {};
        for (auto& p : params)
        {
            p.mode = 0; p.sync = 0; p.rate = 0.5f;
        }

        std::array<const warpr::LfoShapeTable*, 10> tables {};
        tables[0] = hiTable.get();
        tables[1] = loTable.get();

        std::array<warpr::LfoInstance, 10> inst {};
        std::array<float, 10> prev, start, end, ext;
        prev.fill (0.5f);
        ext.fill (0.5f);

        warpr::LfoBankIO io;
        io.params = params.data();
        io.tables = tables.data();
        io.mm = mm.get();
        io.lfoCount = 2;
        io.timing.n = 32; io.timing.fs = kFs;
        io.ownMask = 0x3u;
        io.extStart = io.extEnd = ext.data();

        warpr::advanceLfoBank (io, inst.data(), prev.data(), start.data(), end.data());

        const double hz = warpr::lfoRateHz (0.5f), hzMax = warpr::lfoRateHz (1.0f), hzMin = warpr::lfoRateHz (0.0f);
        const double step1 = 32.0 / kFs;

        // chunk 1: LFO2 sees LFO1's NEW value (+1 -> rate norm 1.0 -> 50 Hz); LFO1 sees LFO2's previous value (0.5 -> mod 0 -> unchanged)
        check (std::abs (inst[1].progress - step1 * hzMax) < 1.0e-12 && std::abs (inst[0].progress - step1 * hz) < 1.0e-12,
               "chunk 1: the lower-index source acts in the SAME chunk (LFO 2 at 50 Hz), the higher-index source is one chunk late (LFO 1 unchanged)");

        warpr::advanceLfoBank (io, inst.data(), prev.data(), start.data(), end.data());

        // chunk 2: LFO1 now sees LFO2's value of chunk 1 (= 0 -> mod -1 -> rate norm 0 -> 0.01 Hz)
        check (std::abs (inst[0].progress - (step1 * hz + step1 * hzMin)) < 1.0e-12,
               "chunk 2: the delayed source arrives (LFO 1 drops to 0.01 Hz): deterministic one-chunk delay");

        // a source the bank does not own is read from ext (the global bank sees the voice LFOs as a mean)
        std::array<warpr::LfoInstance, 10> inst2 {};
        std::array<float, 10> prev2, s2, e2, ext2;
        prev2.fill (0.5f);
        ext2.fill (0.5f);
        ext2[0] = 1.0f;                                // LFO 1 belongs to the voices; its mean is +1
        io.ownMask = 0x2u;
        io.extStart = io.extEnd = ext2.data();
        warpr::advanceLfoBank (io, inst2.data(), prev2.data(), s2.data(), e2.data());
        check (std::abs (inst2[1].progress - step1 * hzMax) < 1.0e-12 && inst2[0].progress == 0.0 && e2[0] == 1.0f,
               "a bank only advances the LFOs it owns; the others contribute their external value");
    }

    //==========================================================================
    // 6a. zipper bound (class level)
    //==========================================================================
    void testZipperClassLevel()
    {
        std::printf ("\n[D8] no zipper: step of a modulated value per chunk\n");

        const auto sine = tableOf (presetShapeJson ("SINE"));
        const std::vector<warpr::ModMatrixSnapshot::RouteIn> routes = { { 1, warpr::ParamId::warpAmount, 1.0f }, { 1, warpr::ParamId::filterCutoff, 1.0f } };
        const auto mm = warpr::ModMatrixSnapshot::compile (routes.data(), 2);

        warpr::LfoBlockParams p;
        p.mode = 0; p.sync = 0; p.rate = 1.0f;                        // 50 Hz, the fastest LFO
        std::array<warpr::LfoBlockParams, 10> params;
        params.fill (p);
        std::array<const warpr::LfoShapeTable*, 10> tables {};
        tables[0] = sine.get();

        std::array<warpr::LfoInstance, 10> inst {};
        std::array<float, 10> prev, start, end, ext;
        prev.fill (0.5f);
        ext.fill (0.5f);

        warpr::LfoBankIO io;
        io.params = params.data(); io.tables = tables.data(); io.mm = mm.get(); io.lfoCount = 1;
        io.timing.n = 32; io.timing.fs = kFs; io.ownMask = 1u;
        io.extStart = io.extEnd = ext.data();

        float d0[warpr::kNumParams], d1[warpr::kNumParams];
        float lastAmt = -1.0f, lastCut = -1.0f, worstAmt = 0.0f, worstCutNorm = 0.0f, worstDelta = 0.0f;

        for (int c = 0; c < 4000; ++c)
        {
            warpr::advanceLfoBank (io, inst.data(), prev.data(), start.data(), end.data());
            mm->evaluate (start.data(), end.data(), 1, d0, d1);

            const float amt = warpr::applyModulation (warpr::ParamId::warpAmount, 0.5f, d1[(int) warpr::toIndex (warpr::ParamId::warpAmount)]);
            const float cut = warpr::applyModulation (warpr::ParamId::filterCutoff, 1000.0f, d1[(int) warpr::toIndex (warpr::ParamId::filterCutoff)]);

            if (lastAmt >= 0.0f && c > 3)
            {
                worstAmt = std::max (worstAmt, std::abs (amt - lastAmt));
                worstCutNorm = std::max (worstCutNorm, std::abs (warpr::paramRange (warpr::ParamId::filterCutoff).to01 (cut)
                                                                 - warpr::paramRange (warpr::ParamId::filterCutoff).to01 (lastCut)));
                worstDelta = std::max (worstDelta, std::abs (d1[(int) warpr::toIndex (warpr::ParamId::warpAmount)] - (d0[(int) warpr::toIndex (warpr::ParamId::warpAmount)])));
            }

            lastAmt = amt;
            lastCut = cut;
        }

        // HAND-DERIVED: mod = 2v - 1 has amplitude 0.8 (SINE spans 0.1..0.9); at 50 Hz a 32-sample chunk advances
        // 50 * 32 / 48000 = 1/30 cycle, so the steepest step is 0.8 * 2 pi / 30 = 0.1676 (the 2 ms slew only lowers it)
        const float bound = 0.8f * 2.0f * (float) kPi / 30.0f;
        std::printf ("        (max per-chunk step: value %.4f, cutoff norm %.4f, delta start->end %.4f; bound %.4f)\n",
                     (double) worstAmt, (double) worstCutNorm, (double) worstDelta, (double) bound);
        check (worstAmt <= bound * 1.001f && worstCutNorm <= bound * 1.001f && worstDelta <= bound * 1.001f,
               "a 50 Hz sine LFO at depth 1 moves a destination by at most 0.8 * 2 pi / 30 per 32-sample chunk");
    }

    //==========================================================================
    // processor-level fixtures
    //==========================================================================
    struct Rig
    {
        std::unique_ptr<PluginProcessor> p = std::make_unique<PluginProcessor>();
        juce::AudioBuffer<float> buf { 2, 256 };
        int blockSize = 256;
        double fs = kFs;

        explicit Rig (double sampleRate = kFs, int bs = 256) : blockSize (bs), fs (sampleRate)
        {
            buf.setSize (2, bs);
            p->setPlayConfigDetails (0, 2, sampleRate, bs);
            p->prepareToPlay (sampleRate, bs);
        }

        ~Rig() { p->setPlayHead (nullptr); }

        void block (juce::MidiBuffer& midi)
        {
            buf.clear();
            p->processBlock (buf, midi);
        }

        void run (int blocks)
        {
            juce::MidiBuffer m;
            for (int i = 0; i < blocks; ++i)
            {
                m.clear();
                block (m);
            }
        }

        /** One block whose first event (sample 0) is the note. */
        void noteOnBlock (int note, float vel = 0.8f)
        {
            juce::MidiBuffer m;
            m.addEvent (juce::MidiMessage::noteOn (1, note, vel), 0);
            block (m);
        }

        void noteOffBlock (int note)
        {
            juce::MidiBuffer m;
            m.addEvent (juce::MidiMessage::noteOff (1, note), 0);
            block (m);
        }

        void param (const char* id, float real)
        {
            if (auto* prm = p->apvts.getParameter (id))
                prm->setValueNotifyingHost (prm->convertTo0to1 (real));
            else
            {
                std::printf ("  FAIL  unknown parameter '%s'\n", id);
                ++failures;
            }
        }

        void lfo (int n, const juce::String& shape, int mode, int dir, int sync, float rate, float phaseDeg = 0.0f, float smooth = 0.0f)
        {
            const juce::String id = "lfo" + juce::String (n);
            param ((id + "Mode").toRawUTF8(), (float) mode);
            param ((id + "Dir").toRawUTF8(), (float) dir);
            param ((id + "Sync").toRawUTF8(), (float) sync);
            param ((id + "Rate").toRawUTF8(), rate);
            param ((id + "Phase").toRawUTF8(), phaseDeg);
            param ((id + "Smooth").toRawUTF8(), smooth);
            p->getBackendImpl().setLfoShapeJson (n - 1, shape);
        }

        void matrix (const juce::String& json) { p->getBackendImpl().setModMatrixJson (json); }

        int slotOfNote (int note) const
        {
            for (int s = 0; s < warpr::WarprSynth::totalSlots; ++s)
            {
                const auto info = p->getSynth().getVoiceInfo (s);
                if (info.active && ! info.ghost && info.note == note)
                    return s;
            }
            return -1;
        }
    };

    juce::String route (int src, const char* dst, double depth)
    {
        return juce::String::formatted (R"({"src":%d,"dst":"%s","depth":%.6f})", src, dst, depth);
    }

    juce::String matrixOf (std::initializer_list<juce::String> routes)
    {
        juce::String out = "[";
        bool first = true;
        for (const auto& r : routes)
        {
            out << (first ? "" : ",") << r;
            first = false;
        }
        return out + "]";
    }

    //==========================================================================
    // 6b. processor-level zipper bound
    //==========================================================================
    std::vector<float> renderLeft (Rig& rig, double seconds, int note, double noteSeconds)
    {
        const int total = (int) std::lround (seconds * rig.fs);
        std::vector<float> out;
        out.reserve ((size_t) total);

        for (int pos = 0; pos < total; pos += rig.blockSize)
        {
            juce::MidiBuffer m;
            if (pos == 0)
                m.addEvent (juce::MidiMessage::noteOn (1, note, 0.8f), 0);
            if (pos <= (int) (noteSeconds * rig.fs) && (int) (noteSeconds * rig.fs) < pos + rig.blockSize)
                m.addEvent (juce::MidiMessage::noteOff (1, note), (int) (noteSeconds * rig.fs) - pos);

            rig.block (m);
            out.insert (out.end(), rig.buf.getReadPointer (0), rig.buf.getReadPointer (0) + rig.blockSize);
        }

        return out;
    }

    float maxStepOf (const std::vector<float>& x, size_t from)
    {
        float step = 0.0f;
        for (size_t i = from + 1; i < x.size(); ++i)
            step = std::max (step, std::abs (x[i] - x[i - 1]));
        return step;
    }

    void testZipperProcessor()
    {
        std::printf ("\n[D8] processor-level step bound: a hard-stepping LFO on the cutoff does not click\n");

        auto setup = [] (Rig& r, bool isOpen)
        {
            r.param ("filterEnable", 1.0f);
            r.param ("filterType", 3.0f);                       // LP 24
            r.param ("filterCutoff", isOpen ? 20000.0f : 2000.0f);
            r.param ("oversampling", 0.0f);
            r.param ("ampSustain", 1.0f);
        };

        Rig fixedRig, openRig, modRig;
        setup (fixedRig, false);
        setup (openRig, true);
        setup (modRig, false);

        modRig.lfo (1, presetShapeJson ("SQUARE"), 0, 0, 0, 0.6f);              // ~2.5 Hz hard steps
        modRig.matrix (matrixOf ({ route (1, "filterCutoff", 0.4) }));

        const auto yRef = renderLeft (fixedRig, 1.0, 36, 2.0);
        const auto yOpen = renderLeft (openRig, 1.0, 36, 2.0);
        const auto yMod = renderLeft (modRig, 1.0, 36, 2.0);

        const size_t from = (size_t) (0.1 * kFs);
        const float sRef = maxStepOf (yRef, from), sOpen = maxStepOf (yOpen, from), sMod = maxStepOf (yMod, from);
        const auto scMod = warpr::tests::scan (yMod.data(), (int) yMod.size());

        // HAND-DERIVED: a 24 dB/oct low-pass cannot make a larger sample step than the unfiltered (open) signal; a hard
        // cutoff step passes through the 2 ms slew, the chunk interpolation and the filter's own smoothing, so the modulated
        // step stays under the open-filter step (bound has 10 % margin for the sample-rate-dependent filter ripple)
        std::printf ("        (max step: cutoff 2 kHz fixed %.4f, open filter %.4f, square-modulated %.4f)\n", (double) sRef, (double) sOpen, (double) sMod);
        check (scMod.allFinite && sMod <= sOpen * 1.1f && sMod > 0.0f, "square LFO on the cutoff: no step beyond the open-filter step (no zipper)");
        check (! std::equal (yMod.begin(), yMod.end(), yRef.begin()), "the modulated render differs from the fixed-cutoff render (the route is live)");
    }

    //==========================================================================
    // 7. mode semantics through the processor
    //==========================================================================
    void testModeSemantics()
    {
        std::printf ("\n[D8] mode semantics (A1 assumptions): FREE / RETRIG / ENV / MONO, viz phases, lfoCount\n");

        const double hz = warpr::lfoRateHz (0.5f);
        const double perBlock = 256.0 * hz / kFs;                 // cycles per 256-sample block at rate 0.5
        auto approx = [] (double a, double b) { return std::abs (a - b) < 1.0e-9; };

        // ---- RETRIG: per voice, reset at note-on to the phase offset, independent progress
        {
            Rig rig;
            rig.lfo (1, presetShapeJson ("SAW UP"), 1, 0, 0, 0.5f, 90.0f);
            rig.matrix (matrixOf ({ route (1, "grainShape", 0.5) }));
            rig.run (2);
            rig.noteOnBlock (60);
            rig.run (39);                                          // note A: 40 blocks of life
            rig.noteOnBlock (64);
            rig.run (9);                                           // note B: 10 blocks of life

            const int a = rig.slotOfNote (60), b = rig.slotOfNote (64);
            auto& syn = rig.p->getSynth();
            check (a >= 0 && b >= 0 && a != b, "two sounding voices found");

            if (a >= 0 && b >= 0)
            {
                // Note A stays SOUNDING through note B's 10 blocks (no note-off), so its retriggered
                // LFO has run 50 blocks when we look: noteOn + 39 + (B's noteOn block) + 9.  The first
                // expectation here said 40 by forgetting that overlap; the measured value was exactly
                // 50 blocks.  Independence is proven by B sitting at exactly 10 while A is at 50.
                const double pa0 = 0.25 + 50.0 * perBlock, pb0 = 0.25 + 10.0 * perBlock;
                std::printf ("        RETRIG progress  A: measured %.9f expected %.9f   B: measured %.9f expected %.9f   (1 block = %.9f)\n",
                             syn.getVoiceLfoProgress (a, 0), 50.0 * perBlock, syn.getVoiceLfoProgress (b, 0), 10.0 * perBlock, perBlock);
                std::printf ("        RETRIG phase     A: measured %.6f expected %.6f   B: measured %.6f expected %.6f\n",
                             (double) syn.getVoiceLfoPhase (a, 0), pa0 - std::floor (pa0), (double) syn.getVoiceLfoPhase (b, 0), pb0 - std::floor (pb0));
                check (approx (syn.getVoiceLfoProgress (a, 0), 50.0 * perBlock) && approx (syn.getVoiceLfoProgress (b, 0), 10.0 * perBlock),
                       "RETRIG: every voice runs its own LFO from its own note-on (50 vs 10 blocks of progress)");
                check (std::abs ((double) syn.getVoiceLfoPhase (a, 0) - (pa0 - std::floor (pa0))) < 1.0e-5
                           && std::abs ((double) syn.getVoiceLfoPhase (b, 0) - (pb0 - std::floor (pb0))) < 1.0e-5,
                       "RETRIG: the phase starts at the phase parameter (90 deg) and advances from there");
            }

            float ph[10];
            syn.readLfoPhases (ph);
            check (ph[0] >= 0.0f && ph[1] < 0.0f && ph[9] < 0.0f, "viz: a used voice LFO reports the newest voice's phase, unused LFOs -1");

            // a Mono-style retrigger is not tested here (Poly); a new note after release starts at the offset again
            rig.noteOffBlock (60);
            rig.noteOffBlock (64);
        }

        // ---- FREE: one global instance, never reset by notes, runs without any note
        {
            Rig rig;
            rig.lfo (1, presetShapeJson ("SAW UP"), 0, 0, 0, 0.5f);
            rig.matrix (matrixOf ({ route (1, "grainShape", 0.5) }));
            rig.run (10);
            auto& syn = rig.p->getSynth();
            check (approx (syn.getGlobalLfoProgress (0), 10.0 * perBlock), "FREE: runs with no note playing");
            rig.noteOnBlock (60);
            rig.run (9);
            check (approx (syn.getGlobalLfoProgress (0), 20.0 * perBlock), "FREE: a note-on does not reset it");

            float ph[10];
            syn.readLfoPhases (ph);
            check (ph[0] >= 0.0f && std::abs ((double) ph[0] - (20.0 * perBlock - std::floor (20.0 * perBlock))) < 1.0e-5,
                   "viz: the global LFO reports its own phase");
        }

        // ---- ENV: plays once, then holds; release does not restart it
        {
            Rig rig;
            rig.lfo (1, presetShapeJson ("SAW UP"), 2, 0, 0, 1.0f);              // 50 Hz = 20 ms
            rig.matrix (matrixOf ({ route (1, "grainShape", 0.5) }));
            rig.run (2);
            rig.noteOnBlock (60);
            rig.run (8);                                                          // 9 blocks = 48 ms (shape done after 20 ms, slew settled)
            const int s = rig.slotOfNote (60);
            auto& syn = rig.p->getSynth();
            const float v1 = syn.getVoiceLfoValue (s, 0);
            check (s >= 0 && std::abs (syn.getVoiceLfoPhase (s, 0) - 1.0f) < 1.0e-6f && std::abs (v1 - 0.9f) < 2.0e-3f,
                   "ENV: reached the end of the shape and holds its last value (0.9)");
            rig.noteOffBlock (60);
            rig.run (1);
            check (std::abs (syn.getVoiceLfoPhase (s, 0) - 1.0f) < 1.0e-6f && std::abs (syn.getVoiceLfoValue (s, 0) - v1) < 1.0e-4f,
                   "ENV: note-off does not restart it");
        }

        // ---- MONO: one shared instance, reset when a phrase starts
        {
            Rig rig;
            rig.lfo (1, presetShapeJson ("SAW UP"), 3, 0, 0, 0.5f);
            rig.matrix (matrixOf ({ route (1, "grainShape", 0.5) }));
            rig.run (20);
            auto& syn = rig.p->getSynth();
            check (approx (syn.getGlobalLfoProgress (0), 20.0 * perBlock), "MONO: runs before any note");
            rig.noteOnBlock (60);
            check (approx (syn.getGlobalLfoProgress (0), 1.0 * perBlock), "MONO: the first note of a phrase resets it");
            rig.run (9);
            rig.noteOnBlock (64);                                               // 60 still held: same phrase
            check (approx (syn.getGlobalLfoProgress (0), 11.0 * perBlock), "MONO: a second held note continues the phrase (no reset)");
            rig.noteOffBlock (60);
            rig.noteOffBlock (64);
            rig.run (3);
            rig.noteOnBlock (67);
            check (approx (syn.getGlobalLfoProgress (0), 1.0 * perBlock), "MONO: after every key was released the next note starts a new phrase");
        }

        // ---- lfoCount: LFOs at or above it are idle (viz -1) and their routes are ignored
        {
            Rig rig;
            rig.lfo (3, presetShapeJson ("SAW UP"), 0, 0, 0, 0.5f);
            rig.lfo (1, presetShapeJson ("SAW UP"), 0, 0, 0, 0.5f);
            rig.matrix (matrixOf ({ route (1, "grainShape", 0.5), route (3, "grainSize", 0.5) }));
            rig.p->getBackendImpl().setLfoCount (2);
            rig.run (4);
            float ph[10];
            rig.p->getSynth().readLfoPhases (ph);
            check (ph[0] >= 0.0f && ph[2] < 0.0f, "lfoCount 2: LFO 3 is idle (-1) even though a route names it");
            rig.p->getBackendImpl().setLfoCount (3);
            rig.run (4);
            rig.p->getSynth().readLfoPhases (ph);
            check (ph[2] >= 0.0f, "lfoCount 3: LFO 3 runs");
        }

        // ---- the viz frame carries the phases (VizAnalyzer)
        {
            Rig rig;
            rig.lfo (2, presetShapeJson ("SINE"), 0, 0, 0, 0.5f);
            rig.matrix (matrixOf ({ route (2, "grainShape", 0.5) }));
            rig.run (200);                                                        // > 33 ms, silent
            WarprVizFrame f;
            bool got = rig.p->getBackendImpl().vizFifo().popLatest (f);
            check (got && f.lfoPhase[1] >= 0.0f && f.lfoPhase[0] < 0.0f && f.lfoPhase[2] < 0.0f,
                   "VizFrame.lfoPhase[]: the used LFO has a real phase, the others stay -1 - frames flow while silent");
        }
    }

    //==========================================================================
    // 8. structural latching
    //==========================================================================
    void testLatching()
    {
        std::printf ("\n[D8] structural latching: unisonVoices / polyphony sampled at note-on\n");

        Rig rig;
        rig.param ("unisonVoices", 4.0f);
        rig.param ("oversampling", 0.0f);
        rig.lfo (1, constShapeJson (1.0), 0, 0, 0, 0.5f);
        rig.matrix (matrixOf ({ route (1, "unisonVoices", 0.4) }));
        rig.run (4);                                               // the LFO has run: value +1 (global)

        rig.noteOnBlock (60);
        const int a = rig.slotOfNote (60);
        // 4 -> norm 3/7 = 0.4286, + 0.4 = 0.8286 -> 1 + 7 * 0.8286 = 6.8 -> 7
        check (a >= 0 && rig.p->getSynth().getVoiceInfo (a).unison == 7, "LFO at +1: unisonVoices 4 -> 7 at note-on (6.8 rounds to 7)");

        rig.lfo (1, constShapeJson (0.0), 0, 0, 0, 0.5f);          // now -1
        rig.run (20);
        check (rig.p->getSynth().getVoiceInfo (a).unison == 7, "the sounding voice keeps its latched unison while the LFO moves");

        rig.noteOnBlock (64);
        const int b = rig.slotOfNote (64);
        check (b >= 0 && rig.p->getSynth().getVoiceInfo (b).unison == 1, "LFO at -1: the NEXT note gets 1.2 -> 1 oscillator");
        check (rig.p->getSynth().getVoiceInfo (a).unison == 7, "...and the first note is still 7");

        // polyphony: base 16, LFO at -1 -> limit 1: a second note steals the first (3 ms ghost fade)
        Rig poly;
        poly.param ("oversampling", 0.0f);
        poly.lfo (1, constShapeJson (0.0), 0, 0, 0, 0.5f);
        poly.matrix (matrixOf ({ route (1, "polyphony", 1.0) }));
        poly.run (4);
        poly.noteOnBlock (60);
        poly.noteOnBlock (64);
        poly.noteOnBlock (67);
        check (poly.p->getSynth().getNumNonGhostVoices() == 1, "polyphony 16 with the LFO at -1: limit 1 at note-on, only the newest note is non-ghost");

        Rig polyUp;
        polyUp.param ("oversampling", 0.0f);
        polyUp.param ("polyphony", 2.0f);
        polyUp.lfo (1, constShapeJson (1.0), 0, 0, 0, 0.5f);
        polyUp.matrix (matrixOf ({ route (1, "polyphony", 1.0) }));
        polyUp.run (4);
        polyUp.noteOnBlock (60);
        polyUp.noteOnBlock (64);
        polyUp.noteOnBlock (67);
        polyUp.noteOnBlock (71);
        std::printf ("        polyphony 2 + LFO +1: non-ghost voices after 4 notes = %d (expected 4)\n", polyUp.p->getSynth().getNumNonGhostVoices());
        check (polyUp.p->getSynth().getNumNonGhostVoices() == 4,"polyphony 2 with the LFO at +1 and depth 1: the limit rises to 32 at note-on");
    }

    //==========================================================================
    // 9. backend
    //==========================================================================
    void testBackend()
    {
        std::printf ("\n[D8] backend: setLfoCount / setLfoShapeJson / setModMatrixJson / getUiStateJson / state load\n");

        auto a = std::make_unique<PluginProcessor>();
        auto& be = a->getBackendImpl();

        const juce::String shape1 = R"({"preset":"SAW UP","points":[],"bends":[{"x":0.5,"v":0.8}],"table":null,"drift":null})";
        const juce::String matrix = R"([{"src":1,"dst":"filterCutoff","depth":0.5},{"src":2,"dst":"warpMode","depth":0.5},{"src":11,"dst":"filterMix","depth":0.5},{"src":3,"dst":"nope","depth":1},{"src":1,"dst":"masterGain","depth":2}])";

        be.setLfoCount (5);
        be.setLfoShapeJson (1, shape1);
        be.setModMatrixJson (matrix);

        a->getModEngine().beginBlock();
        check (a->getModEngine().lfoCount() == 5, "setLfoCount publishes the count to the engine");
        check (a->getModEngine().matrix() != nullptr && a->getModEngine().matrix()->getNumRoutes() == 2,
               "setModMatrixJson: the audio-side matrix keeps only the 2 valid routes");

        const auto* t1 = a->getModEngine().tables()[1];
        check (t1 != nullptr && std::abs (t1->eval (0.25) - 0.45f) < 1.0e-4f && std::abs (t1->eval (0.5) - 0.8f) < 1.0e-4f,
               "setLfoShapeJson: the published table is SAW UP + bend (0.45 at t=1/4, 0.8 at the bend)");
        const auto* t0 = a->getModEngine().tables()[0];
        check (t0 != nullptr && std::abs (t0->eval (0.25) - 0.9f) < 1.0e-4f && std::abs (t0->eval (0.0) - 0.1f) < 1.0e-4f,
               "a never-edited LFO plays the UI's default shape (newShape(): v = 0.1, 0.9, 0.35, 0.75, 0.1)");

        const auto ui = juce::JSON::parse (be.getUiStateJson());
        const auto* shapes = ui.getProperty ("lfoShapes", {}).getArray();
        const auto* mx = ui.getProperty ("modMatrix", {}).getArray();
        check (ui.getProperty ("lfoCount", 0) == juce::var (5) && shapes != nullptr && shapes->size() == 5
                   && (*shapes)[1].getProperty ("preset", {}).toString() == "SAW UP"
                   && (*shapes)[0].getProperty ("preset", {}).toString() == "CUSTOM",
               "getUiStateJson: lfoCount 5, five shapes (edited one verbatim, unedited ones = the default shape)");
        check (mx != nullptr && mx->size() == 5, "getUiStateJson: the matrix document comes back verbatim (all 5 entries - the UI owns the document)");

        // state round trip -> the loaded processor publishes the same tables / matrix
        juce::MemoryBlock blob;
        a->getStateInformation (blob);
        auto b = std::make_unique<PluginProcessor>();
        b->setStateInformation (blob.getData(), (int) blob.getSize());
        b->getModEngine().beginBlock();
        const auto* bt1 = b->getModEngine().tables()[1];
        check (b->getModEngine().lfoCount() == 5 && b->getModEngine().matrix()->getNumRoutes() == 2 && bt1 != nullptr
                   && std::abs (bt1->eval (0.25) - 0.45f) < 1.0e-4f,
               "state load republishes lfoCount, tables and matrix to the audio side");

        // unchanged documents are not rebuilt (a state load republishes all ten shapes)
        be.setLfoShapeJson (1, shape1);
        a->getModEngine().beginBlock();
        check (a->getModEngine().tables()[1] == t1, "an identical document is not rebuilt / republished (same table object)");

        // a malformed shape document builds the default shape instead of failing
        be.setLfoShapeJson (2, "{{{{");
        a->getModEngine().beginBlock();
        const auto* t2 = a->getModEngine().tables()[2];
        check (t2 != nullptr && std::abs (t2->eval (0.25) - 0.9f) < 1.0e-4f, "malformed shape JSON -> the default shape (never a crash, never silence)");
    }

    //==========================================================================
    // 10. hook-up: every modulatable parameter is wired
    //==========================================================================
    struct HookParam { const char* id; int patch; };

    float maxDiff (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b)
    {
        float d = 0.0f;
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < a.getNumSamples(); ++i)
                d = std::max (d, std::abs (a.getSample (ch, i) - b.getSample (ch, i)));
        return d;
    }

    void applyPatch (Rig& rig, int patch)
    {
        auto& r = rig;
        r.param ("oversampling", 0.0f);
        r.param ("ampAttack", 5.0f);
        r.param ("ampDecay", 100.0f);
        r.param ("ampSustain", 0.6f);
        r.param ("ampRelease", 100.0f);

        if (patch == 0)                                           // classic: warp FM + shaper + filter + unison
        {
            r.param ("warpMode", 54.0f);  r.param ("modOscSource", 0.0f);
            r.param ("warpAmount", 0.4f); r.param ("warpParamA", 0.5f); r.param ("warpParamB", 0.3f);
            r.param ("warpMix", 0.8f);    r.param ("modOscRatio", 2.0f);
            r.param ("unisonVoices", 3.0f); r.param ("unisonDetune", 20.0f); r.param ("unisonSpread", 0.5f);
            r.param ("shaperEnable", 1.0f); r.param ("shaperMorph", 0.4f); r.param ("shaperMix", 0.7f);
            r.param ("filterEnable", 1.0f); r.param ("filterType", 3.0f); r.param ("filterCutoff", 2000.0f);
            r.param ("filterResonance", 0.3f); r.param ("filterDrive", 6.0f); r.param ("filterMorph", 0.3f);
            r.param ("filterKeyTrack", 0.3f); r.param ("filterMix", 0.8f);
            r.p->getBackendImpl().setShaperCurveJson ("A", R"({"interp":"linear","points":[{"x":-1,"y":-1},{"x":-0.5,"y":-1},{"x":0.5,"y":1},{"x":1,"y":1}]})");
        }
        else                                                      // granular (1 = Loop, 2 = Cloud)
        {
            // A 1 s noise sample: on the 2048-frame default single cycle a start-position / scan shift is only a phase
            // shift of a periodic wave, which a hook-up probe cannot tell from "not wired".
            juce::Random rng (99);
            juce::AudioBuffer<float> nb (1, 48000);
            for (int i = 0; i < 48000; ++i)
                nb.setSample (0, i, 0.3f * (rng.nextFloat() * 2.0f - 1.0f));
            r.p->getSampleStore().publish (warpr::SampleData::create (nb, kFs, "noise", false, false));

            r.param ("playMode", 1.0f);
            r.param ("grainMode", patch == 1 ? 1.0f : 2.0f);
            r.param ("warpEnable", 0.0f);
            r.param ("filterEnable", 0.0f);
            r.param ("grainPosition", 0.3f); r.param ("grainScan", 0.3f); r.param ("grainScanDist", 0.5f);
            r.param ("grainSize", 60.0f); r.param ("grainShape", 0.5f); r.param ("grainVariation", 0.3f);
            r.param ("grainSpread", 0.3f); r.param ("grainReverse", 0.5f); r.param ("grainDensity", 6.0f);
        }
    }

    juce::AudioBuffer<float> hookRender (Rig& rig, const juce::String& lfo1Shape, const juce::String& matrix, bool lfo2 = false)
    {
        rig.lfo (1, lfo1Shape, 0, 0, 0, 0.5f);

        if (lfo2)
            rig.lfo (2, presetShapeJson ("SQUARE"), 0, 0, 0, 0.5f, 90.0f, 0.0f);       // FREE, Hz, ~0.7 Hz square, phase 90 deg

        rig.matrix (matrix);

        juce::MidiBuffer midi;
        midi.addEvent (juce::MidiMessage::noteOn (1, 60, 0.8f), 0);
        midi.addEvent (juce::MidiMessage::noteOn (1, 67, 0.8f), (int) (0.12 * kFs));
        midi.addEvent (juce::MidiMessage::noteOff (1, 60), (int) (0.25 * kFs));
        midi.addEvent (juce::MidiMessage::noteOff (1, 67), (int) (0.25 * kFs));
        return warpr::tests::render (*rig.p, midi, kFs, 256, 0.45);
    }

    void testHookup (bool full)
    {
        std::printf ("\n[D8] hook-up: every modulatable parameter changes the sound when an LFO drives it\n");

        const std::vector<HookParam> hooks = {
            { "sampleStart", 0 }, { "sampleEnd", 0 }, { "rootNote", 0 }, { "transpose", 0 }, { "fineTune", 0 },
            { "warpAmount", 0 }, { "warpParamA", 0 }, { "warpParamB", 0 }, { "modOscRatio", 0 }, { "warpMix", 0 },
            { "shaperMorph", 0 }, { "shaperMix", 0 },
            { "ampAttack", 0 }, { "ampDecay", 0 }, { "ampSustain", 0 }, { "ampRelease", 0 },
            { "filterCutoff", 0 }, { "filterResonance", 0 }, { "filterDrive", 0 }, { "filterMorph", 0 }, { "filterKeyTrack", 0 }, { "filterMix", 0 },
            { "unisonDetune", 0 }, { "unisonSpread", 0 }, { "glideTime", 0 }, { "polyphony", 0 }, { "unisonVoices", 0 }, { "masterGain", 0 },
            { "lfo2Rate", 0 }, { "lfo2Phase", 0 }, { "lfo2Smooth", 0 },
            { "grainPosition", 1 }, { "grainScan", 1 }, { "grainScanDist", 1 }, { "grainSize", 1 }, { "grainShape", 1 },
            { "grainVariation", 1 }, { "grainSpread", 1 }, { "grainReverse", 1 },
            { "grainDensity", 2 }, { "grainVariation", 2 }
        };

        // every Float / Int parameter is in the list exactly once for its class (grainVariation appears for both Loop and Cloud)
        std::vector<std::string> covered;
        for (const auto& h : hooks)
            covered.push_back (h.id);
        for (int n = 1; n <= 10; ++n)
            for (const char* suffix : { "Rate", "Phase", "Smooth" })
                if (n != 2)
                    covered.push_back ("lfo" + std::to_string (n) + suffix);       // same code path as LFO 2 (loop over the LFO index)

        int missing = 0;
        for (const auto& m : warpr::kParamMeta)
            if ((m.kind == warpr::ParamKind::Float || m.kind == warpr::ParamKind::Int)
                && std::find (covered.begin(), covered.end(), std::string (m.id)) == covered.end())
            {
                ++missing;
                DETAIL ("        not covered: %s\n", m.id);
            }
        check (missing == 0, "the hook-up list covers all 67 Float / Int destinations (LFO 3..10 share LFO 2's code path)");

        Rig rigs[3];
        for (int pidx = 0; pidx < 3; ++pidx)
            applyPatch (rigs[pidx], pidx);

        const juce::String hi = constShapeJson (1.0), lo = constShapeJson (0.0);
        int notWired = 0;
        bool baselineDeterministic = true;
        float baselineLevel[3] = { 0, 0, 0 };

        for (int pidx = 0; pidx < 3; ++pidx)
        {
            // baseline: a live matrix (so the modulation path is the one running) whose only route ends in an idle LFO
            // (LFO 10 is above lfoCount 3), i.e. a route with no audible effect
            const juce::String idleRoute = route (2, "lfo10Smooth", 0.5);
            const auto base = hookRender (rigs[pidx], hi, matrixOf ({ idleRoute }));
            const auto again = hookRender (rigs[pidx], hi, matrixOf ({ idleRoute }));
            baselineDeterministic = baselineDeterministic && maxDiff (base, again) == 0.0f;
            baselineLevel[pidx] = base.getMagnitude (0, 0, base.getNumSamples());

            // the LFO-2 baseline (LFO 2 drives the pitch through fineTune; LFO 1 only feeds the lfo2X destinations)
            const juce::String lfo2Route = route (2, "fineTune", 0.5);
            const auto base2 = hookRender (rigs[pidx], hi, matrixOf ({ lfo2Route }), true);

            for (const auto& h : hooks)
            {
                if (h.patch != pidx)
                    continue;

                const bool viaLfo2 = std::string (h.id).rfind ("lfo2", 0) == 0;
                const juce::String r = viaLfo2 ? matrixOf ({ lfo2Route, route (1, h.id, 1.0) }) : matrixOf ({ route (1, h.id, 1.0) });
                const auto& ref = viaLfo2 ? base2 : base;

                float best = maxDiff (ref, hookRender (rigs[pidx], hi, r, viaLfo2));

                if (best <= 1.0e-5f)
                    best = std::max (best, maxDiff (ref, hookRender (rigs[pidx], lo, r, viaLfo2)));

                if (full || best <= 1.0e-5f)
                    std::printf ("        %-14s patch %d: max |modulated - baseline| = %.4g\n", h.id, pidx, (double) best);

                if (best <= 1.0e-5f)
                {
                    ++notWired;
                    DETAIL ("        NOT WIRED (or inaudible in the test patch): %s\n", h.id);
                }
            }
        }

        check (baselineDeterministic, "baseline renders are bit-identical (determinism: differences below come from the route)");
        check (baselineLevel[0] > 0.01f && baselineLevel[1] > 0.001f && baselineLevel[2] > 0.001f, "all three patches are audible");
        check (notWired == 0, "every listed destination changes the output when driven by a constant LFO at +1 or -1, depth 1");
    }

    //==========================================================================
    // 11. host transport through the processor
    //==========================================================================
    struct TestPlayHead : juce::AudioPlayHead
    {
        bool provide = true, playing = true;
        double ppq = 0.0, bpm = 120.0;
        int num = 4, den = 4;

        juce::Optional<PositionInfo> getPosition() const override
        {
            if (! provide)
                return {};

            PositionInfo info;
            info.setPpqPosition (ppq);
            info.setBpm (bpm);
            info.setIsPlaying (playing);
            info.setTimeSignature (TimeSignature { num, den });
            return info;
        }
    };

    void testHostTransport()
    {
        std::printf ("\n[D8] host transport: a real AudioPlayHead drives a grid-locked LFO\n");

        Rig rig;
        TestPlayHead head;
        rig.p->setPlayHead (&head);

        // FREE + Sync, 1/8 = 0.5 beat, Forward
        rig.lfo (1, presetShapeJson ("SAW UP"), 0, 0, 1, 9.0f / 16.0f);
        rig.matrix (matrixOf ({ route (1, "grainShape", 0.5) }));

        auto stepBlock = [&] (double ppqAtStart, bool playing)
        {
            head.ppq = ppqAtStart;
            head.playing = playing;
            rig.run (1);
            return rig.p->getSynth().getGlobalLfoPhase (0);
        };

        auto expectPhase = [&] (double ppqEnd) { const double c = ppqEnd / 0.5; return c - std::floor (c); };
        const double blockBeats = 256.0 * 120.0 / (60.0 * kFs);

        double worst = 0.0;
        for (double ppq : { 0.0, 37.25, 1000.125, 3.0, 16.0 })
        {
            const double ph = (double) stepBlock (ppq, true);
            double diff = std::abs (ph - expectPhase (ppq + blockBeats));
            diff = std::min (diff, 1.0 - diff);
            worst = std::max (worst, diff);
        }
        std::printf ("        (worst phase error after jumps / a loop point: %.3g)\n", worst);
        check (worst < 1.0e-4, "processBlock reads getPlayHead(): the grid-locked LFO phase == frac(ppq / beatsPerCycle) after every jump");

        // tempo change
        head.bpm = 90.0;
        const double bb = 256.0 * 90.0 / (60.0 * kFs);
        const double ph90 = (double) stepBlock (40.0, true);
        double d90 = std::abs (ph90 - expectPhase (40.0 + bb));
        d90 = std::min (d90, 1.0 - d90);
        check (d90 < 1.0e-4, "a tempo change is followed at the next block");

        // stopped host: the LFO keeps moving (internal clock); no playhead: 120 bpm
        const double before = (double) rig.p->getSynth().getGlobalLfoPhase (0);
        const double afterStop = (double) stepBlock (40.0, false);
        check (std::abs (afterStop - before) > 1.0e-4, "stopped host: the LFO continues on the internal clock");

        head.provide = false;
        const double a1 = (double) stepBlock (0.0, true);
        const double a2 = (double) stepBlock (0.0, true);
        check (std::abs (a2 - a1) > 1.0e-4, "no position info at all: the internal clock keeps the LFO running");
        rig.p->setPlayHead (nullptr);
    }

    //==========================================================================
    // 12. allocations
    //==========================================================================
    void testAllocations()
    {
        std::printf ("\n[D8] zero allocations with the LFO system live\n");

        Rig rig;
        rig.param ("oversampling", 1.0f);
        rig.param ("unisonVoices", 3.0f);
        rig.param ("filterCutoff", 4000.0f);
        for (int n = 1; n <= 4; ++n)
            rig.lfo (n, presetShapeJson (n % 2 ? "SINE" : "SWING"), n % 4, n % 3, n % 2, 0.5f, 30.0f * (float) n, 0.2f);
        rig.p->getBackendImpl().setLfoCount (4);
        rig.matrix (matrixOf ({ route (1, "filterCutoff", 0.5), route (2, "warpAmount", 0.4), route (3, "unisonDetune", 0.6),
                                route (4, "lfo1Rate", 0.3), route (2, "unisonVoices", 0.3), route (1, "masterGain", 0.2),
                                route (3, "glideTime", 0.4), route (4, "grainSize", 0.5), route (1, "ampAttack", 0.5) }));
        rig.run (4);

        juce::MidiBuffer m;
        m.addEvent (juce::MidiMessage::noteOn (1, 60, 0.8f), 0);
        m.addEvent (juce::MidiMessage::noteOn (1, 64, 0.8f), 64);
        rig.block (m);                                            // warm-up (first notes)

        // the MIDI buffers are built OUTSIDE the counter (MidiBuffer::addEvent allocates; that is the harness, not processBlock)
        juce::MidiBuffer evOn, evOff, none;
        evOn.addEvent (juce::MidiMessage::noteOn (1, 67, 0.7f), 17);
        evOff.addEvent (juce::MidiMessage::noteOff (1, 60), 3);

        std::size_t allocations = 1;
        {
            warpr::tests::ScopedAllocCount counter;
            for (int i = 0; i < 100; ++i)
            {
                if (i == 10)      rig.block (evOn);
                else if (i == 50) rig.block (evOff);
                else              rig.block (none);
            }
            allocations = counter.count();
        }
        check (allocations == 0, "processBlock with 4 LFOs (all modes), a 9-route matrix and note events performs zero allocations");
    }

    //==========================================================================
    // 13. CPU
    //==========================================================================
    double timeRender (Rig& rig, const juce::String& matrix, bool modulated)
    {
        if (modulated)
        {
            for (int n = 1; n <= 10; ++n)
                rig.lfo (n, presetShapeJson ("SINE"), 1, 0, 0, 0.6f);
            rig.p->getBackendImpl().setLfoCount (10);
        }

        rig.matrix (matrix);
        rig.p->prepareToPlay (kFs, 256);
        rig.run (2);

        juce::MidiBuffer m;
        for (int note = 48; note < 64; ++note)
            m.addEvent (juce::MidiMessage::noteOn (1, note, 0.8f), 0);
        rig.block (m);
        rig.run (8);

        double best = 1.0e9;
        const int blocks = 90;                                    // 90 x 256 = 0.48 s of audio

        for (int rep = 0; rep < 2; ++rep)
        {
            const double t0 = juce::Time::getMillisecondCounterHiRes();
            rig.run (blocks);
            best = std::min (best, juce::Time::getMillisecondCounterHiRes() - t0);
        }

        std::printf ("        (%s run: %d voices still sounding)\n", modulated ? "modulated" : "baseline", rig.p->getSynth().getNumNonGhostVoices());
        return best / (1000.0 * (double) (blocks * 256) / kFs);          // core fraction
    }

    void testCpu()
    {
        std::printf ("\n[D8] CPU: 10 LFOs x 16 voices x 3 routes at 8x oversampling vs the same patch without modulation\n");

        // (no ampSustain: five summed depth-0.3 routes can drive the sustain to 0, which frees the voices and makes the
        //  "modulated" run measure silence)
        const char* dests[6] = { "filterCutoff", "filterMorph", "unisonSpread", "fineTune", "filterKeyTrack", "filterDrive" };
        juce::String matrix = "[";
        for (int n = 1; n <= 10; ++n)
            for (int k = 0; k < 3; ++k)
                matrix << ((n == 1 && k == 0) ? "" : ",") << route (n, dests[(n + 2 * k) % 6], 0.3);
        matrix << "]";

        auto configure = [] (Rig& rig)
        {
            rig.param ("oversampling", 3.0f);                    // 8x
            rig.param ("polyphony", 16.0f);
            rig.param ("filterEnable", 1.0f);
            rig.param ("filterType", 3.0f);
            rig.param ("filterCutoff", 8000.0f);
            rig.param ("ampSustain", 0.8f);
            rig.param ("warpEnable", 0.0f);
        };

        Rig base, mod;
        configure (base);
        configure (mod);

        const double coreBase = timeRender (base, "[]", false);
        const double coreMod = timeRender (mod, matrix, true);
        const double deltaPoints = (coreMod - coreBase) * 100.0;

        // HAND-DERIVED gate: per voice and 32-sample chunk the modulated patch adds 10 LFO advances (~60 flops + one exp),
        // a 30-route accumulate, a 270-float memset pair and ~40 applyModulation calls (pow / exp / log, ~100 flops) = ~6000
        // flops; x 16 voices x 1500 chunks/s = ~150 Mflop/s = ~5 % of a ~3 GFlop/s core.  The plan allows 15 points.
        std::printf ("        (core load: no modulation %.1f %%, modulated %.1f %%, delta %+.2f points of one core)\n",
                     coreBase * 100.0, coreMod * 100.0, deltaPoints);

        // Wall-clock gates are meaningless under sanitizer instrumentation (TSan ~10x: measured 926 %
        // baseline core load); print the numbers above but only gate the uninstrumented build.
       #if defined (__has_feature)
        #if __has_feature (thread_sanitizer) || __has_feature (address_sanitizer)
         #define WARPR_SANITIZED_BUILD 1
        #endif
       #endif
       #if WARPR_SANITIZED_BUILD
        std::printf ("        (sanitizer build: CPU gate reported, not enforced)\n");
       #else
        check (deltaPoints <= 15.0, "modulation costs <= 15 percentage points of one core (plan D8)");
       #endif
    }
}

//==============================================================================
int runPhaseD8Tests (bool full)
{
    failures = 0;
    printedDetails = 0;

    std::printf ("\n==== Phase D8: LFO system + modulation matrix ====\n");

    testGoldenVectors();
    testHandDerivedAnchors();
    testEngine();
    testSyncLock (full);
    testTransportClock();
    testMatrixMath();
    testCompileAndClasses();
    testLfoOnLfo();
    testZipperClassLevel();
    testZipperProcessor();
    testModeSemantics();
    testLatching();
    testBackend();
    testHookup (full);
    testHostTransport();
    testAllocations();
    testCpu();

    return failures;
}

JUCE_END_IGNORE_WARNINGS_GCC_LIKE
