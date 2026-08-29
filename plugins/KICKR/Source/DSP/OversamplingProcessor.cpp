#include "DSP/OversamplingProcessor.h"

#include <cmath>

namespace kickr
{
    void OversamplingProcessor::prepare (double baseSampleRate, int maximumBlockSize)
    {
        sampleRate   = baseSampleRate;
        maxBlockSize = juce::jmax (1, maximumBlockSize);
        activeOrder  = 0;
        pendingOrder.store (0);   // KickEngine::prepare drives the initial factor immediately after

        // Factor 1x — real zero-latency identity object (JUCE inserts a dummy stage).
        oversamplers[0] = std::make_unique<juce::dsp::Oversampling<float>> (
            static_cast<size_t> (kNumChannels));

        // Factors 2x / 4x / 8x — pre-built now, activated in Phase 2.10.
        for (int order = 1; order < kNumFactors; ++order)
            oversamplers[static_cast<size_t> (order)] = std::make_unique<juce::dsp::Oversampling<float>> (
                static_cast<size_t> (kNumChannels),
                static_cast<size_t> (order),
                juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR,
                true,    // isMaxQuality
                true);   // useIntegerLatency

        for (auto& os : oversamplers)
        {
            os->initProcessing (static_cast<size_t> (maxBlockSize));
            os->reset();
        }

        // Cache all 4 integer latencies (order-0 identity object -> 0).
        for (int order = 0; order < kNumFactors; ++order)
            latencySamples[static_cast<size_t> (order)] = static_cast<int> (std::lround (
                oversamplers[static_cast<size_t> (order)]->getLatencyInSamples()));
    }

    void OversamplingProcessor::reset()
    {
        for (auto& os : oversamplers)
            if (os != nullptr)
                os->reset();
    }

    void OversamplingProcessor::setFactorChoice (int choiceIndex) noexcept
    {
        // Audio thread, every block — only stash the request. KickEngine performs the
        // swap (applyPendingFactor) at a safe point while a ~64-sample fade covers the seam.
        pendingOrder.store (orderForChoice (choiceIndex));
    }

    void OversamplingProcessor::applyPendingFactor() noexcept
    {
        activeOrder = pendingOrder.load();
        if (auto* os = active())
            os->reset();   // already initProcessing-ed in prepare() — no allocation
    }

    int OversamplingProcessor::latencyForOrder (int order) const noexcept
    {
        return latencySamples[static_cast<size_t> (juce::jlimit (0, kNumFactors - 1, order))];
    }

    juce::dsp::AudioBlock<float>
    OversamplingProcessor::processSamplesUp (const juce::dsp::AudioBlock<const float>& input) noexcept
    {
        if (auto* os = active())
            return os->processSamplesUp (input);

        return {};
    }

    void OversamplingProcessor::processSamplesDown (juce::dsp::AudioBlock<float>& output) noexcept
    {
        if (auto* os = active())
            os->processSamplesDown (output);
    }

    int OversamplingProcessor::getLatencySamples() const noexcept
    {
        if (auto* os = active())
            return static_cast<int> (std::lround (os->getLatencyInSamples()));

        return 0;
    }
}
