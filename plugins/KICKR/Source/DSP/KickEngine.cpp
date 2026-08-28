#include "DSP/KickEngine.h"
#include "Parameters/ParameterIDs.h"

#include <cmath>

namespace kickr
{
    void KickEngine::prepare (double sampleRate, int maximumBlockSize)
    {
        baseSampleRate = juce::jmax (1.0, sampleRate);
        maxBlockSize   = juce::jmax (1, maximumBlockSize);

        oversampling.prepare (baseSampleRate, maxBlockSize);
        fsOversampled = oversampling.getOversampledRate();     // == baseSampleRate at 1x

        for (auto& v : voices)
            v.prepare (fsOversampled);

        transientShaper.prepare (fsOversampled);

        // 1 / (fade length in oversampled samples) — fsOversampled is fixed until Phase 2.10.
        retriggerThetaInc = 1.0 / juce::jmax (1.0, kRetriggerFadeMs * 0.001 * fsOversampled);

        scratch.setSize (OversamplingProcessor::kNumChannels, maxBlockSize, false, false, true);
        scratch.clear();

        // Mono per-voice render scratch @ the oversampled rate. Sized for the worst case
        // (max block x max OS factor) so Phase 2.10 needs no resize here.
        const int maxOsSamples = maxBlockSize * (1 << (OversamplingProcessor::kNumFactors - 1));
        for (auto& b : voiceScratch)
        {
            b.setSize (1, maxOsSamples, false, false, true);
            b.clear();
        }

        // Base-rate DC blocker, ~5 Hz corner (below the 25 Hz sub — Phase 2.5).
        const auto dcR = 1.0f - (juce::MathConstants<float>::twoPi * 5.0f
                                 / static_cast<float> (baseSampleRate));
        for (auto& b : dcBlockers)
        {
            b.R = juce::jlimit (0.9f, 0.99999f, dcR);
            b.reset();
        }

        // Cache raw parameter pointers (Phase 2.1 params only).
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

        reset();
    }

    void KickEngine::reset()
    {
        oversampling.reset();

        for (auto& v : voices)
            v.reset();

        transientShaper.reset();

        activeVoice   = 0;
        incomingVoice = 1;
        fadeActive    = false;
        theta         = 0.0;

        scratch.clear();
        for (auto& b : voiceScratch)
            b.clear();
        for (auto& b : dcBlockers)
            b.reset();
    }

    float KickEngine::resolvePitchHz (int noteNumber) const noexcept
    {
        float baseHz;

        if (snap.tuneMode == 0)   // MIDI Pitch
        {
            const auto noteHz = 440.0f * std::exp2 (static_cast<float> (noteNumber - 69) / 12.0f);
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
        v.noteOn (freqHz, note, velLevelGain, velClickGain, snap.bodyDecayMs, 0);

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

        // Render each live voice into its own mono scratch (the body is mono).
        float* dataA = voiceScratch[0].getWritePointer (0);
        float* dataB = voiceScratch[1].getWritePointer (0);

        const bool fading = fadeActive;

        voices[static_cast<size_t> (activeVoice)].renderMono (dataA, upNum);
        if (fading)
            voices[static_cast<size_t> (incomingVoice)].renderMono (dataB, upNum);

        // Voice mix (equal-power crossfade during a retrigger) -> summed-mono
        // TransientShaper (AD-10 in-region) -> both output channels of `up`.
        double th = theta;

        for (int i = 0; i < upNum; ++i)
        {
            float mono;

            if (fading)
            {
                float gOld = 1.0f, gNew = 0.0f;
                dsputils::equalPowerGains (static_cast<float> (th), gOld, gNew);
                mono = gOld * dataA[i] + gNew * dataB[i];

                th += retriggerThetaInc;
                if (th > 1.0)
                    th = 1.0;
            }
            else
            {
                mono = dataA[i];
            }

            // Phase 2.11: transientAttackEff = transientAttack + macroPunch offset
            // (the offset is added upstream in processBlock via setParams).
            const float shaped = dsputils::sanitize (transientShaper.processSample (mono));

            for (int ch = 0; ch < upCh; ++ch)
                up.setSample (ch, i, shaped);
        }

        if (fading)
        {
            theta = th;

            if (theta >= 1.0)
            {
                voices[static_cast<size_t> (activeVoice)].reset();   // free the outgoing voice
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

        // PHASE 2.10 hook — pinned to 1x for now.
        if (pOversampling != nullptr)
            oversampling.setFactorChoice (static_cast<int> (pOversampling->load()));

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

        // Per-block refresh on BOTH voices (either can be rendering during a crossfade):
        // body level + pitch-contour coefficients (keeps automation live mid-voice).
        for (auto& v : voices)
        {
            v.setBodyLevel (snap.bodyLevel);
            v.setPitchParams (effectivePitchStartRatio (lastVel01), snap.pitchTimeMs, snap.pitchCurve);
            v.setClickParams (snap.clickLevel, snap.clickToneHz, snap.clickTimeMs, snap.clickPitchHz);
            v.setSubParams (snap.subLevel, snap.subFreqHz, snap.subDecayMs);
            // Phase 2.11: tail* below = tailLevel/Length/Tone + macroTail offsets.
            v.setTailParams (snap.tailLevel, snap.tailLengthMs, snap.tailTone01, snap.tailDrive01);
            v.setNoiseParams (snap.noiseLevel, snap.noiseDecayMs, snap.noiseTone01, snap.noiseType);
        }

        // Phase 2.11: transientAttackEff = snap.transientAttack + macroPunch offset.
        const float transientAttackEff = snap.transientAttack;
        transientShaper.setParams (transientAttackEff, snap.transientSustain);

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

        // Base-rate DC blocker + NaN/Inf guard on the final bus (after processSamplesDown).
        const int numCh = juce::jmin (buffer.getNumChannels(),
                                      static_cast<int> (dcBlockers.size()));
        for (int ch = 0; ch < numCh; ++ch)
        {
            auto* data = buffer.getWritePointer (ch);
            auto& blk  = dcBlockers[static_cast<size_t> (ch)];

            for (int i = 0; i < numSamples; ++i)
                data[i] = dsputils::sanitize (blk.process (data[i]));
        }

        // PHASE 3.2: analyzer tap here — copy the post-DC mono sum into the lock-free
        // FIFO / double buffer. No-op this phase.
    }
}
