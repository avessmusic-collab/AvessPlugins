#pragma once

#include <atomic>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>

#include "DSP/BodyOscillator.h"
#include "DSP/AmplitudeEnvelope.h"
#include "DSP/PitchEnvelope.h"
#include "DSP/ClickGenerator.h"
#include "DSP/SubOscillator.h"
#include "DSP/TailGenerator.h"
#include "DSP/NoiseGenerator.h"
#include "DSP/SamplePlayer.h"
#include "Sampling/SampleBuffer.h"

namespace kickr
{
    /**
        One triggered kick (architecture.md -> KickVoice).

        PHASE 2.1: aggregates BodyOscillator + AmplitudeEnvelope. PHASE 2.2: PitchEnvelope
        now drives per-sample body frequency (ratio-domain snap+settle, phase-continuous).
        Monophonic; `noteOff()` is ignored (the kick runs to completion).
        Renders the body layer x `bodyLevel` x velocity-scaled level.

        PHASE 2.3: KickEngine owns `std::array<KickVoice, 2>` and equal-power crossfades
        between them on retrigger, so it renders each voice into its own scratch buffer
        (via `renderStereo` since Phase 2.9) and mixes with the fade gains.

        PHASE 2.4: aggregates a per-voice `ClickGenerator` (3-part synthesised click).
        The click is summed with the body BEFORE the voice's mono output — it carries its
        own `clickLevel` + click-velocity gain internally, so it is NOT scaled by
        `bodyLevel` (which now applies to the body layer only). The whole voice is then
        scaled by the strong per-voice velocity level.

        PHASE 2.5: aggregates a per-voice `SubOscillator` (independent mono sub sine, fixed
        `subFreq`, phase-0 on trigger, own exp AD envelope). Summed with body + click
        BEFORE the voice's mono output — it carries `subLevel` internally, so it is NOT
        scaled by `bodyLevel`. NOT velocity-scaled in v1 (only the whole-voice `velLevel`
        applies).

        PHASE 2.6: aggregates a per-voice `TailGenerator` (dedicated LF sine locked to the
        resolved `fundamentalEff` the body got — NO pitch envelope, NOT a body-bus tap —
        with a slow ~10 ms attack so it sits behind the transient, exp decay = `tailLength`,
        a `StateVariableTPTFilter` low-pass driven by `tailTone`, and a `tanh` `tailDrive`
        shaper — the whole layer in-region, AD-10). Summed with body + click + sub BEFORE
        the voice's mono output; carries `tailLevel` internally, so it is NOT scaled by
        `bodyLevel`. NOT velocity-scaled. The voice frees only when the body amp env AND the
        click AND the sub AND the tail are all done.

        PHASE 2.7: aggregates a per-voice `NoiseGenerator` (optional white/pink/filtered
        noise layer, DEFAULT OFF — `noiseLevel` defaults to 0.0 so it is exactly silent and
        `noteOn` no-ops). Summed with body + click + sub + tail BEFORE the voice's mono
        output; carries `noiseLevel` internally, so it is NOT scaled by `bodyLevel`. NOT
        velocity-scaled. Factored into the voice-free check alongside ampEnv / click / sub /
        tail.

        PHASE 2.9: the voice is now STEREO (`renderStereo(left, right, n)`). Sub, Tail,
        Noise and the Body *fundamental* stay mono (same sample to L/R). The Body path
        gets a first-order all-pass decorrelator on the R channel only, blended by
        `bodyWidth` (`r = lerp(m, allpass(m), bodyWidth)`, `g = 0.7`) — mostly affects
        `bodyHarmonics` content; the 55 Hz fundamental is below the audible-decorrelation
        range. The `ClickGenerator` becomes stereo via `clickWidth` (2-stream noise
        decorrelation + <1 ms osc/impulse inter-channel delay). `bodyWidth = 0`,
        `clickWidth = 0` -> `L == R` (mono). The sample layer stays mono for v1.

        PHASE 2.7b: aggregates a per-voice `SamplePlayer` (the 6th layer — the user's own
        recorded kick from the managed bank; AD-11). Its `const SampleBuffer*` is captured
        once at `noteOn` (held for the voice's life) so a mid-note bank swap can't affect a
        ringing voice. Two smoothed 0/1 gates (~5 ms, click-free toggling):
          - `synthGate` multiplies the 5 synth layers (body/sub/click/tail/noise),
          - `sampleGate` multiplies the SamplePlayer layer.
        `synthEnable == false` -> synthGate -> 0 ; `sampleEnable == false` (or no sample
        loaded) -> sampleGate -> 0 ; both on -> blended. The SamplePlayer carries its own
        `sampleLevel x velFactor` internally.
        Voice lifetime (fixed 2026-08-31 — was synth-envelopes-only, which killed the voice
        and force-reset the sample mid-playback as soon as the 5 SYNTH envelopes finished,
        regardless of `synthEnable`/whether the sample was still actively playing): the
        voice frees only once the 5 synth envelopes AND `sample.isActive()` are all done.
        `sample.isActive()` is already false immediately for a disabled/unloaded sample
        layer, so a synth-only voice frees exactly as before.
        `bodyHarmonics` lands in Phase 2.8+.
    */
    class KickVoice
    {
    public:
        KickVoice() = default;

        void prepare (double newFsOversampled) noexcept;
        void reset() noexcept;

        /** PHASE 2.10 — OS factor changed mid-voice: fan `updateOversampledRate` out to
            every layer member (coefficient-only — a ringing voice keeps going, only its
            rate-dependent coefficients change) and re-rate the smoothed gains/gates. */
        void updateOversampledRate (double newFsOversampled) noexcept;

        /** Per-block: set the target body level (0..1). Lightly smoothed to avoid clicks. */
        void setBodyLevel (float level01) noexcept;

