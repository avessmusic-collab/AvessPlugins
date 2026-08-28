#include "DSP/OversamplingProcessor.h"

#include <cmath>

namespace kickr
{
    void OversamplingProcessor::prepare (double baseSampleRate, int maximumBlockSize)
    {
        sampleRate   = baseSampleRate;
        maxBlockSize = juce::jmax (1, maximumBlockSize);
        activeOrder  = 0;   // PHASE 2.1: pinned to 1x. Phase 2.10 reads `oversampling`.

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
    }

    void OversamplingProcessor::reset()
    {
        for (auto& os : oversamplers)
            if (os != nullptr)
                os->reset();
    }

    void OversamplingProcessor::setFactorChoice (int choiceIndex) noexcept
    {
        // PHASE 2.10: atomic pointer swap + ~64-sample fade + audio-thread recompute of
        // every in-region coefficient for the new fsOversampled, then setLatencySamples
        // on the message thread. PHASE 2.1: the factor is pinned to 1x.
        juce::ignoreUnused (choiceIndex);
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
