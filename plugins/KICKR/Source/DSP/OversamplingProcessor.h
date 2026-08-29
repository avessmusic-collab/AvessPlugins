#pragma once

#include <array>
#include <atomic>
#include <memory>
#include <juce_dsp/juce_dsp.h>

namespace kickr
{
    /**
        Oversampling region substrate (AD-2 / AD-10).

        Owns one pre-built juce::dsp::Oversampling<float> instance per factor
        (1x / 2x / 4x / 8x = log2 order 0..3), `filterHalfBandPolyphaseIIR`,
        `useIntegerLatency = true`, `numChannels = 2`. The OS region is meant to wrap
        the ENTIRE voice + master chain through the tanh safety limiter — every
        nonlinearity oversampled by construction.

        PHASE 2.10: real factor switching is live. `setFactorChoice(choiceIndex)` is called
        from the audio thread every block and only stores `pendingOrder` (atomic) — no swap.
        `KickEngine` checks `factorChangePending()` at block start; while a ~64-sample
        fade-to-silence covers the seam it calls `applyPendingFactor()` (adopts the pending
        order, `reset()`s the newly-active OS object — both are already `initProcessing`-ed,
        no allocation) and recomputes every in-region coefficient for the new `fsOversampled`.
        The message thread reads `pendingLatencySamples()` from an APVTS listener and calls
        `AudioProcessor::setLatencySamples` (never mid-block). All 4 per-factor latencies are
        cached in `prepare()` and readable lock-free from either thread.

        Usage per (sub-)block:
            auto up = processSamplesUp (zeroedBaseBlock);   // work block @ fsOversampled
            // ... render whole voice + master chain into `up` ...
            processSamplesDown (baseBlock);                  // band-limit + decimate
    */
    class OversamplingProcessor
    {
    public:
        static constexpr int kNumFactors  = 4;   // 1x, 2x, 4x, 8x  (log2 order 0..3)
        static constexpr int kNumChannels = 2;

        OversamplingProcessor() = default;

        /** log2 oversampling order for choice index 0..3. */
        static constexpr int orderForChoice (int choiceIndex) noexcept
        {
            return juce::jlimit (0, kNumFactors - 1, choiceIndex);
        }

        void prepare (double baseSampleRate, int maximumBlockSize);
        void reset();

        /** Audio thread, every block: stash the requested order. No swap happens here. */
        void setFactorChoice (int choiceIndex) noexcept;

        /** Audio thread: true when the pending order differs from the active one. */
        bool factorChangePending() const noexcept { return pendingOrder.load() != activeOrder; }

        /** Audio thread, at a safe point (block start, under a fade): adopt the pending
            order and reset the newly-active OS object. Allocation-free. */
        void applyPendingFactor() noexcept;

        /** round(getLatencyInSamples()) for `order` (0..3), cached in prepare(). */
        int latencyForOrder (int order) const noexcept;

        /** Lock-free from either thread: the latency the pending order would report. */
        int pendingLatencySamples() const noexcept { return latencyForOrder (pendingOrder.load()); }

        /** Up-sample a base-rate block into the internal work buffer (@ fsOversampled). */
        juce::dsp::AudioBlock<float> processSamplesUp (const juce::dsp::AudioBlock<const float>& input) noexcept;

        /** Band-limit + decimate the internal work buffer back into `output` (base rate). */
        void processSamplesDown (juce::dsp::AudioBlock<float>& output) noexcept;

        double getBaseSampleRate()   const noexcept { return sampleRate; }
        double getOversampledRate()  const noexcept { return sampleRate * static_cast<double> (1 << activeOrder); }
        int    getOversamplingFactor() const noexcept { return 1 << activeOrder; }

        /** round(activeOs->getLatencyInSamples()); 0 at 1x. */
        int getLatencySamples() const noexcept;

    private:
        juce::dsp::Oversampling<float>* active() const noexcept
        {
            return oversamplers[static_cast<size_t> (activeOrder)].get();
        }

        double sampleRate   { 44100.0 };
        int    maxBlockSize { 512 };
        int    activeOrder  { 0 };   // audio-thread only

        std::atomic<int>             pendingOrder { 0 };   // written by setFactorChoice
        std::array<int, kNumFactors> latencySamples { {} }; // filled in prepare(), const after

        std::array<std::unique_ptr<juce::dsp::Oversampling<float>>, kNumFactors> oversamplers;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (OversamplingProcessor)
    };
}
