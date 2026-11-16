#include "DSP/WarprSynth.h"
#include "DSP/CpuGuard.h"
#include "DSP/WarpModeTable.h"

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
        (void) GrainWindowTable::get();               // ... and the grain window ramps (D7a)

        for (auto& v : voices)
            v.prepare (newSampleRate, *halfband);

        clock.prepare (newSampleRate);
        reset();

        guardLevel.store (0, std::memory_order_relaxed);        // the processor restarts its CpuGuard in prepareToPlay too
        guardLowered.store (0, std::memory_order_relaxed);
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

        // D8: global LFO state back to "never run", the clock to its default, the viz phases to idle
        for (auto& l : gInst)
            l = LfoInstance {};

        gPrev.fill (0.5f);
        gStart.fill (0.5f);
        gEnd.fill (0.5f);
        voiceMean.fill (0.5f);
        monoResetPending = false;
        sliceNoteActive = false;
        lastPolyLimit = -1;
        masterDelta = 0.0f;
        blockPos = 0;
        sampleClock = 0;
        clock.reset();

        for (auto& p : lfoPhaseOut)
            p.store (-1.0f, std::memory_order_relaxed);
    }

    //==========================================================================
    // Per-block parameters
    //==========================================================================
    void WarprSynth::syncShaperToVoices() noexcept
    {
        if (shaper == nullptr)
        {
            vp.shaperCur = nullptr;
            vp.shaperPrev = nullptr;
            vp.shaperFadeBase = 0;
            return;
        }

        vp.shaperCur = shaper->current();
        vp.shaperPrev = shaper->previous();
        vp.shaperFadeBase = shaper->fadeBase();
        vp.shaperFadeLen = shaper->fadeLength();
    }

    void WarprSynth::setBlockParams (const ParamBlock& pb) noexcept
    {
        // Shaper first: adopt a pending LUT set (crossfade bookkeeping) before the minOS below reads it.
        if (shaper != nullptr)
            shaper->beginBlock();

        syncShaperToVoices();

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

        vp.warpEnable = pb.getBool (ParamId::warpEnable);
        vp.warpMode = juce::jlimit (0, kNumWarpModes - 1, pb.getInt (ParamId::warpMode));
        vp.warpAmount = juce::jlimit (0.0f, 1.0f, pb.get (ParamId::warpAmount));
        vp.warpA = juce::jlimit (0.0f, 1.0f, pb.get (ParamId::warpParamA));
        vp.warpB = juce::jlimit (0.0f, 1.0f, pb.get (ParamId::warpParamB));
        vp.warpMix = juce::jlimit (0.0f, 1.0f, pb.get (ParamId::warpMix));
        vp.modOscSource = juce::jlimit (0, 4, pb.getInt (ParamId::modOscSource));
        vp.modOscRatio = juce::jlimit (0.25f, 16.0f, pb.get (ParamId::modOscRatio));

        vp.shaperEnable = pb.getBool (ParamId::shaperEnable);
        vp.shaperMorph = juce::jlimit (0.0f, 1.0f, pb.get (ParamId::shaperMorph));
        vp.shaperMix = juce::jlimit (0.0f, 1.0f, pb.get (ParamId::shaperMix));

        vp.filterEnable = pb.getBool (ParamId::filterEnable);
        vp.filterType = juce::jlimit (0, kNumFilterTypes - 1, pb.getInt (ParamId::filterType));
        vp.filterCutoffHz = juce::jlimit (20.0f, 20000.0f, pb.get (ParamId::filterCutoff));
        vp.filterRes = juce::jlimit (0.0f, 1.0f, pb.get (ParamId::filterResonance));
        vp.filterDriveDb = juce::jlimit (0.0f, 24.0f, pb.get (ParamId::filterDrive));
        vp.filterMorph = juce::jlimit (0.0f, 1.0f, pb.get (ParamId::filterMorph));
        vp.filterKeyTrack = juce::jlimit (0.0f, 1.0f, pb.get (ParamId::filterKeyTrack));
        vp.filterMix = juce::jlimit (0.0f, 1.0f, pb.get (ParamId::filterMix));

        vp.playMode = juce::jlimit (0, 1, pb.getInt (ParamId::playMode));
        vp.grainMode = juce::jlimit (0, 2, pb.getInt (ParamId::grainMode));
        vp.grainPosition = juce::jlimit (0.0f, 1.0f, pb.get (ParamId::grainPosition));
        vp.grainScan = juce::jlimit (0.0f, 1.0f, pb.get (ParamId::grainScan));
        vp.grainScanDist = juce::jlimit (-1.0f, 1.0f, pb.get (ParamId::grainScanDist));
        vp.grainScanLoop = pb.getBool (ParamId::grainScanLoop);
        vp.grainScanKey = pb.getBool (ParamId::grainScanKey);
        vp.grainSizeMs = juce::jlimit (2.0f, 2000.0f, pb.get (ParamId::grainSize));
        vp.grainShape = juce::jlimit (0.0f, 1.0f, pb.get (ParamId::grainShape));
        vp.grainVariation = juce::jlimit (0.0f, 1.0f, pb.get (ParamId::grainVariation));
        vp.grainSpread = juce::jlimit (0.0f, 1.0f, pb.get (ParamId::grainSpread));
        vp.grainReverse = juce::jlimit (0.0f, 1.0f, pb.get (ParamId::grainReverse));
        vp.grainDensity = juce::jlimit (1, 20, pb.getInt (ParamId::grainDensity));

        voiceMode = juce::jlimit (0, 2, pb.getInt (ParamId::voiceMode));
        polyLimit = juce::jlimit (1, maxVoices, pb.getInt (ParamId::polyphony));
        unisonLatched = juce::jlimit (1, WarprVoice::maxUnison, pb.getInt (ParamId::unisonVoices));
        osLatched = 1 << juce::jlimit (0, 3, pb.getInt (ParamId::oversampling));

        // minOS is ENFORCED (risk R1(b)): while the warp stage is audibly active, the factor latched at note-on is
        // max(user oversampling, the selected mode's minOS).  A mid-note mode change keeps the latched factor.
        if (vp.warpEnable && vp.warpAmount > 0.0f && vp.warpMix > 0.0f)
            osLatched = std::max (osLatched, std::min (8, warpModeInfo (vp.warpMode).minOS));

        // The shaper has its own floor, derived from how hard the CURRENT curve is (ShaperLutSet kink metric: a
        // hard-clip-like curve needs 4x, a smooth one 1x).  Same rule: enabled and audible, latched at note-on.
        if (shaper != nullptr && vp.shaperEnable && vp.shaperMix > 0.0f)
            osLatched = std::max (osLatched, std::min (8, shaper->minOsFor (vp.shaperMorph)));

        // ---- D8: adopt pending LFO tables / matrix, read the 10 LFOs' parameters, split them into global / per-voice banks
        if (modEngine != nullptr)
        {
            modEngine->beginBlock();
            matrixPtr = modEngine->matrix();
            slicePtr = modEngine->sliceMap();
            lfoCountBlock = std::min (modEngine->lfoCount(), (int) kNumLfos);

            for (int i = 0; i < kNumLfos; ++i)
            {
                tablePtrs[(size_t) i] = modEngine->tables()[i];
                lfoParams[(size_t) i] = readLfoParams (pb, i);
            }

            globalOwnMask = voiceOwnMask = 0;

            for (int i = 0; i < lfoCountBlock; ++i)
            {
                const int mode = lfoParams[(size_t) i].mode;
                const bool voiceType = mode == (int) LfoMode::Retrig || mode == (int) LfoMode::Env;
                (voiceType ? voiceOwnMask : globalOwnMask) |= 1u << i;
            }

            modCtx.mm = matrixPtr;
            modCtx.params = lfoParams.data();
            modCtx.tables = tablePtrs.data();
            modCtx.lfoCount = lfoCountBlock;
            modCtx.voiceOwnMask = voiceOwnMask;
            modCtx.gStart = gStart.data();
            modCtx.gEnd = gEnd.data();
        }
        else
        {
            matrixPtr = nullptr;
            slicePtr = nullptr;
            lfoCountBlock = 0;
        }
    }

    float WarprSynth::modulatedMasterGainDb (float baseDb) const noexcept
    {
        return applyModulation (ParamId::masterGain, baseDb, masterDelta);
    }

    void WarprSynth::readLfoPhases (float* out) const noexcept
    {
        for (int i = 0; i < kNumLfos; ++i)
            out[i] = lfoPhaseOut[(size_t) i].load (std::memory_order_relaxed);
    }

    /** Class N (polyphony, unisonVoices, glideTime) and ampAttack: the destination is sampled once, at note-on.  Sources: the
        global LFOs' current values; the voice LFOs' value at their start phase (a fresh note has not run yet). */
    float WarprSynth::latchedValue (ParamId p, float base) const noexcept
    {
        if (matrixPtr == nullptr || matrixPtr->empty() || ! matrixPtr->hasDestination (p))
            return base;

        float srcMod[kNumLfos];

        for (int j = 0; j < kNumLfos; ++j)
        {
            if (j >= lfoCountBlock)
                srcMod[j] = 0.0f;
            else if (((voiceOwnMask >> j) & 1u) != 0u)
                srcMod[j] = 2.0f * lfoStartValue (lfoParams[(size_t) j], tablePtrs[(size_t) j]) - 1.0f;
            else
                srcMod[j] = 2.0f * gEnd[(size_t) j] - 1.0f;
        }

        return applyModulation (p, base, matrixPtr->delta (p, srcMod));
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

    int WarprSynth::getOscillatorQuarterUnitsInUse() const noexcept
    {
        int quarter = 0;
        for (const auto& v : voices)
            if (v.isActive() && ! v.ghost)
                quarter += v.getOscCostQ() * v.getUnison();
        return quarter;
    }

    int WarprSynth::getOscillatorUnitsInUse() const noexcept
    {
        return (getOscillatorQuarterUnitsInUse() + 3) / 4;
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
            info.costQ = v.getOscCostQ();
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

        // D10 CPU guard: the oversampling factor LATCHED for this new note is capped by the guard level (8/4/2/1).  Level 0
        // is the identity, so the default path is unchanged.  Sounding voices are never touched.
        int os = osLatched;
        {
            const int cap = CpuGuard::osCap (guardLevel.load (std::memory_order_relaxed));

            if (os > cap)
            {
                os = cap;
                guardLowered.fetch_add (1, std::memory_order_relaxed);
            }
        }

        // D8: unisonVoices is a latched destination - the matrix is sampled here, at note-on
        int uni = juce::jlimit (1, WarprVoice::maxUnison, juce::roundToInt (latchedValue (ParamId::unisonVoices, (float) unisonLatched)));
        const int budget = oscBudget.load (std::memory_order_relaxed);

        // D7b: a granular note is charged by its grain cost (see the header); Classic playback keeps cost = os.
        // (a slice note is Classic playback whatever playMode says, so it is charged as Classic)
        const int costQ = oscillatorCostQ (os, vp.playMode == 1 && ! sliceNoteActive, grainsPerChannel (vp.grainMode, vp.grainDensity));

        if (budget > 0)
        {
            for (int guard = 0; guard < totalSlots + 2; ++guard)
            {
                const int fit = (budget * 4 - getOscillatorQuarterUnitsInUse()) / costQ;

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
        info.oscCostQ = costQ;
        info.glideFromSemis = lastPlayedPitch >= 0.0 ? lastPlayedPitch : (double) note;
        info.glideMs = sliceNoteActive ? 0.0 : glideNow();         // a slice voice is unpitched: nothing to glide
        info.age = ++noteCounter;
        info.startStamp = sampleClock + (std::uint64_t) blockPos;
        info.slice = sliceNoteActive;
        info.sliceLo = sliceNoteLo;
        info.sliceHi = sliceNoteHi;

        slot->start (info, vp, currentSample, modEngine != nullptr ? &modCtx : nullptr);
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

        // One-Shot choke (owner bug report 2026-11-16: "a second playhead line ... moves slower
        // than the playhead").  A latched one-shot voice keeps playing after its key is released,
        // so each new tap used to STACK another full-length copy: a new One-Shot note therefore
        // cuts every other free-running one-shot first, drum-pad style.  Notes started inside the
        // 30 ms window keep sounding - a chord lands in one gesture.  Slice voices keep their own
        // rules (same-note ghost above; polyphonic across pads).
        if (vp.loopMode == 2 && vp.playMode == 0 && ! sliceNoteActive)
        {
            const std::uint64_t now    = sampleClock + (std::uint64_t) blockPos;
            const std::uint64_t window = (std::uint64_t) std::llround (0.030 * sampleRate);

            for (auto& v : voices)
                if (v.isActive() && ! v.ghost && v.isTriggerLatched() && ! v.isSliceVoice()
                    && now - v.startedAt() > window)
                    makeGhost (v, ghostFadeSamples());
        }

        // D8: polyphony is a latched destination - sampled here, at note-on (live reduction below uses the plain value)
        const int limit = juce::jlimit (1, maxVoices, juce::roundToInt (latchedValue (ParamId::polyphony, (float) polyLimit)));

        while (getNumNonGhostVoices() >= limit)
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

        // D11: a slice note never retargets / retriggers (it has no pitch); a slice voice is never retargeted by a later
        // ordinary note either.  Both start a fresh voice through a ghost fade.
        if (cur != nullptr && cur->isSounding() && ! sliceNoteActive && ! cur->isSliceVoice())
        {
            if (legato && keysWereAlive)
            {
                cur->retarget (note, glideNow());              // overlap: pitch only
                return;
            }

            if (! legato && vp.loopMode != 2)
            {
                cur->retarget (note, glideNow());              // envelope retrigger from the current level
                cur->retriggerEnvelope (attackNow());
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
        // D11 SLICE MODE: the map published for this block decides.  Outside the slice keyboard nothing plays (and the note
        // never enters the note stack - the key is simply not mapped).
        int slice = -1;

        if (slicePtr != nullptr && slicePtr->on)
        {
            slice = slicePtr->sliceFor (note);

            if (slice < 0)
                return;
        }

        const bool keysWereAlive = stackCount > 0;
        pushNote (note);

        if (currentSample == nullptr)
            return;

        if (! keysWereAlive)
            monoResetPending = true;                 // D8: a new phrase restarts the MONO LFOs (at the next chunk)

        if (slice >= 0)
        {
            sliceNoteActive = true;                  // latched into the voice by startNewVoice (the map is immutable this block)
            sliceNoteLo = slicePtr->edges[(size_t) slice];
            sliceNoteHi = slicePtr->edges[(size_t) slice + 1];
        }

        if (voiceMode == 0)
            startPoly (note, velocity);
        else
            startMono (note, velocity, voiceMode == 2, keysWereAlive);

        sliceNoteActive = false;

        if (slice < 0)
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
            v->releaseKey();                                  // a one-shot trigger voice ignores it (plays to its end)
        }
        else if (v->note == removedNote && ! v->isTriggerLatched())    // a trigger voice keeps its pitch: no fall-back to the stack
        {
            v->retarget ((int) stack[(size_t) (stackCount - 1)], glideNow());

            if (voiceMode == 1 && vp.loopMode != 2)
                v->retriggerEnvelope (attackNow());
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
            v->releaseKey();
        }
        else if (! inStack (v->note) && ! v->isTriggerLatched())
        {
            v->retarget ((int) stack[(size_t) (stackCount - 1)], glideNow());

            if (voiceMode == 1 && vp.loopMode != 2)
                v->retriggerEnvelope (attackNow());
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
                    v.releaseKey();
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
                    v.releaseKey();
                }
        }
        else
        {
            monoReevaluate();
        }
    }

    void WarprSynth::allNotesOff() noexcept
    {
        // CC123 is a panic: it releases one-shot trigger voices too (release(), not releaseKey()).
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

        // Live reduction happens when the polyphony PARAMETER changes.  Running it every block would also cancel a
        // note-on limit raised by modulation (the latched class N: voices admitted above the plain value must live).
        if (voiceMode == 0 && polyLimit != lastPolyLimit)
        {
            while (getNumNonGhostVoices() > polyLimit)
            {
                auto* victim = pickVictim();
                if (victim == nullptr)
                    break;
                makeGhost (*victim, ghostFadeSamples());
            }
        }

        lastPolyLimit = polyLimit;
    }

    void WarprSynth::renderRange (float* outL, float* outR, int n) noexcept
    {
        if (n <= 0)
            return;

        // 32-sample chunks, chunk-outer / voice-inner: the global LFOs advance once per chunk and every voice sees the same
        // chunk boundaries it always had (a voice renders chunk k of the range exactly as render() would have).
        for (int off = 0; off < n; off += WarprVoice::chunkSize)
        {
            const int m = std::min (WarprVoice::chunkSize, n - off);
            const ModContext* mc = beginChunkModulation (m);

            for (auto& v : voices)
                if (v.isActive())
                    v.renderModulated (vp, outL + off, outR + off, m, off, mc);

            endChunkModulation();
            blockPos += m;
        }

        if (shaper != nullptr)
        {
            shaper->advance (n);                  // A/B-set crossfade progress, in base samples
            syncShaperToVoices();
        }

        harvestFinishedVoices();
    }

    //==========================================================================
    // D8: per-chunk modulation bookkeeping
    //==========================================================================
    const ModContext* WarprSynth::beginChunkModulation (int m) noexcept
    {
        if (modEngine == nullptr || lfoCountBlock <= 0)
            return nullptr;

        if (monoResetPending)
        {
            for (int i = 0; i < kNumLfos; ++i)
                if (lfoParams[(size_t) i].mode == (int) LfoMode::Mono)
                    gInst[(size_t) i].restart();

            monoResetPending = false;
        }

        LfoChunkTiming t;
        t.n = m;
        t.fs = sampleRate;
        t.ppq0 = clock.ppqAt (blockPos);
        t.ppq1 = clock.ppqAt (blockPos + m);
        t.bpm = clock.bpm();
        t.barBeats = clock.barBeats();

        LfoBankIO io;
        io.params = lfoParams.data();
        io.tables = tablePtrs.data();
        io.mm = matrixPtr;
        io.lfoCount = lfoCountBlock;
        io.timing = t;
        io.ownMask = globalOwnMask;
        io.extStart = voiceMean.data();                      // voice LFOs reach the global bank as a one-chunk-late mean
        io.extEnd = voiceMean.data();

        advanceLfoBank (io, gInst.data(), gPrev.data(), gStart.data(), gEnd.data());

        modCtx.timing = t;
        return &modCtx;
    }

    void WarprSynth::endChunkModulation() noexcept
    {
        if (modEngine == nullptr || matrixPtr == nullptr || matrixPtr->empty())
        {
            masterDelta = 0.0f;
            return;
        }

        // mean of the voice-owned LFOs over the sounding voices (feeds the global bank and masterGain one chunk later)
        const std::uint32_t need = voiceOwnMask & matrixPtr->usedSourceMask();

        for (int i = 0; i < kNumLfos; ++i)
        {
            if (((need >> i) & 1u) == 0u)
                continue;

            float sum = 0.0f;
            int count = 0;

            for (const auto& v : voices)
                if (v.isSounding())
                {
                    sum += v.lfoEndValue (i);
                    ++count;
                }

            voiceMean[(size_t) i] = count > 0 ? sum / (float) count : 0.5f;
        }

        if (matrixPtr->hasDestination (ParamId::masterGain))
        {
            float srcMod[kNumLfos];

            for (int j = 0; j < kNumLfos; ++j)
                srcMod[j] = j < lfoCountBlock ? 2.0f * gEnd[(size_t) j] - 1.0f : 0.0f;    // gEnd[j] of a voice LFO = its mean

            masterDelta = matrixPtr->delta (ParamId::masterGain, srcMod);
        }
        else
        {
            masterDelta = 0.0f;
        }
    }

    void WarprSynth::publishLfoPhases() noexcept
    {
        const WarprVoice* newest = nullptr;

        for (const auto& v : voices)
            if (v.isSounding() && (newest == nullptr || v.age > newest->age))
                newest = &v;

        for (int i = 0; i < kNumLfos; ++i)
        {
            float ph = -1.0f;

            if (modEngine != nullptr && matrixPtr != nullptr && i < lfoCountBlock && ((matrixPtr->usedSourceMask() >> i) & 1u) != 0u)
            {
                if (((voiceOwnMask >> i) & 1u) != 0u)
                {
                    if (newest != nullptr)
                        ph = newest->lfoPhase (i);
                }
                else
                {
                    ph = gInst[(size_t) i].phase;
                }
            }

            lfoPhaseOut[(size_t) i].store (ph, std::memory_order_relaxed);
        }
    }

    void WarprSynth::publishPlayheads() noexcept
    {
        int n = 0;
        for (const auto& v : voices)
        {
            if (n >= maxVoices)
                break;
            if (! v.isSounding())
                continue;
            playheadPos[(size_t) n].store (v.playheadNorm(), std::memory_order_relaxed);
            playheadLevel[(size_t) n].store (v.envLevel(), std::memory_order_relaxed);
            ++n;
        }
        playheadCount.store (n, std::memory_order_relaxed);

        // sounding grains of the granular voices (oscillator 0 of each), for the waveform grain cloud
        int ng = 0;
        for (const auto& v : voices)
        {
            if (ng >= maxVizGrains)
                break;
            if (! v.isSounding())
                continue;

            float gp[maxVizGrains], gl[maxVizGrains];
            const int c = v.collectGrains (gp, gl, maxVizGrains - ng);

            for (int i = 0; i < c; ++i)
            {
                grainPos[(size_t) ng].store (gp[i], std::memory_order_relaxed);
                grainLevel[(size_t) ng].store (gl[i], std::memory_order_relaxed);
                ++ng;
            }
        }
        grainCount.store (ng, std::memory_order_relaxed);
    }

    int WarprSynth::readGrains (float* posOut, float* levelOut, int maxOut) const noexcept
    {
        const int n = std::min (std::min (grainCount.load (std::memory_order_relaxed), maxVizGrains), maxOut);
        for (int i = 0; i < n; ++i)
        {
            posOut[i]   = grainPos[(size_t) i].load (std::memory_order_relaxed);
            levelOut[i] = grainLevel[(size_t) i].load (std::memory_order_relaxed);
        }
        return n;
    }

    int WarprSynth::readPlayheads (float* posOut, float* levelOut, int maxOut) const noexcept
    {
        const int n = std::min (std::min (playheadCount.load (std::memory_order_relaxed), maxVoices), maxOut);
        for (int i = 0; i < n; ++i)
        {
            posOut[i]   = playheadPos[(size_t) i].load (std::memory_order_relaxed);
            levelOut[i] = playheadLevel[(size_t) i].load (std::memory_order_relaxed);
        }
        return n;
    }

    void WarprSynth::renderBlock (float* outL, float* outR, int numSamples, const juce::MidiBuffer& midi) noexcept
    {
        flushBacklog();
        applyModeAndPolyphonyChanges();

        clock.beginBlock (transportInfo, numSamples);
        blockPos = 0;

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

        publishPlayheads();
        publishLfoPhases();
        sampleClock += (std::uint64_t) numSamples;
    }
}
