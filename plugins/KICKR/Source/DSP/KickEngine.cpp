#include "DSP/KickEngine.h"
#include "Parameters/ParameterIDs.h"

#include <cmath>

namespace kickr
{
    void KickEngine::prepare (double sampleRate, int maximumBlockSize)
    {
        baseSampleRate = juce::jmax (1.0, sampleRate);
        maxBlockSize   = juce::jmax (1, maximumBlockSize);

        // Cache raw parameter pointers FIRST — the OS setup below needs `pOversampling`.
        pFundamental    = apvts.getRawParameterValue (id::fundamental);
        pPitchStart     = apvts.getRawParameterValue (id::pitchStart);
        pPitchTime      = apvts.getRawParameterValue (id::pitchTime);
        pPitchCurve     = apvts.getRawParameterValue (id::pitchCurve);
        pBodyLevel      = apvts.getRawParameterValue (id::bodyLevel);
        pBodyDecay      = apvts.getRawParameterValue (id::bodyDecay);
        pTuneMode       = apvts.getRawParameterValue (id::tuneMode);
        pTune           = apvts.getRawParameterValue (id::tune);
        pFineTune       = apvts.getRawParameterValue (id::fineTune);
        pVelSensitivity = apvts.getRawParameterValue (id::velSensitivity);
        pOversampling   = apvts.getRawParameterValue (id::oversampling);
        pTransientAttack  = apvts.getRawParameterValue (id::transientAttack);
        pTransientSustain = apvts.getRawParameterValue (id::transientSustain);
        pDrive            = apvts.getRawParameterValue (id::drive);
        pCharacter        = apvts.getRawParameterValue (id::character);
        pDriveMix         = apvts.getRawParameterValue (id::driveMix);
        pLow              = apvts.getRawParameterValue (id::low);
        pMid              = apvts.getRawParameterValue (id::mid);
        pHigh             = apvts.getRawParameterValue (id::high);
        pBodyWidth        = apvts.getRawParameterValue (id::bodyWidth);
        pClickWidth       = apvts.getRawParameterValue (id::clickWidth);
        pOutputWidth      = apvts.getRawParameterValue (id::outputWidth);
        pOutput           = apvts.getRawParameterValue (id::output);
        pMix              = apvts.getRawParameterValue (id::mix);
        pLimiter          = apvts.getRawParameterValue (id::limiter);
        pClickLevel       = apvts.getRawParameterValue (id::clickLevel);
        pClickTone        = apvts.getRawParameterValue (id::clickTone);
        pClickTime        = apvts.getRawParameterValue (id::clickTime);
        pClickPitch       = apvts.getRawParameterValue (id::clickPitch);
        pSubLevel         = apvts.getRawParameterValue (id::subLevel);
        pSubFreq          = apvts.getRawParameterValue (id::subFreq);
        pSubDecay         = apvts.getRawParameterValue (id::subDecay);
        pTailLevel        = apvts.getRawParameterValue (id::tailLevel);
        pTailLength       = apvts.getRawParameterValue (id::tailLength);
        pTailTone         = apvts.getRawParameterValue (id::tailTone);
        pTailDrive        = apvts.getRawParameterValue (id::tailDrive);
        pNoiseLevel       = apvts.getRawParameterValue (id::noiseLevel);
        pNoiseDecay       = apvts.getRawParameterValue (id::noiseDecay);
        pNoiseTone        = apvts.getRawParameterValue (id::noiseTone);
        pNoiseType        = apvts.getRawParameterValue (id::noiseType);
        pSynthEnable      = apvts.getRawParameterValue (id::synthEnable);
        pSampleEnable     = apvts.getRawParameterValue (id::sampleEnable);
        pSampleLevel      = apvts.getRawParameterValue (id::sampleLevel);
        pSampleStart      = apvts.getRawParameterValue (id::sampleStart);
        pSampleEnd        = apvts.getRawParameterValue (id::sampleEnd);
        pSampleReverse    = apvts.getRawParameterValue (id::sampleReverse);
        pSampleTune       = apvts.getRawParameterValue (id::sampleTune);
        pSampleFine       = apvts.getRawParameterValue (id::sampleFine);
        pSampleMidiTrack  = apvts.getRawParameterValue (id::sampleMidiTrack);
        pSampleAttack     = apvts.getRawParameterValue (id::sampleAttack);
        pSampleDecay      = apvts.getRawParameterValue (id::sampleDecay);
        pSampleHP         = apvts.getRawParameterValue (id::sampleHP);
        pSampleLP         = apvts.getRawParameterValue (id::sampleLP);
        pSampleCrush      = apvts.getRawParameterValue (id::sampleCrush);

        pMacroPunch       = apvts.getRawParameterValue (id::macroPunch);
        pMacroBody        = apvts.getRawParameterValue (id::macroBody);
        pMacroCrush       = apvts.getRawParameterValue (id::macroCrush);
        pMacroTail        = apvts.getRawParameterValue (id::macroTail);

        const auto macroInit = [] (std::atomic<float>* p) noexcept
        { return p != nullptr ? p->load() : 0.5f; };
        // Fix (2026-08-31): these smoothers are only ticked (getNextValue()) ONCE per
        // processBlock() call — once per HOST BLOCK, not once per sample — but were
        // reset() against `baseSampleRate` (i.e. calibrated as if ticked once per
        // sample). That made the "~20 ms" ramp take ~20 ms worth of BLOCKS instead: at a
        // typical 512-sample block, ~10 seconds for a macro knob move to actually reach
        // its target, scaling with the host's buffer size. Calibrate against ticks/sec
        // (block rate) instead, so the ramp actually completes in ~20 ms of audio time.
        const double blockRateHz = baseSampleRate / static_cast<double> (maxBlockSize);
        for (auto* sm : { &macroPunchSm, &macroBodySm, &macroCrushSm, &macroTailSm })
            sm->reset (blockRateHz, 0.02);   // ~20 ms — click-free macro sweeps
        macroPunchSm.setCurrentAndTargetValue (macroInit (pMacroPunch));
        macroBodySm .setCurrentAndTargetValue (macroInit (pMacroBody));
        macroCrushSm.setCurrentAndTargetValue (macroInit (pMacroCrush));
        macroTailSm .setCurrentAndTargetValue (macroInit (pMacroTail));

        oversampling.prepare (baseSampleRate, maxBlockSize);

        // PHASE 2.10 — adopt the current `oversampling` choice immediately (not playing
        // yet, so no fade): the components below are then prepared at the right rate.
        if (pOversampling != nullptr)
            oversampling.setFactorChoice (static_cast<int> (pOversampling->load()));
        oversampling.applyPendingFactor();

        fsOversampled = oversampling.getOversampledRate();
        switchFadeInSamples = 0;

        for (auto& v : voices)
            v.prepare (fsOversampled);

        transientShaper.prepare (fsOversampled);
        waveshaperL.prepare (fsOversampled);     // PHASE 2.8/2.9 — coefficients vs fsOversampled
        waveshaperR.prepare (fsOversampled);
        outputStage.prepare (fsOversampled);     // PHASE 2.9 — tone / crossover / limiter vs fsOversampled

        // 1 / (fade length in oversampled samples). Recomputed on every OS factor change.
        retriggerThetaInc = 1.0 / juce::jmax (1.0, kRetriggerFadeMs * 0.001 * fsOversampled);

        scratch.setSize (OversamplingProcessor::kNumChannels, maxBlockSize, false, false, true);
        scratch.clear();

        // Mono per-voice render scratch @ the oversampled rate. Sized for the worst case
        // (max block x max OS factor) so Phase 2.10 needs no resize here.
        const int maxOsSamples = maxBlockSize * (1 << (OversamplingProcessor::kNumFactors - 1));
        for (auto& b : voiceScratch)
        {
            b.setSize (2, maxOsSamples, false, false, true);   // PHASE 2.9 — stereo per-voice scratch
            b.clear();
        }

        // (The DC blocker moved in-region into OutputStage at the Phase 2.9 checkpoint —
        //  it must run before the safety limiter, not after it.)

        reset();
    }

