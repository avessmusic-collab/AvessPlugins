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
        apX1 = 0.0f;
        apY1 = 0.0f;
        bodyLevel.setCurrentAndTargetValue (bodyLevel.getTargetValue());
        synthGate.setCurrentAndTargetValue  (synthGate.getTargetValue());
        sampleGate.setCurrentAndTargetValue (sampleGate.getTargetValue());
    }

    void KickVoice::updateOversampledRate (double newFsOversampled) noexcept
    {
        fsOversampled = juce::jmax (1.0, newFsOversampled);

        body.updateOversampledRate (fsOversampled);
        ampEnv.updateOversampledRate (fsOversampled);
        pitchEnv.updateOversampledRate (fsOversampled);
        click.updateOversampledRate (fsOversampled);
        sub.updateOversampledRate (fsOversampled);
        tail.updateOversampledRate (fsOversampled);
        noise.updateOversampledRate (fsOversampled);
        sample.updateOversampledRate (fsOversampled);

        // Re-rate the smoothed gains/gates (keeps their current value at target — they
        // are re-targeted every block by the engine anyway).
        bodyLevel.reset  (fsOversampled, 0.02);
        synthGate.reset  (fsOversampled, 0.005);
        sampleGate.reset (fsOversampled, 0.005);
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
                                    float clickTimeMs, float clickPitchHz,
                                    float clickWidth01) noexcept
    {
        click.setParams (clickLevel, clickToneHz, clickTimeMs, clickPitchHz, clickWidth01);
    }

    void KickVoice::setBodyWidth (float bodyWidth01) noexcept
    {
        bodyWidthAmt = juce::jlimit (0.0f, 1.0f, bodyWidth01);
    }

    void KickVoice::setMorph (float morph01) noexcept
    {
        body.setMorph (morph01);
    }

    void KickVoice::setMorphMode (int mode) noexcept
    {
        body.setMorphMode (mode);
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
        // (Retrigger level continuity is KickEngine's declick-fade concern.)
        bodyLevel.setCurrentAndTargetValue (bodyLevel.getTargetValue());
        synthGate.setCurrentAndTargetValue  (synthGate.getTargetValue());
        sampleGate.setCurrentAndTargetValue (sampleGate.getTargetValue());

        active = true;
    }

    void KickVoice::renderStereo (float* left, float* right, int numSamples) noexcept
    {
        if (left == nullptr || right == nullptr || numSamples <= 0)
            return;

        if (! active)
        {
            juce::FloatVectorOperations::clear (left,  numSamples);
            juce::FloatVectorOperations::clear (right, numSamples);
            return;
        }

        int i = 0;
        for (; i < numSamples; ++i)
        {
            // PHASE 2.7b — smoothed 0/1 layer gates + the sample layer (carries sampleLevel x velFactor).
            // 2026-09-02: rendered BEFORE the body so Mode::fmFromSample can use the
            // sample as its FM modulator (see BodyOscillator::renderSample).
            // sample.renderSample() must run exactly once per sample regardless of mode.
            // 2026-09-03 (user request — "decouple fm modulation from sample velocity.
            // i want to modulate with the sample volume at 0"): the modulator is now the
            // PRE-level/velocity tap (lastRawSample), NOT the audible post-level output —
            // SAMPLE level at 0 still warps at full depth, and velocity no longer changes
            // the warp between hits. It IS still scaled by the smoothed sampleGate, so
            // toggling SAMPLE off mid-note fades the modulation out click-free instead of
            // leaving an inaudible modulator warping the body (code-review CONFIRMED bug).
            const float synthG  = synthGate.getNextValue();
            const float sampleG = sampleGate.getNextValue();
            const float sampleM = sample.renderSample();                 // mono for v1 (audible layer)
            const float sampleMod = sample.lastRawSample() * sampleG;    // FM:Sample modulator

            float bodyM = 0.0f;
            if (ampEnv.isActive())
            {
                // Ratio-domain pitch drop, phase-continuous (frequency only).
                const float freqHz = pitchEnv.nextFrequency (baseFrequencyHz);
                body.setFrequency (freqHz);

                const float env = ampEnv.tick();
                const float osc = body.renderSample (sampleMod);   // only consulted by Mode::fmFromSample
                const float lvl = bodyLevel.getNextValue();
                bodyM = osc * env * lvl;                    // body-level -> body only
            }

            // PHASE 2.9 — first-order all-pass decorrelator on R, blended by bodyWidth.
            //   y = -g*x + x_{-1} + g*y_{-1}
            const float ap = -kAllpassG * bodyM + apX1 + kAllpassG * apY1;
            apX1 = bodyM;
            apY1 = ap;
            dsputils::flushDenormal (apY1);

            const float bodyL = bodyM;
            const float bodyR = bodyM + bodyWidthAmt * (ap - bodyM);   // lerp(m, allpass, width)

            float clickL = 0.0f;
            float clickR = 0.0f;
            click.renderStereo (clickL, clickR);           // carries clickLevel + click vel

            const float subM   = sub.renderSample();        // carries subLevel   (mono, not vel-scaled)
            const float tailM  = tail.renderSample();       // carries tailLevel  (mono, not vel-scaled)
            const float noiseM = noise.renderSample();      // carries noiseLevel (mono, not vel-scaled)

            const float monoLayers = subM + tailM + noiseM;
            const float L = synthG * (bodyL + clickL + monoLayers) * velLevel + sampleG * sampleM;
            const float R = synthG * (bodyR + clickR + monoLayers) * velLevel + sampleG * sampleM;

            left[i]  = dsputils::sanitize (L);
            right[i] = dsputils::sanitize (R);

            // Fix (2026-08-31, user report: "the sample doesn't play whole sometimes"):
            // this used to ignore the sample layer entirely, so as soon as the 5 SYNTH
            // envelopes finished (a few hundred ms by default — they run to completion
            // regardless of synthEnable/synthGate) the whole voice was killed and
            // sample.reset() forcibly cut off the sample mid-playback, even with
            // synthEnable off and a multi-second sample still actively rendering. Now the
            // voice only frees once the sample layer is ALSO done (sample.isActive() is
            // already false immediately when the layer is disabled or has no buffer, so
            // synth-only / no-sample voices free exactly as before).
            if (! ampEnv.isActive() && ! click.isActive() && ! sub.isActive()
                && ! tail.isActive() && ! noise.isActive() && ! sample.isActive())
            {
                active = false;
                sample.reset();   // PHASE 2.7b — drop the buffer pointer so it can be retired
                ++i;
                break;
            }
        }

        for (; i < numSamples; ++i)
        {
            left[i]  = 0.0f;
            right[i] = 0.0f;
        }
    }
}