        /**
            Per-block: refresh the pitch-envelope contour coefficients.
            `startRatioEff` is the velocity-scaled effective `pitchStart` (>= 1), computed
            by KickEngine; `timeMs` = `pitchTime`, `curve` = `pitchCurve`.
        */
        void setPitchParams (float startRatioEff, float timeMs, float curve) noexcept;

        /**
            Per-block: forward the CLICK-group snapshot to the ClickGenerator
            (`clickLevel`, `clickTone` Hz, `clickTime` ms, `clickPitch` Hz, `clickWidth`).
            No APVTS reads inside the voice.
        */
        void setClickParams (float clickLevel, float clickToneHz,
                             float clickTimeMs, float clickPitchHz,
                             float clickWidth01) noexcept;

        /** PHASE 2.9 — per-block: body-path stereo decorrelation amount (`bodyWidth` 0..1). */
        void setBodyWidth (float bodyWidth01) noexcept;

        /**
            Per-block: forward the SUB-group snapshot to the SubOscillator
            (`subLevel` 0..1, `subFreq` Hz, `subDecay` ms). No APVTS reads inside the voice.
        */
        void setSubParams (float subLevel, float subFreqHz, float subDecayMs) noexcept;

        /**
            Per-block: forward the TAIL-group snapshot to the TailGenerator
            (`tailLevel` 0..1, `tailLength` ms, `tailTone` 0..1, `tailDrive` 0..1).
            No APVTS reads inside the voice.
            // Phase 2.11: tail* args are the macroTail-offset effective values.
        */
        void setTailParams (float tailLevel, float tailLengthMs,
                            float tailTone01, float tailDrive01) noexcept;

        /**
            Per-block: forward the NOISE-group snapshot to the NoiseGenerator
            (`noiseLevel` 0..1, `noiseDecay` ms, `noiseTone` 0..1, `noiseType` 0..2).
            No APVTS reads inside the voice. Default `noiseLevel` 0.0 => the layer is off.
        */
        void setNoiseParams (float noiseLevel, float noiseDecayMs,
                             float noiseTone01, int noiseType) noexcept;

        /**
            Per-block: forward the SAMPLE-group snapshot to the SamplePlayer + set the two
            layer gate targets (0/1, ~5 ms smoothed). No APVTS reads inside the voice.
            `synthGate01` = `synthEnable`; `sampleGate01` = `sampleEnable && sample loaded`.
        */
        void setSampleParams (const SamplePlayer::SampleParams& p,
                              float synthGate01, float sampleGate01) noexcept;

        /**
            Trigger. `velLevelGain` is the pre-resolved strong velocity level multiplier
            (`lerp(1, vel/127, velSensitivity)`) applied to the whole voice; `velClickGain`
            is the moderate click-only factor (`lerp(1, vel/127, velSensitivity·0.6)`).
            `bodyDecayMs` is the current Body Decay. `sampleBuf` is the sample buffer
            (nullptr = no sample) captured for the voice's life; `sampleVelFactor` scales
            the sample layer level. `sampleOffset` is informational (the engine already
            split the block).
        */
        void noteOn (float freqHz, int noteNumber, float velLevelGain, float velClickGain,
                     float bodyDecayMs, const SampleBuffer* sampleBuf, float sampleVelFactor,
                     int sampleOffset) noexcept;

        void noteOff() noexcept {}   // ignored — kick runs to completion

        /**
            OVERWRITE `left[0 .. numSamples)` and `right[0 .. numSamples)` with this
            voice's stereo output. Writes exact zeros when the voice is (or becomes)
            inactive, so the caller can always read the full span. Used by KickEngine's
            retrigger crossfade. Left channel is bit-identical to the pre-2.9 mono path
            when `bodyWidth == 0` and the click width leaves the base stream on L.
        */
        void renderStereo (float* left, float* right, int numSamples) noexcept;

        bool isActive() const noexcept { return active; }

    private:
        double fsOversampled { 44100.0 };

        // Fix (2026-08-31): was a plain bool, written by the audio thread and read
        // cross-thread by PluginProcessor::retireUnreferenced() (message thread, via
        // KickEngine::anyVoiceActive()) with no synchronisation whatsoever — a data race
        // (UB in the C++ memory model) that gates whether a retired SampleBuffer is safe
        // to free. A stale/never-visible read could let the message thread free a buffer
        // this voice's SamplePlayer still holds a raw pointer into. std::atomic (default
        // seq_cst on this single flag, touched a few times per block/note, never per
        // sample — no measurable cost) makes every write visible and well-defined.
        std::atomic<bool> active { false };

        float  baseFrequencyHz { 55.0f };
        float  velLevel        { 1.0f };

        // PHASE 2.9 — body-path R-channel all-pass decorrelator (blended by bodyWidth).
        static constexpr float kAllpassG { 0.7f };
        float  bodyWidthAmt { 0.0f };
        float  apX1         { 0.0f };
        float  apY1         { 0.0f };

        BodyOscillator            body;
        AmplitudeEnvelope         ampEnv;
        PitchEnvelope             pitchEnv;   // PHASE 2.2 — ratio-domain pitch drop
        ClickGenerator            click;      // PHASE 2.4 — 3-part synthesised click
        SubOscillator             sub;        // PHASE 2.5 — independent mono sub sine
        TailGenerator             tail;       // PHASE 2.6 — dedicated LF tail / rumble
        NoiseGenerator            noise;      // PHASE 2.7 — optional white/pink/filtered noise
        SamplePlayer              sample;     // PHASE 2.7b — the 6th layer (user sample)
        juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> bodyLevel;
        juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> synthGate;   // PHASE 2.7b
        juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> sampleGate;  // PHASE 2.7b
    };
}
