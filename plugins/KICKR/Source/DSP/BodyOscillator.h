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

        2026-09-02 (user request — "make the morph knob have choices like Serum's warp
        mode") — `morphMode` (8 selectable modes, see `Mode` below) turns `morph` into a
        per-mode AMOUNT knob rather than one fixed algorithm. `Mode::bendSkew` (0, the
        default) is exactly the v2 behaviour above, unchanged, bit-identical. Every mode
        except `pd` reuses the SAME attack-only shaping (`morphAmt x shapeEnv`) as
        bendSkew — the tail always relaxes toward a clean sine, keeping every mode
        "musical" on a kick rather than a sustained texture. `pd` is the one exception:
        it applies its warp continuously (attack AND tail), which is what makes it read
        as audibly distinct from `bendSkew` rather than a redundant duplicate.

        At `morph == 0` every mode collapses to the exact same `std::sin (phase)` path —
        mode selection has zero effect until the amount knob is moved, so switching modes
        with `morph` at 0 is inaudible and non-breaking.

        Two extra internal-only phase accumulators, both hard-reset (with the main phase)
        on `noteOn()` for the same layering phase-stability reason:
          - `syncPhase` — Mode::sync only. A second accumulator running at
            `phaseIncrement x (1 + amount x kSyncMaxRatio)`; every time IT wraps, the
            main `phase` (the audible "slave") is hard-reset to 0 — classic hard sync.
          - `modPhase`  — Mode::fm / am / rm only. One shared fixed-ratio
            (`kModRatio` x body pitch) internal sine modulator; fm phase-modulates the
            carrier, am/rm differ only in whether the modulator is unipolar-shifted
            (am) or left bipolar (rm) before multiplying — see `renderSample()`.
        Mode::fmFromSample takes its modulator from `renderSample()`'s `externalMod`
        argument (the SamplePlayer layer's live output, fed in by KickVoice) instead of
        `modPhase` — same phase-modulation math as `fm`, different modulator source.
        A missing/disabled sample layer already renders 0, so this mode degrades
        gracefully to "no effect" with no special-casing needed here.

        All 8 modes stay analytically bounded (every formula below is built from
        `std::sin` / lerps of bounded signals, never an unbounded multiply), so `morph`
        at its "musical/tame ceiling" of 1.0 cannot produce a runaway regardless of mode.
    */
    class BodyOscillator
    {
    public:
        /** AudioParameterChoice index order — keep in sync with `kMorphModeNames` in
            ParameterLayout.h and the `morphMode` choice list. */
        enum Mode : int
        {
            bendSkew     = 0,   // existing CZ-style attack-only phase-skew (default)
            sync         = 1,   // hard sync to a faster internal rate
            fold         = 2,   // sine wavefolder
            fm           = 3,   // self-FM (internal fixed-ratio modulator)
            fmFromSample = 4,   // FM using the Sample layer as the modulator
            pd           = 5,   // Casio-CZ-style phase distortion, continuous (not attack-only)
            am           = 6,   // amplitude modulation (internal fixed-ratio modulator)
            rm           = 7,   // ring modulation (internal fixed-ratio modulator)
        };

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
            KickVoice::noteOn applies to bodyLevel and the gates). Also resets the
            Mode::sync / Mode::fm/am/rm side accumulators for the same reason. */
        void noteOn() noexcept
        {
            phase     = 0.0;
            syncPhase = 0.0;
            modPhase  = 0.0;
            shapeEnv  = 1.0f;
            morphSm.setCurrentAndTargetValue (morphSm.getTargetValue());
        }

        /** Per-sample safe: recomputes the phase increment for `hz`. */
        void setFrequency (float hz) noexcept;

        /** Per-block: morph amount 0 (pure sine) .. 1 (max effect for the selected mode).
            Smoothed per-sample over kMorphSmoothSeconds (bug-scan 2026-09-01: an instant
            automation jump mid-cycle switched shape at a point where they differ by up to
            full scale — a ~7x slew spike, i.e. a click. Same treatment as every other
            per-block level/shape control in the engine). */
        void setMorph (float morph01) noexcept { morphSm.setTargetValue (juce::jlimit (0.0f, 1.0f, morph01)); }

        /** Per-block: which of the 8 warp modes `morph` currently drives. Not smoothed —
            same convention as every other Choice parameter in the engine (noiseType,
            filterType, tuneMode read directly, no crossfade); mode changes are a patch
            decision, not something automated at audio rate. */
        void setMorphMode (int mode) noexcept { morphMode = juce::jlimit ((int) bendSkew, (int) rm, mode); }

        /** `externalMod` is the Sample layer's current sample value, ONLY consulted by
            Mode::fmFromSample (every other mode ignores it) — see KickVoice::renderStereo,
            which renders the sample layer first each sample so this can be threaded in. */
        float renderSample (float externalMod = 0.0f) noexcept;

        static constexpr float  kMaxSkew           = 0.90f;   // knob at 100% -> d = 0.05 (never 0)
        static constexpr double kShapeDecaySeconds = 0.040;   // attack-only: skew x e^(-t / 40 ms)
        static constexpr double kMorphSmoothSeconds = 0.010;

        // Per-mode "musical/tame ceiling" tuning constants (amount=1 lands here, not at
        // a textbook-extreme value) — see plugins/KICKR/.ideas/improvements/
        // morph-warp-modes-research.md for the reasoning behind each mode's algorithm.
        static constexpr double kSyncMaxRatio = 5.0;    // sync: amount=1 -> ~6:1 master:slave
        static constexpr float  kFoldGainMax  = 5.0f;   // fold: amount=1 -> gain 6 in sin(gain*x)
        static constexpr float  kFMIndexMax   = 3.0f;   // fm / fmFromSample: amount=1 -> +-3 rad phase-mod
        // fm/am/rm internal modulator, x body pitch. DELIBERATELY NOT an integer (2026-09-03
        // fix, user report "AM and RM don't work or effect too subtle"): an exact 2:1 ratio
        // is harmonically LOCKED to the carrier, so AM/RM just reinforce an existing member
        // of the harmonic series — the classic "detuned/metallic/alien" RM character (and
        // AM's tremolo character) needs an INHARMONIC ratio to produce sidebands that clash
        // with the carrier's own harmonics. Same fixed value used by fm/am/rm alike.
        static constexpr double kModRatio     = 2.71;

    private:
        double fsOversampled { 44100.0 };
        double phase         { 0.0 };   // radians, wrapped to [0, 2pi) — the audible oscillator
        double phaseIncrement { 0.0 };
        float  frequencyHz   { 55.0f };

        // Mode::sync — second accumulator; wrapping it hard-resets `phase` (see renderSample()).
        double syncPhase { 0.0 };
        // Mode::fm / am / rm — shared internal fixed-ratio modulator oscillator.
        double modPhase          { 0.0 };
        double modPhaseIncrement { 0.0 };

        // Attack-only shape envelope: 1.0 at noteOn, x shapeCoef per oversampled sample.
        float  shapeEnv  { 0.0f };
        float  shapeCoef { 0.0f };

        int morphMode { bendSkew };

        // 0 = pure sine (default — renderSample takes the exact original std::sin path
        // whenever the smoothed value is 0, so pre-morph output is bit-identical).
        juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> morphSm { 0.0f };
    };
}
