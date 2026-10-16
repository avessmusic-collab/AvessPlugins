#pragma once

#include <array>
#include <cmath>
#include <cstdint>

// Immutable analysis snapshot published by the processor's analysis worker
// (moved verbatim from mockups/v8-PluginEditor.h; keep the guard so the v8
// editor header can still be adopted unchanged in Stage 3).
// Unavailable values are NaN / -inf and are sent to the UI as null (shown as
// dashes) - the UI never fabricates a measurement.
#ifndef PRISM_ANALYSIS_SNAPSHOT_DEFINED
#define PRISM_ANALYSIS_SNAPSHOT_DEFINED 1
namespace prism
{
    inline constexpr int kSourceCount        = 2;      // v1: main + 1 sidechain
    inline constexpr int kSpectrumPoints     = 640;    // log-spaced 20 Hz .. 20 kHz, dBFS (true, un-tilted)
    inline constexpr int kMaxWaveformColumns = 16;     // new columns per snapshot (84.33 columns/s nominal)
    inline constexpr int kGonioPairs         = 900;    // L,R pairs
    // Scope window must cover 20 ms (10 divisions x 2 ms) + up to 800 samples of rising-zero trigger search:
    //   192 kHz: 0.020 s * 192000 = 3840, + 800 = 4640 samples  (4096 was too short above ~164 kHz).
    // 8192 covers every host rate up to 192 kHz with headroom (and 352.8 kHz would need 7856).
    inline constexpr int kScopeSamples       = 8192;

    struct SourceSnapshot
    {
        bool  hasSpectrum = false;
        std::array<float, kSpectrumPoints> spectrumDb {};

        std::int64_t waveformFirstColumn = -1;                // absolute column index of waveformColumns[0] (-1 = none); lets the page place columns by index
        int   waveformColumnCount = 0;                        // each column: bass, mids, highs (0..1), flag (0 none, 1 beat, 2 bar)
        std::array<std::array<float, 4>, kMaxWaveformColumns> waveformColumns {};

        int   gonioPairCount = 0;
        std::array<float, 2 * kGonioPairs> gonio {};          // interleaved L,R in -1..1

        int   scopeCount = 0;
        std::array<float, kScopeSamples> scope {};            // time-domain window in -1..1

        float truePeakMaxDb   = -INFINITY;                    // max since reset_all
        float samplePeakMaxDb = -INFINITY;
        bool  hasStereo       = false;
        float correlation     = 0.0f;                         // -1..+1
        float widthPercent    = 0.0f;                         // 0..100
        float balanceDb       = 0.0f;
    };

    struct AnalysisSnapshot
    {
        std::uint64_t sequence     = 0;                       // strictly increasing; the editor skips repeats
        double        sampleRate   = 0.0;
        float         bpm          = NAN;                     // optional tempo for the waveform header annotation (NaN -> dashes)
        std::int64_t  barAnchorStreamFrame = -1;              // source-0 stream frame of the most recent downbeat (bar event); -1 = none yet
        int           timeSigNumerator = 0, timeSigDenominator = 0;   // host time signature (0 = unknown); optional, additive
        bool          signalPresent = false;                  // false -> UI shows "NO SIGNAL"

        float momentaryLufs  = NAN;                           // 400 ms
        float shortTermLufs  = NAN;                           // 3 s
        float integratedLufs = NAN;                           // NaN until gated data exists / after reset_integrated
        float loudnessRange  = NAN;                           // LU

        std::array<SourceSnapshot, kSourceCount> sources {};
    };
} // namespace prism
#endif
