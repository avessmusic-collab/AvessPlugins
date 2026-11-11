#include "DSP/ShaperCurve.h"

#include <algorithm>
#include <cmath>

namespace warpr
{
    namespace
    {
        constexpr double kMinDx = 1.0e-6;          // the editor's Math.max(dx, 1e-6)

        bool isNumber (const juce::var& v) noexcept
        {
            return v.isInt() || v.isInt64() || v.isDouble();
        }

        juce::String fmt (double v)
        {
            if (juce::exactlyEqual (v, 0.0))
                v = 0.0;                           // never write "-0"

            return juce::String::formatted ("%.9g", v);
        }
    }

    //==========================================================================
    void ShaperCurve::setIdentity()
    {
        interp = Interp::Smooth;
        points = { { -1.0, -1.0 }, { 0.0, 0.0 }, { 1.0, 1.0 } };
    }

    bool ShaperCurve::operator== (const ShaperCurve& other) const noexcept
    {
        if (interp != other.interp || points.size() != other.points.size())
            return false;

        for (size_t i = 0; i < points.size(); ++i)
            if (! juce::exactlyEqual (points[i].x, other.points[i].x) || ! juce::exactlyEqual (points[i].y, other.points[i].y))
                return false;

        return true;
    }

    //==========================================================================
    bool ShaperCurve::fromJson (const juce::String& json, ShaperCurve& out)
    {
        if (json.isEmpty())
            return false;

        juce::var parsed;
        if (juce::JSON::parse (json, parsed).failed())
            return false;

        auto* obj = parsed.getDynamicObject();
        if (obj == nullptr)
            return false;

        Interp newInterp = Interp::Smooth;

        if (obj->hasProperty ("interp"))
        {
            const auto iv = obj->getProperty ("interp");
            if (! iv.isString())
                return false;

            const auto s = iv.toString();
            if (s == "linear")      newInterp = Interp::Linear;
            else if (s == "smooth") newInterp = Interp::Smooth;
            else                    return false;
        }

        const auto pv = obj->getProperty ("points");
        auto* arr = pv.getArray();

        if (arr == nullptr || arr->size() < 2 || arr->size() > maxPoints)
            return false;

        std::vector<ShaperPoint> pts;
        pts.reserve ((size_t) arr->size());

        for (const auto& item : *arr)
        {
            auto* p = item.getDynamicObject();
            if (p == nullptr)
                return false;

            const auto xv = p->getProperty ("x"), yv = p->getProperty ("y");
            if (! isNumber (xv) || ! isNumber (yv))
                return false;

            const double x = (double) xv, y = (double) yv;
            if (! std::isfinite (x) || ! std::isfinite (y))
                return false;

            pts.push_back ({ juce::jlimit (-1.0, 1.0, x), juce::jlimit (-1.0, 1.0, y) });
        }

        std::stable_sort (pts.begin(), pts.end(), [] (const ShaperPoint& a, const ShaperPoint& b) { return a.x < b.x; });

        out.interp = newInterp;
        out.points = std::move (pts);
        return true;
    }

    juce::String ShaperCurve::toJson() const
    {
        juce::String s;
        s << "{\"interp\":\"" << (interp == Interp::Linear ? "linear" : "smooth") << "\",\"points\":[";

        for (size_t i = 0; i < points.size(); ++i)
        {
            if (i > 0)
                s << ",";

            s << "{\"x\":" << fmt (points[i].x) << ",\"y\":" << fmt (points[i].y) << "}";
        }

        s << "]}";
        return s;
    }

    //==========================================================================
    std::vector<ShaperPoint> ShaperCurve::halfPoints() const
    {
        std::vector<ShaperPoint> half;

        for (const auto& p : points)
            if (p.x >= 0.0)
                half.push_back (p);

        if (half.empty() || half.front().x > 0.0)
            half.insert (half.begin(), ShaperPoint { 0.0, 0.0 });

        return half;
    }

    void ShaperCurve::scaleY (double k) noexcept
    {
        for (auto& p : points)
            p.y = juce::jlimit (-1.0, 1.0, p.y * k);
    }

    //==========================================================================
    double ShaperCurve::evalPoints (const std::vector<ShaperPoint>& pts, double x, Interp interp) noexcept
    {
        const size_t n = pts.size();

        if (n == 0)
            return 0.0;
        if (n == 1)
            return pts[0].y;
        if (x <= pts[0].x)
            return pts[0].y;
        if (x >= pts[n - 1].x)
            return pts[n - 1].y;

        size_t i = 0;
        while (i < n - 2 && x > pts[i + 1].x)
            ++i;

        const ShaperPoint& p0 = pts[i];
        const ShaperPoint& p1 = pts[i + 1];
        const double dx = std::max (p1.x - p0.x, kMinDx);
        const double t = (x - p0.x) / dx;

        if (interp == Interp::Linear)
            return p0.y + (p1.y - p0.y) * t;

        auto slope = [&pts] (size_t j) { return (pts[j + 1].y - pts[j].y) / std::max (pts[j + 1].x - pts[j].x, kMinDx); };
        auto tangent = [&] (size_t j)
        {
            if (j == 0)
                return slope (0);
            if (j == n - 1)
                return slope (n - 2);

            const double a = slope (j - 1), b = slope (j);
            return a * b <= 0.0 ? 0.0 : 2.0 * a * b / (a + b);
        };

        const double m0 = tangent (i) * dx, m1 = tangent (i + 1) * dx;
        const double t2 = t * t, t3 = t2 * t;

        return (2.0 * t3 - 3.0 * t2 + 1.0) * p0.y + (t3 - 2.0 * t2 + t) * m0
             + (-2.0 * t3 + 3.0 * t2) * p1.y + (t3 - t2) * m1;
    }

    double ShaperCurve::eval (double x, bool symmetric) const
    {
        return Evaluator (*this, symmetric) (x);
    }

    //==========================================================================
    ShaperCurve::Evaluator::Evaluator (const ShaperCurve& curve, bool symmetricMode)
        : pts (symmetricMode ? curve.halfPoints() : curve.points),
          mode (curve.interp),
          symmetric (symmetricMode)
    {
    }

    double ShaperCurve::Evaluator::operator() (double x) const noexcept
    {
        if (! symmetric)
            return evalPoints (pts, x, mode);

        return x >= 0.0 ? evalPoints (pts, x, mode) : -evalPoints (pts, -x, mode);
    }

    std::vector<double> ShaperCurve::Evaluator::breakpoints() const
    {
        std::vector<double> bp;

        for (const auto& p : pts)
        {
            bp.push_back (p.x);

            if (symmetric)
                bp.push_back (-p.x);
        }

        if (symmetric)
            bp.push_back (0.0);

        std::sort (bp.begin(), bp.end());
        bp.erase (std::unique (bp.begin(), bp.end()), bp.end());
        bp.erase (std::remove_if (bp.begin(), bp.end(), [] (double v) { return ! (v > -1.0 && v < 1.0); }), bp.end());
        return bp;
    }
}
