#include "TransitionEngine.h"
#include <algorithm>
#include <cmath>

namespace drop
{

void TransitionEngine::setCustomPoints(const std::vector<CurvePoint>& points)
{
    std::vector<CurvePoint> sanitized;
    sanitized.reserve(points.size());
    for (auto p : points)
    {
        p.x = juce::jlimit(0.0f, 1.0f, p.x);
        p.y = juce::jlimit(0.0f, 1.0f, p.y);
        sanitized.push_back(p);
    }

    // stable_sort (NOT sort): points with equal x must keep their original
    // relative (insertion) order so the de-duplication step below
    // deterministically keeps the LAST-written point for any given x.
    std::stable_sort(sanitized.begin(), sanitized.end(),
               [](const CurvePoint& a, const CurvePoint& b) { return a.x < b.x; });

    // De-duplicate equal-x points (keep the last one written for that x) so
    // x ends up strictly increasing, per architecture.md.
    std::vector<CurvePoint> deduped;
    deduped.reserve(sanitized.size());
    for (auto& p : sanitized)
    {
        if (!deduped.empty() && std::abs(deduped.back().x - p.x) < 1.0e-6f)
            deduped.back() = p;
        else
            deduped.push_back(p);
    }

    numCustomPoints = juce::jmin((int) deduped.size(), kMaxCustomPoints);
    for (int i = 0; i < numCustomPoints; ++i)
        customPoints[(size_t) i] = deduped[(size_t) i];
}

float TransitionEngine::evaluate(float t) const noexcept
{
    t = juce::jlimit(0.0f, 1.0f, t);

    float result;
    switch (curveType)
    {
        case CurveType::Linear:           result = evaluateLinear(t); break;
        case CurveType::Exponential:      result = evaluateExponential(t); break;
        case CurveType::SCurve:           result = evaluateSCurve(t); break;
        case CurveType::LateAcceleration: result = evaluateLateAcceleration(t); break;
        case CurveType::Custom:           result = evaluateCustom(t); break;
        default:                          result = t; break;
    }

    // MUSICAL-PROGRESSION FIX (2026-09-16, user DAW report: "8 bars and the
    // effect is active only at the end of the last bar"). Every non-linear
    // curve above is end-weighted BY DESIGN ("most tension near the end"),
    // but at the DEFAULT tension of 50% they were so extreme that the
    // module-internal sweeps they drive (filter cutoff position, gate rate,
    // pitch amount, noise rise) barely moved for most of the build:
    // evaluate(0.25) measured Exponential=0.031, LateAcceleration=0.075,
    // SCurve=0.113 - i.e. a quarter of the way into an 8-bar build the
    // filter had swept 3-11% and was therefore inaudible.
    //
    // Fix: blend every shaped curve a fixed amount toward linear. The
    // linear term guarantees continuous, audible early motion; the shaped
    // term preserves each curve type's character and their relative
    // ordering (Exponential still later-blooming than SCurve than
    // LateAcceleration than Linear at the same tension), so the `tension`
    // parameter keeps its documented meaning. Custom curves are exempt:
    // the user drew that shape explicitly and it is honored verbatim.
    if (curveType != CurveType::Linear && curveType != CurveType::Custom)
        result = t + (result - t) * kCurveBlendDepth;

    return juce::jlimit(0.0f, 1.0f, result);
}

float TransitionEngine::evaluateLinear(float t) const noexcept
{
    return t;
}

float TransitionEngine::evaluateExponential(float t) const noexcept
{
    // Higher tension -> higher exponent -> more time spent "flat" early,
    // steep rise late (architecture.md Exponential config).
    const float exponent = 1.0f + tension * 3.0f; // 1 (linear) .. 4 (steep late rise)
    return std::pow(t, exponent);
}

float TransitionEngine::evaluateSCurve(float t) const noexcept
{
    // Base smoothstep, optionally blended toward a steeper logistic curve as
    // tension increases (architecture.md S-Curve config).
    const float smoothstep = t * t * (3.0f - 2.0f * t);

    constexpr float k = 10.0f;
    const float raw0 = 1.0f / (1.0f + std::exp(k * 0.5f));   // logistic(0)
    const float raw1 = 1.0f / (1.0f + std::exp(-k * 0.5f));  // logistic(1)
    const float rawT = 1.0f / (1.0f + std::exp(-k * (t - 0.5f)));
    const float span = raw1 - raw0;
    const float logisticNorm = span > 1.0e-9f ? (rawT - raw0) / span : t;

    return (1.0f - tension) * smoothstep + tension * logisticNorm;
}

float TransitionEngine::evaluateLateAcceleration(float t) const noexcept
{
    // Piecewise: a slow linear ramp for the first (1 - tensionBias) fraction
    // of the build, then accelerates to 1.0 in the remaining tensionBias
    // fraction (architecture.md LateAcceleration config). Default tension
    // (0.5, from the `tension` APVTS parameter's 50% default) places
    // tensionBias at 0.25 - "most tension near the end by default."
    const float tensionBias = juce::jlimit(0.05f, 0.95f, 0.25f + (tension - 0.5f) * 0.5f);
    const float breakpointT = 1.0f - tensionBias;              // fraction of TIME before acceleration begins
    const float breakpointValue = breakpointT * 0.55f;         // fraction of PROGRESS reached by the slow ramp (was 0.3f - see MUSICAL-PROGRESSION FIX in evaluate(): the slow ramp reached only 22.5% of progress in 75% of the build)

    if (t <= breakpointT)
        return breakpointT > 0.0f ? (t / breakpointT) * breakpointValue : 0.0f;

    const float remainingSpan = juce::jmax(1.0e-6f, 1.0f - breakpointT);
    const float remainingT = (t - breakpointT) / remainingSpan;
    return breakpointValue + remainingT * (1.0f - breakpointValue);
}

float TransitionEngine::evaluateCustom(float t) const noexcept
{
    // Safe default when no custom points are set (architecture.md: "safe
    // default when absent").
    if (numCustomPoints == 0)
        return t;

    if (numCustomPoints == 1)
        return juce::jlimit(0.0f, 1.0f, customPoints[0].y);

    if (t <= customPoints[0].x)
        return juce::jlimit(0.0f, 1.0f, customPoints[0].y);

    if (t >= customPoints[(size_t) (numCustomPoints - 1)].x)
        return juce::jlimit(0.0f, 1.0f, customPoints[(size_t) (numCustomPoints - 1)].y);

    // Linear interpolation between the bracketing pair. Points are clamped
    // and sorted by setCustomPoints(), so this is a simple, safe, RT-safe
    // evaluation (Catmull-Rom, mentioned as an option in architecture.md, is
    // left as a future refinement over this already-clamped/deterministic
    // baseline).
    for (int i = 0; i < numCustomPoints - 1; ++i)
    {
        const auto& a = customPoints[(size_t) i];
        const auto& b = customPoints[(size_t) (i + 1)];
        if (t >= a.x && t <= b.x)
        {
            const float span = b.x - a.x;
            const float frac = span > 1.0e-9f ? (t - a.x) / span : 0.0f;
            return juce::jlimit(0.0f, 1.0f, a.y + frac * (b.y - a.y));
        }
    }

    return t; // unreachable given the bracketing checks above; defensive fallback
}

} // namespace drop
