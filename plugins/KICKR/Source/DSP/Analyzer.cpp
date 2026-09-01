#include "DSP/Analyzer.h"

#include <algorithm>
#include <cmath>

namespace kickr
{
    void Analyzer::prepare (double sampleRate)
    {
        currentSampleRate = juce::jmax (1.0, sampleRate);

        // Built ONCE here (message thread) — never per frame, never on the audio thread.
        fft    = std::make_unique<juce::dsp::FFT> (kFftOrder);
        window = std::make_unique<juce::dsp::WindowingFunction<float>> (
                     static_cast<size_t> (kFftSize),
                     juce::dsp::WindowingFunction<float>::hann,
                     false);

        specFifo.reset();
        specRing.fill (0.0f);

        waveBuffer.fill (0.0f);

        fftScratch.fill (0.0f);
        drainBuf.fill (0.0f);
        spectrumDb.fill (-120.0f);

        captureFull = true;
        armedSkipSamples = 0;
        waveState.store (static_cast<std::uint64_t> (genShadow) << 32, std::memory_order_relaxed);
        // genShadow itself is left as-is (a monotonically increasing tag across prepare() calls)
    }

    //========================================================================== audio thread
    void Analyzer::armCapture (int skipSamples) noexcept
    {
        // New trigger: restart the streamed capture from sample 0 AND bump the generation
        // in one atomic store (see the waveState doc comment) so the message thread never
        // observes a torn (generation, writePos) pair. `skipSamples` (the note-on's exact
        // offset within the block that's about to be rendered/pushed) is consumed by the
        // very next pushBlock() call, below, so capture sample 0 IS the note-on, always —
        // no leading silence, regardless of where in the block the trigger landed.
        captureFull = false;
        armedSkipSamples = juce::jmax (0, skipSamples);
        ++genShadow;
        waveState.store (static_cast<std::uint64_t> (genShadow) << 32, std::memory_order_release);
    }

    void Analyzer::pushBlock (const float* left, const float* right, int numSamples) noexcept
    {
        if (numSamples <= 0 || left == nullptr || right == nullptr)
            return;

        // ---- (a) streamed one-kick waveform capture ---------------------------------
        //  Single writer (this thread). Append, then publish the new length (packed with
        //  the unchanged current generation, see waveState) with a release store so the
        //  message thread sees the samples before the count.
        if (! captureFull)
        {
            // Drop the pre-trigger portion of THIS call once, right after arming (armCapture()
            // and the one pushBlock() for that same host block always pair up within a single
            // processBlock() — see PluginProcessor.cpp). `skip` is always < numSamples: the
            // note-on that armed this capture is BY DEFINITION inside the block being pushed.
            const int skip = juce::jmin (armedSkipSamples, numSamples);
            armedSkipSamples -= skip;

            const auto packed = waveState.load (std::memory_order_relaxed);
            const int  pos    = static_cast<int> (packed & 0xffffffffu);
            const int  n      = juce::jmin (numSamples - skip, kWaveCaptureLen - pos);

            for (int i = 0; i < n; ++i)
                waveBuffer[static_cast<size_t> (pos + i)] = 0.5f * (left[skip + i] + right[skip + i]);

            waveState.store ((static_cast<std::uint64_t> (genShadow) << 32)
                                  | static_cast<std::uint32_t> (pos + n),
                             std::memory_order_release);

            if (pos + n >= kWaveCaptureLen)
                captureFull = true;
        }

        // ---- (b) spectrum ring — always; drop silently if the FIFO is full ----------
        int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
        specFifo.prepareToWrite (numSamples, start1, size1, start2, size2);

        for (int i = 0; i < size1; ++i)
            specRing[static_cast<size_t> (start1 + i)] = 0.5f * (left[i] + right[i]);

        for (int i = 0; i < size2; ++i)
            specRing[static_cast<size_t> (start2 + i)] = 0.5f * (left[size1 + i] + right[size1 + i]);

        specFifo.finishedWrite (size1 + size2);
    }

    //======================================================================== message thread
    int Analyzer::getWaveform (std::array<float, kWaveCaptureLen>& dst,
                               std::uint32_t& generation) const noexcept
    {
        // One packed acquire load -> generation and length always come from the SAME
        // pushBlock()/armCapture() store, never a torn mix of two independent atomics
        // (2026-08-31 fix). The release store ensures the [0, len) samples are visible too.
        const auto packed = waveState.load (std::memory_order_acquire);
        generation = static_cast<std::uint32_t> (packed >> 32);
        const int len = juce::jlimit (0, kWaveCaptureLen,
                                      static_cast<int> (packed & 0xffffffffu));

        for (int i = 0; i < len; ++i)
            dst[static_cast<size_t> (i)] = waveBuffer[static_cast<size_t> (i)];

        return len;
    }

    void Analyzer::updateSpectrum() noexcept
    {
        if (fft == nullptr || window == nullptr)
            return;

        int ready = specFifo.getNumReady();
        if (ready < kFftSize)
            return;

        // Keep only the most recent kFftSize samples so the display never lags.
        const int discard = ready - kFftSize;
        if (discard > 0)
        {
            int s1 = 0, z1 = 0, s2 = 0, z2 = 0;
            specFifo.prepareToRead (discard, s1, z1, s2, z2);
            specFifo.finishedRead (z1 + z2);
        }

        int s1 = 0, z1 = 0, s2 = 0, z2 = 0;
        specFifo.prepareToRead (kFftSize, s1, z1, s2, z2);
        for (int i = 0; i < z1; ++i) drainBuf[static_cast<size_t> (i)]      = specRing[static_cast<size_t> (s1 + i)];
        for (int i = 0; i < z2; ++i) drainBuf[static_cast<size_t> (z1 + i)] = specRing[static_cast<size_t> (s2 + i)];
        specFifo.finishedRead (z1 + z2);

        fftScratch.fill (0.0f);
        for (int i = 0; i < kFftSize; ++i)
            fftScratch[static_cast<size_t> (i)] = drainBuf[static_cast<size_t> (i)];

        window->multiplyWithWindowingTable (fftScratch.data(), static_cast<size_t> (kFftSize));
        fft->performFrequencyOnlyForwardTransform (fftScratch.data());

        // Hann coherent-gain compensation (~2.0) + FFT-size normalisation.
        const float norm    = 4.0f / static_cast<float> (kFftSize);
        const float smooth  = 0.6f;   // one-pole per bin for a calmer display

        for (int b = 0; b < kNumBins; ++b)
        {
            const float mag = fftScratch[static_cast<size_t> (b)] * norm;
            const float db  = juce::Decibels::gainToDecibels (mag + 1.0e-9f, -120.0f);
            auto& prev = spectrumDb[static_cast<size_t> (b)];
            prev = smooth * prev + (1.0f - smooth) * db;
        }
    }
}
