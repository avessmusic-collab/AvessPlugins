#include "DSP/MasterFilter.h"

namespace kickr
{
    void MasterFilter::applyType() noexcept
    {
        svf.setType  (type == Type::lowPass ? juce::dsp::StateVariableTPTFilterType::lowpass
                                            : juce::dsp::StateVariableTPTFilterType::highpass);
        svf2.setType (type == Type::lowPass ? juce::dsp::StateVariableTPTFilterType::lowpass
                                            : juce::dsp::StateVariableTPTFilterType::highpass);
        onePole.setType (type == Type::lowPass ? juce::dsp::FirstOrderTPTFilterType::lowpass
                                               : juce::dsp::FirstOrderTPTFilterType::highpass);
    }

    void MasterFilter::prepare (double fsOversampled) noexcept
    {
        fs = juce::jmax (1.0, fsOversampled);
        juce::dsp::ProcessSpec spec;
        spec.sampleRate       = fs;
        spec.maximumBlockSize = 32;
        spec.numChannels      = 2;
        svf.prepare (spec);
        svf2.prepare (spec);
        onePole.prepare (spec);
        svf2.setResonance (0.70710678f);   // fixed Butterworth-ish companion stage (24 dB)
        applyType();
        cutoff.reset (fs, 0.02);
        lastCutoff = lastQ = -1.0f;
        reset();
    }

    void MasterFilter::reset() noexcept
    {
        svf.reset();
        svf2.reset();
        onePole.reset();
        cutoff.setCurrentAndTargetValue (cutoff.getTargetValue());
        wasEnabled = enabled;
    }

    void MasterFilter::updateOversampledRate (double newFsOversampled) noexcept
    {
        fs = juce::jmax (1.0, newFsOversampled);
        juce::dsp::ProcessSpec spec;
        spec.sampleRate       = fs;
        spec.maximumBlockSize = 32;
        spec.numChannels      = 2;
        svf.prepare (spec);   // same channel count -> no allocation; clears state (covered by the OS-switch fade)
        svf2.prepare (spec);
        onePole.prepare (spec);
        svf2.setResonance (0.70710678f);
        applyType();
        cutoff.reset (fs, 0.02);
        lastCutoff = lastQ = -1.0f;
    }

    void MasterFilter::setParams (bool on, int newType, int newSlope, float freqHz, float res01) noexcept
    {
        enabled = on;

        const Type t = newType == 1 ? Type::highPass : Type::lowPass;
        if (t != type)
        {
            type = t;
            applyType();
        }

        const int s = juce::jlimit (0, 2, newSlope);
        if (s != slopeIdx)
        {
            // Reset only the stages the new slope brings in — svf1 keeps its state so the
            // base 12 dB path never glitches on a slope switch.
            if (s == 1) onePole.reset();
            if (s == 2) svf2.reset();
            slopeIdx = s;
            lastCutoff = -1.0f;   // push the smoothed cutoff into the newly-active stage
        }

        cutoff.setTargetValue (juce::jlimit (20.0f, juce::jmin (20000.0f, static_cast<float> (fs * 0.45)), freqHz));

        const float q = qFromResonance (res01);
        if (std::abs (q - lastQ) > 1.0e-4f)
        {
            lastQ = q;
            svf.setResonance (q);
        }
    }

    void MasterFilter::process (float& l, float& r) noexcept
    {
        if (! enabled)
        {
            wasEnabled = false;
            cutoff.getNextValue();
            return;
        }
        if (! wasEnabled)
        {
            svf.reset();       // fresh state on switch-on
            svf2.reset();
            onePole.reset();
            wasEnabled = true;
        }

        const float c = cutoff.getNextValue();
        if (std::abs (c - lastCutoff) > 0.01f)
        {
            lastCutoff = c;
            svf.setCutoffFrequency (c);
            if (slopeIdx == 1) onePole.setCutoffFrequency (c);
            if (slopeIdx == 2) svf2.setCutoffFrequency (c);
        }

        l = svf.processSample (0, l);
        r = svf.processSample (1, r);

        if (slopeIdx == 1)
        {
            l = onePole.processSample (0, l);
            r = onePole.processSample (1, r);
            onePole.snapToZero();
        }
        else if (slopeIdx == 2)
        {
            l = svf2.processSample (0, l);
            r = svf2.processSample (1, r);
            svf2.snapToZero();
        }

        l = dsputils::sanitize (l);
        r = dsputils::sanitize (r);
        svf.snapToZero();
    }
}
