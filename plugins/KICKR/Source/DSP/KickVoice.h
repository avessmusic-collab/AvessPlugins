#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>

#include "DSP/BodyOscillator.h"
#include "DSP/AmplitudeEnvelope.h"
#include "DSP/PitchEnvelope.h"

namespace kickr
{
    /**
        One triggered kick (architecture.md -> KickVoice).

        PHASE 2.1: aggregates BodyOscillator + AmplitudeEnvelope (+ PitchEnvelope
        passthrough). Monophonic; `noteOff()` is ignored (the kick runs to completion).
        Renders the body layer x `bodyLevel` x velocity-scaled level and ADDS it into the
        oversampled work block (AD-10). Sub / Click / Tail / Noise / SamplePlayer +
        `bodyHarmonics` + the 2-voice retrigger crossfade land in later phases.
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
            Trigger. `velLevelGain` is the pre-resolved velocity level multiplier
            (`lerp(1, vel/127, velSensitivity)`), `bodyDecayMs` the current Body Decay.
            `sampleOffset` is informational for now (the engine already split the block).
        */
        void noteOn (float freqHz, int noteNumber, float velLevelGain,
                     float bodyDecayMs, int sampleOffset) noexcept;

        void noteOff() noexcept {}   // ignored — kick runs to completion

        /** Add this voice's output into `block[startSample .. startSample+numSamples)`. */
        void renderAdd (juce::dsp::AudioBlock<float>& block, int startSample, int numSamples) noexcept;

        bool isActive() const noexcept { return active; }

    private:
        double fsOversampled { 44100.0 };
        bool   active        { false };

        float  baseFrequencyHz { 55.0f };
        float  velLevel        { 1.0f };

        BodyOscillator            body;
        AmplitudeEnvelope         ampEnv;
        PitchEnvelope             pitchEnv;   // PHASE 2.2 — passthrough for now
        juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> bodyLevel;
    };
}