    void KickEngine::updateInRegionRate (double newFsOversampled)
    {
        fsOversampled = juce::jmax (1.0, newFsOversampled);

        for (auto& v : voices)
            v.updateOversampledRate (fsOversampled);

        transientShaper.updateOversampledRate (fsOversampled);
        waveshaperL.updateOversampledRate (fsOversampled);
        waveshaperR.updateOversampledRate (fsOversampled);
        outputStage.updateOversampledRate (fsOversampled);

        retriggerThetaInc = 1.0 / juce::jmax (1.0, kRetriggerFadeMs * 0.001 * fsOversampled);
    }

    void KickEngine::reset()
    {
        oversampling.reset();

        for (auto& v : voices)
            v.reset();

        transientShaper.reset();
        waveshaperL.reset();
        waveshaperR.reset();
        outputStage.reset();

        activeVoice        = 0;
        incomingVoice      = 1;
        fadeActive         = false;
        theta              = 0.0;
        retiringVoice      = -1;
        switchFadeInSamples = 0;

        macroPunchSm.setCurrentAndTargetValue (macroPunchSm.getTargetValue());
        macroBodySm .setCurrentAndTargetValue (macroBodySm .getTargetValue());
        macroCrushSm.setCurrentAndTargetValue (macroCrushSm.getTargetValue());
        macroTailSm .setCurrentAndTargetValue (macroTailSm .getTargetValue());

        scratch.clear();
        for (auto& b : voiceScratch)
            b.clear();
    }

