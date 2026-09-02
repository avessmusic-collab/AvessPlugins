#include "DSP/MasterFilter.h"

namespace kickr
{
    void MasterFilter::prepare (double fsOversampled) noexcept
    {
        fs = juce::jmax (1.0, fsOversampled);
        juce::dsp::ProcessSpec spec;
        spec.sampleRate       = fs;
        spec.maximumBlockSize = 32;
        spec.numChannels      = 2;
        svf.prepare (spec);
        svf.setType (type == Type::lowPass ? juce::dsp::StateVariableTPTFilterType::lowpass
                                           : juce::dsp::StateVariableTPTFilterType::highpass);
        cutoff.reset (fs, 0.02);
        lastCutoff = lastQ = -1.0f;
        reset();
    }

    void MasterFilter::reset() noexcept
    {
        svf.reset();
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
        svf.setType (type == Type::lowPass ? juce::dsp::StateVariableTPTFilterType::lowpass
                                           : juce::dsp::StateVariableTPTFilterType::highpass);
        cutoff.reset (fs, 0.02);
        lastCutoff = lastQ = -1.0f;
    }

    void MasterFilter::setParams (bool on, int newType, float freqHz, float res01) noexcept
    {
        enabled = on;

        const Type t = newType == 1 ? Type::highPass : Type::lowPass;
        if (t != type)
        {
            type = t;
            svf.setType (type == Type::lowPass ? juce::dsp::StateVariableTPTFilterType::lowpass
                                               : juce::dsp::StateVariableTPTFilterType::highpass);
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
            wasEnabled = true;
        }

        const float c = cutoff.getNextValue();
        if (std::abs (c - lastCutoff) > 0.01f)
        {
            lastCutoff = c;
            svf.setCutoffFrequency (c);
        }

        l = dsputils::sanitize (svf.processSample (0, l));
        r = dsputils::sanitize (svf.processSample (1, r));
        svf.snapToZero();
    }
}
