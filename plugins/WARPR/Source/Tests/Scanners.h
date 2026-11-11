#pragma once
/*
  Scanners.h - signal-quality probes shared by the per-phase test blocks:
    scan()        NaN/inf, peak, DC offset, largest sample-to-sample step
    aliasDb()     FFT alias meter: energy NOT at harmonics of f0, relative to the harmonic energy
*/

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace warpr::tests
{
    struct ScanResult
    {
        bool   allFinite = true;
        float  peak      = 0.0f;
        double mean      = 0.0;       // DC offset
        float  maxStep   = 0.0f;      // largest |x[n] - x[n-1]|
    };

    inline ScanResult scan (const float* x, int n)
    {
        ScanResult r;
        double sum = 0.0;

        for (int i = 0; i < n; ++i)
        {
            const float v = x[i];
            if (! std::isfinite (v))
            {
                r.allFinite = false;
                continue;
            }

            r.peak = std::max (r.peak, std::abs (v));
            sum += (double) v;
            if (i > 0 && std::isfinite (x[i - 1]))
                r.maxStep = std::max (r.maxStep, std::abs (v - x[i - 1]));
        }

        r.mean = n > 0 ? sum / (double) n : 0.0;
        return r;
    }

    inline ScanResult scan (const juce::AudioBuffer<float>& b, int channel)
    {
        return scan (b.getReadPointer (channel), b.getNumSamples());
    }

    /** FFT alias meter.  Takes the LAST 2^order samples of `x`, applies a Hann window and returns
        10*log10(inharmonic power / harmonic power) in dB, where "harmonic" means within
        +-guardBins of k*f0 (k >= 1).  Use an f0 that is an exact FFT bin centre
        (f0 = sampleRate * m / 2^order) so a clean tone measures below about -90 dB.
        Returns +200 if the input is too short or silent. */
    inline double aliasDb (const float* x, int n, double sampleRate, double f0, int order = 15, int guardBins = 4)
    {
        const int size = 1 << order;
        if (n < size || f0 <= 0.0)
            return 200.0;

        juce::dsp::FFT fft (order);
        std::vector<float> data ((size_t) size * 2, 0.0f);

        const float* tail = x + (n - size);
        for (int i = 0; i < size; ++i)
        {
            const double w = 0.5 - 0.5 * std::cos (2.0 * juce::MathConstants<double>::pi * (double) i / (double) size);
            data[(size_t) i] = tail[i] * (float) w;
        }

        fft.performFrequencyOnlyForwardTransform (data.data());    // magnitudes in data[0 .. size/2]

        const double binHz = sampleRate / (double) size;
        std::vector<bool> harmonic ((size_t) (size / 2 + 1), false);

        for (int k = 1; k * f0 < sampleRate * 0.5; ++k)
        {
            const int centre = (int) std::lround ((double) k * f0 / binHz);
            for (int b = centre - guardBins; b <= centre + guardBins; ++b)
                if (b >= 1 && b <= size / 2)
                    harmonic[(size_t) b] = true;
        }

        double powHarmonic = 0.0, powOther = 0.0;
        for (int b = 1; b <= size / 2; ++b)                         // skip DC
        {
            const double p = (double) data[(size_t) b] * (double) data[(size_t) b];
            (harmonic[(size_t) b] ? powHarmonic : powOther) += p;
        }

        if (powHarmonic <= 1.0e-20)
            return 200.0;

        return 10.0 * std::log10 (std::max (powOther, 1.0e-30) / powHarmonic);
    }
}
