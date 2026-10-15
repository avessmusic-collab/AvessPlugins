#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace prism
{
    // Streaming 4x polyphase true-peak detector (BS.1770-5 Annex 2 structure: 48 taps = 4 phases x 12 taps
    // per channel). Used at EVERY host sample rate (4x is a superset of the "oversample to >= 192 kHz"
    // reading of the standard for rates >= 48 kHz). Worker-thread only; no allocation.
    //
    // COEFFICIENTS ARE DERIVED, NOT EMBEDDED: a 48-tap Kaiser-windowed-sinc low-pass on the 4x grid
    // (cutoff 0.92 x input Nyquist, beta 5, centre on tap 24 so phase 0 is the near-identity sample phase,
    // exactly like the Annex 2 filter), then each polyphase branch is normalised to unity DC gain.
    // Passband / image-rejection are verified numerically by the loudness-independent tests
    // (true_peak_filter_response): reconstruction error < 2 % up to fs/4, images rejected by >= 38 dB.
    //
    // Peak value = max |y| over the 4 phases of every input sample, both channels; it never decays.
    // The TP >= SP invariant is enforced where the two are folded for publication (AnalysisEngine).
    class TruePeakMeter
    {
    public:
        static constexpr int kTaps = 12, kPhases = 4;
        using Coeffs = std::array<std::array<float, kTaps>, kPhases>;   // [phase][tap], tap 0 = OLDEST input sample

        static const Coeffs& coefficients()
        {
            static const Coeffs c = build();
            return c;
        }

        void prepare(int numChannels) noexcept
        {
            channels = std::clamp(numChannels, 1, 2);
            reset();
        }

        // FIR history + maximum.
        void reset() noexcept
        {
            notifyGap();
            maxLin = 0.0f;
        }
        void resetMax() noexcept { maxLin = 0.0f; }
        // Stream discontinuity (ring overrun): drop FIR history so the jump does not ring through the filter.
        void notifyGap() noexcept
        {
            for (auto& ch : chan) { ch.h.fill(0.0f); ch.pos = 0; ch.primed = false; }
        }

        void process(const float* l, const float* r, int n) noexcept
        {
            float peak = 0.0f;
            float y[kPhases];
            for (int i = 0; i < n; ++i)
            {
                filterSample(0, l[i], y);
                peak = std::max({ peak, std::abs(y[0]), std::abs(y[1]), std::abs(y[2]), std::abs(y[3]) });
                if (channels == 2)
                {
                    filterSample(1, r[i], y);
                    peak = std::max({ peak, std::abs(y[0]), std::abs(y[1]), std::abs(y[2]), std::abs(y[3]) });
                }
            }
            maxLin = std::max(maxLin, peak);
        }

        // One input sample -> 4 interpolated outputs (phase 0 .. 3 = +0, +1/4, +2/4, +3/4 sample).
        // Output phase p of input n sits at input time n - 6 + p/4 (the filter's group delay is 6 samples).
        void filterSample(int channel, float x, float* out4) noexcept
        {
            auto& c = chan[(size_t) channel];
            if (! c.primed)
            {
                // First sample after reset / gap / start: extend it flat into the history instead of a zero
                // history, so a stream that begins (or resumes) mid-signal does not see a 0 -> x step whose
                // Gibbs overshoot would read as a false inter-sample peak.
                c.h.fill(x);
                c.primed = true;
            }
            c.h[(size_t) c.pos] = x;
            c.h[(size_t) (c.pos + kTaps)] = x;               // duplicated so the last 12 samples are contiguous
            c.pos = (c.pos + 1) % kTaps;
            const float* w = c.h.data() + c.pos;             // oldest -> newest
            const auto& k = coefficients();
            for (int p = 0; p < kPhases; ++p)
            {
                float acc = 0.0f;
                for (int t = 0; t < kTaps; ++t) acc += k[(size_t) p][(size_t) t] * w[t];
                out4[p] = acc;
            }
        }

        float maxLinear() const noexcept { return maxLin; }

        // Magnitude response of the composite 48-tap filter at f (in units of the INPUT sample rate), dB,
        // normalised so DC = 0 dB. For tests.
        static double responseDb(double fOverFs)
        {
            const auto& k = coefficients();
            double re = 0.0, im = 0.0, dc = 0.0;
            const double w4 = 2.0 * 3.14159265358979323846 * fOverFs / 4.0;   // radians per 4x-grid sample
            for (int p = 0; p < kPhases; ++p)
                for (int j = 0; j < kTaps; ++j)
                {
                    const int kk = kTaps - 1 - j;                // convolution tap index (0 = newest)
                    const int m = 4 * kk + p;
                    const double v = (double) k[(size_t) p][(size_t) j];
                    re += v * std::cos(w4 * m);
                    im -= v * std::sin(w4 * m);
                    dc += v;
                }
            return 20.0 * std::log10(std::sqrt(re * re + im * im) / dc);
        }

    private:
        static Coeffs build()
        {
            constexpr double kPi = 3.14159265358979323846, cutoff = 0.92, beta = 5.0;
            auto i0 = [](double x) {
                double s = 1.0, t = 1.0;
                for (int k = 1; k < 40; ++k) { t *= (x / (2.0 * k)) * (x / (2.0 * k)); s += t; }
                return s;
            };

            double h[kTaps * kPhases];
            for (int m = 0; m < kTaps * kPhases; ++m)
            {
                const int i = m - 24;                            // centred on tap 24 -> phase 0 centre tap is index 6
                const double x = cutoff * (double) i / 4.0;
                const double sinc = x == 0.0 ? 1.0 : std::sin(kPi * x) / (kPi * x);
                const double r = (double) i / 24.0;
                const double win = i0(beta * std::sqrt(std::max(0.0, 1.0 - r * r))) / i0(beta);
                h[m] = cutoff * sinc * win;
            }

            Coeffs out {};
            for (int p = 0; p < kPhases; ++p)
            {
                double sum = 0.0;
                for (int k = 0; k < kTaps; ++k) sum += h[4 * k + p];
                for (int j = 0; j < kTaps; ++j)
                    out[(size_t) p][(size_t) j] = (float) (h[4 * (kTaps - 1 - j) + p] / sum);   // oldest-first
            }
            return out;
        }

        struct Chan { std::array<float, 2 * kTaps> h {}; int pos = 0; bool primed = false; };
        std::array<Chan, 2> chan {};
        int channels = 2;
        float maxLin = 0.0f;
    };
} // namespace prism
