#pragma once

#include <array>

#include <juce_dsp/juce_dsp.h>

#include "Utilities/DSPUtils.h"
#include "DSP/ColorLimiter.h"

namespace kickr
{
    /**
        Master output section (architecture.md -> OutputStage, plan.md Phase 2.9).

        Runs INSIDE the OS region (AD-10) — coefficients vs `fsOversampled`:

          processTone()   3-band tone, fixed PRE-distortion (invoked by KickEngine
                          before the TransientShaper):
                            low  : low shelf  @ 130 Hz  (-12..+12 dB)
                            mid  : peak/bell  @ 750 Hz  Q 0.7  (-12..+12 dB)
                            high : high shelf @ 5 kHz   (-12..+12 dB)
                          `juce::dsp::IIR::Filter<float>` x2 channels per band; the dB
                          set is cached and the biquads recomputed in-place only when a
                          band moves > 0.01 dB (no per-block heap in steady state). A
                          0 dB shelf / bell is an exact unity pass-through.

          processOutput() crossover + width + mix + gain + limiter (invoked after the
                          per-channel Waveshaper):
                            1. `juce::dsp::LinkwitzRileyFilter<float>` LR split @ 130 Hz.
                               Low band -> forced mono 0.5*(L+R) to both channels.
                               High band -> M/S: side *= widthFactor(outputWidth),
                               widthFactor = 2*outputWidth  (0 -> 0, 0.5 -> 1, 1 -> 2).
                            2. Mix — equal-power blend processed <-> silence (AD-7):
                               *= sin(mix * pi/2). Default 1.0 -> unity.
                            3. Output gain — dbToGain(output), -24..+12 dB, smoothed.
                            4. DC blocker (in-region, ~5 Hz) — MUST sit before the
                               limiter: a DC blocker after the limiter re-introduces
                               edge peaks above the ceiling on a heavily-clipped kick
                               (found at the Phase 2.9 checkpoint). Corner well below
                               the 25 Hz sub so the low end is untouched.
                            5. Safety limiter (AD-4) — `limiter` on: zero-latency
                               soft-clip `y = ceil * tanh(x / ceil)`, ceil = -0.5 dBFS.
                               Truly the last processing before the output, so the
                               ceiling is guaranteed. off: pass-through (may exceed 0 dBFS).

        Only the analyzer taps + a final NaN/Inf guard live in KickEngine, AFTER
        `processSamplesDown`.

        `bodyWidth` / `clickWidth` are per-layer (forwarded to the voices by KickEngine)
        and are NOT used in this class — only `outputWidth`.

        RT-safe: no alloc / lock / IO in processTone / processOutput. Tone + crossover
        filter states and the smoothed gains are denormal-flushed / snapped each sample.
    */
    class OutputStage
    {
    public:
        static constexpr float kMonoCrossoverHz         = 130.0f;
        static constexpr float kLimiterCeilingDb        = -0.5f;
        static constexpr float kLimiterCeilingGain      = 0.94406088f;   // dbToGain(-0.5) — also the base-rate safety-clamp ceiling
        static constexpr int   kLimiterLookaheadSamples = 0;      // AD-4 hook — always 0 in v1

        static constexpr float kLowShelfHz  = 130.0f;
        static constexpr float kMidBellHz   = 750.0f;
        static constexpr float kMidBellQ    = 0.7f;
        static constexpr float kHighShelfHz = 5000.0f;
        static constexpr float kShelfQ      = 0.70710678f;

        void prepare (double fsOversampled) noexcept;
        void reset() noexcept;
        // NOTE (bug-scan 2026-09-01): there was briefly a per-note-on `resetFilterState()`
        // that cleared only `crossover` (to suppress its few-ms LF ring-down of a just-killed
        // voice). Removed — code-review CONFIRMED the reset itself stepped the outgoing
        // voice's LF tail by up to its full amplitude on the one sample the new note can't
        // yet mask. Filters are reset only by reset() (full engine reset); nothing in this
        // class is touched per note-on. See KickEngine::handleNoteOn.

        /** PHASE 2.10 — OS factor changed: recompute the tone biquad coefficients
            (in-place — IIR state is KEPT), the DC-blocker pole, and the mix/out gain
            smoother rates for the new sample rate; re-`prepare` the Linkwitz-Riley
            crossover (its integrator states clear — covered by the switch fade).
            Coefficient-only otherwise. */
        void updateOversampledRate (double newFsOversampled) noexcept;

        /** Per block (from KickEngine — never reads APVTS itself). */
        void setParams (float lowDb, float midDb, float highDb,
                        float outputWidth01, float outputDb,
                        const ColorLimiter::Params& limiterParams, float mix01) noexcept;
        /*  2026-09-02 (user request): step 5 is now the `ColorLimiter` (Ableton Color
            Limiter control set: loudness / ceiling / lookahead / release / saturation /
            color) instead of the fixed -0.5 dBFS tanh soft-clip. Its look-ahead delay is
            the plugin's only non-oversampling latency (reported by the processor). */
        int limiterLatencySamples() const noexcept { return limiter.latencySamples(); }

        /** PRE-distortion 3-band tone, per sample, in-region. */
        void processTone (float& l, float& r) noexcept;

        /** Crossover / M-S width / mix / output gain / safety limiter, per sample, in-region. */
        void processOutput (float& l, float& r) noexcept;

    private:
        // ArrayCoefficients returns a stack std::array -> assigning it into a
        // Filter's Coefficients reuses the already-allocated storage (allocation-free
        // after the first call in prepare()).
        using ArrayCoeffs = juce::dsp::IIR::ArrayCoefficients<float>;
        using Filter      = juce::dsp::IIR::Filter<float>;

        void refreshTone() noexcept;

        double fs { 44100.0 };

        // Tone — one biquad per band per channel.
        Filter lowL,  lowR;
        Filter midL,  midR;
        Filter highL, highR;
        float  lowDbCached  { 0.0f };
        float  midDbCached  { 0.0f };
        float  highDbCached { 0.0f };

        // Stereo width / mono crossover.
        juce::dsp::LinkwitzRileyFilter<float> crossover;   // 2 channels, split @ 130 Hz
        float widthFactor { 1.0f };                        // 2 * outputWidth

        // Mix + output gain + limiter.
        juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> mixGain;
        juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> outGain;
        ColorLimiter limiter;                              // 2026-09-02 — replaces the fixed soft-clip

        // In-region DC blocker — before the limiter (see processOutput step 4).
        std::array<dsputils::DCBlocker, 2> dcBlock;

        JUCE_LEAK_DETECTOR (OutputStage)
    };
}
