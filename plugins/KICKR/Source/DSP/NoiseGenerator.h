#pragma once

#include <array>

#include <juce_core/juce_core.h>
#include <juce_dsp/juce_dsp.h>

namespace kickr
{
    /**
        Optional white / pink / filtered noise layer (architecture.md -> NoiseGenerator;
        Parameter Mapping rows 19-22).

        PHASE 2.7. Per-voice layer inside KickVoice, summed with body + click + sub + tail
        BEFORE the voice's mono output. Runs at `fsOversampled` (in-region, AD-10).

        DEFAULT OFF: `noiseLevel` defaults to 0.0 -> `noteOn` no-ops (true bypass),
        `renderSample()` returns exact 0.0f, `isActive()` stays false. No idle noise, no DC.

        - juce::Random `rng` -- a per-NoiseGenerator member (NOT shared). Seeded
          deterministically (`rng.setSeed(kSeed)`, kSeed = 0x6e6f697a = "noiz") on every
          `noteOn` so every trigger renders the identical noise and offline renders reproduce.
        - `noiseType` (Choice 0..2):
            White (0)    = raw rng noise -> `noiseTone` one-pole tilt (dark <-> bright).
            Pink  (1)    = white -> Paul Kellet 7-pole pink filter -> same `noiseTone` tilt.
            Filtered (2) = white -> juce::dsp::StateVariableTPTFilter<float> band-pass,
                           centre mapped logarithmically from `noiseTone`:
                           centre = 200 * pow(12000/200, noiseTone) (200 Hz -> 12 kHz),
                           Q ~= 1.5, coeffs vs `fsOversampled`, clamped
                           [20, fsOversampled*0.45], refreshed per block.
          Tilt (White / Pink): first-order shelving tilt around a ~1.5 kHz pivot. one-pole
          `low += coef*(in - low)`, `high = in - low`,
          out = `2*(1-tone)*low + 2*tone*high` -> tone 0 = dark (+6 dB low), tone 0.5 =
          flat / unity, tone 1 = bright. Clean tilt, not a precise EQ.
        - AD envelope: fixed 0.1 ms raised-cosine attack (click-free) -> one-pole exp decay
          `coef = DSPUtils::expDecayCoef(noiseDecay, fsOversampled)` (`noiseDecay` 20-500 ms).
          No sustain. `minLengthSamples = attack + 5 ms`, hard
          `maxLengthSamples ~= 2*noiseDecay + 50 ms` fallback (same pattern as
          SubOscillator / TailGenerator).
        - Output: `noise_after_filter * noiseEnv * noiseLevel`. MONO (added identically to
          L/R downstream). NOT velocity-scaled (the whole-voice `velLevel` still applies).
        - `isActive()` (env floor + min / max-length fallback) -> factored into
          KickVoice::isActive() alongside ampEnv / click / sub / tail.

        RT-safe: no allocation / lock / log / IO. `juce::Random::nextFloat()` is fine on the
        audio thread. The AD env value, the pink-filter state, the tilt state and the SVF
        state are denormal-flushed each sample; the output is sanitized.

        // Phase 2.9: no stereo for the noise layer in v1 (mono, like sub / tail).
    */
    class NoiseGenerator
    {
    public:
        static constexpr juce::int64 kSeed        = 0x6e6f697aLL;    // "noiz"
        static constexpr float       kAttackMs    = 0.1f;            // fixed click-free attack
        static constexpr float       kFloorGain   = 3.16227766e-5f;  // -90 dB
        static constexpr float       kTiltPivotHz = 1500.0f;         // fixed tilt pivot
        static constexpr float       kPinkScale   = 0.11f;           // Kellet output normalisation

        void prepare (double fsOversampled) noexcept;
        void reset() noexcept;

        /** Per-block from KickEngine -> KickVoice. No APVTS reads inside.
            `noiseType`: 0 = White, 1 = Pink, 2 = Filtered. */
        void setParams (float noiseLevel, float noiseDecayMs,
                        float noiseTone01, int noiseType) noexcept;

        /** Trigger: re-seed the rng, reset the filters, arm the AD env.
            No-ops (true bypass) when `noiseLevel < 1e-6` (the common case — default 0). */
        void noteOn() noexcept;

        /** Summed mono noise sample. Returns exactly 0 once the envelope has finished. */
        float renderSample() noexcept;

        bool isActive() const noexcept { return active; }

    private:
        void  updateDerived() noexcept;
        float tilt (float x) noexcept;
        float pink (float white) noexcept;

        double fs           { 44100.0 };
        float  nyquistLimit { 19845.0f };   // fs * 0.45

        // Per-block params.
        float level   { 0.0f };
        float decayMs { 60.0f };
        float tone01  { 0.5f };
        int   type    { 0 };

        // Derived per block.
        float decayCoef  { 0.0f };
        float tiltCoef   { 0.178f };
        float bpCentreHz { 1549.0f };

        // Tilt one-pole state (White / Pink).
        float tiltState { 0.0f };

        // Paul Kellet 7-pole pink filter state.
        std::array<float, 7> pk { {} };

        // AD envelope.
        float env           { 0.0f };
        int   attackSamples { 1 };
        int   attackPos     { 0 };
        bool  attacking     { false };

        // Lifecycle.
        int  samplesSinceTrigger { 0 };
        int  minLengthSamples    { 0 };
        int  maxLengthSamples    { 0 };
        bool active              { false };

        juce::Random rng;   // per-NoiseGenerator, NOT shared
        juce::dsp::StateVariableTPTFilter<float> bp;   // Filtered (type 2)
    };
}
