#include "DSP/LfoTable.h"

#include <algorithm>
#include <cmath>

namespace warpr
{
    namespace
    {
        constexpr double kTau = 6.283185307179586;

        struct PresetEntry { const char* name; LfoShapeDesc::Preset preset; };

        constexpr PresetEntry kPresetNames[LfoShapeDesc::numPresets] = {
            { "CUSTOM",     LfoShapeDesc::Preset::Custom },
            { "SINE",       LfoShapeDesc::Preset::Sine },
            { "TRIANGLE",   LfoShapeDesc::Preset::Triangle },
            { "SAW UP",     LfoShapeDesc::Preset::SawUp },
            { "SAW DOWN",   LfoShapeDesc::Preset::SawDown },
            { "SQUARE",     LfoShapeDesc::Preset::Square },
            { "PUMP",       LfoShapeDesc::Preset::Pump },
            { "STEPS 4",    LfoShapeDesc::Preset::Steps4 },
            { "STEPS 8",    LfoShapeDesc::Preset::Steps8 },
            { "STEPS 16",   LfoShapeDesc::Preset::Steps16 },
            { "SWING",      LfoShapeDesc::Preset::Swing },
            { "S&H NOISE",  LfoShapeDesc::Preset::SandH },
            { "DRIFT",      LfoShapeDesc::Preset::Drift }
        };

        bool numberOf (const juce::var& v, double& out) noexcept
        {
            if (! (v.isInt() || v.isInt64() || v.isDouble() || v.isBool()))
                return false;

            out = static_cast<double> (v);
            return std::isfinite (out);
        }

        double clampd (double v, double lo, double hi) noexcept { return v < lo ? lo : (v > hi ? hi : v); }

        /** index.html stepsFn(n) converted to v. */
        double stepsValue (int n, double t) noexcept
        {
            const int k = std::min (n - 1, (int) std::floor (t * (double) n));
            return 0.9 - 0.8 * ((double) k / (double) (n - 1));
        }

        /** index.html presetFn(), y -> v = 1 - y.  `t` in [0, 1]. */
        double presetFn (const LfoShapeDesc& d, double t) noexcept
        {
            using P = LfoShapeDesc::Preset;

            switch (d.preset)
            {
                case P::Sine:      return 0.5 + 0.4 * std::sin (kTau * t);
                case P::Triangle:  return 0.9 - 0.8 * std::abs (2.0 * t - 1.0);
                case P::SawUp:     return 0.1 + 0.8 * std::fmod (t, 1.0);
                case P::SawDown:   return 0.9 - 0.8 * std::fmod (t, 1.0);
                case P::Square:    return t < 0.5 ? 0.9 : 0.1;
                case P::Pump:      return 0.1 + 0.8 * std::exp (-4.5 * t);
                case P::Steps4:    return stepsValue (4, t);
                case P::Steps8:    return stepsValue (8, t);
                case P::Steps16:   return stepsValue (16, t);
                case P::Swing:
                {
                    const double u = t < 0.62 ? std::pow (t / 0.62, 1.5) : 1.0 - 0.75 * ((t - 0.62) / 0.38);
                    return 0.1 + 0.8 * u;
                }
                case P::SandH:
                {
                    if (d.table.empty())
                        return 0.5;

                    const int len = (int) d.table.size();
                    const int k = std::min (len - 1, (int) std::floor (t * (double) len));
                    return 1.0 - d.table[(size_t) std::max (0, k)];
                }
                case P::Drift:
                    return 1.0 - (0.5 + 0.26 * std::sin (kTau * t + d.driftA) + 0.14 * std::sin (2.0 * kTau * t + d.driftB));
                case P::Custom:
                default:           break;
            }

            return 0.5;
        }
    }

    //==========================================================================
    // LfoShapeDesc
    //==========================================================================
    const char* LfoShapeDesc::presetName (Preset p) noexcept
    {
        for (const auto& e : kPresetNames)
            if (e.preset == p)
                return e.name;

        return "CUSTOM";
    }

    bool LfoShapeDesc::presetFromName (const juce::String& name, Preset& out) noexcept
    {
        for (const auto& e : kPresetNames)
            if (name == e.name)
            {
                out = e.preset;
                return true;
            }

        return false;
    }

    LfoShapeDesc LfoShapeDesc::defaultShape()
    {
        // index.html newShape(): y = .9 .1 .65 .25 .9  ->  v = 1 - y
        LfoShapeDesc d;
        d.preset = Preset::Custom;
        d.points = { { 0.0, 0.1, 0.0 }, { 0.25, 0.9, 0.0 }, { 0.5, 0.35, 0.0 }, { 0.75, 0.75, 0.0 }, { 1.0, 0.1, 0.0 } };
        return d;
    }

