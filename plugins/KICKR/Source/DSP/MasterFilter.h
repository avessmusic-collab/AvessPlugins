#pragma once

#include <juce_dsp/juce_dsp.h>
#include "Utilities/DSPUtils.h"

namespace kickr
{
    /**
        2026-09-02 (user request): "add to the big screen after wave spectrum a same button
        for filter with a filter with choices of low pass high pass classic ableton style
        filter. and make a button to turn it on and off."

        Master filter — cascade of TPT stages (2 channels), low-pass or high-pass,
        in-region (fsOversampled), placed after the 7-curve waveshaper and before the
        crossover / width / limiter output stage, so it shapes the whole kick like an
        Ableton Auto Filter on the track would. Parameters:
          filterOn     Bool    default OFF -> exact bypass (no processing, bit-identical)
          filterType   Choice  Low Pass / High Pass
          filterSlope  Choice  12 / 18 / 24 dB per octave (2026-09-07; default 18)
          filterFreq   20..20000 Hz (log), default 1000 Hz
          filterRes    0..1, default 0.2 -> Q = 0.5 * 10^(1.4 * res)  (0.5 .. ~12.6)

        Slope cascade (research: pitch-module-and-filter-slope-research.md):
          12 dB  svf1 only — the pre-slope signal path, bit-exact
          18 dB  svf1 + one-pole FirstOrderTPTFilter (6 dB)
          24 dB  svf1 + svf2 at fixed Q = 0.707
        User resonance drives svf1 alone with the same Q map at every slope, so the RES
        knob feels identical across slopes ("musical" scheme; the Butterworth-correct
        per-stage Qs were considered and rejected — res behaviour would change per slope).
        All stages are prepared up front; a slope switch only selects the path and resets
        the newly-entering stages (svf1 keeps its state, so widening the slope mid-note
        cannot glitch the base path).

        Cutoff is smoothed per sample (20 ms) so dragging the node on the FILTER page is
        zipper-free; the TPT structures are stable under fast modulation. Off -> on
        resets the state so a stale tail never pops in.
    */
    class MasterFilter
    {
    public:
        enum class Type { lowPass = 0, highPass = 1 };

        void prepare (double fsOversampled) noexcept;
        void reset() noexcept;
        void updateOversampledRate (double newFsOversampled) noexcept;

        void setParams (bool on, int type, int slope, float freqHz, float res01) noexcept;

        void process (float& l, float& r) noexcept;

        static float qFromResonance (float res01) noexcept
        {
            return 0.5f * std::pow (10.0f, 1.4f * juce::jlimit (0.0f, 1.0f, res01));
        }

    private:
        void applyType() noexcept;

        double fs { 44100.0 };
        bool   enabled { false };
        bool   wasEnabled { false };
        Type   type { Type::lowPass };
        int    slopeIdx { 1 };         // 0 = 12 dB, 1 = 18 dB (default), 2 = 24 dB
        float  lastCutoff { -1.0f };
        float  lastQ { -1.0f };
        juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> cutoff { 1000.0f };
        juce::dsp::StateVariableTPTFilter<float> svf;       // stage 1 — carries the resonance
        juce::dsp::StateVariableTPTFilter<float> svf2;      // stage 2 for 24 dB, fixed Q 0.707
        juce::dsp::FirstOrderTPTFilter<float>    onePole;   // +6 dB stage for 18 dB
    };
}
