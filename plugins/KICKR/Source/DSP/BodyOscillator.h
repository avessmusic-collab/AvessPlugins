#pragma once

#include <juce_core/juce_core.h>

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
    */
    class BodyOscillator
    {
    public:
        void prepare (double newFsOversampled) noexcept;
        void reset() noexcept;

        /** PHASE 2.10 — OS factor changed: recompute the phase increment for the new
            rate, KEEP the running phase (coefficient-only, no state reset). */
        void updateOversampledRate (double newFsOversampled) noexcept;

        /** Hard phase reset — call on every trigger. */
        void noteOn() noexcept { phase = 0.0; }

        /** Per-sample safe: recomputes the phase increment for `hz`. */
        void setFrequency (float hz) noexcept;

        float renderSample() noexcept;

    private:
        double fsOversampled { 44100.0 };
        double phase         { 0.0 };   // radians, wrapped to [0, 2pi)
        double phaseIncrement { 0.0 };
        float  frequencyHz   { 55.0f };
    };
}
