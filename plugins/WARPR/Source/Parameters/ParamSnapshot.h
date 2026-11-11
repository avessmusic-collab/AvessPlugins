#pragma once
/*
  ParamSnapshot - cached atomic pointers to all 135 raw parameter values.

  Bound ONCE (message thread, processor constructor).  The audio thread then reads values by
  enum index: no string lookups, no locks, no allocation in processBlock.  `read()` copies all
  135 values into a plain ParamBlock once per audio block (relaxed loads).

  Raw values are DENORMALISED (Float: real units; Int: the integer as float; Bool: 0/1;
  Choice: the index as float).  Convert Int/Choice with juce::roundToInt (NOT truncation:
  Int ranges include negatives, e.g. transpose = -24).
*/

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <atomic>

#include "Parameters/ParamTable.h"

namespace warpr
{
    /** Plain per-block copy of every parameter's raw value. */
    struct ParamBlock
    {
        std::array<float, kNumParams> values {};

        float get (ParamId p) const noexcept      { return values[toIndex (p)]; }
        int   getInt (ParamId p) const noexcept   { return juce::roundToInt (values[toIndex (p)]); }
        bool  getBool (ParamId p) const noexcept  { return values[toIndex (p)] >= 0.5f; }
    };

    class ParamSnapshot
    {
    public:
        /** MESSAGE THREAD.  Resolves every id once; asserts if the layout and the table diverge. */
        void bind (juce::AudioProcessorValueTreeState& apvts)
        {
            for (std::size_t i = 0; i < kNumParams; ++i)
            {
                raw[i] = apvts.getRawParameterValue (kParamMeta[i].id);
                jassert (raw[i] != nullptr);
            }
        }

        /** AUDIO THREAD (and any thread): wait-free atomic read. */
        float get (ParamId p) const noexcept
        {
            const auto* a = raw[toIndex (p)];
            return a != nullptr ? a->load (std::memory_order_relaxed) : 0.0f;
        }

        int  getInt (ParamId p) const noexcept   { return juce::roundToInt (get (p)); }
        bool getBool (ParamId p) const noexcept  { return get (p) >= 0.5f; }

        /** AUDIO THREAD: copy all values once per block. */
        void read (ParamBlock& out) const noexcept
        {
            for (std::size_t i = 0; i < kNumParams; ++i)
                out.values[i] = raw[i] != nullptr ? raw[i]->load (std::memory_order_relaxed) : 0.0f;
        }

    private:
        std::array<std::atomic<float>*, kNumParams> raw {};
    };
}
