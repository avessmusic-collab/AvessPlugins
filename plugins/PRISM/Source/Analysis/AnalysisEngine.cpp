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
            s.stereo.prepare(config.sampleRate);
            s.loudness.prepare(config.sampleRate, registry[i].numChannels);
            s.truePeak.prepare(registry[i].numChannels);
            s.wave.prepare(config.sampleRate, registry[i].numChannels);
        }
        spectrum.prepare(config.sampleRate);
    }

    SpectrumEngine::Params AnalysisEngine::readSpectrumParams() const noexcept
    {
        SpectrumEngine::Params p;
        p.mode = readAnalysisMode();
        if (controls.fftSizeIndex != nullptr)
            p.sizeIndex = std::clamp((int) std::lround(controls.fftSizeIndex->load(std::memory_order_relaxed)), 0, SpectrumEngine::kNumSizes - 1);
        if (controls.windowIndex != nullptr)
            p.windowIndex = std::clamp((int) std::lround(controls.windowIndex->load(std::memory_order_relaxed)), 0, SpectrumEngine::kNumWindows - 1);
        return p;
    }

    void AnalysisEngine::attachControls(const ExternalControls& c) noexcept
    {
        controls = c;
        seenResetAll        = c.resetAllEpoch != nullptr ? c.resetAllEpoch->load(std::memory_order_relaxed) : 0;
        seenResetIntegrated = c.resetIntegratedEpoch != nullptr ? c.resetIntegratedEpoch->load(std::memory_order_relaxed) : 0;
    }

    int AnalysisEngine::scopeLengthFor(double fs) noexcept
    {
        return std::min(kScopeSamples, (int) std::ceil(0.020 * fs) + 800 + 256);
    }

    int AnalysisEngine::gonioStrideFor(double fs) noexcept
    {
        return std::max(1, (int) std::lround(3.0 * fs / 48000.0));
    }

    int AnalysisEngine::readAnalysisMode() const noexcept
    {
        if (controls.analysisMode == nullptr)
            return 2;   // default: combined
        return std::clamp((int) std::lround(controls.analysisMode->load(std::memory_order_relaxed)), 0, 2);
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
        // Beat / bar events are derived from the HOST's authoritative fields (not from "integer ppq" or "ppq % 4"):
        //   downbeat      = PositionInfo::ppqPositionOfLastBarStart
        //   beat spacing  = 4 / timeSignature.denominator quarter notes   (6/8 -> 8th-note beats, 3/4 -> quarter beats)
        //   bar length    = numerator * 4 / denominator quarter notes
        // Each event's sample offset inside the block comes from the ppq delta and the tempo (sub-block accurate, +-1 frame).
        // Negative ppq (count-in / pre-roll) emits nothing; a backwards jump (loop) forgets what was emitted (clean re-anchor).
        if (playHead == nullptr)
        {
            transportState.store(-1, std::memory_order_relaxed);
            bpm.store(NAN, std::memory_order_relaxed);
            sigNum.store(0, std::memory_order_relaxed);
            sigDen.store(0, std::memory_order_relaxed);
            lastEmittedPpq = -1.0e300;
            return;
        }

        const auto pos = playHead->getPosition();
        if (! pos.hasValue())
        {
            transportState.store(-1, std::memory_order_relaxed);
            bpm.store(NAN, std::memory_order_relaxed);
            sigNum.store(0, std::memory_order_relaxed);
            sigDen.store(0, std::memory_order_relaxed);
            lastEmittedPpq = -1.0e300;
            return;
        }

        transportState.store(pos->getIsPlaying() ? 1 : 0, std::memory_order_relaxed);   // host transport: lets the page freeze the strip while paused
        const auto tempo = pos->getBpm();
        bpm.store(tempo.hasValue() ? (float) *tempo : NAN, std::memory_order_relaxed);

        int num = 4, den = 4;
        bool hasTs = false;
        if (const auto ts = pos->getTimeSignature())
            if (ts->numerator > 0 && ts->denominator > 0)
            {
                num = ts->numerator;
                den = ts->denominator;
                hasTs = true;
            }
        sigNum.store(hasTs ? num : 0, std::memory_order_relaxed);
        sigDen.store(hasTs ? den : 0, std::memory_order_relaxed);

        const auto ppqOpt = pos->getPpqPosition();
        if (! (pos->getIsPlaying() && tempo.hasValue() && ppqOpt.hasValue() && *tempo > 0.0
               && config.sampleRate > 0.0 && lastBlockWritten && n > 0))
        {
            lastEmittedPpq = -1.0e300;
            prevPpq = -1.0e300;
            return;
        }

        const double p0 = *ppqOpt;
        if (p0 < prevPpq - 1.0e-6)          // transport jumped backwards (loop / locate): forget which beats we emitted
            lastEmittedPpq = -1.0e300;
        prevPpq = p0;

        const double ppqPerSample = *tempo / 60.0 / config.sampleRate;
        const double p1 = p0 + (double) n * ppqPerSample;
        const double halfSample = 0.5 * ppqPerSample;
        const double unit = 4.0 / (double) den;               // quarter notes per beat
        const double barQuarters = (double) num * unit;

        double b0 = 0.0;                                       // a bar start (quarter notes)
        if (const auto lb = pos->getPpqPositionOfLastBarStart())
            b0 = *lb;
        b0 += std::floor((p0 - b0) / barQuarters + 1.0e-12) * barQuarters;   // normalise: b0 <= p0 < b0 + bar (stale / future anchors, long blocks)

        std::int64_t k = (std::int64_t) std::ceil((p0 - halfSample - b0) / unit - 1.0e-9);
        for (int guard = 0; guard < 64; ++guard, ++k)
        {
            const double bp = b0 + (double) k * unit;
            if (bp >= p1 - halfSample)
                break;
            if (bp < -halfSample)                              // count-in / pre-roll: no events before ppq 0
                continue;
            if (bp <= lastEmittedPpq + 0.5 * unit)             // already emitted by the previous block
                continue;

            const auto off = (std::int64_t) std::llround((bp - p0) / ppqPerSample);
            const auto clamped = (std::uint64_t) std::clamp<std::int64_t>(off, 0, n - 1);
            const std::int64_t kk = ((k % num) + num) % num;
            beatQueue.push({ lastBlockStart + clamped, kk == 0 });
            lastEmittedPpq = bp;
        }
    }

    //==========================================================================
    // WORKER / TESTS
    void AnalysisEngine::pump()
    {
        float* dst[2] = { scratchL.data(), scratchR.data() };

        // reset_all (message thread -> epoch counter): clear the peak maxima. Other resets arrive in later phases.
        if (controls.resetAllEpoch != nullptr)
        {
            const auto e = controls.resetAllEpoch->load(std::memory_order_relaxed);
            if (e != seenResetAll)
            {
                seenResetAll = e;
                for (int k = 0; k < registry.getActiveCount(); ++k)
                {
                    auto& st = sources[(size_t) registry.getActiveIndex(k)];
                    st.samplePeak.exchange(0.0f, std::memory_order_relaxed);
                    st.samplePeakMaxLin = 0.0f;
                    st.loudness.resetIntegrated();   // reset_all also clears I + LRA
                    st.truePeak.reset();             // ... and both peak maxima (SP above, TP here); FIR history too,
                                                     //     so pre-reset samples cannot ring into the new measurement
                }
            }
        }
        if (controls.resetIntegratedEpoch != nullptr)
        {
            const auto e = controls.resetIntegratedEpoch->load(std::memory_order_relaxed);
            if (e != seenResetIntegrated)
            {
                seenResetIntegrated = e;
                for (int k = 0; k < registry.getActiveCount(); ++k)
                    sources[(size_t) registry.getActiveIndex(k)].loudness.resetIntegrated();   // I + LRA only
            }
        }

        const bool editorWork = spectrumEnabled.load(std::memory_order_relaxed);

        // Beat / bar events first: an event is pushed AFTER its block was written to the ring, so every event
        // popped here refers to frames the extractor has (almost always) not processed yet -> pending flag.
        // Events for already-finished columns patch the log (see WaveformExtractor::markBeat).
        {
            BeatEvent be;
            while (beatQueue.pop(be))
            {
                beatLog[(size_t) (beatsSeen % kBeatLogSize)] = be;
                ++beatsSeen;
                if (be.isBar && (std::int64_t) be.frameIndex > lastBarFrame)
                    lastBarFrame = (std::int64_t) be.frameIndex;   // monotonic: a loop jump never moves the anchor backwards in stream time
                if (editorWork)
                    for (int k = 0; k < registry.getActiveCount(); ++k)
                        sources[(size_t) registry.getActiveIndex(k)].wave.markBeat(be.frameIndex, be.isBar);
            }
        }

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
                const std::uint64_t chunkStart = st.history.framesSeen();
                st.history.push(src, got);
                newFrames = true;

                // Loudness (always on, editor open or not) on the sanitised frames. A ring overrun is a
                // stream discontinuity at ring position lastDropPos: filter state + in-flight block are
                // discarded exactly there (see LoudnessMeter::notifyGap).
                {
                    const std::uint64_t ov = st.ring.getOverrunCount();
                    if (ov != st.lastOverrunLoud)
                    {
                        st.lastOverrunLoud = ov;
                        st.pendingGap = true;
                        st.pendingGapPos = st.ring.getLastDropPos();
                    }
                    st.history.copyAbsolute(0, chunkStart, got, loudL.data());
                    st.history.copyAbsolute(1, chunkStart, got, loudR.data());
                    int split = got;
                    bool doGap = false;
                    if (st.pendingGap && st.pendingGapPos < chunkStart + (std::uint64_t) got)
                    {
                        split = st.pendingGapPos > chunkStart ? (int) (st.pendingGapPos - chunkStart) : 0;
                        doGap = true;
                        st.pendingGap = false;
                    }
                    // Waveform columns are editor-active work. After an inactive period the column stream is
                    // re-synchronised to the absolute frame index (the partial column is not logged).
                    if (editorWork && st.waveNeedsSkip)
                    {
                        st.wave.skipTo(chunkStart);
                        st.waveNeedsSkip = false;
                    }
                    if (! editorWork)
                        st.waveNeedsSkip = true;

                    if (split > 0)
                    {
                        st.loudness.process(loudL.data(), loudR.data(), split);
                        st.truePeak.process(loudL.data(), loudR.data(), split);
                        if (editorWork) st.wave.process(loudL.data(), loudR.data(), split);
                    }
                    if (doGap)
                    {
                        st.loudness.notifyGap();
                        st.truePeak.notifyGap();
                        if (editorWork) st.wave.notifyGap();
                        if (split < got)
                        {
                            st.loudness.process(loudL.data() + split, loudR.data() + split, got - split);
                            st.truePeak.process(loudL.data() + split, loudR.data() + split, got - split);
                            if (editorWork) st.wave.process(loudL.data() + split, loudR.data() + split, got - split);
                        }
                    }
                }

                // Worker-side metering on the SANITISED frames just pushed (two contiguous segments per channel).
                const bool stereoSrc = registry[registry.getActiveIndex(k)].numChannels == 2;
                const auto segL = st.history.latest(0, got);
                const auto segR = st.history.latest(1, got);
                const float* ls[2] = { segL.a, segL.b };
                const float* rs[2] = { segR.a, segR.b };
                const int ns[2] = { segL.na, segL.nb };
                for (int part = 0; part < 2; ++part)
                {
                    if (ns[part] <= 0)
                        continue;
                    if (stereoSrc)
                        st.stereo.process(ls[part], rs[part], ns[part]);
                    for (int i = 0; i < ns[part]; ++i)
                    {
                        const float a = std::max(std::abs(ls[part][i]), stereoSrc ? std::abs(rs[part][i]) : 0.0f);
                        st.framesSinceLoud = a > kSignalThresholdLin ? 0 : st.framesSinceLoud + 1;
                    }
                }
            }

            const std::uint64_t ov = st.ring.getOverrunCount();
            if (ov != st.lastOverrunSeen)
            {
                st.gapEvents += ov - st.lastOverrunSeen;   // discontinuity: later phases reset filter state here
                st.lastOverrunSeen = ov;
            }

            st.samplePeakMaxLin = std::max(st.samplePeakMaxLin, st.samplePeak.exchange(0.0f, std::memory_order_relaxed));

            // Editor-active work: FFT hops (keyed to absolute frame index; see SpectrumEngine).
            if (editorWork)
                spectrum.advance(registry.getActiveIndex(k), st.history, readSpectrumParams());
        }
    }

    std::shared_ptr<const AnalysisSnapshot> AnalysisEngine::buildSnapshot()
    {
        auto snap = std::make_shared<AnalysisSnapshot>();
        snap->sequence   = sequence.fetch_add(1, std::memory_order_relaxed) + 1;
        snap->sampleRate = config.sampleRate;
        snap->bpm        = bpm.load(std::memory_order_relaxed);
        snap->barAnchorStreamFrame = lastBarFrame;
        snap->transportPlaying = transportState.load(std::memory_order_relaxed);
        snap->timeSigNumerator   = sigNum.load(std::memory_order_relaxed);
        snap->timeSigDenominator = sigDen.load(std::memory_order_relaxed);

        const double fs = config.sampleRate;
        const int mode = readAnalysisMode();
        const std::uint64_t holdFrames = (std::uint64_t) (kSignalHoldSeconds * fs);
        const int scopeLen = scopeLengthFor(fs);
        const int stride = gonioStrideFor(fs);
        constexpr float kInvSqrt2 = 0.70710678118654752f;

        // Consumer-acknowledged waveform delivery. The editor tells us (via the processor) which snapshot
        // sequence it last fetched; the map written at publish time translates that into the highest column it
        // has seen. Every snapshot then carries all columns newer than that point (newest 16 at most), so a
        // skipped or unfetched snapshot never leaves a gap and a fetched one never repeats a column.
        if (controls.lastFetchedSequence != nullptr)
        {
            const std::uint64_t f = controls.lastFetchedSequence->load(std::memory_order_relaxed);
            if (f != 0)
            {
                const auto& e = ackMap[(size_t) (f % kAckMapSize)];
                if (e.sequence == f)
                    for (int k = 0; k < registry.getActiveCount(); ++k)
                    {
                        const int idx = registry.getActiveIndex(k);
                        if (idx < kSourceCount)
                            sources[(size_t) idx].ackedCol = std::max(sources[(size_t) idx].ackedCol, e.highest[(size_t) idx]);
                    }
            }
        }
        else   // no feedback channel: assume the previous snapshot was delivered (latest-only behaviour)
        {
            for (int k = 0; k < registry.getActiveCount(); ++k)
            {
                const int idx = registry.getActiveIndex(k);
                if (idx < kSourceCount)
                    sources[(size_t) idx].ackedCol = std::max(sources[(size_t) idx].ackedCol, sources[(size_t) idx].lastHighest);
            }
        }
        AckEntry ackNow;
        ackNow.sequence = snap->sequence;
        for (auto& h : ackNow.highest) h = -1;

        for (int k = 0; k < registry.getActiveCount(); ++k)
        {
            const int idx = registry.getActiveIndex(k);
            if (idx >= kSourceCount)
                continue;

            const auto& info = registry[idx];
            auto& st = sources[(size_t) idx];
            auto& out = snap->sources[(size_t) idx];

            out.samplePeakMaxDb = st.samplePeakMaxLin > 0.0f ? (float) (20.0 * std::log10((double) st.samplePeakMaxLin))
                                                             : -INFINITY;
            // True peak: streaming 4x FIR maximum, folded with the lossless sample peak so that the
            // DSP invariant TP >= SP always holds (the interpolator's phase 0 is not an exact identity).
            const float tpLin = std::max(st.truePeak.maxLinear(), st.samplePeakMaxLin);
            out.truePeakMaxDb = tpLin > 0.0f ? (float) (20.0 * std::log10((double) tpLin)) : -INFINITY;

            snap->signalPresent = snap->signalPresent || st.framesSinceLoud < holdFrames;

            spectrum.publish(idx, out);

            {   // waveform columns: [bass, mids, highs, flag], oldest first
                std::array<WaveformExtractor::Column, kMaxWaveformColumns> cols {};
                const int n = st.wave.collectNewer(st.ackedCol, kMaxWaveformColumns, cols.data());
                if (n > 0)
                {
                    // Columns older than the ones we can still send are permanently lost: count each once.
                    const auto first = (std::int64_t) cols[0].index;
                    st.waveDropped += (std::uint64_t) st.wave.countBetween(std::max(st.ackedCol, st.droppedCountedUpTo), first);
                    st.droppedCountedUpTo = std::max(st.droppedCountedUpTo, first - 1);
                }
                out.waveformColumnCount = n;
                out.waveformFirstColumn = n > 0 ? (std::int64_t) cols[0].index : -1;
                {   // in-progress column (published immediately; completes later into waveformColumns with the same index)
                    float live3[3] = { 0.0f, 0.0f, 0.0f };
                    out.liveColumnIndex = st.wave.liveColumn(live3);
                    out.liveColumn = { live3[0], live3[1], live3[2] };
                }
                for (int i = 0; i < n; ++i)
                    out.waveformColumns[(size_t) i] = { cols[(size_t) i].bass, cols[(size_t) i].mid, cols[(size_t) i].high, cols[(size_t) i].flag, cols[(size_t) i].onsetFrac };
                const std::int64_t highest = n > 0 ? (std::int64_t) cols[(size_t) (n - 1)].index : st.ackedCol;
                st.lastHighest = highest;
                ackNow.highest[(size_t) idx] = highest;
            }

            if (idx == 0)   // global loudness fields = source 0 (main input); NaN until available, never 0
            {
                snap->momentaryLufs  = st.loudness.momentaryLufs();
                snap->shortTermLufs  = st.loudness.shortTermLufs();
                snap->integratedLufs = st.loudness.integratedLufs();
                snap->loudnessRange  = st.loudness.loudnessRange();
            }

            out.hasStereo = info.numChannels == 2;
            if (out.hasStereo)
            {
                const auto m = st.stereo.compute();
                out.correlation = m.correlation;
                out.widthPercent = m.widthPercent;
                out.balanceDb = m.balanceDb;
            }

            // Scope: most recent `scopeCount` contiguous frames, oldest first. Only real frames
            // are published (count = min(window, framesSeen)); nothing is zero-padded.
            const auto L = st.history.latest(0, scopeLen);
            const auto R = st.history.latest(1, scopeLen);
            out.scopeCount = L.total();
            int w = 0;
            for (int part = 0; part < 2; ++part)
            {
                const float* l = part == 0 ? L.a : L.b;
                const float* r = part == 0 ? R.a : R.b;
                const int n = part == 0 ? L.na : L.nb;
                for (int i = 0; i < n; ++i, ++w)
                {
                    float v = l[i];
                    if (info.numChannels == 2)
                        v = mode == 0 ? l[i] : (mode == 1 ? (l[i] + r[i]) * kInvSqrt2 : 0.5f * (l[i] + r[i]));
                    out.scope[(size_t) w] = v;
                }
            }

            // Gonio: 900 raw L/R pairs, every `stride`-th frame, ending at the newest frame.
            const std::uint64_t seen = st.history.framesSeen();
            const int avail = (int) std::min<std::uint64_t>(seen, StreamHistory::kCapacity);
            const int pairs = avail > 0 ? std::min(kGonioPairs, (avail - 1) / stride + 1) : 0;
            out.gonioPairCount = pairs;
            if (pairs > 0)
            {
                const int span = (pairs - 1) * stride + 1;       // frames covered, ending at the newest
                const auto gl = st.history.latest(0, span);
                const auto gr = st.history.latest(1, span);
                for (int p = 0; p < pairs; ++p)
                {
                    const int off = p * stride;                  // offset from the oldest covered frame
                    const bool inA = off < gl.na;
                    out.gonio[(size_t) (2 * p)]     = inA ? gl.a[off] : gl.b[off - gl.na];
                    out.gonio[(size_t) (2 * p + 1)] = inA ? gr.a[off] : gr.b[off - gr.na];
                }
            }
        }
        ackMap[(size_t) (snap->sequence % kAckMapSize)] = ackNow;
        return snap;
    }

    bool AnalysisEngine::takeNewFramesFlag() noexcept
    {
        const bool f = newFrames;
        newFrames = false;
        return f;
    }
} // namespace prism
