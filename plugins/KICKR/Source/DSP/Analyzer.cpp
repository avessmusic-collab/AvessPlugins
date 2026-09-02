#include "DSP/Analyzer.h"

#include <algorithm>
#include <cmath>

namespace kickr
{
    void Analyzer::prepare (double sampleRate)
    {
        currentSampleRate = juce::jmax (1.0, sampleRate);

        // Built here (message thread) and in setResolutionOrder() — never on the audio thread.
        setResolutionOrder (fftOrder);

        specFifo.reset();
        specRing.fill (0.0f);

        waveBuffer.fill (0.0f);

        fftScratch.fill (0.0f);
        history.fill (0.0f);
        historyPos = 0;
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
            // 2026-09-02: no longer always < numSamples — the processor adds the Color
            // Limiter's look-ahead latency (up to 10 ms) so the capture aligns with the
            // DELAYED onset; the remainder carries over to the following pushBlock() calls.
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

    void Analyzer::setResolutionOrder (int order)
    {
        fftOrder = juce::jlimit (kMinFftOrder, kMaxFftOrder, order);
        fft      = std::make_unique<juce::dsp::FFT> (fftOrder);
        window   = std::make_unique<juce::dsp::WindowingFunction<float>> (
                       static_cast<size_t> (getFftSize()),
                       juce::dsp::WindowingFunction<float>::hann,
                       false);
        spectrumDb.fill (-120.0f);
    }

    void Analyzer::updateSpectrum() noexcept
    {
        if (fft == nullptr || window == nullptr)
            return;

        const int fftSize = getFftSize();
        const int numBins = getNumBins();

        // Drain EVERYTHING new into the rolling history (overlapping frames: each update
        // analyses the most recent fftSize samples, whatever arrived since the last call).
        const int ready = specFifo.getNumReady();
        if (ready > 0)
        {
            int s1 = 0, z1 = 0, s2 = 0, z2 = 0;
            specFifo.prepareToRead (ready, s1, z1, s2, z2);
            auto push = [this] (const float* src, int n)
            {
                for (int i = 0; i < n; ++i)
                {
                    history[static_cast<size_t> (historyPos)] = src[i];
                    historyPos = (historyPos + 1) % kMaxFftSize;
                }
            };
            push (specRing.data() + s1, z1);
            push (specRing.data() + s2, z2);
            specFifo.finishedRead (z1 + z2);
        }

        // Most recent fftSize samples, oldest first.
        fftScratch.fill (0.0f);
        int rp = (historyPos - fftSize + kMaxFftSize) % kMaxFftSize;
        for (int i = 0; i < fftSize; ++i)
        {
            fftScratch[static_cast<size_t> (i)] = history[static_cast<size_t> (rp)];
            rp = (rp + 1) % kMaxFftSize;
        }

        window->multiplyWithWindowingTable (fftScratch.data(), static_cast<size_t> (fftSize));
        fft->performFrequencyOnlyForwardTransform (fftScratch.data());

        // Hann coherent-gain compensation (~2.0) + FFT-size normalisation: a 0 dBFS sine
        // reads 0 dB in its bin at every resolution.
        const float norm = 4.0f / static_cast<float> (fftSize);
        // Pro-Q-style ballistics: instant attack, release at `releaseDbPerSec` (Speed).
        const float releasePerFrame = releaseDbPerSec / frameRate;

        for (int b = 0; b < numBins; ++b)
        {
            const float mag = fftScratch[static_cast<size_t> (b)] * norm;
            const float db  = juce::Decibels::gainToDecibels (mag + 1.0e-9f, -120.0f);
            auto& prev = spectrumDb[static_cast<size_t> (b)];
            prev = db > prev ? db : juce::jmax (db, prev - releasePerFrame);
        }
    }
}
