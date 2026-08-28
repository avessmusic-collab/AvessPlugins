#pragma once

#include <array>
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

        PHASE 2.1: pinned to factor 1x (order 0) — a real zero-latency identity
        juce::dsp::Oversampling object (JUCE adds a dummy stage). `fsOversampled == fs`
        so the rest of the DSP is trivially verifiable. The 2x/4x/8x instances are
        pre-built here but unused; the atomic-swap / real-factor switching + coefficient
        refresh is Phase 2.10 (see `setFactorChoice` hook).

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

        /** PHASE 2.10 hook — currently a no-op: the factor is pinned to 1x. */
        void setFactorChoice (int choiceIndex) noexcept;

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
        int    activeOrder  { 0 };   // PHASE 2.1: pinned to 0 (1x). Phase 2.10 unpins.

        std::array<std::unique_ptr<juce::dsp::Oversampling<float>>, kNumFactors> oversamplers;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (OversamplingProcessor)
    };
}
