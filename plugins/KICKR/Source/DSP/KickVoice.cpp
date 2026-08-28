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
        click.prepare (fsOversampled);

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
        click.reset();
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

    void KickVoice::setClickParams (float clickLevel, float clickToneHz,
                                    float clickTimeMs, float clickPitchHz) noexcept
    {
        click.setParams (clickLevel, clickToneHz, clickTimeMs, clickPitchHz);
    }

    void KickVoice::noteOn (float freqHz, int noteNumber, float velLevelGain, float velClickGain,
                            float bodyDecayMs, int sampleOffset) noexcept
    {
        juce::ignoreUnused (noteNumber, sampleOffset);   // PHASE 2.7b uses noteNumber

        baseFrequencyHz = freqHz;
        velLevel        = velLevelGain;

        body.setFrequency (freqHz);
        body.noteOn();                 // phase -> 0 (layering phase-stability)
        pitchEnv.noteOn();             // arm the ratio-domain contour (tSinceTrigger -> 0)
        ampEnv.noteOn (bodyDecayMs);
        click.noteOn (velClickGain);   // arm the 3-part click (uses the per-block snapshot)

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
            float bodyOut = 0.0f;
            if (ampEnv.isActive())
            {
                // Ratio-domain pitch drop, phase-continuous (frequency only —
                // BodyOscillator integrates phase and is never reset mid-fall).
                const float freqHz = pitchEnv.nextFrequency (baseFrequencyHz);
                body.setFrequency (freqHz);

                const float env = ampEnv.tick();
                const float osc = body.renderSample();
                const float lvl = bodyLevel.getNextValue();
                bodyOut = osc * env * lvl;                 // body-level -> body only
            }

            const float clickOut = click.renderSample();   // carries clickLevel + click vel

            const float s = dsputils::sanitize ((bodyOut + clickOut) * velLevel);

            const int idx = startSample + i;
            for (int ch = 0; ch < numCh; ++ch)
                block.addSample (ch, idx, s);

            if (! ampEnv.isActive() && ! click.isActive())
            {
                active = false;
                break;
            }
        }
    }

    void KickVoice::renderMono (float* mono, int numSamples) noexcept
    {
        if (mono == nullptr || numSamples <= 0)
            return;

        if (! active)
        {
            juce::FloatVectorOperations::clear (mono, numSamples);
            return;
        }

        int i = 0;
        for (; i < numSamples; ++i)
        {
            float bodyOut = 0.0f;
            if (ampEnv.isActive())
            {
                // Ratio-domain pitch drop, phase-continuous (frequency only).
                const float freqHz = pitchEnv.nextFrequency (baseFrequencyHz);
                body.setFrequency (freqHz);

                const float env = ampEnv.tick();
                const float osc = body.renderSample();
                const float lvl = bodyLevel.getNextValue();
                bodyOut = osc * env * lvl;                 // body-level -> body only
            }

            const float clickOut = click.renderSample();   // carries clickLevel + click vel

            mono[i] = dsputils::sanitize ((bodyOut + clickOut) * velLevel);

            if (! ampEnv.isActive() && ! click.isActive())
            {
                active = false;
                ++i;
                break;
            }
        }

        for (; i < numSamples; ++i)
            mono[i] = 0.0f;
    }
}
