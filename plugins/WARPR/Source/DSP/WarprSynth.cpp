#include "DSP/WarprSynth.h"

#include <algorithm>
#include <cmath>

namespace warpr
{
    namespace
    {
        constexpr int bendRangeTable[3] = { 2, 12, 24 };
    }

    //==========================================================================
    // Setup
    //==========================================================================
    void WarprSynth::prepare (double newSampleRate, int /*maxBlockSize*/)
    {
        sampleRate = newSampleRate;
        halfband = &HalfbandCoeffs::get();            // designed here, never on the audio thread
        (void) SincTable::get();                      // build the interpolation table now, not at the first note

        for (auto& v : voices)
            v.prepare (newSampleRate, *halfband);

        reset();
    }

    void WarprSynth::reset()
    {
        for (auto& v : voices)
        {
            v.kill();

            if (auto* s = v.takeSample())
                s->decReferenceCount();               // audio is stopped: direct release is safe
        }

        for (int i = 0; i < backlogCount; ++i)
            if (backlog[(size_t) i] != nullptr)
                backlog[(size_t) i]->decReferenceCount();

        backlogCount = 0;
        stackCount = 0;
        sustained.fill (false);
        pedalDown = false;
        bendNorm = 0.0f;
        modWheel = 0.0f;
        noteCounter = 0;
        lastPlayedPitch = -1.0;
        lastVoiceMode = voiceMode;
        vp.bendSemis = 0.0f;
    }

    //==========================================================================
    // Per-block parameters
    //==========================================================================
    void WarprSynth::setBlockParams (const ParamBlock& pb) noexcept
    {
        vp.sampleStart = pb.get (ParamId::sampleStart);
        vp.sampleEnd = pb.get (ParamId::sampleEnd);
        vp.rootNote = pb.getInt (ParamId::rootNote);
        vp.transpose = pb.getInt (ParamId::transpose);
        vp.fineTuneCents = pb.get (ParamId::fineTune);
        vp.loopMode = juce::jlimit (0, 2, pb.getInt (ParamId::loopMode));
        vp.direction = juce::jlimit (0, 1, pb.getInt (ParamId::direction));

        vp.attackMs = pb.get (ParamId::ampAttack);
        vp.decayMs = pb.get (ParamId::ampDecay);
        vp.sustain = pb.get (ParamId::ampSustain);
        vp.releaseMs = pb.get (ParamId::ampRelease);

        vp.unisonDetuneCents = pb.get (ParamId::unisonDetune);
        vp.unisonSpread = pb.get (ParamId::unisonSpread);
        vp.glideMs = pb.get (ParamId::glideTime);

        vp.bendRangeSemis = bendRangeTable[juce::jlimit (0, 2, pb.getInt (ParamId::bendRange))];
        vp.bendSemis = bendNorm * (float) vp.bendRangeSemis;
        vp.interpQuality = interpQuality.load (std::memory_order_relaxed);

        voiceMode = juce::jlimit (0, 2, pb.getInt (ParamId::voiceMode));
        polyLimit = juce::jlimit (1, maxVoices, pb.getInt (ParamId::polyphony));
        unisonLatched = juce::jlimit (1, WarprVoice::maxUnison, pb.getInt (ParamId::unisonVoices));
        osLatched = 1 << juce::jlimit (0, 3, pb.getInt (ParamId::oversampling));
    }

    //==========================================================================
    // Queries
    //==========================================================================
    int WarprSynth::getNumActiveVoices() const noexcept
    {
        int n = 0;
        for (const auto& v : voices)
            if (v.isActive())
                ++n;
        return n;
    }

    int WarprSynth::getNumNonGhostVoices() const noexcept
    {
        int n = 0;
        for (const auto& v : voices)
            if (v.isActive() && ! v.ghost)
                ++n;
        return n;
    }

    int WarprSynth::getOscillatorUnitsInUse() const noexcept
    {
        int units = 0;
        for (const auto& v : voices)
            if (v.isActive() && ! v.ghost)
                units += v.getOs() * v.getUnison();
        return units;
    }

    WarprSynth::VoiceInfo WarprSynth::getVoiceInfo (int slot) const noexcept
    {
        VoiceInfo info;

        if (slot >= 0 && slot < totalSlots)
        {
            const auto& v = voices[(size_t) slot];
            info.active = v.isActive();
            info.ghost = v.ghost;
            info.note = v.note;
            info.unison = v.getUnison();
            info.os = v.getOs();
            info.level = v.envLevel();
        }

        return info;
    }