    float KickEngine::resolvePitchHz (int noteNumber) const noexcept
    {
        float baseHz;

        if (snap.tuneMode == 0)   // MIDI Pitch
        {
            // kMidiPitchRecentreSemitones (-24 = 2 octaves): see the constant's doc —
            // recentres concert pitch so a "C3" trigger is a proper low kick, not 261 Hz.
            const auto noteHz = 440.0f * std::exp2 ((static_cast<float> (noteNumber - 69)
                                                     + kMidiPitchRecentreSemitones) / 12.0f);
            // AD-6: `fundamental` is a global semitone offset 12*log2(fundamental/55)
            //       -> multiplicative ratio (fundamental / 55).
            baseHz = noteHz * (snap.fundamental / 55.0f);
        }
        else                      // Fixed Frequency
        {
            baseHz = snap.fundamental;
        }

        const auto semis = snap.tune + snap.fineTune / 100.0f;
        const auto hz    = baseHz * dsputils::pitchRatio (semis);

        return juce::jlimit (10.0f, 5000.0f, hz);
    }

    float KickEngine::effectivePitchStartRatio (float v01) const noexcept
    {
        // architecture Parameter Mapping row 2 — velocity (subtle):
        //   effPitchStart = 1 + (pitchStart - 1) * lerp(1, 0.5 + 0.5*v01, velSensitivity * 0.35)
        const float v      = juce::jlimit (0.0f, 1.0f, v01);
        const float amt    = juce::jlimit (0.0f, 1.0f, snap.velSens * 0.35f);
        const float vScale = 1.0f + amt * ((0.5f + 0.5f * v) - 1.0f);   // lerp(1, target, amt)

        return juce::jmax (1.0f, 1.0f + (snap.pitchStartRatio - 1.0f) * vScale);
    }

    void KickEngine::triggerVoice (KickVoice& v, float freqHz, int note,
                                   float velLevelGain, float velClickGain, float v01) noexcept
    {
        // architecture MIDI routing — sample level velocity: lerp(1, v01, velSensitivity).
        const float sampleVelFactor = 1.0f + snap.velSens * (juce::jlimit (0.0f, 1.0f, v01) - 1.0f);

        v.noteOn (freqHz, note, velLevelGain, velClickGain, snap.bodyDecayMs,
                  currentSampleBuf, sampleVelFactor, 0);

        // Refresh the pitch contour with THIS note's velocity-scaled pitchStart so the
        // freshly-armed fall is correct from sample 0.
        v.setPitchParams (effectivePitchStartRatio (v01), snap.pitchTimeMs, snap.pitchCurve);
    }

