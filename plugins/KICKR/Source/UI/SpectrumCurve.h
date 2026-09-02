#pragma once

#include <cmath>
#include <vector>

#include <juce_core/juce_core.h>

#include "DSP/Analyzer.h"

namespace kickr
{
    /**
        2026-09-02 (user: "feels like the spectrum doesnt show the high end correctly ...
        look at how fabfilter pro q 3 works and implement the same settings") — the shared
        Pro-Q-3-style curve builder used by the SPECTRUM page and the FILTER page backdrop.

        Why the old drawing under-read the high end: it plotted EVERY FFT bin at its own
        level. A kick's tonal low end lands in one bin (reads at full level) while its
        broadband highs (click / noise) are spread over hundreds of bins, each of which is
        tiny — so the top of the display sagged tens of dB below where the energy really was
        and turned into a jagged comb. Pro-Q instead shows one value per PIXEL COLUMN on
        the log axis: the ENERGY average of every bin that falls into the column (highs),
        interpolated between bins where a column is narrower than one bin (lows), lightly
        smoothed, then tilted by N dB/oct around 1 kHz so a natural (pink-ish) spectrum
        reads flat. That is what buildSpectrumColumns() does.
    */
    inline void buildSpectrumColumns (const Analyzer& analyzer, int numColumns,
                                      float fMinHz, float fMaxHz, std::vector<float>& outDb)
    {
        outDb.assign (static_cast<size_t> (juce::jmax (0, numColumns)), -200.0f);
        if (numColumns < 2)
            return;

        const auto&  spec    = analyzer.getSpectrumDb();
        const int    numBins = analyzer.getNumBins();
        const double sr      = juce::jmax (1000.0, analyzer.getSampleRate());
        const double binHz   = (sr * 0.5) / (double) numBins;
        const double fMin    = juce::jmax (1.0, (double) fMinHz);
        const double fMax    = juce::jmax (fMin * 1.01, juce::jmin ((double) fMaxHz, sr * 0.5));
        const double ratio   = fMax / fMin;
        const float  tilt    = analyzer.getView().tiltDbPerOct;

        auto freqAt = [&] (double col) { return fMin * std::pow (ratio, col / (double) (numColumns - 1)); };
        auto dbAtBin = [&] (int b) { return spec[static_cast<size_t> (juce::jlimit (0, numBins - 1, b))]; };

        // Constant-Q smoothing window: at least the pixel column, at least 1/48 octave —
        // light enough to leave a low tonal peak alone (there it is narrower than a bin and
        // the column interpolates), wide enough that a broadband high end reads as a smooth,
        // flat line instead of per-bin noise (the Pro-Q look).
        const double halfOct = std::pow (2.0, 1.0 / 96.0);   // half of 1/48 octave
        std::vector<float> raw (static_cast<size_t> (numColumns));
        for (int i = 0; i < numColumns; ++i)
        {
            const double fc = freqAt (i);
            const double fl = juce::jmin (freqAt (i - 0.5), fc / halfOct);
            const double fh = juce::jmax (freqAt (i + 0.5), fc * halfOct);
            const int bLo = static_cast<int> (std::ceil  (fl / binHz));
            const int bHi = static_cast<int> (std::floor (fh / binHz));

            float db;
            if (bHi >= bLo && bLo >= 1)
            {
                // Energy average of every bin inside this column.
                double p = 0.0; int n = 0;
                for (int b = bLo; b <= juce::jmin (bHi, numBins - 1); ++b, ++n)
                    p += std::pow (10.0, (double) dbAtBin (b) / 10.0);
                db = n > 0 ? static_cast<float> (10.0 * std::log10 (juce::jmax (1.0e-12, p / (double) n))) : -120.0f;
            }
            else
            {
                // Column narrower than a bin: interpolate between neighbours (dB domain).
                const double pos = fc / binHz;
                const int    b0  = juce::jlimit (1, numBins - 1, static_cast<int> (std::floor (pos)));
                const float  t   = static_cast<float> (juce::jlimit (0.0, 1.0, pos - (double) b0));
                db = dbAtBin (b0) + t * (dbAtBin (b0 + 1) - dbAtBin (b0));
            }

            // Tilt around 1 kHz (Pro-Q's "Tilt" — 4.5 dB/oct makes pink noise flat).
            db += tilt * static_cast<float> (std::log2 (fc / 1000.0));
            raw[static_cast<size_t> (i)] = db;
        }

        // Light 3-tap smoothing for a Pro-Q-like continuous curve.
        for (int i = 0; i < numColumns; ++i)
        {
            const float a = raw[static_cast<size_t> (juce::jmax (0, i - 1))];
            const float b = raw[static_cast<size_t> (i)];
            const float c = raw[static_cast<size_t> (juce::jmin (numColumns - 1, i + 1))];
            outDb[static_cast<size_t> (i)] = 0.25f * a + 0.5f * b + 0.25f * c;
        }
    }
}