    //==========================================================================
    // Sample reference hand-back (never frees on the audio thread)
    //==========================================================================
    void WarprSynth::deferRelease (SampleData* s) noexcept
    {
        if (s == nullptr)
            return;

        if (backlogCount == 0 && retireList.push (s, &SampleStore::releaseRef))
            return;

        if (backlogCount < (int) backlog.size())
            backlog[(size_t) backlogCount++] = s;
        else
            leakedRefs.fetch_add (1, std::memory_order_relaxed);      // intentional leak > crash/free on the audio thread
    }

    void WarprSynth::flushBacklog() noexcept
    {
        while (backlogCount > 0 && retireList.hasSpace())
        {
            retireList.push (backlog[(size_t) (backlogCount - 1)], &SampleStore::releaseRef);
            --backlogCount;
        }
    }

    void WarprSynth::harvestFinishedVoices() noexcept
    {
        for (auto& v : voices)
        {
            if (v.isActive())
                continue;

            if (v.consumeGuardKill())
                guardKills.fetch_add (1, std::memory_order_relaxed);

            if (auto* s = v.takeSample())
                deferRelease (s);
        }
    }

    //==========================================================================
    // Note stack
    //==========================================================================
    bool WarprSynth::inStack (int note) const noexcept
    {
        for (int i = 0; i < stackCount; ++i)
            if (stack[(size_t) i] == (unsigned char) note)
                return true;
        return false;
    }

    void WarprSynth::removeNote (int note) noexcept
    {
        int w = 0;
        for (int i = 0; i < stackCount; ++i)
            if (stack[(size_t) i] != (unsigned char) note)
                stack[(size_t) w++] = stack[(size_t) i];
        stackCount = w;
    }

    void WarprSynth::pushNote (int note) noexcept
    {
        removeNote (note);
        sustained[(size_t) note] = false;

        if (stackCount < (int) stack.size())
            stack[(size_t) stackCount++] = (unsigned char) note;
    }

    //==========================================================================
    // Voice selection
    //==========================================================================
    int WarprSynth::ghostFadeSamples() const noexcept
    {
        return std::max (1, (int) std::lround (0.003 * sampleRate));
    }

    void WarprSynth::makeGhost (WarprVoice& v, int fadeSamples) noexcept
    {
        v.beginFade (fadeSamples);
    }

    WarprVoice* WarprSynth::findIdleSlot() noexcept
    {
        for (auto& v : voices)
            if (! v.isActive())
                return &v;
        return nullptr;
    }

    WarprVoice* WarprSynth::reclaimGhost() noexcept
    {
        WarprVoice* best = nullptr;

        for (auto& v : voices)
            if (v.isActive() && v.ghost && (best == nullptr || v.fadeSamplesLeft() < best->fadeSamplesLeft()))
                best = &v;

        if (best != nullptr)
        {
            best->kill();                                  // rare: every slot busy; the nearly finished ghost is cut
            if (auto* s = best->takeSample())
                deferRelease (s);
        }

        return best;
    }

    WarprVoice* WarprSynth::pickVictim() noexcept
    {
        WarprVoice* quietestInRelease = nullptr;
        WarprVoice* oldest = nullptr;

        for (auto& v : voices)
        {
            if (! v.isActive() || v.ghost)
                continue;

            if (v.envState() == AmpEnvelope::State::Release
                && (quietestInRelease == nullptr || v.envLevel() < quietestInRelease->envLevel()))
                quietestInRelease = &v;

            if (oldest == nullptr || v.age < oldest->age)
                oldest = &v;
        }

        return quietestInRelease != nullptr ? quietestInRelease : oldest;
    }

    WarprVoice* WarprSynth::findMonoVoice() noexcept
    {
        WarprVoice* newest = nullptr;

        for (auto& v : voices)
            if (v.isActive() && ! v.ghost && (newest == nullptr || v.age > newest->age))
                newest = &v;

        return newest;
    }

