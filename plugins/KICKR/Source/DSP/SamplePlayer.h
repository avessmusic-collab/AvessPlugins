#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>

#include "Sampling/SampleBuffer.h"

namespace kickr
{
    /**
        The 6th layer — one user sample played back per voice (architecture.md ->
        SamplePlayer; AD-10 / AD-11; Parameter Mapping rows 46-59).

        PHASE 2.7b. Per-voice, inside KickVoice, summed with the synth layers BEFORE the
        TransientShaper. Runs entirely at `fsOversampled` (in-region, AD-10) — it resamples
        the file straight to the oversampled rate and its `sampleCrush` nonlinearity is
        oversampled by construction.

        Reads a `const SampleBuffer*` it does NOT own (PluginProcessor owns it; the pointer
        is captured once at `noteOn` and held for the voice's life so a mid-note bank swap
        can't pull the buffer out). Never allocates, never copies the buffer, never locks.

        Per-sample signal path (architecture order):
          1. Resample  — fractional read position advanced by
             `ratio = (sourceRate / fsOversampled) * 2^((tune + fine/100 + midiOffset)/12)`
             (negative when reversed, starting from `sampleEnd`).
             `midiOffset = sampleMidiTrack ? (noteNumber - rootNote) : 0`.
             4-point (3rd-order) Lagrange interpolation; stereo files are averaged to mono
             for the voice accumulator.
          2. Trim      — read window clamped to `[start*N, end*N]`; ~1 ms raised-cosine
             fades at `noteOn` and at the window end. Past the end -> silence for the rest
             of the voice.
          3. AD env    — `sampleAttack` raised-cosine up, `sampleDecay` exp down.
          4. HP -> LP  — `juce::dsp::StateVariableTPTFilter<float>` x2 vs `fsOversampled`,
             bypassed at the range extremes (HP <= 20 Hz, LP >= 20000 Hz), Q ~0.7.
          5. Crush     — `bits = lerp(16, 4, crush)`, `q = 2^(bits-1)`, `y = round(x*q)/q`;
             + sample-and-hold decimation `hold = round(lerp(1, 16, crush))`.
             `crush = 0` -> exactly bit-transparent (both skipped).
          6. `x sampleLevel x velFactor`.

        `isActive()` — used by `KickVoice`'s internal voice-freeing check (fixed 2026-08-31:
        it used to be ignored there, so the synth envelopes finishing killed the whole
        voice — and force-`reset()` this player — out from under a still-playing sample).
        Also stops reading past the buffer / trim-window end regardless.

        RT-safe: no alloc / lock / log / IO. Interpolator is stateless-per-call; both SVFs
        are prepared in `prepare`. The AD env + both SVF states are denormal-flushed each
        sample; the output is sanitized.

        // Phase 2.11: `sampleLevel` also gains a `macroBody` offset (added upstream in
        //             KickEngine, like the other layers).
    */
    class SamplePlayer
    {
    public:
        struct SampleParams
        {
            float enable    { 0.0f };   // 0/1 gate — 0 => the layer does no work
            float level     { 0.7f };
            float start01   { 0.0f };
            float end01     { 1.0f };
            bool  reverse   { false };
            float tuneSemis { 0.0f };
            float fineCents { 0.0f };
            bool  midiTrack { true };
            float attackMs  { 0.0f };
            float decayMs   { 2200.0f };   // 2026-08-31: was 800 — see ParameterLayout.h note
            float hpHz      { 20.0f };    // 20 = off
            float lpHz      { 20000.0f }; // 20000 = off
            float crush01   { 0.0f };
        };

        static constexpr float kFloorGain = 1.0e-5f;

        void prepare (double fsOversampled) noexcept;
        void reset() noexcept;

        /** PHASE 2.10 — OS factor changed mid-note: recompute the resample ratio, decay
            coef, filter cutoffs and crush timing for the new rate (re-`prepare` the two
            SVFs — states clear, covered by the switch fade); rescale the attack/`sinceOn`
            counters. The source read position is in SOURCE samples (rate-independent) and
            is KEPT. Coefficient-only otherwise. */
        void updateOversampledRate (double newFsOversampled) noexcept;

        /** Per-block from KickEngine -> KickVoice. Pre-resolved effective values (velocity
            already folded into the caller's `lpHz`; `level` gets `velFactor` at render).
            No APVTS reads inside. */
        void setParams (const SampleParams& p) noexcept;

        /** Trigger. Captures the buffer for the voice's life. nullptr or a disabled layer
            -> silent (no work). `vel` scales the output level. */
        void noteOn (const SampleBuffer* newBuf, int noteNumber, float vel) noexcept;

        /** One mono sample. Exactly 0 when the layer is inactive / finished. */
        float renderSample() noexcept;

        /** 2026-09-03 (user request — "decouple fm modulation from sample velocity. i want
            to modulate with the sample volume at 0"): the value the LAST renderSample()
            produced BEFORE the final `x sampleLevel x velFactor` multiply — i.e. the
            sample's full processed character (trim/fades/AD env/HP/LP/crush) at unit
            level. KickVoice feeds this, not the audible post-level output, into
            BodyOscillator's Mode::fmFromSample modulator, so FM depth is set by the Morph
            knob alone: SAMPLE level 0 still modulates (silent sample, full warp), and
            velocity no longer changes the warp depth between hits. 0 whenever
            renderSample() last returned the inactive/finished/past-end silence path. */
        float lastRawSample() const noexcept { return lastRaw; }

        bool isActive() const noexcept { return active && ! finished; }

    private:
        void  updateDerived() noexcept;
        float sampleAt (int idx) const noexcept;
        float interpolate() noexcept;

        double fs           { 44100.0 };
        float  nyquistLimit { 19845.0f };
        int    fadeLen      { 44 };       // ~1 ms in samples

        SampleParams params;

        // Captured buffer (not owned).
        const SampleBuffer* buf { nullptr };
        int    nSrc      { 0 };
        int    nSrcCh    { 1 };
        double srcRate   { 44100.0 };
        int    rootNote  { 60 };   // C3 — overwritten from the buffer's own rootNote on noteOn
        int    noteNum   { 60 };
        float  velFactor { 1.0f };

        // Derived per block / on noteOn.
        double ratio     { 1.0 };
        int    startIdx  { 0 };
        int    endIdx    { 0 };
        bool   reverse   { false };
        float  decayCoef { 0.0f };

        bool   hpActive  { false };
        bool   lpActive  { false };

        bool   crushActive { false };
        float  crushQ      { 32768.0f };
        float  invCrushQ   { 1.0f / 32768.0f };
        int    crushHold   { 1 };

        // Running state.
        double readPos   { 0.0 };
        int    sinceOn   { 0 };
        float  env       { 0.0f };
        int    attackSamples { 0 };
        int    attackPos     { 0 };
        bool   attacking     { false };
        int    crushCounter  { 0 };
        float  crushHeld     { 0.0f };
        float  lastRaw       { 0.0f };   // 2026-09-03 — pre-level/velocity tap, see lastRawSample()
        bool   active    { false };
        bool   finished  { true };

        juce::dsp::StateVariableTPTFilter<float> hp;
        juce::dsp::StateVariableTPTFilter<float> lp;
    };
}
