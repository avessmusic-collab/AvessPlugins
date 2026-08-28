#pragma once

#include <juce_dsp/juce_dsp.h>

namespace kickr
{
    /**
        Sustained tail / rumble layer (architecture.md -> TailGenerator; Parameter
        Mapping rows 15-18).

        PHASE 2.6. Per-voice layer inside KickVoice, summed with body + click + sub
        BEFORE the voice's mono output. The whole layer (osc + LP + tailDrive shaper)
        renders at `fsOversampled` (in-region, AD-10) so the tanh harmonics are
        genuinely oversampled once Phase 2.10 enables 2x+.

        - Source: a DEDICATED low-frequency sine oscillator locked to `fundamentalEff`
          (the resolved note frequency the body got, BEFORE the pitch-envelope
          multiplier). Custom phase accumulator + std::sin; `sin(phase)` is taken
          BEFORE advancing the phase so sample 0 is exactly sin(0) = 0. Phase reset to
          0 on every noteOn. NOT a tap of the body bus; no pitch envelope - steady
          frequency at `fundamentalEff` for the whole note (matches BodyOscillator /
          ClickGenerator component B / SubOscillator onset).
        - Envelope: slower raised-cosine attack (~10 ms, so the tail sits BEHIND the
          transient / click, not competing) -> one-pole exponential decay = `tailLength`
          (20-2000 ms), `coef = DSPUtils::expDecayCoef(tailLength, fsOversampled)`. No
          sustain.
        - `tailTone`: juce::dsp::StateVariableTPTFilter<float> LOW-PASS, cutoff mapped
          logarithmically 120 Hz -> 4 kHz as `tailTone` 0->1:
          `cutoff = 120 * pow(4000/120, tailTone)`, moderate Q ~0.7, coeffs vs
          `fsOversampled`, refreshed per block, clamped to [20, fsOversampled*0.45].
        - `tailDrive`: tanh waveshaper on the post-filter tail. Pre-gain
          `g = 1 + tailDrive*7` (1 -> ~8). `shaped = tanh(g*x) / tanh(g)`
          (peak-preserving), blended in PARALLEL by `tailDrive` so `tailDrive = 0` is
          exactly bit-transparent and low levels are not just made quieter.
        - Order: osc -> env -> LP(tailTone) -> tanh(tailDrive) -> x tailLevel.
        - Output: MONO (added identically to L/R downstream). NOT velocity-scaled
          (the whole-voice `velLevel` still applies downstream). `noteOn` no-ops when
          `tailLevel < 1e-6` (true bypass).
        - `isActive()` (env floor + min length + max-length fallback, same pattern as
          SubOscillator) -> factored into KickVoice::isActive() alongside ampEnv,
          click and sub.

        RT-safe: no allocation / lock / log / IO. The AD running value is
        denormal-flushed each sample; the SVF state via snapToZero(); the output is
        sanitized.

        // Phase 2.11: + macroTail offsets (tailLevel +, tailLength +, tailTone +).
    */
    class TailGenerator
    {
    public:
        static constexpr float kAttackMs  = 10.0f;             // slow - sits behind the transient
        static constexpr float kFloorGain = 3.16227766e-5f;    // -90 dB

        void prepare (double fsOversampled) noexcept;
        void reset() noexcept;

        /** Per-block from KickEngine -> KickVoice. No APVTS reads inside. */
        void setParams (float tailLevel, float tailLengthMs,
                        float tailTone01, float tailDrive01) noexcept;

        /** Trigger: phase -> 0, lock the osc to `fundamentalEffHz`, arm the AD env.
            No-ops (true bypass) when `tailLevel < 1e-6`. */
        void noteOn (float fundamentalEffHz) noexcept;

        /** Summed mono tail sample. Returns exactly 0 once the envelope has finished. */
        float renderSample() noexcept;

        bool isActive() const noexcept { return active; }

    private:
        void updateDerived() noexcept;

        double fs           { 44100.0 };
        float  nyquistLimit { 19845.0f };   // fs * 0.45

        // Per-block params.
        float level    { 0.3f };
        float lengthMs { 200.0f };
        float tone01   { 0.5f };
        float drive01  { 0.2f };

        // Derived per block.
        float decayCoef        { 0.0f };
        float lpCutoffHz       { 692.0f };
        float driveGain        { 2.4f };   // 1 + drive01 * 7
        float invTanhDriveGain { 1.0f };   // 1 / tanh(driveGain)
        bool  driveActive      { true };

        // Oscillator.
        float  fundamentalHz { 55.0f };
        double phase         { 0.0 };   // radians, wrapped to [0, 2pi)
        double phaseInc      { 0.0 };

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

        juce::dsp::StateVariableTPTFilter<float> lp;
    };
}
