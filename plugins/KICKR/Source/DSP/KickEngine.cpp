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
        pMorph            = apvts.getRawParameterValue (id::morph);
        pMorphMode        = apvts.getRawParameterValue (id::morphMode);
        pClickWidth       = apvts.getRawParameterValue (id::clickWidth);
        pOutputWidth      = apvts.getRawParameterValue (id::outputWidth);
        pOutput           = apvts.getRawParameterValue (id::output);
        pMix              = apvts.getRawParameterValue (id::mix);
        pLimiter          = apvts.getRawParameterValue (id::limiter);
        pLimLoudness      = apvts.getRawParameterValue (id::limLoudness);     // 2026-09-02 — Color Limiter
        pLimCeiling       = apvts.getRawParameterValue (id::limCeiling);
        pLimLookahead     = apvts.getRawParameterValue (id::limLookahead);
        pLimRelease       = apvts.getRawParameterValue (id::limRelease);
        pLimSaturation    = apvts.getRawParameterValue (id::limSaturation);
        pLimColor         = apvts.getRawParameterValue (id::limColor);
        pFilterOn         = apvts.getRawParameterValue (id::filterOn);       // 2026-09-02 — master filter
        pFilterType       = apvts.getRawParameterValue (id::filterType);
        pFilterFreq       = apvts.getRawParameterValue (id::filterFreq);
        pFilterRes        = apvts.getRawParameterValue (id::filterRes);
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
        masterFilter.prepare (fsOversampled);    // 2026-09-02
        waveshaperR.prepare (fsOversampled);
        outputStage.prepare (fsOversampled);     // PHASE 2.9 — tone / crossover / limiter vs fsOversampled

        // 1 / (fade length in oversampled samples). Recomputed on every OS factor change.
        declickThetaInc = 1.0 / juce::jmax (1.0, kDeclickFadeMs * 0.001 * fsOversampled);

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
        masterFilter.updateOversampledRate (fsOversampled);   // 2026-09-02
        waveshaperR.updateOversampledRate (fsOversampled);
        outputStage.updateOversampledRate (fsOversampled);

        declickThetaInc = 1.0 / juce::jmax (1.0, kDeclickFadeMs * 0.001 * fsOversampled);
    }

    void KickEngine::reset()
    {
        oversampling.reset();

        for (auto& v : voices)
            v.reset();

        transientShaper.reset();
        waveshaperL.reset();
        masterFilter.reset();   // 2026-09-02
        waveshaperR.reset();
        outputStage.reset();

        activeVoice        = 0;
        incomingVoice      = 1;
        fadeActive         = false;
        theta              = 0.0;
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

        // 2026-09-01 (user request): "the transient of the kick shifts if i rapid fire them
        // or change notes". The per-voice oscillators/envelopes were already fully
        // monophonic (hard voice-steal above), but TransientShaper's attack/release
        // envelope followers kept running continuously across notes: residual follower
        // energy from the previous hit changed how quickly the new hit's transient region
        // was detected — an audible, retrigger-speed-dependent shift in attack character.
        // Resetting the FOLLOWERS on every note-on (not just steals) makes every hit's
        // transient detection start from an identical clean state. Only the followers: the
        // applied-gain smoother is left continuous (bug-scan 2026-09-01, code-review
        // CONFIRMED — snapping it to unity was a one-sample step of up to 6 dB on a still-
        // loud tail whenever Sustain/Attack were non-zero).
        //
        // Deliberately NOT reset here (bug-scan 2026-09-01, code-review CONFIRMED):
        // OutputStage's crossover. An earlier version reset it too, to suppress the filter's
        // few-ms low-frequency ring-down of the just-killed voice, but the reset itself
        // stepped the OUTGOING voice's LF tail by up to its full amplitude (the filter's
        // steady-state vs cold output differ by its own memory) at the one sample where the
        // new note is still at phase 0 and can't mask it — a click, bought only a ~3-point
        // reduction in ring-down, and cost the click layer ~2.6 dB of onset. Also not reset:
        // the tone biquads / DC blocker / mix+out gain smoothers / Waveshaper's adaptive RMS
        // makeup (see NOTES.md for each).
        transientShaper.resetFollowers();

        // A third trigger while the previous declick fade is still running.
        if (fadeActive)
        {
            if (theta <= 0.0)
            {
                // Bug-scan 2026-09-01 (code-review CONFIRMED): the 3rd hit landed on the SAME
                // sample as the 2nd (a doubled note on a flam — ordinary DAW MIDI). No
                // renderSegment ran between them, so the incoming voice hasn't produced a
                // single sample yet and the outgoing one is still at FULL gain (gOld = 1).
                // Bare-resetting the outgoing voice here (the old behaviour) was a one-sample
                // cut of a loud voice — a click. Instead: re-arm the still-silent incoming
                // slot with the new note and leave the outgoing fade exactly as it is, so no
                // voice that has made a sound is ever cut without its 0.75 ms ramp.
                auto& incoming = voices[static_cast<size_t> (incomingVoice)];
                incoming.reset();
                triggerVoice (incoming, freqHz, note, velLevelGain, velClickGain, v01);
                return;
            }

            // Mid-fade (0 < theta < 1): three hits inside one 0.75 ms window with the middle
            // one already audible. Snap the fade to done (free the outgoing voice, promote the
            // incoming one) so we never have > 2 voices live — the documented 2-slot limit.
            voices[static_cast<size_t> (activeVoice)].reset();
            activeVoice = incomingVoice;
            fadeActive  = false;
            theta       = 0.0;
        }

        auto& active = voices[static_cast<size_t> (activeVoice)];

        if (active.isActive())
        {
            // Retrigger while ringing -> the new note starts in the other voice slot
            // IMMEDIATELY at full level (no fade-in — it takes priority instantly), while
            // the old voice gets a short kDeclickFadeMs fade-out and is then unconditionally
            // reset() the moment that fade completes (in renderSegment) — every layer,
            // no exceptions, so nothing ever lingers audibly past the two notes overlapping.
            incomingVoice = 1 - activeVoice;
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

        const bool fading = fadeActive;

        voices[static_cast<size_t> (activeVoice)].renderStereo (aL, aR, upNum);
        if (fading)
            voices[static_cast<size_t> (incomingVoice)].renderStereo (bL, bR, upNum);

        // Voice mix (hard steal: outgoing fades out over kDeclickFadeMs, incoming plays at
        // full level from sample 0, no fade-in) -> Tone (PRE-distortion) -> stereo-linked
        // TransientShaper -> per-channel Waveshaper -> OutputStage (crossover / width / mix
        // / gain / limiter). Everything in-region (AD-10).
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
                // One-sided cosine fall 1 -> 0 (numerically identical to the gOld half of the
                // former equalPowerGains() call; the gNew half has been unused since the
                // incoming voice stopped being faded in).
                const float gOld = std::cos (static_cast<float> (th) * juce::MathConstants<float>::halfPi);
                L = gOld * aL[i] + bL[i];
                R = gOld * aR[i] + bR[i];

                th += declickThetaInc;
                if (th > 1.0)
                    th = 1.0;
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
            masterFilter.process (L, R);                  // 2026-09-02 — master LP/HP (bypass when off)
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
                // 2026-09-01 (user request — strict mono voice-steal): unconditionally
                // reset() the outgoing voice the instant its declick fade completes, no
                // matter whether any of its layers (body/click/sub/tail/noise/sample) would
                // otherwise still be ringing. By this point its output gain has already
                // ramped to ~0 via gOld above, so the hard reset itself is inaudible.
                voices[static_cast<size_t> (activeVoice)].reset();

                activeVoice = incomingVoice;
                fadeActive  = false;
                theta       = 0.0;
            }
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
        snap.morph01      = load (pMorph,       0.0f);   // 2026-09-01 — body waveform morph
        snap.morphMode    = static_cast<int> (load (pMorphMode, 0.0f));   // 2026-09-02
        snap.clickWidth01 = load (pClickWidth,  0.3f);
        snap.outputWidth01 = load (pOutputWidth, 0.5f);
        snap.outputDb     = load (pOutput, 0.0f);        // PHASE 2.9 — OUTPUT
        snap.mix01        = load (pMix,    1.0f);
        snap.limiterOn    = load (pLimiter, 1.0f) > 0.5f;
        snap.limLoudnessDb   = load (pLimLoudness,   0.0f);   // 2026-09-02 — Color Limiter
        snap.limCeilingDb    = load (pLimCeiling,   -0.5f);
        snap.limLookaheadMs  = load (pLimLookahead,  1.5f);
        snap.limReleaseMs    = load (pLimRelease,   50.0f);
        snap.limSaturation01 = load (pLimSaturation, 0.0f);
        snap.limColor01      = load (pLimColor,      0.5f);
        snap.filterOn        = load (pFilterOn,      0.0f) > 0.5f;   // 2026-09-02 — master filter
        snap.filterType      = static_cast<int> (std::lround (load (pFilterType, 0.0f)));
        snap.filterFreqHz    = load (pFilterFreq, 1000.0f);
        snap.filterRes01     = load (pFilterRes,     0.2f);
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
            v.setMorph (snap.morph01);                                  // 2026-09-01 — body waveform morph
            v.setMorphMode (snap.morphMode);                            // 2026-09-02 — warp mode selector
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
        masterFilter.setParams (snap.filterOn, snap.filterType, snap.filterFreqHz, snap.filterRes01);   // 2026-09-02

        ColorLimiter::Params lim;   // 2026-09-02
        lim.enabled      = snap.limiterOn;
        lim.loudnessDb   = snap.limLoudnessDb;
        lim.ceilingDb    = snap.limCeilingDb;
        lim.lookaheadMs  = snap.limLookaheadMs;
        lim.releaseMs    = snap.limReleaseMs;
        lim.saturation01 = snap.limSaturation01;
        lim.color01      = snap.limColor01;
        outputStage.setParams (snap.lowDb, snap.midDb, snap.highDb,
                               snap.outputWidth01, snap.outputDb,
                               lim, snap.mix01);

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
            handleNoteOn (message);   // hard voice-steal: incoming at full level from sample 0, outgoing gets a 0.75 ms declick then reset()
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
        const float ceilGain = dsputils::dbToGain (juce::jlimit (-24.0f, 0.0f, snap.limCeilingDb));   // 2026-09-02 — follows the CEILING knob
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
