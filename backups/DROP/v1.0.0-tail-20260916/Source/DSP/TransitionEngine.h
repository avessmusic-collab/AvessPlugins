#pragma once
#include <juce_core/juce_core.h>
#include <array>
#include <vector>

/**
 * TransitionEngine - maps a normalized elapsed-time fraction t (0-1) within
 * the current transition phase to a normalized progress value (0-1), shaped
 * by one of five curve types.
 *
 * See plugins/DROP/.ideas/architecture.md > Core Components > TransitionEngine
 * and plugins/DROP/.ideas/implementation-spec.md > TRANSITIONENGINE.
 *
 * evaluate() is pure/stateless per-call math (aside from reading the
 * already-set curveType/tension/customPoints members) - no allocation, RT-safe,
 * safe to call every sample or every processing segment. setCustomPoints()
 * is NOT RT-safe (it copies into a fixed-size array from a std::vector) and
 * must only be called from the message thread (e.g. loading state / UI
 * curve editing), never from processBlock().
 */
namespace drop
{

enum class CurveType
{
    Linear = 0,
    Exponential,
    SCurve,
    LateAcceleration,
    Custom
};

struct CurvePoint
{
    float x = 0.0f;
    float y = 0.0f;
};

class TransitionEngine
{
public:
    // architecture.md: Custom curve control points "sampled at 9-17 control points."
    static constexpr int kMaxCustomPoints = 17;

    void setCurveType(CurveType type) noexcept { curveType = type; }
    CurveType getCurveType() const noexcept { return curveType; }

    /** Normalized 0-1 (already mapped from the `tension` APVTS 0-100% parameter). Shapes curve steepness / late-acceleration bias. */
    void setTension(float normalizedTension) noexcept { tension = juce::jlimit(0.0f, 1.0f, normalizedTension); }
    float getTension() const noexcept { return tension; }

    /**
     * Replaces the Custom curve's control points. Points are clamped to
     * [0,1]x[0,1], sorted, and de-duplicated by x so x is strictly
     * increasing (architecture.md: "x monotonically increasing, enforced on
     * insert"). An empty list is a safe, explicit default (evaluate()
     * degrades to Linear behavior when no points are set - architecture.md
     * "safe default when absent"). Message-thread-only.
     */
    void setCustomPoints(const std::vector<CurvePoint>& points);

    int getNumCustomPoints() const noexcept { return numCustomPoints; }

    /** Evaluates progress at normalized time t (clamped to [0,1]). Result is always clamped to [0,1]. RT-safe. */
    float evaluate(float t) const noexcept;

private:
    float evaluateLinear(float t) const noexcept;
    float evaluateExponential(float t) const noexcept;
    float evaluateSCurve(float t) const noexcept;
    float evaluateLateAcceleration(float t) const noexcept;
    float evaluateCustom(float t) const noexcept;

    CurveType curveType = CurveType::Linear;
    float tension = 0.5f;

    std::array<CurvePoint, (size_t) kMaxCustomPoints> customPoints {};
    int numCustomPoints = 0;
};

} // namespace drop
