#include "SpectrumEngine.h"

#include <algorithm>
#include <cmath>

namespace prism
{
    namespace
    {
        constexpr double kPi = 3.14159265358979323846;
        constexpr float kInvSqrt2 = 0.70710678118654752f;

        // Log-point edges: point i spans [edge(i), edge(i+1)); the bands tile 20 Hz .. 20 kHz exactly,
        // so every FFT bin centre in range belongs to exactly one point.
        double edgeHz(int j) noexcept { return 20.0 * std::pow(1000.0, ((double) j - 0.5) / (double) (kSpectrumPoints - 1)); }
    }

    SpectrumEngine::SpectrumEngine() = default;

    double SpectrumEngine::pointFrequency(int i) noexcept
    {
        return 20.0 * std::pow(1000.0, (double) i / (double) (kSpectrumPoints - 1));
    }

    void SpectrumEngine::prepare(double sampleRate)
    {
        fs = sampleRate;
        prepared = false;
        if (fs <= 0.0)
            return;

        ffts.clear();
        for (int s = 0; s < kNumSizes; ++s)
        {
            ffts.push_back(std::make_unique<juce::dsp::FFT>(10 + s));

            const int n = kSizes[(size_t) s];
            for (int wi = 0; wi < kNumWindows; ++wi)
            {
                auto& t = windows[(size_t) s][(size_t) wi];
                t.w.assign((size_t) n, 0.0f);
                t.sum = t.sumSquares = 0.0;
                for (int i = 0; i < n; ++i)
                {
                    // Periodic (DFT-even) windows: the correct form for spectral analysis. S is taken
                    // numerically from the table, so the normaliser always matches the table.
                    const double x = 2.0 * kPi * (double) i / (double) n;
                    const double v = wi == 0 ? 0.5 - 0.5 * std::cos(x)
                                             : 0.35875 - 0.48829 * std::cos(x) + 0.14128 * std::cos(2.0 * x) - 0.01168 * std::cos(3.0 * x);
                    t.w[(size_t) i] = (float) v;
                    t.sum += (double) t.w[(size_t) i];
                    t.sumSquares += (double) t.w[(size_t) i] * (double) t.w[(size_t) i];
                }
            }
            buildMap(s);
        }

        fftBuf.assign((size_t) (2 * kMaxFft), 0.0f);
        bufL.assign((size_t) kMaxFft, 0.0f);
        bufR.assign((size_t) kMaxFft, 0.0f);
        binDb.assign((size_t) (kMaxFft / 2 + 1), kFloorDb);
        prepared = true;
    }

    void SpectrumEngine::buildMap(int sizeIndex)
    {
        const int n = kSizes[(size_t) sizeIndex];
        const int nyq = n / 2;
        const double df = fs / (double) n;

        for (int i = 0; i < kSpectrumPoints; ++i)
        {
            PointMap m;
            const double lo = edgeHz(i), hi = edgeHz(i + 1);
            int kLo = (int) std::ceil(lo / df - 1.0e-9);
            int kHi = (int) std::ceil(hi / df - 1.0e-9) - 1;   // half-open [lo, hi)
            kLo = std::max(kLo, 1);

            if (kLo > nyq)
            {
                m.beyondNyquist = true;
            }
            else if (kHi >= kLo)
            {
                m.kLo = kLo;
                m.kHi = std::min(kHi, nyq);       // bins finer than the point spacing: max over the band
            }
            else
            {
                const double f = pointFrequency(i);   // bins coarser than the point spacing: dB interpolation
                const double pos = f / df;
                int k0 = (int) std::floor(pos);
                float frac = (float) (pos - (double) k0);
                if (k0 >= nyq) { k0 = nyq - 1; frac = 1.0f; }
                m.kLo = 0; m.kHi = -1;
                m.k0 = k0;
                m.frac = frac;
            }
            maps[(size_t) sizeIndex][(size_t) i] = m;
        }
    }

    //==========================================================================
    void SpectrumEngine::advance(int source, const StreamHistory& h, const Params& pIn)
    {
        if (! prepared || source < 0 || source >= kMaxSources || h.numChannels() <= 0)
            return;

        Params p;
        p.sizeIndex   = std::clamp(pIn.sizeIndex, 0, kNumSizes - 1);
        p.windowIndex = std::clamp(pIn.windowIndex, 0, kNumWindows - 1);
        p.mode        = std::clamp(pIn.mode, 0, 2);

        auto& st = states[(size_t) source];
        if (st.acc.empty())
            st.acc.assign((size_t) (kMaxFft / 2 + 1), 0.0);

        if (! st.activeValid || ! (st.active == p))
        {
            // Settings change takes effect at the next hop: the old accumulation is in another domain.
            st.active = p;
            st.activeValid = true;
            std::fill(st.acc.begin(), st.acc.end(), 0.0);
            st.accHops = 0;
            st.have = false;
            st.nextHopEnd = 0;
        }

        const std::uint64_t n = (std::uint64_t) kSizes[(size_t) p.sizeIndex];
        const std::uint64_t hop = n / 4;
        const std::uint64_t seen = h.framesSeen();
        if (seen < n)
            return;

        if (st.nextHopEnd == 0)
            st.nextHopEnd = roundUp(std::max<std::uint64_t>(n, st.lastEnd + 1), hop);

        // Skip hops whose window has already scrolled out of the history (worker stalled / re-enabled).
        const std::uint64_t cap = (std::uint64_t) StreamHistory::kCapacity;
        const std::uint64_t oldestEnd = (seen > cap ? seen - cap : 0) + n;
        if (st.nextHopEnd < oldestEnd)
            st.nextHopEnd = roundUp(oldestEnd, hop);

        while (st.nextHopEnd <= seen)
        {
            computeHop(st, h, st.nextHopEnd);
            st.lastEnd = st.nextHopEnd;
            st.nextHopEnd += hop;
        }
    }

