#include "LoudnessMeter.h"

#include <complex>

namespace prism
{
    namespace
    {
        constexpr double kPi = 3.14159265358979323846;
    }

    //==========================================================================
    BiquadCoeffs KWeighting::stage1(double fs)
    {
        const double f0 = 1681.974450955533, G = 3.999843853973347, Q = 0.7071752369554196;
        const double K  = std::tan(kPi * f0 / fs);
        const double Vh = std::pow(10.0, G / 20.0);
        const double Vb = std::pow(Vh, 0.4996667741545416);
        const double a0 = 1.0 + K / Q + K * K;

        BiquadCoeffs c;
        c.b0 = (Vh + Vb * K / Q + K * K) / a0;
        c.b1 = 2.0 * (K * K - Vh) / a0;
        c.b2 = (Vh - Vb * K / Q + K * K) / a0;
        c.a1 = 2.0 * (K * K - 1.0) / a0;
        c.a2 = (1.0 - K / Q + K * K) / a0;
        return c;
    }

    BiquadCoeffs KWeighting::stage2(double fs)
    {
        const double f0 = 38.13547087602444, Q = 0.5003270373238773;
        const double K  = std::tan(kPi * f0 / fs);
        const double a0 = 1.0 + K / Q + K * K;

        BiquadCoeffs c;
        c.b0 = 1.0; c.b1 = -2.0; c.b2 = 1.0;
        c.a1 = 2.0 * (K * K - 1.0) / a0;
        c.a2 = (1.0 - K / Q + K * K) / a0;
        return c;
    }

    double KWeighting::gainDb(double fs, double hz)
    {
        using C = std::complex<double>;
        const double w = 2.0 * kPi * hz / fs;
        const C z1 = std::polar(1.0, -w), z2 = std::polar(1.0, -2.0 * w);
        auto h = [&](const BiquadCoeffs& c) { return (c.b0 + c.b1 * z1 + c.b2 * z2) / (1.0 + c.a1 * z1 + c.a2 * z2); };
        return 20.0 * std::log10(std::abs(h(stage1(fs)) * h(stage2(fs))));
    }

    //==========================================================================
    void LoudnessMeter::prepare(double sampleRate, int numChannels)
    {
        fs = sampleRate;
        channels = std::clamp(numChannels, 1, 2);
        chunkLen = std::max(1, (int) std::llround(0.01 * fs));
        c1 = KWeighting::stage1(fs);
        c2 = KWeighting::stage2(fs);

        cnt1.assign((size_t) kBins, 0u); cnt2.assign((size_t) kBins, 0u);
        en1.assign((size_t) kBins, 0.0);  en2.assign((size_t) kBins, 0.0);

        st = {};
        acc[0] = acc[1] = 0.0;
        chunkFill = 0;
        ring.fill(0.0);
        ringPos = ringFilled = 0;
        blockChunks = lraChunks = 0;
        blocksStored = lraStored = gaps = 0;
        intDirty = lraDirty = true;
        intCache = lraCache = NAN;
    }

    void LoudnessMeter::process(const float* l, const float* r, int n) noexcept
    {
        if (chunkLen <= 0 || n <= 0)
            return;

        for (int i = 0; i < n; ++i)
        {
            {
                const double y = filt(c2, st[0][1], filt(c1, st[0][0], (double) l[i]));
                acc[0] += y * y;
            }
            if (channels == 2)
            {
                const double y = filt(c2, st[1][1], filt(c1, st[1][0], (double) r[i]));
                acc[1] += y * y;
            }

            if (++chunkFill >= chunkLen)
            {
                const double z = (acc[0] + acc[1]) / (double) chunkLen;   // channel weights: L = R = 1.0
                acc[0] = acc[1] = 0.0;
                chunkFill = 0;

                // flush denormal-prone filter state after silence
                for (auto& ch : st)
                    for (auto& s : ch)
                    {
                        if (std::abs(s.z1) < 1.0e-30) s.z1 = 0.0;
                        if (std::abs(s.z2) < 1.0e-30) s.z2 = 0.0;
                    }

                pushChunkEnergy(z);
            }
        }
    }

    void LoudnessMeter::notifyGap() noexcept
    {
        // Policy: nothing in flight may span the discontinuity.
        st = {};
        acc[0] = acc[1] = 0.0;
        chunkFill = 0;                 // partial 10 ms chunk discarded
        blockChunks = 0;               // in-flight 400 ms gating block discarded (next one needs 40 post-gap chunks)
        lraChunks = 0;                 // in-flight 3 s LRA window discarded (next sample needs 300 post-gap chunks)
        ring.fill(0.0);                // momentary / short-term windows restart too: M is NaN for 400 ms and S for 3 s,
        ringPos = ringFilled = 0;      //   so no M / S / LRA value is ever a blend of pre- and post-gap audio
        ++gaps;                        // integrated / LRA HISTOGRAMS survive: only reset clears them
    }

    void LoudnessMeter::resetIntegrated() noexcept
    {
        std::fill(cnt1.begin(), cnt1.end(), 0u);
        std::fill(cnt2.begin(), cnt2.end(), 0u);
        std::fill(en1.begin(), en1.end(), 0.0);
        std::fill(en2.begin(), en2.end(), 0.0);
        blocksStored = lraStored = 0;
        blockChunks = lraChunks = 0;   // first block / LRA sample must be built from post-reset audio only
        intDirty = lraDirty = true;
    }

