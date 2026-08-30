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

        for (auto& b : waveBuffers)
            b.fill (0.0f);

        fftScratch.fill (0.0f);
        drainBuf.fill (0.0f);
        spectrumDb.fill (-120.0f);

        fillingIndex = 0;
        writePos     = 0;
        lastSeenReady = -1;
        captureArmed.store (false, std::memory_order_relaxed);
        captureReadyBuffer.store (-1, std::memory_order_relaxed);
    }

    //========================================================================== audio thread
    void Analyzer::armCapture() noexcept
    {
        // Restart the write index of the (inactive) filling buffer and arm.
        writePos = 0;
        captureArmed.store (true, std::memory_order_relaxed);
    }

    void Analyzer::pushBlock (const float* left, const float* right, int numSamples) noexcept
    {
        if (numSamples <= 0 || left == nullptr || right == nullptr)
            return;

        // ---- (a) one-kick waveform capture (double buffer + atomic publish) ----------
        if (captureArmed.load (std::memory_order_relaxed))
        {
            auto& buf = waveBuffers[static_cast<size_t> (fillingIndex)];
            const int n = juce::jmin (numSamples, kWaveCaptureLen - writePos);

            for (int i = 0; i < n; ++i)
                buf[static_cast<size_t> (writePos + i)] = 0.5f * (left[i] + right[i]);

            writePos += n;

            if (writePos >= kWaveCaptureLen)
            {
                captureReadyBuffer.store (fillingIndex, std::memory_order_release);
                fillingIndex ^= 1;
                writePos = 0;
                captureArmed.store (false, std::memory_order_relaxed);
            }
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
    bool Analyzer::getWaveform (std::array<float, kWaveCaptureLen>& dst) const noexcept
    {
        const int ready = captureReadyBuffer.load (std::memory_order_acquire);

        if (ready < 0 || ready == lastSeenReady)
            return false;

        dst = waveBuffers[static_cast<size_t> (ready)];
        lastSeenReady = ready;
        return true;
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
