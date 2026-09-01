#pragma once

#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>

namespace kickr
{
    /**
        Body fundamental oscillator (AD-1).

        Custom phase accumulator + std::sin — NOT juce::dsp::Oscillator: Phase 2.2 needs
        per-sample frequency from PitchEnvelope with guaranteed phase continuity, and
        noteOn() must hard-reset phase to 0 for layering phase-stability. A phase-
        accumulated sine is band-limited by construction (no energy above its
        instantaneous frequency). Runs inside the OS region (AD-10) — the increment uses
        `fsOversampled`.

        PHASE 2.1: clean sine only. `bodyHarmonics` tanh timbral shaping is a later phase.

        2026-09-01 (user request) — `morph`, v2. The first version crossfaded Sine ->
        Triangle -> Saw -> Square; the user found it far too drastic ("the morph knob
        effect is too strong") — the buzz of the harder shapes sat on the whole tail.
        v2 replaces it with two gentler ideas, both chosen by the user:

          1. CZ-style PHASE DISTORTION of the sine (Casio "saw" PD): the phase runs faster
             through the first part of the cycle and slower through the rest, so the
             waveform skews smoothly from a pure sine toward a soft, rounded saw. Harmonics
             rise gradually with the knob — no hard edges anywhere in the travel.
                t   = phase / 2pi in [0, 1)
                d   = 0.5 * (1 - k)                       (k = skew amount, 0 .. kMaxSkew)
                t'  = t < d ? t * (0.5 / d) : 0.5 + (t - d) * (0.5 / (1 - d))
                out = sin (2pi * t')
             k = 0 is exactly a sine; kMaxSkew < 1 keeps d > 0 (no division blow-up).

          2. ATTACK-ONLY: the skew amount is multiplied by a per-trigger envelope that
             starts at 1 on noteOn and decays exponentially with time constant
             kShapeDecaySeconds (40 ms). The transient gets the character; by ~120 ms the
             body is back to a clean sine whatever the knob says. This is what makes the
             control musical on a kick — the tail is where the extra harmonics were wrong.

        `morph == 0` (default) still takes the exact original std::sin path (the smoothed
        value returns exactly 0.0f once settled, and the envelope is multiplicative), so
        pre-morph output is bit-identical and existing presets are unaffected.
    */
    class BodyOscillator
    {
    public:
        void prepare (double newFsOversampled) noexcept;
        void reset() noexcept;

        /** PHASE 2.10 — OS factor changed: recompute the phase increment (and the shape-
            envelope / morph-smoother rates) for the new rate, KEEP the running phase. */
        void updateOversampledRate (double newFsOversampled) noexcept;

        /** Hard phase reset — call on every trigger. Also re-arms the attack-only shape
            envelope (-> 1.0) and snaps the morph smoother to its target (bug-scan
            2026-09-01, code-review CONFIRMED: renderSample() never runs while the voice is
            inactive, so a morph change made during silence left the 10 ms ramp pending and
            the next ISOLATED hit crossfaded for its first 10 ms, while a stealing hit, via
            reset(), started at the target — same "no fade-in on the first hit" rule
            KickVoice::noteOn applies to bodyLevel and the gates). */
        void noteOn() noexcept
        {
            phase    = 0.0;
            shapeEnv = 1.0f;
            morphSm.setCurrentAndTargetValue (morphSm.getTargetValue());
        }

        /** Per-sample safe: recomputes the phase increment for `hz`. */
        void setFrequency (float hz) noexcept;

        /** Per-block: morph amount 0 (pure sine) .. 1 (max phase-skew on the attack).
            Smoothed per-sample over kMorphSmoothSeconds (bug-scan 2026-09-01: an instant
            automation jump mid-cycle switched shape at a point where they differ by up to
            full scale — a ~7x slew spike, i.e. a click. Same treatment as every other
            per-block level/shape control in the engine). */
        void setMorph (float morph01) noexcept { morphSm.setTargetValue (juce::jlimit (0.0f, 1.0f, morph01)); }

        float renderSample() noexcept;

        static constexpr float  kMaxSkew           = 0.90f;   // knob at 100% -> d = 0.05 (never 0)
        static constexpr double kShapeDecaySeconds = 0.040;   // attack-only: skew x e^(-t / 40 ms)
        static constexpr double kMorphSmoothSeconds = 0.010;

    private:
        double fsOversampled { 44100.0 };
        double phase         { 0.0 };   // radians, wrapped to [0, 2pi)
        double phaseIncrement { 0.0 };
        float  frequencyHz   { 55.0f };

        // Attack-only shape envelope: 1.0 at noteOn, x shapeCoef per oversampled sample.
        float  shapeEnv  { 0.0f };
        float  shapeCoef { 0.0f };

        // 0 = pure sine (default — renderSample takes the exact original std::sin path
        // whenever the smoothed value is 0, so pre-morph output is bit-identical).
        juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> morphSm { 0.0f };
    };
}