    void KickEngine::handleNoteOn (const juce::MidiMessage& message)
    {
        const int   note = message.getNoteNumber();
        const float v01  = message.getFloatVelocity();          // velocity / 127

        const float freqHz = resolvePitchHz (note);

        // Velocity -> level (Phase 2.1): lerp(1, v01, velSensitivity)  — strong, whole voice.
        const float velLevelGain = 1.0f + snap.velSens * (v01 - 1.0f);
        // Velocity -> click (Phase 2.4): lerp(1, v01, velSensitivity·0.6) — moderate.
        const float velClickGain = 1.0f + snap.velSens * 0.6f * (v01 - 1.0f);

        lastVel01 = v01;

        // A third trigger mid-fade: snap the current crossfade to done first (free the
        // outgoing voice, promote the incoming one) so we never have > 2 voices live.
        if (fadeActive)
        {
            voices[static_cast<size_t> (activeVoice)].reset();
            activeVoice = incomingVoice;
            fadeActive  = false;
            theta       = 0.0;
        }

        auto& active = voices[static_cast<size_t> (activeVoice)];

        if (active.isActive())
        {
            // Retrigger while ringing -> start the other voice + a 3 ms equal-power
            // crossfade. The old voice keeps running phase-continuously; it is reset
            // only when theta reaches 1 (in renderSegment).
            incomingVoice = 1 - activeVoice;
            // If that slot is a "retiring" voice (its synth crossfade already finished but
            // its sample was still ringing), this new trigger legitimately steals it — only
            // 2 physical voice slots exist. Clear the tracker so it isn't double-rendered.
            if (incomingVoice == retiringVoice)
                retiringVoice = -1;
            auto& incoming = voices[static_cast<size_t> (incomingVoice)];
            incoming.reset();
            triggerVoice (incoming, freqHz, note, velLevelGain, velClickGain, v01);

            theta      = 0.0;
            fadeActive = true;
        }
        else
        {
            // Voice free -> just (re)trigger it, no fade.
            triggerVoice (active, freqHz, note, velLevelGain, velClickGain, v01);
        }
    }

    void KickEngine::renderSegment (juce::AudioBuffer<float>& buffer, int startSample, int numSamples)
    {
        if (numSamples <= 0)
            return;

        juce::dsp::AudioBlock<float> scratchBlock (scratch);
        auto seg = scratchBlock.getSubBlock (0, static_cast<size_t> (numSamples));
        seg.clear();

        // AD-10: up-sample a silent block -> render the whole voice @ fsOversampled ->
        // band-limit + decimate back to base rate. At 1x this is bit-transparent aside
        // from the intended DSP.
        auto up = oversampling.processSamplesUp (seg);

        const int upNum = static_cast<int> (up.getNumSamples());
        const int upCh  = static_cast<int> (up.getNumChannels());

        jassert (upNum <= voiceScratch[0].getNumSamples());

        // Render each live voice into its own STEREO scratch (Phase 2.9).
        float* aL = voiceScratch[0].getWritePointer (0);
        float* aR = voiceScratch[0].getWritePointer (1);
        float* bL = voiceScratch[1].getWritePointer (0);
        float* bR = voiceScratch[1].getWritePointer (1);

        const bool fading   = fadeActive;
        const bool retiring = (retiringVoice >= 0);   // mutually exclusive with `fading`
                                                       // (same "other" slot, see KickEngine.h)

        voices[static_cast<size_t> (activeVoice)].renderStereo (aL, aR, upNum);
        if (fading)
            voices[static_cast<size_t> (incomingVoice)].renderStereo (bL, bR, upNum);
        else if (retiring)
            voices[static_cast<size_t> (retiringVoice)].renderStereo (bL, bR, upNum);

        // Voice mix (equal-power crossfade during a retrigger) -> Tone (PRE-distortion) ->
        // stereo-linked TransientShaper -> per-channel Waveshaper -> OutputStage
        // (crossover / width / mix / gain / limiter). Everything in-region (AD-10).
        double th = theta;

        for (int i = 0; i < upNum; ++i)
        {
            float L = 0.0f;
            float R = 0.0f;

            if (fading)
            {
                // Only the OUTGOING voice fades — the incoming voice starts from a clean
                // silent state (fresh phase / fresh envelopes / a freshly-armed click) so
                // there is nothing to smooth on its side. Attenuating it with a ramping
                // gNew (the old equal-power scheme) swallowed the click's sharp transient
                // for the whole fade window, so a fast retrigger's click came out quiet or
                // silent while an isolated hit (no fade active) played it at full level —
                // an audible, tempo-locked inconsistency ("sometimes there's a click").
                float gOld = 1.0f, gNewUnused = 0.0f;
                dsputils::equalPowerGains (static_cast<float> (th), gOld, gNewUnused);
                L = gOld * aL[i] + bL[i];
                R = gOld * aR[i] + bR[i];

                th += retriggerThetaInc;
                if (th > 1.0)
                    th = 1.0;
            }
            else if (retiring)
            {
                // A voice past its synth crossfade but still sample-ringing (fix above) —
                // no fade shape needed, its own AD env / gates already govern audibility.
                L = aL[i] + bL[i];
                R = aR[i] + bR[i];
            }
            else
            {
                L = aL[i];
                R = aR[i];
            }

            // Macro offsets on transientAttack / drive / character / tone are added
            // upstream in processBlock via setParams (Phase 2.11 markers there).
            outputStage.processTone (L, R);                // 3-band, PRE-distortion
            transientShaper.processStereo (L, R);          // stereo-linked gain
            L = waveshaperL.processSample (L);             // per-channel 7-curve morph
            R = waveshaperR.processSample (R);
            outputStage.processOutput (L, R);             // crossover / width / mix / gain / limiter

            const float outL = dsputils::sanitize (L);
            const float outR = dsputils::sanitize (R);

            if (upCh > 1)
            {
                up.setSample (0, i, outL);
                up.setSample (1, i, outR);
            }
            else
            {
                up.setSample (0, i, 0.5f * (outL + outR));
            }
        }

        if (fading)
        {
            theta = th;

            if (theta >= 1.0)
            {
                // Fix (2026-08-31): the synth crossfade finishing doesn't mean the outgoing
                // voice is DONE — its sample can still be ringing (isActive() now covers
                // that, KickVoice fix above). Only reset here if it's genuinely finished;
                // otherwise demote it to `retiringVoice` so it keeps being rendered/summed
                // (unscaled, see above) until it naturally ends — or a 3rd trigger steals
                // the slot (handleNoteOn), which is the unavoidable 2-voice limit.
                const auto oldIdx = static_cast<size_t> (activeVoice);
                if (voices[oldIdx].isActive())
                    retiringVoice = activeVoice;
                else
                    voices[oldIdx].reset();

                activeVoice = incomingVoice;
                fadeActive  = false;
                theta       = 0.0;
            }
        }
        else if (retiring && ! voices[static_cast<size_t> (retiringVoice)].isActive())
        {
            voices[static_cast<size_t> (retiringVoice)].reset();
            retiringVoice = -1;
        }

        oversampling.processSamplesDown (seg);

        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        {
            const int srcCh = juce::jmin (ch, scratch.getNumChannels() - 1);
            buffer.copyFrom (ch, startSample, scratch, srcCh, 0, numSamples);
        }
    }

