#pragma once

#include <array>
#include <atomic>
#include <cstdint>
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

          - Message thread (the display `Timer`s):
              * `getWaveform(dst, gen)` — copies the *prefix that has been written so far*
                (the audio thread only ever writes at indices >= writePos, so [0, writePos)
                is stable). Returns that valid length + the capture generation, so the
                display draws the kick sweeping in **left -> right** as it plays, with only
                ~one timer tick of latency instead of waiting for a full 341 ms buffer.
              * `updateSpectrum()`  — drains the ring, Hann-windows, FFTs, magnitude->dB
                                      with a one-pole per-bin smoother
              * `getSpectrumDb()`   — the smoothed dB frame for painting
            The `dsp::FFT` + `dsp::WindowingFunction` are built ONCE in `prepare()`.
    */
    class Analyzer
    {
    public:
        static constexpr int kWaveCaptureLen = 16384;   // ~341 ms @ 48 kHz
        static constexpr int kSpecFifoLen    = 32768;   // >= kMaxFftSize + a few blocks of slack
        // 2026-09-02 (user request: "look at how fabfilter pro q 3 works and implement the
        // same settings"): the FFT size is now a RESOLUTION setting (Low 2048 / Medium 4096 /
        // High 8192 / Maximum 16384). Frames OVERLAP — every update transforms the most
        // recent fftSize samples from a rolling history, so a 16384-point analysis still
        // refreshes at the display's 30 Hz instead of once per 341 ms. Buffers are sized for
        // the maximum once; `getNumBins()` is the ACTIVE bin count.
        static constexpr int kMinFftOrder    = 11;
        static constexpr int kMaxFftOrder    = 14;
        static constexpr int kMaxFftSize     = 1 << kMaxFftOrder;   // 16384
        static constexpr int kMaxBins        = kMaxFftSize / 2;     // 8192
        static constexpr int kFftOrder       = 12;                  // default resolution: Medium (Pro-Q's default)
        static constexpr int kFftSize        = 1 << kFftOrder;
        static constexpr int kNumBins        = kFftSize / 2;

        /** Display-side view settings shared by the SPECTRUM and FILTER pages (message
            thread only): Pro-Q-3-style Tilt (dB/oct around 1 kHz) and Range (dB). */
        struct View
        {
            float tiltDbPerOct { 4.5f };   // 0 / 1.5 / 3 / 4.5 / 6
            float rangeDb      { 90.0f };  // 60 / 90 / 120
        };

        Analyzer() = default;

        /** Message thread — size the ring, build the Hann window + `dsp::FFT(11)` once,
            reset every atomic. Safe to call again on a sample-rate change. */
        void prepare (double sampleRate);

        //====================================================================== audio thread
        /** Arm a one-kick capture. Call when a note-on is seen in the block.
            `skipSamples` = the note-on's sample-accurate offset WITHIN that block (e.g.
            `MidiMessageMetadata::samplePosition`) — the first `pushBlock()` after arming
            drops that many leading samples instead of capturing them. Without this, the
            capture starts at sample 0 of the BLOCK containing the note-on, not the note-on
            itself, so a mid-block trigger (the overwhelmingly common case) leaves genuine
            silence at the front of the buffer — a fixed, non-deterministic amount that
            varies with retrigger timing (2026-09-01 user report: "no matter what time i
            input midi at, the soundwave ... appears always fixed and doesn't move
            around" — a display-side amplitude-threshold guess at the onset, tried first,
            couldn't tell a real new-kick onset from the tail of a still-ringing previous
            voice bleeding into the same capture on a fast retrigger, so it was NOT actually
            stable). Sample-accurate at the source removes the ambiguity entirely — default
            0 for any caller that doesn't have (or need) the exact offset. */
        void armCapture (int skipSamples = 0) noexcept;

        /** Append `numSamples` of the post-limiter mono sum (0.5*(L+R)) to the filling
            waveform buffer (while armed) and always push it into the spectrum ring.
            Bounded `memcpy` + atomic stores + one `AbstractFifo` write. Allocation-free. */
        void pushBlock (const float* left, const float* right, int numSamples) noexcept;

        //==================================================================== message thread
        /** Copy the samples captured *so far* for the current trigger into `dst` (the
            audio thread only writes at indices >= the returned length, so the prefix is
            stable). `generation` is bumped on every `armCapture()` so the caller can tell
            a fresh trigger from a still-filling one. Returns the number of valid samples
            in `dst` (0 before the first note). */
        int getWaveform (std::array<float, kWaveCaptureLen>& dst,
                         std::uint32_t& generation) const noexcept;

        /** Drain up to `kFftSize` samples, Hann-window, FFT, magnitude -> dB into the
            member frame with a ~0.6 one-pole per bin. Safe to call at 30 Hz. */
        void updateSpectrum() noexcept;

        /** The smoothed dB frame — only the first `getNumBins()` entries are meaningful. */
        const std::array<float, kMaxBins>& getSpectrumDb() const noexcept { return spectrumDb; }

        double getSampleRate() const noexcept { return currentSampleRate; }

        // ---- Pro-Q-3-style analyzer settings (message thread) ---------------------------
        /** Resolution: FFT order 11..14 (2048..16384). Rebuilds the FFT object (message
            thread), clears the frame. */
        void setResolutionOrder (int order);
        int  getFftOrder() const noexcept { return fftOrder; }
        int  getFftSize()  const noexcept { return 1 << fftOrder; }
        int  getNumBins()  const noexcept { return getFftSize() / 2; }
        /** Speed: release rate of the peak-hold ballistics (attack is instant). */
        void  setReleaseDbPerSecond (float dbPerSec) noexcept { releaseDbPerSec = juce::jmax (1.0f, dbPerSec); }
        float getReleaseDbPerSecond() const noexcept { return releaseDbPerSec; }
        /** How often updateSpectrum() is called (the display timer) — sets dB per frame. */
        void  setFrameRate (float hz) noexcept { frameRate = juce::jmax (1.0f, hz); }
        View&       getView() noexcept       { return view; }
        const View& getView() const noexcept { return view; }

    private:
        double currentSampleRate { 48000.0 };

        // ---- waveform: single buffer, streamed. The audio thread appends and publishes
        //      the write position; the message thread reads it and draws the stable
        //      [0, pos) prefix -> the kick sweeps in left->right as it plays, ~one timer
        //      tick of latency instead of a full 341 ms buffer wait.
        //
        //      Fix (2026-08-31): generation and write-position used to be two SEPARATE
        //      atomics, each read independently in getWaveform(). armCapture() writes
        //      writePos=0 then bumps generation as two separate stores; a message-thread
        //      read landing between them could pair the OLD generation with the NEW
        //      (reset) writePos — a torn, inconsistent snapshot (benign here — a one-
        //      frame display glitch — but still a real race). Packed into ONE atomic
        //      (generation in the high 32 bits, writePos in the low 32) so a single
        //      load/store always yields a consistent pair.
        std::array<float, kWaveCaptureLen> waveBuffer {};
        std::atomic<std::uint64_t> waveState   { 0 };   // (generation << 32) | writePos
        std::uint32_t              genShadow   { 0 };    // audio-thread-only mirror of the
                                                          // current generation (single writer)
        bool                       captureFull { true };    // audio-thread only; true = idle
        int                        armedSkipSamples { 0 };  // audio-thread only; see armCapture()

        // ---- spectrum: AbstractFifo-guarded ring ------------------------------------
        juce::AbstractFifo               specFifo { kSpecFifoLen };
        std::array<float, kSpecFifoLen>  specRing {};

        // ---- message-thread FFT state (built in prepare / setResolutionOrder) ---------
        std::unique_ptr<juce::dsp::FFT>                      fft;
        std::unique_ptr<juce::dsp::WindowingFunction<float>> window;
        int   fftOrder        { kFftOrder };
        float releaseDbPerSec { 60.0f };    // Speed: Medium
        float frameRate       { 30.0f };
        View  view;
        std::array<float, kMaxFftSize * 2> fftScratch {};   // performFrequencyOnly... needs 2*N
        std::array<float, kMaxFftSize>     history    {};   // rolling: the most recent samples, overlapping frames
        int                                historyPos { 0 };
        std::array<float, kMaxBins>        spectrumDb {};

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Analyzer)
    };
}
