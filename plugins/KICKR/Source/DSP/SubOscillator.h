#pragma once

#include <juce_core/juce_core.h>

namespace kickr
{
    /**
        Independent clean sub sine (architecture.md -> SubOscillator).

        PHASE 2.5. Per-voice layer inside KickVoice, summed with body + click BEFORE the
        voice's mono output. Runs at `fsOversampled` (in-region, uniform with the other
        layers — AD-10).

        - Custom phase accumulator + std::sin — `sin(phase)` is taken BEFORE advancing the
          phase, so sample 0 is exactly `sin(0) = 0` (clean phase-0 onset, matches
          BodyOscillator / ClickGenerator component B).
        - Fixed frequency `subFreq` (25-80 Hz UI range; clamped [10, 500] Hz here),
          INDEPENDENT of the pitch envelope and of `fundamental` / tune — just `subFreq`
          straight. The phase increment is recomputed vs `fsOversampled` whenever
          `subFreq` changes (per-block).
        - Phase reset to 0 on every noteOn (research S5 — phase determinism so layered /
          stacked kicks don't comb-filter; verified by the Phase 2.5 null test).
        - Own exponential AD envelope: fixed ~1.5 ms raised-cosine attack (click-free),
          exponential decay `coef = expDecayCoef(subDecay, fsOversampled)` (~-60 dB over
          `subDecay`). No sustain.
        - Output: `sin(phase) * subEnv * subLevel`. MONO — the same value is added to both
          output channels downstream. NOT velocity-scaled in v1 (the whole-voice
          `velLevel` still applies downstream); `subLevel` is the only gain here.
        - `isActive()` is false once the envelope is below -90 dB AND past a short minimum
          length — factored into KickVoice::isActive() alongside `ampEnv` and `click`.

        RT-safe: no allocation / lock / log / IO. The AD running value is denormal-flushed
        each sample; the output is sanitized.
    */
    class SubOscillator
    {
    public:
        static constexpr float kAttackMs  = 1.5f;              // fixed click-free attack
        static constexpr float kFloorGain = 3.16227766e-5f;    // -90 dB

        void prepare (double fsOversampled) noexcept;
        void reset() noexcept;

        /** PHASE 2.10 — OS factor changed mid-note: recompute the phase increment + decay
            coefficient for the new rate and rescale the in-samples counters (wall-clock
            preserved). Running phase / envelope value are KEPT. Coefficient-only. */
        void updateOversampledRate (double newFsOversampled) noexcept;

        /** Per-block from KickEngine -> KickVoice. No APVTS reads inside. */
        void setParams (float subLevel, float subFreqHz, float subDecayMs) noexcept;

        /** Trigger: phase -> 0, arm the AD envelope. No-ops when the layer is off. */
        void noteOn() noexcept;

        /** Summed mono sub sample. Returns exactly 0 once the envelope has finished. */
        float renderSample() noexcept;

        bool isActive() const noexcept { return active; }

    private:
        double fs { 44100.0 };

        // Per-block params.
        float  level    { 0.5f };
        float  freqHz   { 40.0f };
        float  decayMs  { 300.0f };
        double phaseInc { 0.0 };

        // Trigger / running state.
        double phase              { 0.0 };   // radians, wrapped to [0, 2pi)
        float  env                { 0.0f };
        float  decayCoef          { 0.0f };
        int    attackSamples      { 1 };
        int    attackPos          { 0 };
        int    samplesSinceTrigger { 0 };
        int    minLengthSamples   { 0 };
        int    maxLengthSamples   { 0 };
        bool   attacking          { false };
        bool   active             { false };
    };
}
