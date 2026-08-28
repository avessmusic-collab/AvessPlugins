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

        voice.prepare (fsOversampled);

        scratch.setSize (OversamplingProcessor::kNumChannels, maxBlockSize, false, false, true);
        scratch.clear();

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

        reset();
    }

    void KickEngine::reset()
    {
        oversampling.reset();
        voice.reset();
        scratch.clear();
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

    void KickEngine::handleNoteOn (const juce::MidiMessage& message)
    {
        const int   note = message.getNoteNumber();
        const float v01  = message.getFloatVelocity();          // velocity / 127

        const float freqHz = resolvePitchHz (note);

        // Velocity -> level (Phase 2.1): lerp(1, v01, velSensitivity).
        const float velLevelGain = 1.0f + snap.velSens * (v01 - 1.0f);

        lastVel01 = v01;

        voice.noteOn (freqHz, note, velLevelGain, snap.bodyDecayMs, 0);

        // Refresh the pitch contour with THIS note's velocity-scaled pitchStart so the
        // freshly-armed fall is correct from sample 0.
        voice.setPitchParams (effectivePitchStartRatio (v01), snap.pitchTimeMs, snap.pitchCurve);
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
        voice.renderAdd (up, 0, static_cast<int> (up.getNumSamples()));
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
        snap.tuneMode        = static_cast<int> (load (pTuneMode, 0.0f));

        voice.setBodyLevel (snap.bodyLevel);

        // Per-block pitch-contour coefficient refresh (keeps automation live mid-voice).
        voice.setPitchParams (effectivePitchStartRatio (lastVel01), snap.pitchTimeMs, snap.pitchCurve);

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
            handleNoteOn (message);   // retrigger just restarts the voice (crossfade = Phase 2.3)
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
