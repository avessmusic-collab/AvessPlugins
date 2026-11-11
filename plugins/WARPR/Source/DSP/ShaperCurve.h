#pragma once
/*
  ShaperCurve - the message-thread model of one shaper transfer curve (architecture C6 / "E. Shaper").

  Wire format (parameter-spec "Non-Parameter State" item 5, identical in the UI):
      { "interp": "smooth" | "linear", "points": [ { "x": -1..1, "y": -1..1 }, ... ] }   sorted by x

  Interpolation is a bit-for-bit port of the editor's evalPts() (v9-ui.html), so the polyline the user
  sees and the LUT the audio thread reads are the same function:
      linear  : piecewise linear
      smooth  : monotone cubic Hermite.  Tangent at an interior node j is the harmonic mean
                2 a b / (a + b) of the two adjacent secant slopes (0 when a b <= 0); the end tangents are the end
                secants; segment dx is max(dx, 1e-6).  Fritsch-Carlson/Butland class: alpha, beta <= 2, so a
                segment NEVER leaves [y_i, y_i+1] (no overshoot) and monotone data stays monotone.
      outside the first/last point the curve is held flat (the UI does the same).

  SYMMETRIC mode (shaperSymmetry = Symmetric): only the x >= 0 half defines the curve, mirrored odd.
  halfPoints() is the editor's halfPoints(): the points with x >= 0, with (0, 0) prepended when the half does not
  start at x = 0.  Evaluation is f(x) = x >= 0 ? h(x) : -h(-x), which is EXACTLY odd.
  ASYMMETRIC mode: the full point list is used.

  Parsing is strict about structure and tolerant about numbers: non-finite or non-numeric coordinates, fewer
  than 2 or more than 64 points, an unknown "interp" or a non-object document are REJECTED (the model is left
  untouched); coordinates are clamped to [-1, 1] and the points sorted by x (stable), like the editor's
  normaliseCurve().  toJson() is the canonical writer: fixed key order, "%.9g" numbers, no whitespace -
  parse(toJson(c)) re-serialises to the identical text.

  Message thread only (allocates).  Nothing here runs on the audio thread.
*/

#include <juce_core/juce_core.h>

#include <vector>

namespace warpr
{
    struct ShaperPoint
    {
        double x = 0.0;
        double y = 0.0;
    };

    class ShaperCurve
    {
    public:
        enum class Interp { Smooth, Linear };

        static constexpr int maxPoints = 64;

        Interp interp = Interp::Smooth;
        std::vector<ShaperPoint> points;

        ShaperCurve() { setIdentity(); }

        /** (-1,-1), (0,0), (1,1), smooth. */
        void setIdentity();
        static ShaperCurve identity() { return ShaperCurve(); }

        /** True when every point lies exactly on y = x.  Both interpolators reproduce the line through
            such points analytically (collinear secants give unit tangents), so the curve IS f(x) = x and
            the LUT builder stores exact zeros for the remainder tables (the bit-exact-null guarantee). */
        bool isIdentity() const noexcept
        {
            for (const auto& p : points)
                if (! juce::exactlyEqual (p.x, p.y))
                    return false;

            return points.size() >= 2;
        }

        /** Strict parse (see the header comment).  `out` is only written on success. */
        static bool fromJson (const juce::String& json, ShaperCurve& out);

        /** Canonical JSON (fixed key order, %.9g). */
        juce::String toJson() const;

        /** The editor's halfPoints(): points with x >= 0, (0,0) prepended if the half does not start at 0. */
        std::vector<ShaperPoint> halfPoints() const;

        /** y values multiplied by k and clamped to [-1, 1] (NORMALIZE). */
        void scaleY (double k) noexcept;

        /** The editor's evalPts() on an explicit point list. */
        static double evalPoints (const std::vector<ShaperPoint>& pts, double x, Interp interp) noexcept;

        /** Convenience (allocates for symmetric curves): f(x) of the whole curve in the given mode. */
        double eval (double x, bool symmetric) const;

        /** Allocation-free repeated evaluation (LUT builder, tests). */
        class Evaluator
        {
        public:
            Evaluator (const ShaperCurve& curve, bool symmetricMode);

            double operator() (double x) const noexcept;

            /** Sorted unique x of every control point (mirrored in symmetric mode) strictly inside (-1, 1):
                the places where the curve switches from one polynomial to the next. */
            std::vector<double> breakpoints() const;

        private:
            std::vector<ShaperPoint> pts;
            Interp mode = Interp::Smooth;
            bool symmetric = false;
        };

        bool operator== (const ShaperCurve& other) const noexcept;
    };
}
