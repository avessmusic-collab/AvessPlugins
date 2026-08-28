#include "DSP/KickVoice.h"
#include "Utilities/DSPUtils.h"

namespace kickr
{
    void KickVoice::prepare (double fsOS) noexcept
    {
        fsOversampled = juce::jmax (1.0, fsOS);

        body.prepare (fsOversampled);
        ampEnv.prepare (fsOversampled);
        pitchEnv.prepare (fsOversampled);

        bodyLevel.reset (fsOversampled, 0.02);   // 20 ms — click-free level changes
        bodyLevel.setCurrentAndTargetValue (bodyLevel.getTargetValue());

        reset();
    }

    void KickVoice::reset() noexcept
    {
        active = false;
        body.reset();
        ampEnv.reset();
        pitchEnv.reset();
        bodyLevel.setCurrentAndTargetValue (bodyLevel.getTargetValue());
    }

    void KickVoice::setBodyLevel (float level01) noexcept
    {
        bodyLevel.setTargetValue (juce::jlimit (0.0f, 1.0f, level01));
    }

    void KickVoice::setPitchParams (float startRatioEff, float timeMs, float curve) noexcept
    {
        pitchEnv.setParams (startRatioEff, timeMs, curve);
    }

    void KickVoice::noteOn (float freqHz, int noteNumber, float velLevelGain,
                            float bodyDecayMs, int sampleOffset) noexcept
    {
        juce::ignoreUnused (noteNumber, sampleOffset);   // PHASE 2.7b uses noteNumber

        baseFrequencyHz = freqHz;
        velLevel        = velLevelGain;

        body.setFrequency (freqHz);
        body.noteOn();                 // phase -> 0 (layering phase-stability)
        pitchEnv.noteOn();             // arm the ratio-domain contour (tSinceTrigger -> 0)
        ampEnv.noteOn (bodyDecayMs);

        // Start at the current body-level target — no 20 ms fade-in on the first hit.
        // (Retrigger level continuity is Phase 2.3's crossfade concern.)
        bodyLevel.setCurrentAndTargetValue (bodyLevel.getTargetValue());

        active = true;
    }

    void KickVoice::renderAdd (juce::dsp::AudioBlock<float>& block,
                               int startSample, int numSamples) noexcept
    {
        if (! active || numSamples <= 0)
            return;

        const int numCh = static_cast<int> (block.getNumChannels());

        for (int i = 0; i < numSamples; ++i)
        {
            // Ratio-domain pitch drop, phase-continuous (frequency only — BodyOscillator
            // integrates phase and is never reset mid-fall).
            const float freqHz = pitchEnv.nextFrequency (baseFrequencyHz);
            body.setFrequency (freqHz);

            const float env = ampEnv.tick();
            const float osc = body.renderSample();
            const float lvl = bodyLevel.getNextValue();

            const float s = dsputils::sanitize (osc * env * lvl * velLevel);

            const int idx = startSample + i;
            for (int ch = 0; ch < numCh; ++ch)
                block.addSample (ch, idx, s);

            if (! ampEnv.isActive())
            {
                active = false;
                break;
            }
        }
    }
}