    //==========================================================================
    double LoudnessMeter::meanLast(int n) const noexcept
    {
        double s = 0.0;
        for (int i = 1; i <= n; ++i)
            s += ring[(size_t) ((ringPos - i + kShortTermChunks) % kShortTermChunks)];
        return s / (double) n;
    }

    int LoudnessMeter::binOf(double lufs) noexcept
    {
        return std::clamp((int) std::floor((lufs - kMinLufs) / kBinWidth), 0, kBins - 1);
    }

    float LoudnessMeter::toLufs(double ms) noexcept
    {
        return ms < kSilenceMeanSquare ? (float) NAN : (float) (kOffset + 10.0 * std::log10(ms));
    }

    void LoudnessMeter::addToHistogram(std::vector<std::uint32_t>& cnt, std::vector<double>& en, double z, std::uint64_t& stored) noexcept
    {
        if (! (z > 0.0) || cnt.empty())
            return;
        const double l = kOffset + 10.0 * std::log10(z);
        if (l < kMinLufs)   // absolute gate
            return;
        const int b = binOf(l);
        ++cnt[(size_t) b];
        en[(size_t) b] += z;
        ++stored;
    }

    void LoudnessMeter::pushChunkEnergy(double z) noexcept
    {
        ring[(size_t) ringPos] = z;
        ringPos = (ringPos + 1) % kShortTermChunks;
        ringFilled = std::min(ringFilled + 1, kShortTermChunks);
        ++blockChunks;
        ++lraChunks;

        if (blockChunks >= (std::uint64_t) kMomentaryChunks && (blockChunks - kMomentaryChunks) % kBlockHopChunks == 0)
        {
            addToHistogram(cnt1, en1, meanLast(kMomentaryChunks), blocksStored);
            intDirty = true;
        }
        if (lraChunks >= (std::uint64_t) kShortTermChunks && (lraChunks - kShortTermChunks) % kBlockHopChunks == 0)
        {
            addToHistogram(cnt2, en2, meanLast(kShortTermChunks), lraStored);
            lraDirty = true;
        }
    }

    //==========================================================================
    float LoudnessMeter::momentaryLufs() const noexcept
    {
        return ringFilled >= kMomentaryChunks ? toLufs(meanLast(kMomentaryChunks)) : (float) NAN;
    }

    float LoudnessMeter::shortTermLufs() const noexcept
    {
        return ringFilled >= kShortTermChunks ? toLufs(meanLast(kShortTermChunks)) : (float) NAN;
    }

    float LoudnessMeter::integratedLufs() noexcept
    {
        if (! intDirty)
            return intCache;
        intDirty = false;
        intCache = (float) NAN;

        double e = 0.0;
        std::uint64_t n = 0;
        for (int b = 0; b < kBins; ++b) { n += cnt1[(size_t) b]; e += en1[(size_t) b]; }
        if (n == 0)
            return intCache;

        const double gammaR = kOffset + 10.0 * std::log10(e / (double) n) - 10.0;   // relative gate
        const int start = std::max(0, (int) std::floor((gammaR - kMinLufs) / kBinWidth - 0.5) + 1);   // bin centre > gate

        double ge = 0.0;
        std::uint64_t gn = 0;
        for (int b = start; b < kBins; ++b) { gn += cnt1[(size_t) b]; ge += en1[(size_t) b]; }
        if (gn > 0)
            intCache = (float) (kOffset + 10.0 * std::log10(ge / (double) gn));
        return intCache;
    }

    float LoudnessMeter::loudnessRange() noexcept
    {
        if (! lraDirty)
            return lraCache;
        lraDirty = false;
        lraCache = (float) NAN;

        double e = 0.0;
        std::uint64_t n = 0;
        for (int b = 0; b < kBins; ++b) { n += cnt2[(size_t) b]; e += en2[(size_t) b]; }
        if (n == 0)
            return lraCache;

        const double gammaR = kOffset + 10.0 * std::log10(e / (double) n) - 20.0;   // TECH 3342 relative gate: -20 LU
        const int start = std::max(0, (int) std::floor((gammaR - kMinLufs) / kBinWidth - 0.5) + 1);

        std::uint64_t m = 0;
        for (int b = start; b < kBins; ++b) m += cnt2[(size_t) b];
        if (m == 0)
            return lraCache;

        auto valueAtRank = [&](std::uint64_t rank) {
            std::uint64_t cum = 0;
            for (int b = start; b < kBins; ++b)
            {
                cum += cnt2[(size_t) b];
                if (cum > rank)
                    return kMinLufs + ((double) b + 0.5) * kBinWidth;
            }
            return kMinLufs + ((double) (kBins - 1) + 0.5) * kBinWidth;
        };
        const auto rankOf = [&](double p) { return (std::uint64_t) std::floor(p * (double) (m - 1) + 0.5); };

        lraCache = (float) std::max(0.0, valueAtRank(rankOf(0.95)) - valueAtRank(rankOf(0.10)));
        return lraCache;
    }
} // namespace prism
