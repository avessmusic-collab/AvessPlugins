#include "DSP/ModMatrix.h"

#include <cstring>
#include <vector>

namespace warpr
{
    namespace
    {
        struct RangeTable
        {
            std::array<ParamRange, kNumParams> ranges {};

            RangeTable()
            {
                for (std::size_t i = 0; i < kNumParams; ++i)
                {
                    const ParamMeta& m = kParamMeta[i];
                    ParamRange& r = ranges[i];
                    r.lo = m.minValue;
                    r.hi = m.maxValue;
                    r.isInt = m.kind == ParamKind::Int;

                    // juce::NormalisableRange::setSkewForCentre (float arithmetic, same expression)
                    if (m.kind == ParamKind::Float && m.skewCentre > 0.0f)
                        r.skew = std::log (0.5f) / std::log ((m.skewCentre - m.minValue) / (m.maxValue - m.minValue));
                }
            }
        };

        const RangeTable rangeTable;

        bool isLfoParamIndex (int paramIndex, int& lfoIndex) noexcept
        {
            const int base = (int) toIndex (ParamId::lfo1Mode);
            const int rel = paramIndex - base;

            if (rel < 0 || rel >= 80)
                return false;

            const int slot = rel % 8;                // Mode Dir Sync Rate Phase Smooth GridX GridY
            lfoIndex = rel / 8;
            return slot >= 3 && slot <= 5;
        }
    }

    static_assert ((int) toIndex (ParamId::lfo2Mode) - (int) toIndex (ParamId::lfo1Mode) == 8, "LFO blocks are 8 parameters wide");
    static_assert ((int) toIndex (ParamId::lfo10Smooth) - (int) toIndex (ParamId::lfo1Mode) == 9 * 8 + 5, "LFO block layout");

    const ParamRange* paramRanges() noexcept
    {
        return rangeTable.ranges.data();
    }

    ModClass modClassOf (ParamId p) noexcept
    {
        const ParamMeta& m = kParamMeta[toIndex (p)];

        if (m.kind != ParamKind::Float && m.kind != ParamKind::Int)
            return ModClass::None;

        if (p == ParamId::polyphony || p == ParamId::unisonVoices || p == ParamId::glideTime)
            return ModClass::Latched;

        if (p == ParamId::masterGain)
            return ModClass::Global;

        return ModClass::Voice;
    }

    int paramIndexFromId (const juce::String& id) noexcept
    {
        const auto utf8 = id.toRawUTF8();

        for (std::size_t i = 0; i < kNumParams; ++i)
            if (std::strcmp (kParamMeta[i].id, utf8) == 0)
                return (int) i;

        return -1;
    }

    //==========================================================================
    std::unique_ptr<ModMatrixSnapshot> ModMatrixSnapshot::compile (const RouteIn* input, int inputCount)
    {
        auto snap = std::make_unique<ModMatrixSnapshot>();

        std::vector<Route> kept;
        kept.reserve ((size_t) std::max (0, std::min (inputCount, maxRoutes)));

        for (int i = 0; input != nullptr && i < inputCount && (int) kept.size() < maxRoutes; ++i)
        {
            const RouteIn& in = input[i];

            if (in.src < 1 || in.src > numSources)
                continue;

            if (modClassOf (in.dst) == ModClass::None)
                continue;

            const float depth = in.depth > 1.0f ? 1.0f : in.depth;

            if (! (depth > 0.0f))                    // also drops NaN
                continue;

            Route r;
            r.src = in.src - 1;
            r.dst = (int) toIndex (in.dst);
            r.depth = depth;
            kept.push_back (r);
        }

        std::stable_sort (kept.begin(), kept.end(), [] (const Route& l, const Route& r) { return l.dst < r.dst; });

        snap->numRoutes = (int) kept.size();

        for (int k = 0; k < snap->numRoutes; ++k)
        {
            const Route& r = kept[(size_t) k];
            snap->routes[(size_t) k] = r;
            snap->usedMask |= 1u << r.src;

            if (snap->count[(size_t) r.dst] == 0)
                snap->first[(size_t) r.dst] = (std::uint16_t) k;

            ++snap->count[(size_t) r.dst];

            int lfo = 0;

            if (isLfoParamIndex (r.dst, lfo))
                snap->lfoDestMask |= 1u << lfo;
        }

        return snap;
    }

    std::unique_ptr<ModMatrixSnapshot> ModMatrixSnapshot::fromJson (const juce::String& json)
    {
        std::vector<RouteIn> in;

        const juce::var root = json.isNotEmpty() ? juce::JSON::parse (json) : juce::var();

        if (const auto* arr = root.getArray())
        {
            for (const auto& e : *arr)
            {
                if (e.getDynamicObject() == nullptr)
                    continue;

                const juce::var src = e.getProperty ("src", juce::var());
                const juce::var depth = e.getProperty ("depth", juce::var());
                const juce::var dst = e.getProperty ("dst", juce::var());

                if (! (src.isInt() || src.isInt64() || src.isDouble()) || ! (depth.isInt() || depth.isInt64() || depth.isDouble())
                    || ! dst.isString())
                    continue;

                const double s = static_cast<double> (src);

                if (! (s >= 1.0 && s <= (double) numSources) || ! juce::exactlyEqual (std::floor (s), s))
                    continue;

                const int idx = paramIndexFromId (dst.toString());

                if (idx < 0)
                    continue;

                const double dd = static_cast<double> (depth);

                if (! std::isfinite (dd))
                    continue;

                RouteIn r;
                r.src = (int) s;
                r.dst = static_cast<ParamId> (idx);
                r.depth = (float) std::max (0.0, std::min (1.0, dd));
                in.push_back (r);
            }
        }

        return compile (in.data(), (int) in.size());
    }
}