    bool LfoShapeDesc::fromJson (const juce::String& json, LfoShapeDesc& out)
    {
        if (json.isEmpty())
            return false;

        const juce::var root = juce::JSON::parse (json);

        if (root.getDynamicObject() == nullptr)
            return false;

        LfoShapeDesc d;

        if (! presetFromName (root.getProperty ("preset", juce::var()).toString(), d.preset))
            d.preset = Preset::Custom;                      // the UI draws an unknown id through the points path

        if (const auto* pts = root.getProperty ("points", juce::var()).getArray())
        {
            for (const auto& e : *pts)
            {
                double x = 0.0, v = 0.0, c = 0.0;

                if (e.getDynamicObject() == nullptr || ! numberOf (e.getProperty ("x", juce::var()), x)
                    || ! numberOf (e.getProperty ("v", juce::var()), v))
                    continue;

                numberOf (e.getProperty ("c", juce::var()), c);
                d.points.push_back ({ clampd (x, 0.0, 1.0), clampd (v, 0.0, 1.0), clampd (c, -1.0, 1.0) });

                if (d.points.size() >= 256)
                    break;
            }
        }

        if (const auto* bs = root.getProperty ("bends", juce::var()).getArray())
        {
            for (const auto& e : *bs)
            {
                double x = 0.0, v = 0.0;

                if (e.getDynamicObject() == nullptr || ! numberOf (e.getProperty ("x", juce::var()), x)
                    || ! numberOf (e.getProperty ("v", juce::var()), v))
                    continue;

                d.bends.push_back ({ clampd (x, 0.0, 1.0), clampd (v, 0.0, 1.0) });

                if (d.bends.size() >= 256)
                    break;
            }
        }

        if (const auto* tb = root.getProperty ("table", juce::var()).getArray())
        {
            for (const auto& e : *tb)
            {
                double v = 0.0;

                if (numberOf (e, v))
                    d.table.push_back (clampd (v, 0.0, 1.0));

                if (d.table.size() >= 256)
                    break;
            }

            d.hasTable = ! d.table.empty();
        }

        const juce::var drift = root.getProperty ("drift", juce::var());

        if (drift.getDynamicObject() != nullptr)
        {
            double a = 0.0, b = 0.0;
            numberOf (drift.getProperty ("a", juce::var()), a);
            numberOf (drift.getProperty ("b", juce::var()), b);
            d.driftA = a;
            d.driftB = b;
            d.hasDrift = true;
        }

        std::stable_sort (d.points.begin(), d.points.end(), [] (const Point& l, const Point& r) { return l.x < r.x; });
        std::stable_sort (d.bends.begin(), d.bends.end(), [] (const Bend& l, const Bend& r) { return l.x < r.x; });

        if (d.preset == Preset::Custom && d.points.size() < 2)
            d.points = defaultShape().points;               // index.html loadUiState() does the same

        out = std::move (d);
        return true;
    }

    //==========================================================================
    // Evaluator
    //==========================================================================
    LfoTableBuilder::Evaluator::Evaluator (const LfoShapeDesc& desc) : d (desc)
    {
        std::stable_sort (d.points.begin(), d.points.end(),
                          [] (const LfoShapeDesc::Point& l, const LfoShapeDesc::Point& r) { return l.x < r.x; });
        std::stable_sort (d.bends.begin(), d.bends.end(),
                          [] (const LfoShapeDesc::Bend& l, const LfoShapeDesc::Bend& r) { return l.x < r.x; });

        if (d.preset != LfoShapeDesc::Preset::Custom && ! d.bends.empty())
        {
            ax.push_back (0.0);
            adv.push_back (0.0);

            for (const auto& b : d.bends)
            {
                ax.push_back (b.x);
                adv.push_back (b.v - presetFn (d, b.x));
            }

            ax.push_back (1.0);
            adv.push_back (0.0);
        }
    }

    double LfoTableBuilder::Evaluator::custom (double t) const
    {
        const auto& p = d.points;
        const int n = (int) p.size();

        if (n == 0)
            return 0.5;

        if (t < p.front().x)
            return p.front().v;

        const auto it = std::upper_bound (p.begin(), p.end(), t,
                                          [] (double value, const LfoShapeDesc::Point& pt) { return value < pt.x; });
        const int i = (int) (it - p.begin()) - 1;

        if (i >= n - 1)
            return p.back().v;

        const auto& a = p[(size_t) i];
        const auto& b = p[(size_t) i + 1];
        const double u = (t - a.x) / (b.x - a.x);            // b.x > t >= a.x
        return a.v + (b.v - a.v) * std::pow (u, std::exp2 (3.0 * a.c));
    }

    double LfoTableBuilder::Evaluator::preset (double t) const
    {
        const double fn = presetFn (d, t);

        if (ax.empty())
            return fn;

        const int count = (int) ax.size();
        int i = 0;

        while (i < count - 2 && t > ax[(size_t) i + 1])
            ++i;

        const double x0 = ax[(size_t) i], x1 = ax[(size_t) i + 1];
        const double u = (t - x0) / std::max (x1 - x0, 0.001);
        const double uu = u * u * (3.0 - 2.0 * u);
        return fn + adv[(size_t) i] * (1.0 - uu) + adv[(size_t) i + 1] * uu;
    }

    double LfoTableBuilder::Evaluator::operator() (double t) const
    {
        t = clampd (t, 0.0, 1.0);
        return d.preset == LfoShapeDesc::Preset::Custom ? custom (t) : preset (t);
    }

    //==========================================================================
    // Builder
    //==========================================================================
    double LfoTableBuilder::evalExact (const LfoShapeDesc& d, double t)
    {
        return Evaluator (d) (t);
    }

    double LfoTableBuilder::presetValue (const LfoShapeDesc& d, double t)
    {
        t = clampd (t, 0.0, 1.0);

        if (d.preset == LfoShapeDesc::Preset::Custom)
            return Evaluator (d) (t);

        return presetFn (d, t);
    }

    std::unique_ptr<LfoShapeTable> LfoTableBuilder::build (const LfoShapeDesc& d)
    {
        auto table = std::make_unique<LfoShapeTable>();
        const Evaluator ev (d);
        constexpr double h = 1.0 / (double) LfoShapeTable::kCells;
        constexpr double eps = 1.0e-9;

        for (int i = 0; i < LfoShapeTable::kCells; ++i)
        {
            const double t0 = (double) i * h;
            const double t1 = (double) (i + 1) * h;

            table->start[(size_t) i] = (float) clampd (ev (t0), 0.0, 1.0);                       // right limit at the left edge
            table->end[(size_t) i]   = (float) clampd (ev (std::max (t0, t1 - eps)), 0.0, 1.0);   // left limit at the right edge
        }

        return table;
    }
}
