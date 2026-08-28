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
        sub.prepare (fsOversampled);
        tail.prepare (fsOversampled);
        noise.prepare (fsOversampled);
        sample.prepare (fsOversampled);

        bodyLevel.reset (fsOversampled, 0.02);   // 20 ms — click-free level changes
        bodyLevel.setCurrentAndTargetValue (bodyLevel.getTargetValue());

        // PHASE 2.7b — ~5 ms so toggling synthEnable / sampleEnable mid-tail is click-free.
        synthGate.reset  (fsOversampled, 0.005);
        sampleGate.reset (fsOversampled, 0.005);
        synthGate.setCurrentAndTargetValue  (synthGate.getTargetValue());
        sampleGate.setCurrentAndTargetValue (sampleGate.getTargetValue());

        reset();
    }

    void KickVoice::reset() noexcept
    {
        active = false;
        body.reset();
        ampEnv.reset();
        pitchEnv.reset();
        click.reset();
        sub.reset();
        tail.reset();
        noise.reset();
        sample.reset();
        bodyLevel.setCurrentAndTargetValue (bodyLevel.getTargetValue());
        synthGate.setCurrentAndTargetValue  (synthGate.getTargetValue());
        sampleGate.setCurrentAndTargetValue (sampleGate.getTargetValue());
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

    void KickVoice::setSubParams (float subLevel, float subFreqHz, float subDecayMs) noexcept
    {
        sub.setParams (subLevel, subFreqHz, subDecayMs);
    }

    void KickVoice::setTailParams (float tailLevel, float tailLengthMs,
                                   float tailTone01, float tailDrive01) noexcept
    {
        tail.setParams (tailLevel, tailLengthMs, tailTone01, tailDrive01);
    }

    void KickVoice::setNoiseParams (float noiseLevel, float noiseDecayMs,
                                    float noiseTone01, int noiseType) noexcept
    {
        noise.setParams (noiseLevel, noiseDecayMs, noiseTone01, noiseType);
    }

    void KickVoice::setSampleParams (const SamplePlayer::SampleParams& p,
                                     float synthGate01, float sampleGate01) noexcept
    {
        sample.setParams (p);
        synthGate.setTargetValue  (juce::jlimit (0.0f, 1.0f, synthGate01));
        sampleGate.setTargetValue (juce::jlimit (0.0f, 1.0f, sampleGate01));
    }

    void KickVoice::noteOn (float freqHz, int noteNumber, float velLevelGain, float velClickGain,
                            float bodyDecayMs, const SampleBuffer* sampleBuf, float sampleVelFactor,
                            int sampleOffset) noexcept
    {
        juce::ignoreUnused (sampleOffset);

        baseFrequencyHz = freqHz;
        velLevel        = velLevelGain;

        body.setFrequency (freqHz);
        body.noteOn();                 // phase -> 0 (layering phase-stability)
        pitchEnv.noteOn();             // arm the ratio-domain contour (tSinceTrigger -> 0)
        ampEnv.noteOn (bodyDecayMs);
        click.noteOn (velClickGain);   // arm the 3-part click (uses the per-block snapshot)
        sub.noteOn();                  // phase -> 0, arm the sub AD env (per-block snapshot)
        tail.noteOn (freqHz);          // dedicated LF sine locked to fundamentalEff (pre pitch-env)
        noise.noteOn();                // re-seed rng + arm the noise AD env (no-op when noiseLevel 0)
        sample.noteOn (sampleBuf, noteNumber, sampleVelFactor);   // capture the buffer for the voice's life

        // Start at the current body-level / gate targets — no fade-in on the first hit.
        // (Retrigger level continuity is Phase 2.3's crossfade concern.)
        bodyLevel.setCurrentAndTargetValue (bodyLevel.getTargetValue());
        synthGate.setCurrentAndTargetValue  (synthGate.getTargetValue());
        sampleGate.setCurrentAndTargetValue (sampleGate.getTargetValue());

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
            const float subOut   = sub.renderSample();      // carries subLevel (mono, not vel-scaled)
            const float tailOut  = tail.renderSample();     // carries tailLevel (mono, not vel-scaled)
            const float noiseOut = noise.renderSample();    // carries noiseLevel (mono, not vel-scaled)

            // PHASE 2.7b — smoothed 0/1 layer gates + the sample layer (carries sampleLevel x velFactor).
            const float synthG   = synthGate.getNextValue();
            const float sampleG  = sampleGate.getNextValue();
            const float synthSum = bodyOut + clickOut + subOut + tailOut + noiseOut;
            const float sampleOut = sample.renderSample();

            const float s = dsputils::sanitize (synthG * synthSum * velLevel + sampleG * sampleOut);

            const int idx = startSample + i;
            for (int ch = 0; ch < numCh; ++ch)
                block.addSample (ch, idx, s);

            if (! ampEnv.isActive() && ! click.isActive() && ! sub.isActive()
                && ! tail.isActive() && ! noise.isActive())
            {
                active = false;
                sample.reset();   // PHASE 2.7b — drop the buffer pointer so it can be retired
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
            const float subOut   = sub.renderSample();      // carries subLevel (mono, not vel-scaled)
            const float tailOut  = tail.renderSample();     // carries tailLevel (mono, not vel-scaled)
            const float noiseOut = noise.renderSample();    // carries noiseLevel (mono, not vel-scaled)

            // PHASE 2.7b — smoothed 0/1 layer gates + the sample layer (carries sampleLevel x velFactor).
            const float synthG    = synthGate.getNextValue();
            const float sampleG   = sampleGate.getNextValue();
            const float synthSum  = bodyOut + clickOut + subOut + tailOut + noiseOut;
            const float sampleOut = sample.renderSample();

            mono[i] = dsputils::sanitize (synthG * synthSum * velLevel + sampleG * sampleOut);

            if (! ampEnv.isActive() && ! click.isActive() && ! sub.isActive()
                && ! tail.isActive() && ! noise.isActive())
            {
                active = false;
                sample.reset();   // PHASE 2.7b — drop the buffer pointer so it can be retired
                ++i;
                break;
            }
        }

        for (; i < numSamples; ++i)
            mono[i] = 0.0f;
    }
}
