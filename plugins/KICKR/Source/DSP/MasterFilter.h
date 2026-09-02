#pragma once

#include <juce_dsp/juce_dsp.h>
#include "Utilities/DSPUtils.h"

namespace kickr
{
    /**
        2026-09-02 (user request): "add to the big screen after wave spectrum a same button
        for filter with a filter with choices of low pass high pass classic ableton style
        filter. and make a button to turn it on and off."

        Master filter — one `juce::dsp::StateVariableTPTFilter<float>` (2 channels), 12 dB
        low-pass or high-pass, in-region (fsOversampled), placed after the 7-curve
        waveshaper and before the crossover / width / limiter output stage, so it shapes
        the whole kick like an Ableton Auto Filter on the track would. Parameters:
          filterOn    Bool    default OFF -> exact bypass (no processing, bit-identical)
          filterType  Choice  Low Pass / High Pass
          filterFreq  20..20000 Hz (log), default 1000 Hz
          filterRes   0..1, default 0.2 -> Q = 0.5 * 10^(1.4 * res)  (0.5 .. ~12.6)
        Cutoff is smoothed per sample (20 ms) so dragging the node on the FILTER page is
        zipper-free; the SVF's TPT structure is stable under fast modulation. Off -> on
        resets the state so a stale tail never pops in.
    */
    class MasterFilter
    {
    public:
        enum class Type { lowPass = 0, highPass = 1 };

        void prepare (double fsOversampled) noexcept;
        void reset() noexcept;
        void updateOversampledRate (double newFsOversampled) noexcept;

        void setParams (bool on, int type, float freqHz, float res01) noexcept;

        void process (float& l, float& r) noexcept;

        static float qFromResonance (float res01) noexcept
        {
            return 0.5f * std::pow (10.0f, 1.4f * juce::jlimit (0.0f, 1.0f, res01));
        }

    private:
        double fs { 44100.0 };
        bool   enabled { false };
        bool   wasEnabled { false };
        Type   type { Type::lowPass };
        float  lastCutoff { -1.0f };
        float  lastQ { -1.0f };
        juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> cutoff { 1000.0f };
        juce::dsp::StateVariableTPTFilter<float> svf;
    };
}
