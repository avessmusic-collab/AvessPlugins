#pragma once
/*
  LfoTable - LFO shape description (wire format), the two-sided shape table and its builder
  (architecture C11 / section I "Shape tables"; phase D8).

  WIRE FORMAT (parameter-spec "Non-Parameter State" 3; v = value 0..1, 1 = top; the UI's y = 1 - v):
    { "preset": "CUSTOM|SINE|TRIANGLE|SAW UP|SAW DOWN|SQUARE|PUMP|STEPS 4|STEPS 8|STEPS 16|SWING|S&H NOISE|DRIFT",
      "points": [{"x","v","c"}], "bends": [{"x","v"}], "table": [..]|null, "drift": {"a","b"}|null }

  The UI is the source of truth for the shape math (Source/ui/public/index.html: presetFn, the bend anchors in
  lfoRender, and the CUSTOM segment law `seg`).  LfoTableBuilder reproduces those formulas exactly, converted from the
  UI's y coordinate to the wire v with v = 1 - y:
    SINE        v = 0.5 + 0.4 sin(2 pi t)
    TRIANGLE    v = 0.9 - 0.8 |2t - 1|
    SAW UP      v = 0.1 + 0.8 (t mod 1)            SAW DOWN  v = 0.9 - 0.8 (t mod 1)
    SQUARE      v = t < 0.5 ? 0.9 : 0.1
    PUMP        v = 0.1 + 0.8 exp(-4.5 t)
    STEPS n     v = 0.9 - 0.8 min(n-1, floor(t n)) / (n-1)        (n = 4, 8, 16)
    SWING       u = t < 0.62 ? (t/0.62)^1.5 : 1 - 0.75 (t-0.62)/0.38 ;  v = 0.1 + 0.8 u
    S&H NOISE   v = 1 - L[min(len-1, floor(t len))]  with L = the stored table (UI y coordinates; empty -> 0.5)
    DRIFT       v = 1 - (0.5 + 0.26 sin(2 pi t + a) + 0.14 sin(4 pi t + b))
    bends       anchors {x:0,dv:0}, {x_k, v_k - fn(x_k)} (sorted by x), {x:1,dv:0};
                v(t) = fn(t) + dv_i (1 - uu) + dv_{i+1} uu,  u = (t - x_i) / max(x_{i+1} - x_i, 0.001), uu = u^2 (3 - 2u)
    CUSTOM      points sorted by x; on the segment starting at p0: v = p0.v + (p1.v - p0.v) ((t - p0.x)/(p1.x - p0.x))^(2^(3 p0.c))
                (before the first point / after the last point the shape holds that point's value)
  An unknown preset id is drawn by the UI through the CUSTOM path, so it is treated as CUSTOM here too.

  TABLE: kCells uniform cells, each with a START and an END value (the right limit at its left edge and the left limit at
  its right edge), so a discontinuity can only sit on a cell boundary.  Lookup is linear inside the cell.
  AMENDMENT TO AD7 (4096 -> 16384 cells, recorded for the orchestrator): SWING has a slope kink at t = 0.62 inside a cell;
  the linear-interpolation error there is 0.25 h |slope jump| = 2.1e-4 at h = 1/4096 (> the 1e-4 golden-vector tolerance of
  plan D8) and 5.4e-5 at h = 1/16384.  Memory: 2 x 16384 floats = 128 KB per shape, 10 shapes, allocated on the message thread.
  Known limit (any uniform table): a CUSTOM segment with a NEGATIVE curve (c < 0, exponent < 1) has an infinite slope at its
  start, so the first two cells after that point can differ from the analytic curve by more than 1e-4.
*/

#include <juce_core/juce_core.h>

#include <array>
#include <cmath>
#include <memory>
#include <vector>

namespace warpr
{
    //==========================================================================
    struct LfoShapeDesc
    {
        enum class Preset : int { Custom, Sine, Triangle, SawUp, SawDown, Square, Pump, Steps4, Steps8, Steps16, Swing, SandH, Drift };
        static constexpr int numPresets = 13;

        struct Point { double x = 0.0, v = 0.0, c = 0.0; };
        struct Bend  { double x = 0.0, v = 0.0; };

        Preset preset = Preset::Custom;
        std::vector<Point> points;                   // sorted by x (stable)
        std::vector<Bend> bends;                     // sorted by x (stable)
        std::vector<double> table;                   // S&H values as the UI stores them (y coordinates)
        bool hasTable = false;
        double driftA = 0.0, driftB = 0.0;
        bool hasDrift = false;

        static const char* presetName (Preset p) noexcept;
        static bool presetFromName (const juce::String& name, Preset& out) noexcept;

        /** The UI's newShape(): CUSTOM with the five default points. */
        static LfoShapeDesc defaultShape();

        /** Tolerant parse of one shape document.  false = not a JSON object (out untouched).  Out-of-range numbers are
            clamped, unknown presets become CUSTOM, a CUSTOM shape with fewer than two points gets the default points. */
        static bool fromJson (const juce::String& json, LfoShapeDesc& out);
    };

    //==========================================================================
    struct LfoShapeTable
    {
        static constexpr int kCells = 16384;

        std::vector<float> start, end;               // kCells each

        LfoShapeTable() : start ((size_t) kCells, 0.5f), end ((size_t) kCells, 0.5f) {}

        /** phase in [0, 1] (1 returns the END value of the last cell).  Allocation-free, branch-light. */
        float eval (double phase) const noexcept
        {
            const double x = phase * (double) kCells;
            int i = (int) x;
            if (i < 0) i = 0;
            if (i > kCells - 1) i = kCells - 1;
            double f = x - (double) i;
            f = f < 0.0 ? 0.0 : (f > 1.0 ? 1.0 : f);
            const float s = start[(size_t) i];
            return s + (end[(size_t) i] - s) * (float) f;
        }
    };

    //==========================================================================
    class LfoTableBuilder
    {
    public:
        /** Exact (JS-equivalent, wire coordinates, unclamped) value at t in [0, 1]; right-continuous. */
        static double evalExact (const LfoShapeDesc& d, double t);

        /** The analytic preset part only (no bends); CUSTOM returns the custom value. */
        static double presetValue (const LfoShapeDesc& d, double t);

        /** Message thread.  Samples the exact shape (clamped to [0, 1]) into the two-sided table. */
        static std::unique_ptr<LfoShapeTable> build (const LfoShapeDesc& d);

        /** Prepared evaluator (sorted copies, bend anchors) used by build() and evalExact(). */
        class Evaluator
        {
        public:
            explicit Evaluator (const LfoShapeDesc& desc);
            double operator() (double t) const;

        private:
            double custom (double t) const;
            double preset (double t) const;

            LfoShapeDesc d;
            std::vector<double> ax, adv;             // bend anchors
        };
    };
}
