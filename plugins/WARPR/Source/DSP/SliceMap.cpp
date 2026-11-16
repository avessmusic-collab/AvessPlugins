#include "DSP/SliceMap.h"

#include <algorithm>
#include <cmath>

namespace warpr
{
    namespace
    {
        bool isNumber (const juce::var& v) noexcept
        {
            return v.isInt() || v.isInt64() || v.isDouble();
        }
    }

    //==========================================================================
    juce::String SliceMap::defaultJson()
    {
        return R"({"on":false,"baseNote":36,"edges":[]})";
    }

    bool SliceMap::parse (const juce::String& json, SliceMap& out)
    {
        out = SliceMap {};

        if (json.isEmpty())
            return false;

        const juce::var root = juce::JSON::parse (json);
        auto* obj = root.getDynamicObject();

        if (obj == nullptr)
            return false;

        const juce::var edgesVar = obj->getProperty ("edges");
        const juce::Array<juce::var>* arr = edgesVar.getArray();

        if (arr == nullptr || arr->size() < 2)
            return false;

        SliceMap m;

        const juce::var onVar = obj->getProperty ("on");
        m.on = onVar.isBool() ? static_cast<bool> (onVar)
                              : (isNumber (onVar) && ! juce::exactlyEqual (static_cast<double> (onVar), 0.0));

        const juce::var baseVar = obj->getProperty ("baseNote");

        if (isNumber (baseVar))
        {
            const double b = static_cast<double> (baseVar);
            m.baseNote = std::isfinite (b) ? juce::jlimit (0, 127, juce::roundToInt (b)) : 36;
        }

        const int n = std::min (arr->size(), maxSlices + 1);
        float previous = 0.0f;

        for (int i = 0; i < n; ++i)
        {
            const juce::var& e = arr->getReference (i);

            if (! isNumber (e))
                return false;

            const double d = static_cast<double> (e);

            if (! std::isfinite (d))
                return false;

            const float f = static_cast<float> (juce::jlimit (0.0, 1.0, d));

            if (i > 0 && f < previous)
                return false;                                  // descending edge: malformed

            m.edges[(size_t) i] = f;
            previous = f;
        }

        m.count = n - 1;
        out = m;
        return true;
    }

    std::unique_ptr<SliceMap> SliceMap::compile (const juce::String& json)
    {
        auto m = std::make_unique<SliceMap>();
        parse (json, *m);                                      // resets *m to OFF on failure
        return m;
    }

