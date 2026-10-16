#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace prism
{
    struct BiquadCoeffs
    {
        double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;   // a0 normalised to 1
    };

    // ITU-R BS.1770 K-weighting, derived analytically per sample rate in double precision
    // (libebur128 convention: reproduces the BS.1770 48 kHz coefficient table).
    struct KWeighting
    {
        static BiquadCoeffs stage1(double fs);   // pre-filter: high shelf (~ +4 dB above 2 kHz)
        static BiquadCoeffs stage2(double fs);   // RLB high-pass (38 Hz)
        static double gainDb(double fs, double hz);   // |H1 H2| at hz, in dB
    };

    // EBU R128 / BS.1770-5 / TECH 3342 loudness for ONE source (architecture C6). Worker-thread only.
    //
    //  - K-weighted mean-square accumulated in 10 ms chunks; momentary = mean of the last 40 chunks,
    //    short-term = mean of the last 300 (recomputed from the chunk ring on demand: no drift).
    //  - Gating blocks: 400 ms, 75 % overlap = one block every 10 chunks once 40 fresh chunks exist.
    //    Block energies go into a fine histogram (0.01 LU bins, -70..+10 LUFS, 8000 bins: count + energy
    //    sum per bin) so memory is constant for any session length.
    //  - Integrated: absolute gate -70 LUFS, relative gate -10 LU below the power mean of the abs-gated blocks.
    //  - LRA (TECH 3342): short-term values sampled every 100 ms, abs gate -70, relative gate -20 LU,
    //    LRA = P95 - P10 of the gated distribution (histogram bin centres).
    //  - Unavailable values are NaN (never 0).
    class LoudnessMeter
    {
    public:
        static constexpr int kMomentaryChunks = 40;
        static constexpr int kShortTermChunks = 300;
        static constexpr int kBlockHopChunks  = 10;
        static constexpr int kBins            = 8000;
        static constexpr double kMinLufs      = -70.0;
        static constexpr double kBinWidth     = 0.01;
        static constexpr double kOffset       = -0.691;
        static constexpr double kSilenceMeanSquare = 1.0e-12;

        void prepare(double sampleRate, int numChannels);

        // Frames are expected sanitised (finite). r is ignored for mono.
        void process(const float* l, const float* r, int n) noexcept;

        // Ring overrun (stream discontinuity): filter state, the partial chunk, the chunk ring and BOTH in-flight
        // windows (400 ms gating block, 3 s LRA window) are discarded, so nothing measured afterwards blends
        // pre- and post-gap audio: momentary is NaN again until 40 post-gap chunks (400 ms), short-term and the
        // next LRA sample until 300 (3 s), the next gating block until 40. The integrated / LRA histograms
        // (already-committed blocks and samples) survive; only reset clears them.
        void notifyGap() noexcept;

        // reset_integrated: clears gating + LRA histograms only (momentary / short-term are moving windows).
        void resetIntegrated() noexcept;

        // Feed one pre-computed 10 ms chunk energy (sum over channels of K-weighted mean-square).
        // Public so tests can simulate hours of programme without filtering.
        void pushChunkEnergy(double z) noexcept;

        float momentaryLufs() const noexcept;
        float shortTermLufs() const noexcept;
        float integratedLufs() noexcept;
        float loudnessRange() noexcept;

        // introspection
        int  getChunkLength() const noexcept { return chunkLen; }
        std::uint64_t getGapCount() const noexcept { return gaps; }
        std::uint64_t getGatedBlockCount() const noexcept { return blocksStored; }
        std::uint64_t getLraSampleCount() const noexcept { return lraStored; }
        std::size_t getHistogramBytes() const noexcept
        {
            return (cnt1.capacity() + cnt2.capacity()) * sizeof(std::uint32_t) + (en1.capacity() + en2.capacity()) * sizeof(double);
        }

    private:
        struct State { double z1 = 0.0, z2 = 0.0; };
        static double filt(const BiquadCoeffs& c, State& s, double x) noexcept
        {
            const double y = c.b0 * x + s.z1;
            s.z1 = c.b1 * x - c.a1 * y + s.z2;
            s.z2 = c.b2 * x - c.a2 * y;
            return y;
        }
        double meanLast(int n) const noexcept;
        static int binOf(double lufs) noexcept;
        static float toLufs(double meanSquare) noexcept;
        void addToHistogram(std::vector<std::uint32_t>& cnt, std::vector<double>& en, double z, std::uint64_t& stored) noexcept;

        double fs = 0.0;
        int channels = 1;
        int chunkLen = 480;
        BiquadCoeffs c1, c2;
        std::array<std::array<State, 2>, 2> st {};   // [channel][stage]
        double acc[2] = { 0.0, 0.0 };
        int chunkFill = 0;

        std::array<double, kShortTermChunks> ring {};
        int ringPos = 0, ringFilled = 0;
        std::uint64_t blockChunks = 0, lraChunks = 0;

        std::vector<std::uint32_t> cnt1, cnt2;
        std::vector<double> en1, en2;
        std::uint64_t blocksStored = 0, lraStored = 0, gaps = 0;

        bool intDirty = true, lraDirty = true;
        float intCache = NAN, lraCache = NAN;
    };
} // namespace prism
