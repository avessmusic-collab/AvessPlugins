#pragma once
/*
  ModMatrix - the compiled modulation matrix (architecture C11 / section I + "Modulation destination rules"; phase D8).

  WIRE FORMAT (parameter-spec "Non-Parameter State" 4):   [ { "src": 1..10, "dst": "<parameterId>", "depth": 0..1 } ]
  Compile step (message thread): unknown ids, Choice / Bool destinations, src outside 1..10 and depth <= 0 are dropped,
  depth is clamped to 0..1, at most maxRoutes (256) routes are kept (input order).  The routes are stored as flat
  structure-of-arrays sorted by destination (stable), so the audio thread never allocates or searches.

  MATH (EXACT, in the destination's own normalised - i.e. SKEWED - domain, identical to juce::NormalisableRange):
      norm  = convertTo0to1(value)
      norm' = clamp(norm + sum_s depth_s * (2 v_s - 1), 0, 1)
      value' = convertFrom0to1(norm')           Int destinations: round to nearest afterwards (the value can step).
  `applyModulation` with a delta that is exactly 0 returns the value untouched (bit-exact bypass).

  DESTINATION CLASSES
      Voice    V   evaluated per voice per 32-sample chunk (start and end value; consumers interpolate)
      Global   G   masterGain: the global sources, voice-owned LFOs contribute their mean over the active voices
      Latched  N   polyphony, unisonVoices, glideTime: sampled at note-on (a later change never touches a sounding note)
      None         Choice / Bool parameters are not destinations
  Task wording called class N "none"; the architecture (Modulation destination rules) defines N = latched at note-on and
  that is what is implemented.  ampAttack is a V parameter whose only consumer is the note-on, so it behaves as latched.

  LFO-on-LFO: lfoNRate / lfoNPhase / lfoNSmooth are ordinary destinations; LfoEngine.h orders the evaluation.
*/

#include "Parameters/ParamTable.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>

namespace warpr
{
    //==========================================================================
    /** Normalised-domain conversion identical to the APVTS parameters' juce::NormalisableRange (setSkewForCentre). */
    struct ParamRange
    {
        float lo = 0.0f, hi = 1.0f, skew = 1.0f;
        bool isInt = false;

        float to01 (float v) const noexcept
        {
            float p = (v - lo) / (hi - lo);
            p = p < 0.0f ? 0.0f : (p > 1.0f ? 1.0f : p);
            return juce::exactlyEqual (skew, 1.0f) ? p : std::pow (p, skew);
        }

        float from01 (float n) const noexcept
        {
            float p = n < 0.0f ? 0.0f : (n > 1.0f ? 1.0f : n);

            if (! juce::exactlyEqual (skew, 1.0f) && p > 0.0f)
                p = std::exp (std::log (p) / skew);

            return lo + (hi - lo) * p;
        }
    };

    /** One range per parameter (index = ParamId).  Built at static-init time; Choice / Bool entries are plain 0..max. */
    const ParamRange* paramRanges() noexcept;

    inline const ParamRange& paramRange (ParamId p) noexcept { return paramRanges()[toIndex (p)]; }

    /** norm' = clamp(norm + delta, 0, 1) in the parameter's normalised domain; Int ranges round to nearest. */
    inline float applyModulation (const ParamRange& r, float value, float delta) noexcept
    {
        if (juce::exactlyEqual (delta, 0.0f))
            return value;

        float n = r.to01 (value) + delta;
        n = n < 0.0f ? 0.0f : (n > 1.0f ? 1.0f : n);
        float out = r.from01 (n);

        if (r.isInt)
        {
            out = std::floor (out + 0.5f);
            out = out < r.lo ? r.lo : (out > r.hi ? r.hi : out);
        }

        return out;
    }

    inline float applyModulation (ParamId p, float value, float delta) noexcept
    {
        return applyModulation (paramRange (p), value, delta);
    }

    //==========================================================================
    enum class ModClass : std::uint8_t { None, Voice, Global, Latched };

    ModClass modClassOf (ParamId p) noexcept;

    /** Index of a parameter id string in the table (-1 if unknown). */
    int paramIndexFromId (const juce::String& id) noexcept;

    //==========================================================================
    class ModMatrixSnapshot
    {
    public:
        static constexpr int numSources = 10;
        static constexpr int maxRoutes  = 256;

        struct RouteIn { int src = 1; ParamId dst = ParamId::sampleStart; float depth = 0.0f; };

        struct Route
        {
            int src = 0;                 // 0-based LFO index
            int dst = 0;                 // ParamId index
            float depth = 0.0f;
        };

        /** Never nullptr.  Invalid routes are dropped (see the header comment). */
        static std::unique_ptr<ModMatrixSnapshot> compile (const RouteIn* input, int inputCount);

        /** Wire-format JSON; malformed / non-array documents compile to an empty matrix. */
        static std::unique_ptr<ModMatrixSnapshot> fromJson (const juce::String& json);

        bool empty() const noexcept                        { return numRoutes == 0; }
        int  getNumRoutes() const noexcept                 { return numRoutes; }
        const Route& route (int i) const noexcept          { return routes[(size_t) i]; }

        /** Bit i = LFO i+1 is the source of at least one route. */
        std::uint32_t usedSourceMask() const noexcept      { return usedMask; }

        /** Bit i = LFO i+1 is the DESTINATION of a Rate / Phase / Smooth route (LFO-on-LFO). */
        std::uint32_t lfoDestinationMask() const noexcept  { return lfoDestMask; }

        bool hasDestination (ParamId p) const noexcept     { return count[toIndex (p)] > 0; }

        /** sum_s depth_s * srcMod[s] for one destination; `srcMod` = 10 values in [-1, 1] (inactive sources 0). */
        float delta (int paramIndex, const float* srcMod) const noexcept
        {
            const int n = count[(size_t) paramIndex];
            const int f = first[(size_t) paramIndex];
            float sum = 0.0f;

            for (int k = 0; k < n; ++k)
            {
                const Route& r = routes[(size_t) (f + k)];
                sum += r.depth * srcMod[r.src];
            }

            return sum;
        }

        float delta (ParamId p, const float* srcMod) const noexcept { return delta ((int) toIndex (p), srcMod); }

        /** Fills d0 / d1 (kNumParams entries each, indexed by ParamId) with the deltas at the start and the end of a
            chunk.  startV / endV = LFO values 0..1 (mod = 2v - 1); sources >= lfoCount contribute 0. */
        void evaluate (const float* startV, const float* endV, int lfoCount, float* d0, float* d1) const noexcept
        {
            std::fill (d0, d0 + kNumParams, 0.0f);
            std::fill (d1, d1 + kNumParams, 0.0f);

            for (int k = 0; k < numRoutes; ++k)
            {
                const Route& r = routes[(size_t) k];

                if (r.src >= lfoCount)
                    continue;

                d0[r.dst] += r.depth * (2.0f * startV[r.src] - 1.0f);
                d1[r.dst] += r.depth * (2.0f * endV[r.src] - 1.0f);
            }
        }

    private:
        int numRoutes = 0;
        std::uint32_t usedMask = 0, lfoDestMask = 0;
        std::array<Route, (size_t) maxRoutes> routes {};
        std::array<std::uint16_t, kNumParams> first {}, count {};
    };
}