    /** Applies the unison cap of the oscillator budget (own unison first, then steals) and starts the voice. */
    WarprVoice* WarprSynth::startNewVoice (int note, float velocity) noexcept
    {
        if (currentSample == nullptr)
            return nullptr;

        const int os = osLatched;
        int uni = unisonLatched;
        const int budget = oscBudget.load (std::memory_order_relaxed);

        if (budget > 0)
        {
            for (int guard = 0; guard < totalSlots + 2; ++guard)
            {
                const int fit = (budget - getOscillatorUnitsInUse()) / os;

                if (fit >= 1)
                {
                    uni = std::min (uni, fit);
                    break;
                }

                auto* victim = pickVictim();

                if (victim == nullptr)
                {
                    uni = 1;
                    break;
                }

                makeGhost (*victim, ghostFadeSamples());
            }
        }

        WarprVoice* slot = findIdleSlot();

        if (slot == nullptr)
            slot = reclaimGhost();

        if (slot == nullptr)
            return nullptr;

        if (auto* leftover = slot->takeSample())          // a killed slot may still hold its previous reference
            deferRelease (leftover);

        currentSample->incReferenceCount();               // this voice's own reference

        WarprVoice::StartInfo info;
        info.note = note;
        info.velocity = velocity;
        info.os = os;
        info.unison = uni;
        info.glideFromSemis = lastPlayedPitch >= 0.0 ? lastPlayedPitch : (double) note;
        info.glideMs = (double) vp.glideMs;
        info.age = ++noteCounter;

        slot->start (info, vp, currentSample);
        return slot;
    }

    //==========================================================================
    // Note handling
    //==========================================================================
    void WarprSynth::startPoly (int note, float velocity) noexcept
    {
        for (auto& v : voices)
            if (v.isActive() && ! v.ghost && v.note == note)
                makeGhost (v, ghostFadeSamples());           // (1) the same note

        while (getNumNonGhostVoices() >= polyLimit)
        {
            auto* victim = pickVictim();                     // (2) quietest in release, (3) oldest
            if (victim == nullptr)
                break;
            makeGhost (*victim, ghostFadeSamples());
        }

        startNewVoice (note, velocity);
    }

    void WarprSynth::startMono (int note, float velocity, bool legato, bool keysWereAlive) noexcept
    {
        auto* cur = findMonoVoice();

        if (cur != nullptr && cur->isSounding())
        {
            if (legato && keysWereAlive)
            {
                cur->retarget (note, (double) vp.glideMs);              // overlap: pitch only
                return;
            }

            if (! legato && vp.loopMode != 2)
            {
                cur->retarget (note, (double) vp.glideMs);              // envelope retrigger from the current level
                cur->retriggerEnvelope (vp.attackMs);
                cur->setVelocity (velocity);
                return;
            }
        }

        if (cur != nullptr)
            makeGhost (*cur, ghostFadeSamples());

        startNewVoice (note, velocity);
    }

    void WarprSynth::noteOn (int note, float velocity) noexcept
    {
        const bool keysWereAlive = stackCount > 0;
        pushNote (note);

        if (currentSample == nullptr)
            return;

        if (voiceMode == 0)
            startPoly (note, velocity);
        else
            startMono (note, velocity, voiceMode == 2, keysWereAlive);

        lastPlayedPitch = (double) note;
    }

    void WarprSynth::monoNoteRemoved (int removedNote) noexcept
    {
        auto* v = findMonoVoice();

        if (v == nullptr)
            return;

        if (stackCount == 0)
        {
            v->keyHeld = false;
            v->release();
        }
        else if (v->note == removedNote)
        {
            v->retarget ((int) stack[(size_t) (stackCount - 1)], (double) vp.glideMs);

            if (voiceMode == 1 && vp.loopMode != 2)
                v->retriggerEnvelope (vp.attackMs);
        }
    }

    void WarprSynth::monoReevaluate() noexcept
    {
        auto* v = findMonoVoice();

        if (v == nullptr)
            return;

        if (stackCount == 0)
        {
            v->keyHeld = false;
            v->pedalHeld = false;
            v->release();
        }
        else if (! inStack (v->note))
        {
            v->retarget ((int) stack[(size_t) (stackCount - 1)], (double) vp.glideMs);

            if (voiceMode == 1 && vp.loopMode != 2)
                v->retriggerEnvelope (vp.attackMs);
        }
    }

    void WarprSynth::noteOff (int note) noexcept
    {
        if (pedalDown)
        {
            sustained[(size_t) note] = true;                      // stays "alive" until the pedal is released

            if (voiceMode == 0)
                for (auto& v : voices)
                    if (v.isActive() && ! v.ghost && v.note == note && v.keyHeld)
                    {
                        v.keyHeld = false;
                        v.pedalHeld = true;
                    }

            return;
        }

        removeNote (note);

        if (voiceMode == 0)
        {
            for (auto& v : voices)
                if (v.isActive() && ! v.ghost && v.note == note && v.keyHeld)
                {
                    v.keyHeld = false;
                    v.release();
                }
        }
        else
        {
            monoNoteRemoved (note);
        }
    }

