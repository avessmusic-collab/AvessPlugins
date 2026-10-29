#include "StereoWidth.h"

#include <algorithm>
#include <cmath>

float StereoWidth::midCompFor (float width) noexcept
{
    const float w = std::max (width, 0.0f);
    return std::clamp (1.0f / std::sqrt (0.5f * (1.0f + w * w)), kMidFloor, 1.0f);
}

void StereoWidth::prepare (double sampleRate)
{
    const double sr = sampleRate > 0.0 ? sampleRate : 44100.0;

    juce::dsp::ProcessSpec spec;
    spec.sampleRate       = sr;
    spec.maximumBlockSize = 32;
    spec.numChannels      = 2;

    crossover.prepare (spec);
    crossover.setType (juce::dsp::LinkwitzRileyFilterType::lowpass);   // type is irrelevant for the 2-output overload
    crossover.setCutoffFrequency (kCrossoverHz);

    widthSm.reset    (sr, kSmoothSec);
    collapseSm.reset (sr, kSmoothSec);
    depthSm.reset    (sr, kSmoothSec);

    reset();
}

void StereoWidth::reset()
{
    crossover.reset();
    widthSm.setCurrentAndTargetValue    (1.0f);
    collapseSm.setCurrentAndTargetValue (0.0f);
    depthSm.setCurrentAndTargetValue    (0.0f);
}

void StereoWidth::setTargets (float width, float collapse, float moveDepth, bool snap)
{
    const float w = std::clamp (width,     0.0f, 4.0f);
    const float k = std::clamp (collapse,  0.0f, 1.0f);
    const float m = std::clamp (moveDepth, 0.0f, 1.0f);

    if (snap)
    {
        widthSm.setCurrentAndTargetValue (w);
        collapseSm.setCurrentAndTargetValue (k);
        depthSm.setCurrentAndTargetValue (m);
    }
    else
    {
        widthSm.setTargetValue (w);
        collapseSm.setTargetValue (k);
        depthSm.setTargetValue (m);
    }
}

void StereoWidth::process (float* l, float* r, int n, const float* lfoB)
{
    for (int i = 0; i < n; ++i)
    {
        const float w     = widthSm.getNextValue();
        const float k     = collapseSm.getNextValue();
        const float depth = depthSm.getNextValue();
        const float lfo   = lfoB != nullptr ? lfoB[i] : 0.0f;

        float lowL, highL, lowR, highR;
        crossover.processSample (0, l[i], lowL, highL);
        crossover.processSample (1, r[i], lowR, highR);

        // ---- lows: mono-safe, side collapses progressively ----
        const float mLow = 0.5f * (lowL + lowR);
        const float sLow = 0.5f * (lowL - lowR) * (1.0f - 0.8f * k);

        // ---- highs: M/S widener + lfoB sway & rotation ----
        const float mHigh = 0.5f * (highL + highR);
        const float sHigh = 0.5f * (highL - highR);

        const float wEff = std::max (0.0f, w + depth * lfo);
        float mc = midCompFor (wEff);
        float s  = sHigh * wEff;
        float m;

        if (depth > 1.0e-6f)
        {
            const float a  = std::clamp (depth * lfo * kRotation, -kRotation, kRotation);
            const float a2 = a * a;
            const float sn = a * (1.0f - a2 * (1.0f / 6.0f - a2 * (1.0f / 120.0f)));
            const float cs = 1.0f - a2 * (0.5f - a2 * (1.0f / 24.0f));

            // keep the TOTAL on-axis mid loss (compensation x rotation cosine) within 1 dB
            mc = std::clamp (mc, kMidFloor / cs, 1.0f);
            m  = mHigh * mc;

            const float mr = m * cs - s * sn;
            const float sr = m * sn + s * cs;
            m = mr;
            s = sr;
        }
        else
        {
            m = mHigh * mc;
        }

        l[i] = (mLow + sLow) + (m + s);
        r[i] = (mLow - sLow) + (m - s);
    }
}
