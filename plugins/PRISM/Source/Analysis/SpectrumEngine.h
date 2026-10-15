#pragma once

#include <juce_dsp/juce_dsp.h>

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include "AnalysisSnapshot.h"
#include "SourceRegistry.h"
#include "StreamHistory.h"

namespace prism
{
    // Calibrated 640-point dBFS spectrum (architecture C5). Worker-thread only.
    //
    //  - 5 juce::dsp::FFT objects (orders 10..14) and 10 periodic window tables (5 sizes x {Hann,
    //    Blackman-Harris}) are built in prepare(); a size/window switch is a table/pointer switch.
    //  - Hop = N/4, keyed to the ABSOLUTE frame index of the source stream (windows end at multiples of
    //    N/4), so results never depend on block size or on how the worker chunks its drains.
    //  - Coherent-gain normalisation A_k = |X_k| * 2 / S (DC / Nyquist: 1/S), S = sum(w) taken from the
    //    actual table: a bin-centred sine of peak amplitude A reads exactly 20 log10(A) dBFS.
    //  - Hops completed between two publishes are combined as a LINEAR power mean (bin domain); the
    //    640-point log map is applied once per publish to that mean.
    //  - Floor -120 dBFS (silence is exactly -120, never -inf/NaN).
    class SpectrumEngine
    {
    public:
        static constexpr int kNumSizes   = 5;
        static constexpr int kNumWindows = 2;                 // 0 hann, 1 blackmanHarris
        static constexpr int kMaxFft     = 16384;
        static constexpr float kFloorDb  = -120.0f;
        static constexpr double kFloorPower = 1.0e-12;        // -120 dB
        static constexpr std::array<int, kNumSizes> kSizes { 1024, 2048, 4096, 8192, 16384 };

        struct Params
        {
            int sizeIndex   = 2;   // 4096
            int windowIndex = 0;   // hann
            int mode        = 2;   // 0 leftRight, 1 midSide, 2 combined
            bool operator==(const Params& o) const noexcept
            {
                return sizeIndex == o.sizeIndex && windowIndex == o.windowIndex && mode == o.mode;
            }
        };

        SpectrumEngine();

        void prepare(double sampleRate);   // allocates; not for the audio thread

        // Runs every hop whose window ends at or before hist.framesSeen(). Applies new params at the next hop.
        void advance(int source, const StreamHistory& hist, const Params& p);

        // Consumes the accumulated hops (if any) into the source's curve and fills the snapshot fields.
        // hasSpectrum stays false until the first hop of the current settings completed.
        void publish(int source, SourceSnapshot& out);

        //----------------------------------------------------------------------
        // Introspection / tests
        static double pointFrequency(int i) noexcept;   // 20 * 1000^(i/639) Hz
        double getWindowSum(int sizeIndex, int windowIndex) const noexcept        { return windows[(size_t) sizeIndex][(size_t) windowIndex].sum; }
        double getWindowSumSquares(int sizeIndex, int windowIndex) const noexcept { return windows[(size_t) sizeIndex][(size_t) windowIndex].sumSquares; }
        float  getWindowValue(int sizeIndex, int windowIndex, int n) const noexcept { return windows[(size_t) sizeIndex][(size_t) windowIndex].w[(size_t) n]; }
        Params getActiveParams(int source) const noexcept { return states[(size_t) source].active; }
        int    getAccumulatedHops(int source) const noexcept { return states[(size_t) source].accHops; }
        std::uint64_t getTotalHops(int source) const noexcept { return states[(size_t) source].totalHops; }
        // Mean per-bin power (calibrated, linear) of the hops accumulated since the last publish. Non-consuming.
        bool getMeanBinPower(int source, std::vector<double>& out) const;

    private:
        struct WindowTable { std::vector<float> w; double sum = 0.0, sumSquares = 0.0; };
        struct PointMap { int kLo = 0, kHi = -1; int k0 = 0; float frac = 0.0f; bool beyondNyquist = false; };
        struct SourceState
        {
            std::vector<double> acc;
            int accHops = 0;
            bool have = false;
            std::array<float, kSpectrumPoints> curve {};
            Params active;
            bool activeValid = false;
            std::uint64_t nextHopEnd = 0, lastEnd = 0, totalHops = 0;
        };

        void computeHop(SourceState& st, const StreamHistory& h, std::uint64_t endAbs);
        void buildMap(int sizeIndex);
        static std::uint64_t roundUp(std::uint64_t v, std::uint64_t m) noexcept { return ((v + m - 1) / m) * m; }

        double fs = 0.0;
        bool prepared = false;
        std::vector<std::unique_ptr<juce::dsp::FFT>> ffts;
        std::array<std::array<WindowTable, kNumWindows>, kNumSizes> windows;
        std::array<std::array<PointMap, kSpectrumPoints>, kNumSizes> maps;
        std::array<SourceState, kMaxSources> states;

        std::vector<float> fftBuf, bufL, bufR, binDb;
    };
} // namespace prism