    void KickEngine::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
    {
        juce::ScopedNoDenormals noDenormals;

        const int numSamples = buffer.getNumSamples();
        buffer.clear();

        if (numSamples <= 0)
            return;

        // PHASE 2.10 — stash the requested OS factor (no swap here). The swap happens
        // below, after this block renders, under a ~64-sample fade to silence.
        if (pOversampling != nullptr)
            oversampling.setFactorChoice (static_cast<int> (pOversampling->load()));
        const bool osSwitchNow = oversampling.factorChangePending();

        // Snapshot the Phase-2.1 params once per block (atomic reads).
        auto load = [] (std::atomic<float>* p, float fallback) noexcept
        {
            return p != nullptr ? p->load() : fallback;
        };

        snap.fundamental     = load (pFundamental,    55.0f);
        snap.pitchStartRatio = load (pPitchStart,     4.0f);
        snap.pitchTimeMs     = load (pPitchTime,      50.0f);
        snap.pitchCurve      = load (pPitchCurve,     0.7f);
        snap.bodyLevel       = load (pBodyLevel,      1.0f);
        snap.bodyDecayMs     = load (pBodyDecay,      400.0f);
        snap.tune            = load (pTune,           0.0f);
        snap.fineTune        = load (pFineTune,       0.0f);
        snap.velSens         = load (pVelSensitivity, 0.5f);
        snap.transientAttack  = load (pTransientAttack,  0.0f);
        snap.transientSustain = load (pTransientSustain, 0.0f);
        snap.drive01     = load (pDrive,     0.3f);
        snap.character01 = load (pCharacter, 0.0f);
        snap.driveMix01  = load (pDriveMix,  1.0f);
        snap.lowDb        = load (pLow,  0.0f);          // PHASE 2.9 — TONE
        snap.midDb        = load (pMid,  0.0f);
        snap.highDb       = load (pHigh, 0.0f);
        snap.bodyWidth01  = load (pBodyWidth,   0.0f);   // PHASE 2.9 — STEREO
        snap.clickWidth01 = load (pClickWidth,  0.3f);
        snap.outputWidth01 = load (pOutputWidth, 0.5f);
        snap.outputDb     = load (pOutput, 0.0f);        // PHASE 2.9 — OUTPUT
        snap.mix01        = load (pMix,    1.0f);
        snap.limiterOn    = load (pLimiter, 1.0f) > 0.5f;
        snap.clickLevel      = load (pClickLevel, 0.4f);
        snap.clickToneHz     = load (pClickTone,  4000.0f);
        snap.clickTimeMs     = load (pClickTime,  3.0f);
        snap.clickPitchHz    = load (pClickPitch, 5000.0f);
        snap.subLevel        = load (pSubLevel, 0.5f);
        snap.subFreqHz       = load (pSubFreq,  40.0f);
        snap.subDecayMs      = load (pSubDecay, 300.0f);
        snap.tailLevel       = load (pTailLevel,  0.3f);
        snap.tailLengthMs    = load (pTailLength, 200.0f);
        snap.tailTone01      = load (pTailTone,   0.5f);
        snap.tailDrive01     = load (pTailDrive,  0.2f);
        snap.noiseLevel      = load (pNoiseLevel, 0.0f);
        snap.noiseDecayMs    = load (pNoiseDecay, 60.0f);
        snap.noiseTone01     = load (pNoiseTone,  0.5f);
        snap.noiseType       = static_cast<int> (load (pNoiseType, 0.0f));
        snap.tuneMode        = static_cast<int> (load (pTuneMode, 0.0f));

        // PHASE 2.7b — SAMPLE group snapshot.
        snap.synthEnable     = load (pSynthEnable,     1.0f) > 0.5f;
        snap.sampleEnable    = load (pSampleEnable,    0.0f) > 0.5f;
        snap.sampleLevel     = load (pSampleLevel,     0.7f);
        snap.sampleStart     = load (pSampleStart,     0.0f);
        snap.sampleEnd       = load (pSampleEnd,       1.0f);
        snap.sampleReverse   = load (pSampleReverse,   0.0f) > 0.5f;
        snap.sampleTune      = load (pSampleTune,      0.0f);
        snap.sampleFine      = load (pSampleFine,      0.0f);
        snap.sampleMidiTrack = load (pSampleMidiTrack, 1.0f) > 0.5f;
        snap.sampleAttackMs  = load (pSampleAttack,    0.0f);
        snap.sampleDecayMs   = load (pSampleDecay,     2200.0f);   // 2026-08-31: was 800
        snap.sampleHPHz      = load (pSampleHP,        20.0f);
        snap.sampleLPHz      = load (pSampleLP,        20000.0f);
        snap.sampleCrush01   = load (pSampleCrush,     0.0f);

        // ---------------------------------------------------------------------
        // PHASE 2.11 — macro layer. 0.5 = neutral (m = 0 changes NOTHING, exactly:
        // every offset is `+= m*k` or `*= (1 + m*k)` / `*= 2^(...*m)` which is a true
        // identity at m = 0). The APVTS target params are NOT modified — this only
        // shifts the effective values the engine feeds its components, so each target
        // stays independently automatable. Full-throw amounts are tuned at Stage 17.
        macroPunchSm.setTargetValue (load (pMacroPunch, 0.5f));
        macroBodySm .setTargetValue (load (pMacroBody,  0.5f));
        macroCrushSm.setTargetValue (load (pMacroCrush, 0.5f));
        macroTailSm .setTargetValue (load (pMacroTail,  0.5f));
        const float mPunch = (macroPunchSm.getNextValue() - 0.5f) * 2.0f;   // [-1, +1]
        const float mBody  = (macroBodySm .getNextValue() - 0.5f) * 2.0f;
        const float mCrush = (macroCrushSm.getNextValue() - 0.5f) * 2.0f;
        const float mTail  = (macroTailSm .getNextValue() - 0.5f) * 2.0f;

        // PUNCH: sharper transient + tighter, snappier pitch drop + more click.
        snap.transientAttack  = juce::jlimit (-1.0f, 1.0f,  snap.transientAttack + mPunch * 0.5f);
        snap.pitchStartRatio  = juce::jlimit (1.0f, 10.0f,  snap.pitchStartRatio * (1.0f + mPunch * 0.6f));
        snap.pitchTimeMs      = juce::jlimit (5.0f, 500.0f, snap.pitchTimeMs     * (1.0f - mPunch * 0.35f));
        snap.clickLevel       = juce::jlimit (0.0f, 1.0f,   snap.clickLevel      + mPunch * 0.3f);

        // BODY: louder + longer body, deeper tune (<= 3 semitones down).
        snap.bodyLevel        = juce::jlimit (0.0f, 1.0f,      snap.bodyLevel   + mBody * 0.3f);
        snap.bodyDecayMs      = juce::jlimit (20.0f, 2000.0f,  snap.bodyDecayMs * (1.0f + mBody * 0.5f));
        snap.fundamental      = juce::jlimit (25.0f, 150.0f,   snap.fundamental * std::exp2 (-mBody * 3.0f / 12.0f));

        // CRUSH: more drive + morph further toward the digital curves.
        snap.drive01          = juce::jlimit (0.0f, 1.0f, snap.drive01     + mCrush * 0.4f);
        snap.character01      = juce::jlimit (0.0f, 1.0f, snap.character01 + mCrush * 0.4f);

        // TAIL: louder + longer + brighter tail.
        snap.tailLevel        = juce::jlimit (0.0f, 1.0f,     snap.tailLevel    + mTail * 0.35f);
        snap.tailLengthMs     = juce::jlimit (20.0f, 2000.0f, snap.tailLengthMs * (1.0f + mTail * 0.6f));
        snap.tailTone01       = juce::jlimit (0.0f, 1.0f,     snap.tailTone01   + mTail * 0.3f);

        // BODY -> sampleLevel, only while the sample layer is active.
        if (snap.sampleEnable)
            snap.sampleLevel  = juce::jlimit (0.0f, 1.0f, snap.sampleLevel + mBody * 0.3f);
        // ---------------------------------------------------------------------

        // PHASE 2.7b — resolve the effective SAMPLE values once per block.
        //   velocity: sampleLevel *= lerp(1, v01, velSens)  (folded in as `velFactor` at
        //             noteOn); sampleLP *= lerp(1, 0.6 + 0.4*v01, velSens*0.5) (darker).
        const float v01ForSample = juce::jlimit (0.0f, 1.0f, lastVel01);
        const float velLpScale   = 1.0f + snap.velSens * 0.5f
                                          * ((0.6f + 0.4f * v01ForSample) - 1.0f);

        SamplePlayer::SampleParams sp;
        sp.enable    = snap.sampleEnable ? 1.0f : 0.0f;
        sp.level     = snap.sampleLevel;   // (macroBody offset already folded into snap above)
        sp.start01   = snap.sampleStart;
        sp.end01     = snap.sampleEnd;
        sp.reverse   = snap.sampleReverse;
        sp.tuneSemis = snap.sampleTune;
        sp.fineCents = snap.sampleFine;
        sp.midiTrack = snap.sampleMidiTrack;
        sp.attackMs  = snap.sampleAttackMs;
        sp.decayMs   = snap.sampleDecayMs;
        sp.hpHz      = snap.sampleHPHz;
        sp.lpHz      = juce::jlimit (20.0f, 20000.0f, snap.sampleLPHz * velLpScale);
        sp.crush01   = snap.sampleCrush01;

        const float synthGate  = snap.synthEnable ? 1.0f : 0.0f;
        const float sampleGate = (snap.sampleEnable && currentSampleBuf != nullptr) ? 1.0f : 0.0f;

        // Per-block refresh on BOTH voices (either can be rendering during a crossfade):
        // body level + pitch-contour coefficients (keeps automation live mid-voice).
        for (auto& v : voices)
        {
            v.setBodyLevel (snap.bodyLevel);
            v.setPitchParams (effectivePitchStartRatio (lastVel01), snap.pitchTimeMs, snap.pitchCurve);
            v.setClickParams (snap.clickLevel, snap.clickToneHz, snap.clickTimeMs,
                              snap.clickPitchHz, snap.clickWidth01);   // PHASE 2.9 — clickWidth
            v.setBodyWidth (snap.bodyWidth01);                          // PHASE 2.9 — bodyWidth
            v.setSubParams (snap.subLevel, snap.subFreqHz, snap.subDecayMs);
            v.setTailParams (snap.tailLevel, snap.tailLengthMs, snap.tailTone01, snap.tailDrive01);
            v.setNoiseParams (snap.noiseLevel, snap.noiseDecayMs, snap.noiseTone01, snap.noiseType);
            v.setSampleParams (sp, synthGate, sampleGate);
        }

        const float transientAttackEff = snap.transientAttack;   // (macroPunch already folded in)
        transientShaper.setParams (transientAttackEff, snap.transientSustain);

        const float driveEff     = snap.drive01;       // (macroCrush already folded in)
        const float characterEff = snap.character01;
        waveshaperL.setParams (driveEff, characterEff, snap.driveMix01);
        waveshaperR.setParams (driveEff, characterEff, snap.driveMix01);

        // PHASE 2.9 — tone (pre-distortion) + crossover / width / mix / gain / limiter.
        // low/mid/high, outputWidth/output/mix: no macro targets (per the contract).
        outputStage.setParams (snap.lowDb, snap.midDb, snap.highDb,
                               snap.outputWidth01, snap.outputDb,
                               snap.limiterOn, snap.mix01);

        // Sample-accurate sub-block split at each note-on.
        int pos = 0;
        for (const auto metadata : midi)
        {
            const auto message = metadata.getMessage();
            if (! message.isNoteOn())
                continue;

            const int evPos = juce::jlimit (0, numSamples, metadata.samplePosition);
            if (evPos > pos)
                renderSegment (buffer, pos, evPos - pos);

            pos = evPos;
            handleNoteOn (message);   // Phase 2.3: 2-voice 3 ms equal-power retrigger crossfade
        }

        if (pos < numSamples)
            renderSegment (buffer, pos, numSamples - pos);

        // PHASE 2.10 — glitch-free OS factor switch.
        //  (a) if the PREVIOUS block swapped the factor, ramp this block's output 0 -> 1;
        //  (b) if a swap is pending, ramp this block's output -> silence over the last
        //      kSwitchFadeSamples, adopt the new factor, refresh every in-region
        //      coefficient for the new fsOversampled, then arm the fade-in for next block.
        if (switchFadeInSamples > 0)
        {
            const int total = kSwitchFadeSamples;
            const int done  = total - switchFadeInSamples;          // ramp samples already applied
            const int rampN = juce::jmin (switchFadeInSamples, numSamples);
            for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            {
                auto* data = buffer.getWritePointer (ch);
                for (int i = 0; i < rampN; ++i)
                    data[i] *= static_cast<float> (done + i + 1) / static_cast<float> (total);
            }
            switchFadeInSamples -= rampN;
        }

        if (osSwitchNow)
        {
            const int nf = juce::jmin (kSwitchFadeSamples, numSamples);
            for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            {
                auto* data = buffer.getWritePointer (ch);
                for (int i = 0; i < nf; ++i)
                {
                    const float g = static_cast<float> (nf - 1 - i) / static_cast<float> (juce::jmax (1, nf));
                    data[numSamples - nf + i] *= g;                 // 1 -> ~0 over the block tail
                }
            }

            oversampling.applyPendingFactor();
            updateInRegionRate (oversampling.getOversampledRate());
            switchFadeInSamples = kSwitchFadeSamples;               // next block ramps 0 -> 1
        }

        // NaN/Inf guard + final ceiling clamp on the base-rate bus (after
        // processSamplesDown). The musical limiting is the in-region tanh soft-clip
        // (OutputStage, oversampled, alias-free); but at 2x+ the polyphase-IIR
        // *downsampling* filter runs after it and overshoots on a heavily-clipped
        // kick (+1.7 dB was measured at 2x). This hard clamp catches only that
        // residual filter overshoot — it engages on ~0.1% of samples so its own
        // base-rate aliasing is far below the noise floor — and guarantees the
        // -0.5 dBFS digital ceiling when `limiter` is on. `limiter` off -> raw.
        const float ceilGain = OutputStage::kLimiterCeilingGain;
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        {
            auto* data = buffer.getWritePointer (ch);
            if (snap.limiterOn)
                for (int i = 0; i < numSamples; ++i)
                    data[i] = juce::jlimit (-ceilGain, ceilGain, dsputils::sanitize (data[i]));
            else
                for (int i = 0; i < numSamples; ++i)
                    data[i] = dsputils::sanitize (data[i]);
        }

        // PHASE 3.2: the analyzer tap now lives in `KICKRAudioProcessor::processBlock`
        // AFTER `engine.processBlock` (this method) returns — the engine stays pure
        // DSP. The processor arms a one-kick capture on a note-on and calls
        // `Analyzer::pushBlock` with the final post-limiter buffer (bounded memcpy +
        // atomic store + AbstractFifo write only).
    }
}