    void WarprSynth::setPedal (bool down) noexcept
    {
        if (down == pedalDown)
            return;

        pedalDown = down;

        if (down)
            return;

        for (int n = 0; n < 128; ++n)
            if (sustained[(size_t) n])
            {
                sustained[(size_t) n] = false;
                removeNote (n);
            }

        if (voiceMode == 0)
        {
            for (auto& v : voices)
                if (v.isActive() && ! v.ghost && v.pedalHeld)
                {
                    v.pedalHeld = false;
                    v.release();
                }
        }
        else
        {
            monoReevaluate();
        }
    }

    void WarprSynth::allNotesOff() noexcept
    {
        for (auto& v : voices)
            if (v.isActive() && ! v.ghost)
            {
                v.keyHeld = false;
                v.pedalHeld = false;
                v.release();
            }

        stackCount = 0;
        sustained.fill (false);
    }

    void WarprSynth::allSoundOff() noexcept
    {
        for (auto& v : voices)
            if (v.isActive() && ! v.ghost)
                makeGhost (v, ghostFadeSamples());

        stackCount = 0;
        sustained.fill (false);
    }

    //==========================================================================
    // MIDI + block rendering
    //==========================================================================
    void WarprSynth::handleMidi (const juce::MidiMessageMetadata& m) noexcept
    {
        if (m.numBytes < 1)
            return;

        const int status = m.data[0];

        if (status < 0x80 || status >= 0xF0)
            return;

        const int type = status & 0xF0;
        const int d1 = m.numBytes > 1 ? (m.data[1] & 0x7F) : 0;
        const int d2 = m.numBytes > 2 ? (m.data[2] & 0x7F) : 0;

        switch (type)
        {
            case 0x90:
                if (d2 > 0)
                    noteOn (d1, (float) d2 / 127.0f);
                else
                    noteOff (d1);
                break;

            case 0x80:
                noteOff (d1);
                break;

            case 0xE0:
            {
                const int wheel = d1 | (d2 << 7);
                bendNorm = juce::jlimit (-1.0f, 1.0f, (float) (wheel - 8192) / 8192.0f);
                vp.bendSemis = bendNorm * (float) vp.bendRangeSemis;
                break;
            }

            case 0xB0:
                if (d1 == 64)
                    setPedal (d2 >= 64);
                else if (d1 == 1)
                    modWheel = (float) d2 / 127.0f;
                else if (d1 == 120)
                    allSoundOff();
                else if (d1 == 123)
                    allNotesOff();
                break;

            default:
                break;
        }
    }

    void WarprSynth::applyModeAndPolyphonyChanges() noexcept
    {
        if (voiceMode != lastVoiceMode)
        {
            if (voiceMode != 0)
            {
                // switching to Mono / Legato keeps the newest voice; the others fade out over 10 ms
                auto* keep = findMonoVoice();
                const int fade = std::max (1, (int) std::lround (0.010 * sampleRate));

                for (auto& v : voices)
                    if (v.isActive() && ! v.ghost && &v != keep)
                        makeGhost (v, fade);
            }

            lastVoiceMode = voiceMode;
        }

        if (voiceMode == 0)
        {
            while (getNumNonGhostVoices() > polyLimit)
            {
                auto* victim = pickVictim();
                if (victim == nullptr)
                    break;
                makeGhost (*victim, ghostFadeSamples());
            }
        }
    }

    void WarprSynth::renderRange (float* outL, float* outR, int n) noexcept
    {
        if (n <= 0)
            return;

        for (auto& v : voices)
            if (v.isActive())
                v.render (vp, outL, outR, n);

        harvestFinishedVoices();
    }

    void WarprSynth::renderBlock (float* outL, float* outR, int numSamples, const juce::MidiBuffer& midi) noexcept
    {
        flushBacklog();
        applyModeAndPolyphonyChanges();

        int pos = 0;
        auto it = midi.begin();
        const auto end = midi.end();

        while (pos < numSamples)
        {
            while (it != end && (*it).samplePosition <= pos)
            {
                handleMidi (*it);
                ++it;
            }

            int next = numSamples;

            if (it != end)
                next = std::min (next, std::max ((*it).samplePosition, pos + 1));

            renderRange (outL + pos, outR + pos, next - pos);
            pos = next;
        }
    }
}
