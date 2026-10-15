#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <atomic>
#include <memory>
#include <vector>

#include "AnalysisSnapshot.h"
#include "LoudnessMeter.h"
#include "SourceRegistry.h"
#include "SpectrumEngine.h"
#include "StereoMetrics.h"
#include "StreamHistory.h"
#include "../Capture/BeatEventQueue.h"
#include "../Capture/SpscFrameRing.h"

namespace prism
{
    // Deterministic analysis core (architecture C2/C3/C4/C11 skeleton, Phase 2.1).
    //
    //   AUDIO THREAD :  captureBlock(), capturePlayhead()   (wait-free, no alloc, never writes audio)
    //   WORKER/TESTS :  pump(), buildSnapshot()             (plain methods; the worker only schedules them)
    //
    // pump()/buildSnapshot() must be driven by ONE thread at a time (the worker, or a
    // test calling them synchronously). Construction allocates everything; nothing
    // allocates on the audio thread.
    class AnalysisEngine
    {
    public:
        static constexpr int kDrainChunk = 4096;
        static constexpr int kBeatLogSize = 4096;

        explicit AnalysisEngine(const EngineConfig& cfg,
                                int ringLog2Capacity = SpscFrameRing::kDefaultLog2Capacity);

        // Message-thread -> worker controls (all polled by the worker; atomics owned by the processor).
        struct ExternalControls
        {
            const std::atomic<std::uint32_t>* resetAllEpoch        = nullptr;
            const std::atomic<std::uint32_t>* resetIntegratedEpoch = nullptr;   // consumed from Phase 2.4
            const std::atomic<float>*         analysisMode         = nullptr;   // raw APVTS value: 0 L/R, 1 M/S, 2 combined
            const std::atomic<float>*         fftSizeIndex         = nullptr;   // raw APVTS choice index 0..4 (1024..16384)
            const std::atomic<float>*         windowIndex          = nullptr;   // raw APVTS choice index 0 hann, 1 blackmanHarris
        };

        // FFT hops only run while enabled (the worker enables them while the editor is active).
        void setSpectrumEnabled(bool e) noexcept { spectrumEnabled.store(e, std::memory_order_relaxed); }
        SpectrumEngine& getSpectrum() noexcept { return spectrum; }
        LoudnessMeter& getLoudness(int source) noexcept { return sources[(size_t) source].loudness; }
        // Call before the worker starts. Baselines the epochs: resets requested earlier are not re-applied.
        void attachControls(const ExternalControls& c) noexcept;

        static constexpr float kSignalThresholdLin = 3.1622777e-5f;   // -90 dBFS
        static constexpr double kSignalHoldSeconds = 0.5;
        // min(kScopeSamples, ceil(0.020 fs) + 800 + 256)  (architecture C9)
        static int scopeLengthFor(double sampleRate) noexcept;
        // max(1, round(3 fs / 48000))  (architecture C9): 900 pairs span ~56 ms at any rate
        static int gonioStrideFor(double sampleRate) noexcept;

        const EngineConfig& getConfig() const noexcept { return config; }
        const SourceRegistry& getRegistry() const noexcept { return registry; }

        //======================================================================
        // AUDIO THREAD (read-only on the host buffer).
        // chans: the host buffer's read pointers; numBufChannels: its channel count.
        void captureBlock(const float* const* chans, int numBufChannels, int numSamples) noexcept;
        // Call after captureBlock() for the same block. playHead may be null.
        void capturePlayhead(juce::AudioPlayHead* playHead, int numSamples) noexcept;

        //======================================================================
        // WORKER / TESTS
        void pump();                                              // drain rings -> history, fold peaks, pop beats
        std::shared_ptr<const AnalysisSnapshot> buildSnapshot();  // immutable, sequence strictly increasing
        bool takeNewFramesFlag() noexcept;                        // true once per pump() that drained frames

        void setSequenceBase(std::uint64_t base) noexcept { sequence.store(base, std::memory_order_relaxed); }
        std::uint64_t getLastSequence() const noexcept { return sequence.load(std::memory_order_relaxed); }

        // --- introspection (tests / debug stats) ---
        const StreamHistory& getHistory(int source) const noexcept { return sources[(size_t) source].history; }
        SpscFrameRing& getRing(int source) noexcept { return sources[(size_t) source].ring; }
        float getSamplePeakMaxLinear(int source) const noexcept { return sources[(size_t) source].samplePeakMaxLin; }
        std::uint64_t getOverrunCount(int source) const noexcept { return sources[(size_t) source].ring.getOverrunCount(); }
        std::uint64_t getGapEvents(int source) const noexcept { return sources[(size_t) source].gapEvents; }
        std::uint32_t getNonFiniteCount(int source) const noexcept
        {
            return sources[(size_t) source].nonFiniteCount.load(std::memory_order_relaxed);
        }
        BeatEventQueue& getBeatQueue() noexcept { return beatQueue; }
        std::uint64_t getBeatEventsSeen() const noexcept { return beatsSeen; }
        const BeatEvent& getBeatLogEntry(std::uint64_t absoluteIndex) const noexcept
        {
            return beatLog[(size_t) (absoluteIndex % kBeatLogSize)];
        }
        float getBpm() const noexcept { return bpm.load(std::memory_order_relaxed); }

    private:
        struct SourceState
        {
            SpscFrameRing ring;
            StreamHistory history;
            alignas(64) std::atomic<float> samplePeak { 0.0f };    // audio-thread fetch_max, worker exchange(0)
            std::atomic<std::uint32_t> nonFiniteCount { 0 };
            // worker-private
            float samplePeakMaxLin = 0.0f;
            std::uint64_t lastOverrunSeen = 0;
            std::uint64_t gapEvents = 0;
            StereoMetrics stereo;
            LoudnessMeter loudness;
            std::uint64_t lastOverrunLoud = 0;
            bool pendingGap = false;
            std::uint64_t pendingGapPos = 0;
            std::uint64_t framesSinceLoud = std::uint64_t(1) << 62;   // frames since |x| > -90 dBFS
        };

        int readAnalysisMode() const noexcept;
        SpectrumEngine::Params readSpectrumParams() const noexcept;

        SpectrumEngine spectrum;
        std::atomic<bool> spectrumEnabled { true };
        EngineConfig config;
        ExternalControls controls;
        std::uint32_t seenResetAll = 0, seenResetIntegrated = 0;
        SourceRegistry registry;
        std::array<SourceState, kMaxSources> sources;
        BeatEventQueue beatQueue;
        std::atomic<float> bpm { NAN };

        // audio-thread-only playhead state
        std::uint64_t lastBlockStart = 0;
        bool lastBlockWritten = false;
        double prevPpq = -1.0e300;
        std::int64_t lastBeatNumber = INT64_MIN;

        // worker-private
        std::array<float, kDrainChunk> scratchL {}, scratchR {}, loudL {}, loudR {};
        std::array<BeatEvent, kBeatLogSize> beatLog {};
        std::uint64_t beatsSeen = 0;
        bool newFrames = false;
        std::atomic<std::uint64_t> sequence { 0 };
    };
} // namespace prism
