#include "SaturationEngine.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace
{
    constexpr double kLn2 = 0.693147180559945309417232121458;
    constexpr int    kTableSize = 71;                 // drive 1.0 .. 8.0 in 0.1 steps
    constexpr float  kTableStep = 0.1f;

    // log(cosh(u)), double precision (the ADAA difference is ill-conditioned in float).
    inline double logCosh (double u) noexcept
    {
        const double a = std::abs (u);

        if (a > 9.0)
            return a - kLn2;                                      // e^-18 ~ 1.5e-8: exact enough

        return a + std::log1p (std::exp (-2.0 * a)) - kLn2;
    }

    // tanh(b) for the tiny bias values used here (|b| <= 0.08): error < 1e-9.
    inline float tanhSmall (float b) noexcept
    {
        const float b2 = b * b;
        return b * (1.0f - b2 * (1.0f / 3.0f - b2 * (2.0f / 15.0f)));
    }

    // AC RMS of tanh(d * s * z + b(d)) for z ~ N(0,1), numerically integrated (non-RT, prepare only).
    double acRmsTanh (double drive, double sigma, double bias) noexcept
    {
        constexpr int    steps = 1200;
        constexpr double zMax  = 6.0;
        const double dz = 2.0 * zMax / (double) steps;

        double w = 0.0, m1 = 0.0, m2 = 0.0;

        for (int i = 0; i <= steps; ++i)
        {
            const double z  = -zMax + dz * (double) i;
            const double pw = std::exp (-0.5 * z * z) * ((i == 0 || i == steps) ? 0.5 : 1.0);
            const double y  = std::tanh (drive * sigma * z + bias);
            w  += pw;
            m1 += pw * y;
            m2 += pw * y * y;
        }

        m1 /= w;
        m2 /= w;
        return std::sqrt (std::max (m2 - m1 * m1, 1.0e-30));
    }

    const std::array<float, kTableSize>& makeupTable()
    {
        static const std::array<float, kTableSize> table = []
        {
            std::array<float, kTableSize> t {};
            const double ref = acRmsTanh (1.0, (double) SaturationEngine::kReferenceRms,
                                          (double) SaturationEngine::biasFor (1.0f));

            for (int i = 0; i < kTableSize; ++i)
            {
                const float d = SaturationEngine::kMinDrive + (float) i * kTableStep;
                t[(size_t) i] = (float) (ref / acRmsTanh ((double) d, (double) SaturationEngine::kReferenceRms,
                                                          (double) SaturationEngine::biasFor (d)));
            }
            return t;
        }();
        return table;
    }
}

float SaturationEngine::biasFor (float drive) noexcept
{
    return kBias * std::clamp ((drive - 1.0f) / 3.2f, 0.0f, 1.0f);
}

float SaturationEngine::makeupFor (float drive) noexcept
{
    const auto& tab = makeupTable();
    const float pos = std::clamp ((drive - kMinDrive) / kTableStep, 0.0f, (float) (kTableSize - 1));
    const int   i0  = std::min ((int) pos, kTableSize - 2);
    const float fr  = pos - (float) i0;
    return tab[(size_t) i0] + fr * (tab[(size_t) i0 + 1] - tab[(size_t) i0]);
}

void SaturationEngine::prepare (double newSampleRate)
{
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 44100.0;
    dcR = (float) std::exp (-2.0 * 3.14159265358979323846 * kDcHz / sampleRate);

    (void) makeupTable();       // force the one-time table build here, never in the audio callback

    driveSm.reset  (sampleRate, kSmoothSec);
    amountSm.reset (sampleRate, kSmoothSec);
    reset();
}

void SaturationEngine::reset()
{
    for (int c = 0; c < 2; ++c)
        x1[c] = dcX1[c] = dcY1[c] = 0.0f;

    driveSm.setCurrentAndTargetValue (kMinDrive);
    amountSm.setCurrentAndTargetValue (0.0f);
}

void SaturationEngine::setTargets (float drive, float amount, bool snap)
{
    const float d = std::clamp (drive, kMinDrive, kMaxDrive);
    const float a = std::clamp (amount, 0.0f, 1.0f);

    if (snap)
    {
        driveSm.setCurrentAndTargetValue (d);
        amountSm.setCurrentAndTargetValue (a);
    }
    else
    {
        driveSm.setTargetValue (d);
        amountSm.setTargetValue (a);
    }
}

inline float SaturationEngine::shape (int ch, float x, float drive, float bias, float makeup) noexcept
{
    const float xPrev = x1[ch];
    x1[ch] = x;

    const double un = (double) drive * (double) x     + (double) bias;
    const double up = (double) drive * (double) xPrev + (double) bias;

    float y;

    if (std::abs (x - xPrev) < 1.0e-5f)
        y = (float) std::tanh (0.5 * (un + up));
    else
        y = (float) ((logCosh (un) - logCosh (up)) / (un - up));

    y -= tanhSmall (bias);                                   // quiescent offset

    // 12 Hz one-pole DC blocker (wet path only)
    const float yd = y - dcX1[ch] + dcR * dcY1[ch];
    dcX1[ch] = y;
    dcY1[ch] = yd;

    return yd * makeup;
}

void SaturationEngine::process (float* l, float* r, int n)
{
    for (int i = 0; i < n; ++i)
    {
        const float d = driveSm.getNextValue();
        const float a = amountSm.getNextValue();

        if (a <= 0.0f)
        {
            x1[0] = l[i];
            x1[1] = r[i];
            continue;                                        // out == x exactly; DC states idle
        }

        const float bias   = biasFor (d);
        const float makeup = makeupFor (d);

        const float xl = l[i];
        const float xr = r[i];

        l[i] = xl + a * (shape (0, xl, d, bias, makeup) - xl);
        r[i] = xr + a * (shape (1, xr, d, bias, makeup) - xr);
    }
}
