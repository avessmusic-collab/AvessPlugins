#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>

#include "DSP/BodyOscillator.h"
#include "DSP/AmplitudeEnvelope.h"
#include "DSP/PitchEnvelope.h"
#include "DSP/ClickGenerator.h"
#include "DSP/SubOscillator.h"
#include "DSP/TailGenerator.h"
#include "DSP/NoiseGenerator.h"

namespace kickr
{
    /**
        One triggered kick (architecture.md -> KickVoice).

        PHASE 2.1: aggregates BodyOscillator + AmplitudeEnvelope. PHASE 2.2: PitchEnvelope
        now drives per-sample body frequency (ratio-domain snap+settle, phase-continuous).
        Monophonic; `noteOff()` is ignored (the kick runs to completion).
        Renders the body layer x `bodyLevel` x velocity-scaled level.

        PHASE 2.3: KickEngine owns `std::array<KickVoice, 2>` and equal-power crossfades
        between them on retrigger, so it renders each voice into its own mono scratch
        buffer via `renderMono` and mixes with the fade gains. `renderAdd` (Phase 2.1
        stereo-block path) is kept for compatibility.

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
        tail. SamplePlayer + `bodyHarmonics` land in later phases.
    */
    class KickVoice
    {
    public:
        KickVoice() = default;

        void prepare (double newFsOversampled) noexcept;
        void reset() noexcept;

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
            (`clickLevel`, `clickTone` Hz, `clickTime` ms, `clickPitch` Hz).
            No APVTS reads inside the voice.
        */
        void setClickParams (float clickLevel, float clickToneHz,
                             float clickTimeMs, float clickPitchHz) noexcept;

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
            Trigger. `velLevelGain` is the pre-resolved strong velocity level multiplier
            (`lerp(1, vel/127, velSensitivity)`) applied to the whole voice; `velClickGain`
            is the moderate click-only factor (`lerp(1, vel/127, velSensitivity·0.6)`).
            `bodyDecayMs` is the current Body Decay. `sampleOffset` is informational for
            now (the engine already split the block).
        */
        void noteOn (float freqHz, int noteNumber, float velLevelGain, float velClickGain,
                     float bodyDecayMs, int sampleOffset) noexcept;

        void noteOff() noexcept {}   // ignored — kick runs to completion

        /** Add this voice's output into `block[startSample .. startSample+numSamples)`. */
        void renderAdd (juce::dsp::AudioBlock<float>& block, int startSample, int numSamples) noexcept;

        /**
            OVERWRITE `mono[0 .. numSamples)` with this voice's mono output.
            Writes exact zeros when the voice is (or becomes) inactive, so the caller
            can always read the full span. Used by KickEngine's retrigger crossfade.
        */
        void renderMono (float* mono, int numSamples) noexcept;

        bool isActive() const noexcept { return active; }

    private:
        double fsOversampled { 44100.0 };
        bool   active        { false };

        float  baseFrequencyHz { 55.0f };
        float  velLevel        { 1.0f };

        BodyOscillator            body;
        AmplitudeEnvelope         ampEnv;
        PitchEnvelope             pitchEnv;   // PHASE 2.2 — ratio-domain pitch drop
        ClickGenerator            click;      // PHASE 2.4 — 3-part synthesised click
        SubOscillator             sub;        // PHASE 2.5 — independent mono sub sine
        TailGenerator             tail;       // PHASE 2.6 — dedicated LF tail / rumble
        NoiseGenerator            noise;      // PHASE 2.7 — optional white/pink/filtered noise
        juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> bodyLevel;
    };
}