    //==========================================================================
    std::vector<double> autoSliceEdges (const float* c0, const float* c1, int numFrames, double sampleRate,
                                        double startNorm, double endNorm, int count, bool byTransients)
    {
        count = juce::jlimit (1, SliceMap::maxSlices, count);

        double s = juce::jlimit (0.0, 1.0, startNorm);
        double e = juce::jlimit (0.0, 1.0, endNorm);

        if (e < s)
            std::swap (s, e);

        auto equalDivision = [&]
        {
            std::vector<double> out ((size_t) count + 1);

            for (int k = 0; k <= count; ++k)
                out[(size_t) k] = s + (e - s) * (double) k / (double) count;

            out.front() = s;
            out.back() = e;
            return out;
        };

        if (! byTransients || count < 2 || c0 == nullptr || numFrames <= 0 || ! (sampleRate > 0.0))
            return equalDivision();

        const int a = juce::jlimit (0, numFrames, (int) std::ceil (s * (double) numFrames));
        const int b = juce::jlimit (0, numFrames, (int) std::floor (e * (double) numFrames));
        const int len = b - a;
        const int win = std::max (8, (int) std::lround (0.010 * sampleRate));
        const int hop = std::max (1, win / 4);

        if (len < win + hop)
            return equalDivision();

        auto mono = [&] (int i) noexcept -> double
        {
            return c1 != nullptr ? 0.5 * ((double) c0[i] + (double) c1[i]) : (double) c0[i];
        };

        // 1. energy per 10 ms window (prefix sums of x^2, double precision)
        std::vector<double> prefix ((size_t) len + 1, 0.0);

        for (int i = 0; i < len; ++i)
        {
            const double v = mono (a + i);
            prefix[(size_t) i + 1] = prefix[(size_t) i] + v * v;
        }

        const int numWin = (len - win) / hop + 1;
        std::vector<double> energy ((size_t) numWin), rise ((size_t) numWin, 0.0);

        for (int j = 0; j < numWin; ++j)
            energy[(size_t) j] = (prefix[(size_t) (j * hop + win)] - prefix[(size_t) (j * hop)]) / (double) win;

        // 2. half-wave rectified energy rise
        double maxRise = 0.0;

        for (int j = 1; j < numWin; ++j)
        {
            rise[(size_t) j] = std::max (0.0, energy[(size_t) j] - energy[(size_t) (j - 1)]);
            maxRise = std::max (maxRise, rise[(size_t) j]);
        }

        if (! (maxRise > 0.0))
            return equalDivision();

        // 3. candidates: local maxima of the rise, at least 5 % of the strongest, refined to the first sample of the entering
        //    slice that reaches half of that slice's peak
        struct Candidate { double strength; int pos; };
        std::vector<Candidate> candidates;

        for (int j = 1; j < numWin; ++j)
        {
            const double r = rise[(size_t) j];

            if (! (r > rise[(size_t) (j - 1)]) || (j + 1 < numWin && r < rise[(size_t) (j + 1)]) || r < 0.05 * maxRise)
                continue;

            const int p0 = (j - 1) * hop + win;                // first sample that entered the window at step j
            double peak = 0.0;

            for (int i = p0; i < p0 + hop; ++i)
                peak = std::max (peak, std::abs (mono (a + i)));

            int pos = p0;

            for (int i = p0; i < p0 + hop; ++i)
                if (std::abs (mono (a + i)) >= 0.5 * peak)
                {
                    pos = i;
                    break;
                }

            candidates.push_back ({ r, pos });
        }

        // 4. STRICT STRENGTH ORDER (owner request 2026-11-16): the first line is always the
        //    single biggest transient, the next line the next biggest, and so on - raising the
        //    cap only APPENDS lines, it never moves or replaces existing ones.  30 ms minimum
        //    distance to the start, the end and every already-chosen onset (a candidate inside
        //    that gap is skipped and the NEXT-strongest takes its place).
        std::sort (candidates.begin(), candidates.end(), [] (const Candidate& x, const Candidate& y)
                   { return x.strength > y.strength || (juce::exactlyEqual (x.strength, y.strength) && x.pos < y.pos); });

        const int gap = std::max (1, (int) std::lround (0.030 * sampleRate));
        std::vector<int> chosen;

        for (const auto& c : candidates)
        {
            if ((int) chosen.size() >= count - 1)
                break;

            if (c.pos < gap || len - c.pos < gap)
                continue;

            bool spaced = true;                                // (not "far": a Windows header macro)

            for (const int other : chosen)
                if (std::abs (c.pos - other) < gap)
                {
                    spaced = false;
                    break;
                }

            if (spaced)
                chosen.push_back (c.pos);
        }

        // 5. use what was FOUND - `count` is a cap, not a promise.  No grid fallback in transient
        //    mode: raising the cap past the sample's real hits simply stops adding lines.  (The
        //    degenerate cases above - no rise, short region, transient mode off - still grid.)
        if (chosen.empty())
            return equalDivision();

        std::sort (chosen.begin(), chosen.end());

        std::vector<double> out (chosen.size() + 2);
        out.front() = s;

        for (size_t k = 0; k < chosen.size(); ++k)
            out[k + 1] = (double) (a + chosen[k]) / (double) numFrames;

        out.back() = e;
        return out;
    }
}
