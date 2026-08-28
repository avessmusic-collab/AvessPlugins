#pragma once

#include <array>
#include <memory>
#include <juce_dsp/juce_dsp.h>

namespace kickr
{
    /**
        Stage 1 scaffold. Real construction + processing land in Stage 2
        (plan.md Phase 2.1 builds the region shell pinned to 1x; Phase 2.10 enables
        real factors + glitch-free switching).

        AD-2 / AD-10: one pre-built juce::dsp::Oversampling<float> instance per factor
        (1x / 2x / 4x / 8x). The OS region wraps the ENTIRE voice + master chain through
        the tanh safety limiter — every nonlinearity is oversampled by construction.
        `processSamplesUp` is fed a zero block (instrument has no audio input);
        `processSamplesDown` runs after the limiter; only the DC blocker + analyzer taps
        are base-rate.

        Switching (message thread only — runtime rebuild is NOT audio-thread safe):
        atomic pointer swap + ~64-sample output fade + audio-thread recompute of all
        in-region coefficients for the new fsOversampled; setLatencySamples from the
        message-thread listener / prepareToPlay only.
    */
    class OversamplingProcessor
    {
    public:
        static constexpr int   kNumFactors  = 4;   // 1x, 2x, 4x, 8x
        static constexpr int   kNumChannels = 2;

        /** log2 oversampling order for choice index 0..3. */
        static constexpr int orderForChoice (int choiceIndex) noexcept
        {
            return juce::jlimit (0, 3, choiceIndex);
        }

        void prepare (double baseSampleRate, int maximumBlockSize)
        {
            sampleRate   = baseSampleRate;
            maxBlockSize = maximumBlockSize;

            // Stage 2 (Phase 2.1): construct the 4 instances here, e.g.
            //   for (int i = 0; i < kNumFactors; ++i)
            //       oversamplers[(size_t) i] = std::make_unique<juce::dsp::Oversampling<float>>(
            //           kNumChannels, i,
            //           juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR,
            //           /*isMaxQuality*/ true, /*useIntegerLatency*/ true);
            //   each then oversamplers[i]->initProcessing ((size_t) maximumBlockSize);
        }

        void reset() {}

        double getOversampledRate (int choiceIndex) const noexcept
        {
            return sampleRate * static_cast<double> (1 << orderForChoice (choiceIndex));
        }

        int getLatencySamples() const noexcept { return 0; } // Stage 2: round(active->getLatencyInSamples())

    private:
        double sampleRate   { 44100.0 };
        int    maxBlockSize { 512 };

        std::array<std::unique_ptr<juce::dsp::Oversampling<float>>, kNumFactors> oversamplers;
    };
}
