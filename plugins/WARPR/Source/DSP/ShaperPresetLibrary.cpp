#include "DSP/ShaperPresetLibrary.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace warpr::shaperpresets
{
    namespace
    {
        constexpr const char* kNames[kNumPresets] = { "Soft Sat", "Hard Clip", "Tube", "Diode", "Fold",
                                                      "Sine", "Triangle", "Expo", "Custom" };

        /** x = -1, -.75 ... 1 */
        std::vector<double> quarterGrid()
        {
            std::vector<double> xs;
            for (int i = 0; i <= 8; ++i)
                xs.push_back ((double) (i - 4) * 0.25);
            return xs;
        }

        /** Builds an odd curve from an odd function given for |x|: y(-x) = -y(x) exactly. */
        template <class F>
        std::vector<ShaperPoint> oddPoints (const std::vector<double>& xs, F&& fPositive)
        {
            std::vector<ShaperPoint> pts;
            for (double x : xs)
            {
                const double y = x >= 0.0 ? fPositive (x) : -fPositive (-x);
                pts.push_back ({ x, y });
            }
            return pts;
        }

        ShaperCurve make (ShaperCurve::Interp interp, std::vector<ShaperPoint> pts)
        {
            ShaperCurve c;
            c.interp = interp;
            c.points = std::move (pts);
            return c;
        }
    }

    const char* name (int preset) noexcept
    {
        return preset >= 0 && preset < kNumPresets ? kNames[preset] : "";
    }

    bool isLoadable (int preset) noexcept
    {
        return preset >= 0 && preset < kNumPresets && preset != kCustomIndex;
    }

    bool isAsymmetric (int preset) noexcept
    {
        return preset == 2 || preset == 3;
    }

    ShaperCurve generate (int preset)
    {
        switch (preset)
        {
            case 0:   // Soft Sat
            {
                const double norm = 1.0 / std::tanh (2.5);
                return make (ShaperCurve::Interp::Smooth,
                             oddPoints (quarterGrid(), [norm] (double x) { return std::tanh (2.5 * x) * norm; }));
            }

            case 1:   // Hard Clip
                return make (ShaperCurve::Interp::Linear, { { -1.0, -1.0 }, { -0.5, -1.0 }, { 0.5, 1.0 }, { 1.0, 1.0 } });

            case 2:   // Tube (asymmetric)
            {
                auto raw = [] (double x) { return std::tanh (2.2 * (x + 0.15)) - std::tanh (2.2 * 0.15); };
                const double norm = 1.0 / std::max (std::abs (raw (1.0)), std::abs (raw (-1.0)));

                std::vector<ShaperPoint> pts;
                for (int i = 0; i <= 10; ++i)
                {
                    const double x = (double) (2 * i - 10) / 10.0;
                    pts.push_back ({ x, raw (x) * norm });
                }
                return make (ShaperCurve::Interp::Smooth, std::move (pts));
            }

            case 3:   // Diode (asymmetric)
            {
                const double posNorm = 1.0 / (1.0 - std::exp (-3.0));
                const double negNorm = 0.6 / (1.0 - std::exp (-2.1));

                std::vector<ShaperPoint> pts;
                for (double x : { -1.0, -0.7, -0.4, -0.2, -0.1, 0.0, 0.1, 0.2, 0.4, 0.7, 1.0 })
                {
                    const double y = x >= 0.0 ? (1.0 - std::exp (-3.0 * x)) * posNorm
                                              : -(1.0 - std::exp (-2.1 * -x)) * negNorm;
                    pts.push_back ({ x, y });
                }
                return make (ShaperCurve::Interp::Smooth, std::move (pts));
            }

            case 4:   // Fold: triangle fold with two reversals per side, corners on the LUT grid
            {
                auto fold = [] (double x)
                {
                    const double u = 4.0 * x;               // x >= 0
                    if (u <= 1.0) return u;
                    if (u <= 3.0) return 2.0 - u;
                    return u - 4.0;
                };

                const std::vector<double> xs { -1.0, -0.875, -0.75, -0.5, -0.25, -0.125, 0.0, 0.125, 0.25, 0.5, 0.75, 0.875, 1.0 };
                return make (ShaperCurve::Interp::Linear, oddPoints (xs, fold));
            }

            case 5:   // Sine
                return make (ShaperCurve::Interp::Smooth,
                             oddPoints (quarterGrid(), [] (double x) { return std::sin (1.5707963267948966 * x); }));

            case 6:   // Triangle: zigzag, period 1
            {
                // positive side: (0,0) (.25,1) (.5,0) (.75,-1) (1,0)
                auto zig = [] (double x)
                {
                    if (x <= 0.25) return x * 4.0;
                    if (x <= 0.75) return 2.0 - x * 4.0;
                    return x * 4.0 - 4.0;
                };
                return make (ShaperCurve::Interp::Linear, oddPoints (quarterGrid(), zig));
            }

            case 7:   // Expo
            {
                const double norm = 1.0 / (std::exp (3.0) - 1.0);
                return make (ShaperCurve::Interp::Smooth,
                             oddPoints (quarterGrid(), [norm] (double x) { return (std::exp (3.0 * x) - 1.0) * norm; }));
            }

            default:  // Custom / out of range: identity
                return ShaperCurve::identity();
        }
    }
}
