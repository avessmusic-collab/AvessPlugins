#pragma once

#include <array>
#include <atomic>
#include <memory>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>
#include <juce_dsp/juce_dsp.h>

namespace kickr
{
    /**
        Stage 3 Phase 3.2 — real-time analyzer taps for `WaveformDisplay` +
        `SpectrumDisplay` (architecture.md -> "Analyzer (waveform + spectrum taps)").

        Owned by `KICKRAudioProcessor`. All the atomics / buffers / FIFO / FFT live
        here. The lock-free contract (architecture.md):

          - Audio thread (`KICKRAudioProcessor::processBlock`, AFTER `engine.processBlock`
            so the buffer is the final post-limiter output):
              * `armCapture()`   on a note-on in the block
              * `pushBlock(L,R,n)` every block — bounded `memcpy` + atomic store into the
                double-buffered waveform capture, plus a bounded `AbstractFifo` write into
                the spectrum ring. If the FIFO is full the extra samples are dropped.
            NO allocation, NO locks, NO FFT on the audio thread.

          - Message thread (the 30 Hz display `Timer`s):
              * `getWaveform(dst)`  — copies the ready capture if a fresh one exists
              * `updateSpectrum()`  — drains the ring, Hann-windows, FFTs, magnitude->dB
                                      with a one-pole per-bin smoother
              * `getSpectrumDb()`   — the smoothed dB frame for painting
            The `dsp::FFT` + `dsp::WindowingFunction` are built ONCE in `prepare()`.
    */
    class Analyzer
    {
    public:
        static constexpr int kWaveCaptureLen = 16384;   // ~341 ms @ 48 kHz
        static constexpr int kSpecFifoLen    = 8192;
        static constexpr int kFftOrder       = 11;
        static constexpr int kFftSize        = 1 << kFftOrder;   // 2048
        static constexpr int kNumBins        = kFftSize / 2;     // 1024

        Analyzer() = default;

        /** Message thread — size the ring, build the Hann window + `dsp::FFT(11)` once,
            reset every atomic. Safe to call again on a sample-rate change. */
        void prepare (double sampleRate);

        //====================================================================== audio thread
        /** Arm a one-kick capture. Call when a note-on is seen in the block. */
        void armCapture() noexcept;

        /** Append `numSamples` of the post-limiter mono sum (0.5*(L+R)) to the filling
            waveform buffer (while armed) and always push it into the spectrum ring.
            Bounded `memcpy` + atomic stores + one `AbstractFifo` write. Allocation-free. */
        void pushBlock (const float* left, const float* right, int numSamples) noexcept;

        //==================================================================== message thread
        /** Copy the ready capture into `dst` iff a new one has been published since the
            last call. Returns whether `dst` was refreshed. */
        bool getWaveform (std::array<float, kWaveCaptureLen>& dst) const noexcept;

        /** Drain up to `kFftSize` samples, Hann-window, FFT, magnitude -> dB into the
            member frame with a ~0.6 one-pole per bin. Safe to call at 30 Hz. */
        void updateSpectrum() noexcept;

        const std::array<float, kNumBins>& getSpectrumDb() const noexcept { return spectrumDb; }

        double getSampleRate() const noexcept { return currentSampleRate; }

    private:
        double currentSampleRate { 48000.0 };

        // ---- waveform: double buffer + atomic index (architecture.md) ----------------
        std::array<std::array<float, kWaveCaptureLen>, 2> waveBuffers {};
        int               fillingIndex { 0 };          // audio-thread only
        int               writePos     { 0 };          // audio-thread only
        std::atomic<bool> captureArmed { false };
        std::atomic<int>  captureReadyBuffer { -1 };
        mutable int       lastSeenReady { -1 };         // message-thread only

        // ---- spectrum: AbstractFifo-guarded ring ------------------------------------
        juce::AbstractFifo               specFifo { kSpecFifoLen };
        std::array<float, kSpecFifoLen>  specRing {};

        // ---- message-thread FFT state (built in prepare) ---------------------------
        std::unique_ptr<juce::dsp::FFT>                      fft;
        std::unique_ptr<juce::dsp::WindowingFunction<float>> window;
        std::array<float, kFftSize * 2> fftScratch {};   // performFrequencyOnly... needs 2*N
        std::array<float, kFftSize>     drainBuf   {};
        std::array<float, kNumBins>     spectrumDb {};

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Analyzer)
    };
}