    void SpectrumEngine::computeHop(SourceState& st, const StreamHistory& h, std::uint64_t endAbs)
    {
        const Params p = st.active;
        const int n = kSizes[(size_t) p.sizeIndex];
        const int nyq = n / 2;
        const auto& win = windows[(size_t) p.sizeIndex][(size_t) p.windowIndex];
        const float* w = win.w.data();
        const double invS = 1.0 / win.sum;
        const std::uint64_t start = endAbs - (std::uint64_t) n;
        const bool stereo = h.numChannels() == 2;
        auto& fft = *ffts[(size_t) p.sizeIndex];

        auto addPower = [&](double weight) {
            // fftBuf[0..nyq] now holds |X_k|
            const double a0 = (double) fftBuf[0] * invS;
            st.acc[0] += weight * a0 * a0;
            for (int k = 1; k < nyq; ++k)
            {
                const double a = (double) fftBuf[(size_t) k] * 2.0 * invS;
                st.acc[(size_t) k] += weight * a * a;
            }
            const double an = (double) fftBuf[(size_t) nyq] * invS;
            st.acc[(size_t) nyq] += weight * an * an;
        };

        if (! h.copyAbsolute(0, start, n, bufL.data()))
            return;
        if (stereo && p.mode != 0 && ! h.copyAbsolute(1, start, n, bufR.data()))
            return;

        if (! stereo || p.mode == 0)                      // mono source, or Left channel
        {
            for (int i = 0; i < n; ++i) fftBuf[(size_t) i] = bufL[(size_t) i] * w[i];
            fft.performFrequencyOnlyForwardTransform(fftBuf.data(), true);
            addPower(1.0);
        }
        else if (p.mode == 1)                             // Mid = (L + R) / sqrt2
        {
            for (int i = 0; i < n; ++i) fftBuf[(size_t) i] = (bufL[(size_t) i] + bufR[(size_t) i]) * kInvSqrt2 * w[i];
            fft.performFrequencyOnlyForwardTransform(fftBuf.data(), true);
            addPower(1.0);
        }
        else                                              // combined: power mean (|L|^2 + |R|^2) / 2
        {
            for (int i = 0; i < n; ++i) fftBuf[(size_t) i] = bufL[(size_t) i] * w[i];
            fft.performFrequencyOnlyForwardTransform(fftBuf.data(), true);
            addPower(0.5);
            for (int i = 0; i < n; ++i) fftBuf[(size_t) i] = bufR[(size_t) i] * w[i];
            fft.performFrequencyOnlyForwardTransform(fftBuf.data(), true);
            addPower(0.5);
        }

        ++st.accHops;
        ++st.totalHops;
    }

    //==========================================================================
    void SpectrumEngine::publish(int source, SourceSnapshot& out)
    {
        if (! prepared || source < 0 || source >= kMaxSources)
            return;
        auto& st = states[(size_t) source];

        if (st.accHops > 0)
        {
            const int sizeIndex = st.active.sizeIndex;
            const int nyq = kSizes[(size_t) sizeIndex] / 2;
            const double inv = 1.0 / (double) st.accHops;
            for (int k = 0; k <= nyq; ++k)
            {
                const double pw = st.acc[(size_t) k] * inv;
                binDb[(size_t) k] = pw <= kFloorPower ? kFloorDb : (float) (10.0 * std::log10(pw));
            }

            const auto& map = maps[(size_t) sizeIndex];
            for (int i = 0; i < kSpectrumPoints; ++i)
            {
                const PointMap& m = map[(size_t) i];
                float v = kFloorDb;
                if (m.beyondNyquist)
                    v = kFloorDb;
                else if (m.kHi >= m.kLo)
                {
                    v = binDb[(size_t) m.kLo];
                    for (int k = m.kLo + 1; k <= m.kHi; ++k) v = std::max(v, binDb[(size_t) k]);
                }
                else
                {
                    const float a = binDb[(size_t) m.k0], b = binDb[(size_t) std::min(m.k0 + 1, nyq)];
                    v = a + (b - a) * m.frac;
                }
                st.curve[(size_t) i] = std::max(v, kFloorDb);
            }

            st.have = true;
            std::fill(st.acc.begin(), st.acc.end(), 0.0);
            st.accHops = 0;
        }

        out.hasSpectrum = st.have;
        if (st.have)
            out.spectrumDb = st.curve;
    }

    bool SpectrumEngine::getMeanBinPower(int source, std::vector<double>& out) const
    {
        if (source < 0 || source >= kMaxSources)
            return false;
        const auto& st = states[(size_t) source];
        if (st.accHops <= 0)
            return false;
        const int nyq = kSizes[(size_t) st.active.sizeIndex] / 2;
        out.assign((size_t) (nyq + 1), 0.0);
        for (int k = 0; k <= nyq; ++k)
            out[(size_t) k] = st.acc[(size_t) k] / (double) st.accHops;
        return true;
    }
} // namespace prism
