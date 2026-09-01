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

        2026-09-01 (user request) — `morph`: a 3-segment continuous crossfade across four
        phase-aligned shapes (all share the same rising zero-crossing at phase 0, so
        morphing never introduces a phase-alignment click): Sine -> Triangle -> Sawtooth ->
        Square. `morph == 0` (default) is EXACTLY the original pure-sine output — existing
        presets/factory sounds are unaffected unless the knob is moved. Naive (non-band-
        limited) triangle/saw/square — acceptable given this whole voice already renders
        inside the existing 1x/2x/4x/8x oversampling region (AD-10) built specifically to
        control aliasing from nonlinear/waveshaping stages, and the body's fundamental is
        always well below Nyquist at any sane kick pitch.
    */
    class BodyOscillator
    {
    public:
        void prepare (double newFsOversampled) noexcept;
        void reset() noexcept;

        /** PHASE 2.10 — OS factor changed: recompute the phase increment for the new
            rate, KEEP the running phase (coefficient-only, no state reset). */
        void updateOversampledRate (double newFsOversampled) noexcept;

        /** Hard phase reset — call on every trigger. Also snaps the morph smoother to its
            target (bug-scan 2026-09-01, code-review CONFIRMED): renderSample() never runs
            while the voice is inactive, so a morph change made during silence left the
            10 ms ramp pending and the next ISOLATED hit crossfaded shapes for its first
            10 ms, while a stealing hit (which goes through reset()) started at the target —
            same "no fade-in on the first hit" rule KickVoice::noteOn applies to bodyLevel
            and the gates. */
        void noteOn() noexcept
        {
            phase = 0.0;
            morphSm.setCurrentAndTargetValue (morphSm.getTargetValue());
        }

        /** Per-sample safe: recomputes the phase increment for `hz`. */
        void setFrequency (float hz) noexcept;

        /** Per-block: waveform morph position, 0 (sine) .. 1 (square) via triangle/saw.
            Smoothed per-sample over kMorphSmoothSeconds (bug-scan 2026-09-01: an instant
            automation jump mid-cycle switched shapes at a point where they differ by up to
            full scale — a ~7x slew spike, i.e. a click. Same treatment as every other
            per-block level/shape control in the engine). */
        void setMorph (float morph01) noexcept { morphSm.setTargetValue (juce::jlimit (0.0f, 1.0f, morph01)); }

        float renderSample() noexcept;

    private:
        static constexpr double kMorphSmoothSeconds = 0.010;

        static float triangleAt (double t01) noexcept;
        static float sawAt (double t01) noexcept;
        static float squareAt (double t01) noexcept;

        double fsOversampled { 44100.0 };
        double phase         { 0.0 };   // radians, wrapped to [0, 2pi)
        double phaseIncrement { 0.0 };
        float  frequencyHz   { 55.0f };

        // 0 = pure sine (default — renderSample takes the exact original std::sin path
        // whenever the smoothed value is 0, so pre-morph output is bit-identical).
        juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> morphSm { 0.0f };
    };
}
