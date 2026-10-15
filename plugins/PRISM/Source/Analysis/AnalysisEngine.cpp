#include "AnalysisEngine.h"

#include <algorithm>
#include <bit>
#include <cmath>

namespace prism
{
    AnalysisEngine::AnalysisEngine(const EngineConfig& cfg, int ringLog2Capacity)
        : config(cfg)
    {
        registry.build(config);
        for (int n = 0; n < registry.getActiveCount(); ++n)
        {
            const int i = registry.getActiveIndex(n);
            auto& s = sources[(size_t) i];
            s.ring.prepare(registry[i].numChannels, ringLog2Capacity);
            s.history.prepare(registry[i].numChannels);
        }
    }

    //==========================================================================
    // AUDIO THREAD
    void AnalysisEngine::captureBlock(const float* const* chans, int numBufChannels, int n) noexcept
    {
        lastBlockWritten = false;
        if (n <= 0 || chans == nullptr)
            return;

        for (int k = 0; k < registry.getActiveCount(); ++k)
        {
            const int idx = registry.getActiveIndex(k);
            const SourceInfo& info = registry[idx];
            if (info.channelOffset + info.numChannels > numBufChannels)
                continue;   // buffer smaller than the negotiated layout: skip rather than read out of bounds

            auto& st = sources[(size_t) idx];
            const float* const l = chans[info.channelOffset];
            const float* const ptrs[2] = { l, info.numChannels > 1 ? chans[info.channelOffset + 1] : l };

            // One pass: lossless peak + non-finite census (bit test, immune to fast-math).
            float peak = 0.0f;
            std::uint32_t bad = 0;
            for (int c = 0; c < info.numChannels; ++c)
            {
                const float* x = ptrs[c];
                for (int i = 0; i < n; ++i)
                {
                    const std::uint32_t bits = std::bit_cast<std::uint32_t>(x[i]);
                    const bool finite = (bits & 0x7f800000u) != 0x7f800000u;
                    bad += finite ? 0u : 1u;
                    const float a = std::bit_cast<float>(bits & 0x7fffffffu);
                    peak = (finite && a > peak) ? a : peak;
                }
            }
            if (bad != 0)
                st.nonFiniteCount.fetch_add(bad, std::memory_order_relaxed);

            float cur = st.samplePeak.load(std::memory_order_relaxed);
            while (peak > cur && ! st.samplePeak.compare_exchange_weak(cur, peak, std::memory_order_relaxed))
            {
            }

            const std::uint64_t start = st.ring.producerPosition();
            const bool ok = st.ring.write(ptrs, n);
            if (idx == 0)
            {
                lastBlockStart = start;
                lastBlockWritten = ok;
            }
        }
    }

    void AnalysisEngine::capturePlayhead(juce::AudioPlayHead* playHead, int n) noexcept
    {
        if (playHead == nullptr)
        {
            bpm.store(NAN, std::memory_order_relaxed);
            lastBeatNumber = INT64_MIN;
            return;
        }

        const auto pos = playHead->getPosition();
        if (! pos.hasValue())
        {
            bpm.store(NAN, std::memory_order_relaxed);
            lastBeatNumber = INT64_MIN;
            return;
        }

        const auto tempo = pos->getBpm();
        bpm.store(tempo.hasValue() ? (float) *tempo : NAN, std::memory_order_relaxed);

        const auto ppqOpt = pos->getPpqPosition();
        if (! (pos->getIsPlaying() && tempo.hasValue() && ppqOpt.hasValue() && *tempo > 0.0
               && config.sampleRate > 0.0 && lastBlockWritten && n > 0))
        {
            lastBeatNumber = INT64_MIN;
            prevPpq = -1.0e300;
            return;
        }

        const double p0 = *ppqOpt;
        if (p0 < prevPpq - 1.0e-6)          // transport jumped backwards: forget which beats we emitted
            lastBeatNumber = INT64_MIN;
        const double ppqPerSample = *tempo / 60.0 / config.sampleRate;
        const double p1 = p0 + (double) n * ppqPerSample;
        prevPpq = p0;

        double barQuarters = 0.0;
        if (const auto ts = pos->getTimeSignature())
            if (ts->denominator > 0)
                barQuarters = (double) ts->numerator * 4.0 / (double) ts->denominator;

        const double halfSample = 0.5 * ppqPerSample;
        std::int64_t beat = (std::int64_t) std::ceil(p0 - halfSample);
        if (lastBeatNumber != INT64_MIN && beat <= lastBeatNumber)
            beat = lastBeatNumber + 1;

        for (int guard = 0; guard < 32 && (double) beat < p1 - halfSample; ++guard, ++beat)
        {
            const auto off = (std::int64_t) std::llround(((double) beat - p0) / ppqPerSample);
            const auto clamped = (std::uint64_t) std::clamp<std::int64_t>(off, 0, n - 1);

            bool isBar = false;
            if (barQuarters > 0.0)
            {
                const double q = (double) beat / barQuarters;
                isBar = std::abs(q - std::round(q)) < 1.0e-6;
            }
            beatQueue.push({ lastBlockStart + clamped, isBar });
            lastBeatNumber = beat;
        }
    }

    //==========================================================================
    // WORKER / TESTS
    void AnalysisEngine::pump()
    {
        float* dst[2] = { scratchL.data(), scratchR.data() };

        for (int k = 0; k < registry.getActiveCount(); ++k)
        {
            auto& st = sources[(size_t) registry.getActiveIndex(k)];

            // Bounded: at most one ring's worth of chunks per pump (a faster producer waits for the next cycle).
            const int maxIterations = 2 + (int) (st.ring.getCapacity() / (std::uint64_t) kDrainChunk);
            for (int it = 0; it < maxIterations; ++it)
            {
                const int got = st.ring.read(dst, kDrainChunk);
                if (got <= 0)
                    break;
                const float* const src[2] = { dst[0], dst[1] };
                st.history.push(src, got);
                newFrames = true;
            }

            const std::uint64_t ov = st.ring.getOverrunCount();
            if (ov != st.lastOverrunSeen)
            {
                st.gapEvents += ov - st.lastOverrunSeen;   // discontinuity: later phases reset filter state here
                st.lastOverrunSeen = ov;
            }

            st.samplePeakMaxLin = std::max(st.samplePeakMaxLin, st.samplePeak.exchange(0.0f, std::memory_order_relaxed));
        }

        BeatEvent e;
        while (beatQueue.pop(e))
        {
            beatLog[(size_t) (beatsSeen % kBeatLogSize)] = e;
            ++beatsSeen;
        }
    }

    std::shared_ptr<const AnalysisSnapshot> AnalysisEngine::buildSnapshot()
    {
        auto snap = std::make_shared<AnalysisSnapshot>();
        snap->sequence   = sequence.fetch_add(1, std::memory_order_relaxed) + 1;
        snap->sampleRate = config.sampleRate;
        snap->bpm        = bpm.load(std::memory_order_relaxed);
        // Phase 2.1: nothing is measured yet; all other fields keep their "unavailable" defaults.
        return snap;
    }

    bool AnalysisEngine::takeNewFramesFlag() noexcept
    {
        const bool f = newFrames;
        newFrames = false;
        return f;
    }
} // namespace prism
