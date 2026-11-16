#include "DSP/WarprVoice.h"

#include <algorithm>
#include <cmath>

namespace warpr
{
    namespace
    {
        RegionFold::Mode modeFromParam (int loopMode) noexcept
        {
            switch (loopMode)
            {
                case 1:  return RegionFold::Mode::PingPong;
                case 2:  return RegionFold::Mode::OneShot;
                default: return RegionFold::Mode::Forward;
            }
        }

        inline void snapNear (float& value, float target, float tolerance) noexcept
        {
            if (std::abs (target - value) < tolerance)
                value = target;
        }

        /** Source frames per root-note cycle:  Lc = fileSampleRate / midiToHz(root). */
        inline double cycleFrames (double fileSampleRate, double rootSemitones) noexcept
        {
            return fileSampleRate / (440.0 * std::exp2 ((rootSemitones - 69.0) / 12.0));
        }

    }

    /** Filter targets for a chunk: the cutoff follows the voice's glide-smoothed note pitch by `keyTrack`
        (1 = one-to-one, anchored at note 60).  The stage clamps the result to [10 Hz, 0.45 fs].  D8: every
        continuous filter parameter carries its modulation (chunk-end value; the stage interpolates). */
    FilterTargets WarprVoice::makeFilterTargets (const VoiceParams& vp, double pitchSemis) const noexcept
    {
        FilterTargets ft;
        ft.enable = vp.filterEnable;
        ft.type = vp.filterType;
        ft.cutoffHz = (float) filterdesign::keyTrackedCutoff ((double) mod1 (ParamId::filterCutoff, vp.filterCutoffHz),
                                                              (double) mod1 (ParamId::filterKeyTrack, vp.filterKeyTrack), pitchSemis);
        ft.res = mod1 (ParamId::filterResonance, vp.filterRes);
        ft.driveDb = mod1 (ParamId::filterDrive, vp.filterDriveDb);
        ft.morph = mod1 (ParamId::filterMorph, vp.filterMorph);
        ft.mix = mod1 (ParamId::filterMix, vp.filterMix);
        return ft;
    }

    /** A slice voice is always a One-Shot read (whatever loopMode says); every other voice follows the parameter. */
    RegionFold::Mode WarprVoice::regionMode (const VoiceParams& vp) const noexcept
    {
        return sliceLatched ? RegionFold::Mode::OneShot : modeFromParam (vp.loopMode);
    }

    //==========================================================================
    void WarprVoice::prepare (double newSampleRate, const HalfbandCoeffs& coeffs) noexcept
    {
        sampleRate = newSampleRate;
        hb = &coeffs;
        dcCoeff = (float) (1.0 - 6.283185307179586 * 5.0 / newSampleRate);
        filter.prepare (newSampleRate);
        env.prepare (newSampleRate);
        reset();
    }

    void WarprVoice::reset() noexcept
    {
        active = false;
        ghost = false;
        keyHeld = false;
        pedalHeld = false;
        oneShotReleased = false;
        triggerLatched = false;
        sliceLatched = false;
        killedByGuard = false;
        note = -1;
        age = 0;
        velGain = 1.0f;
        glideLeft = 0;
        fadeLeft = fadeTotal = 0;
        bendCur = offsetCur = 0.0f;
        granWCur = scanCur = scanDistCur = 0.0f;
        dcxL = dcxR = dcyL = dcyR = 0.0f;
        region = RegionFold {};

        for (auto& o : osc)
            o = UnisonOsc {};

        prevGainL.fill (0.0f);
        prevGainR.fill (0.0f);
        decimator.reset();
        filter.reset();
        env.reset();

        for (auto& l : lfoInst)
            l = LfoInstance {};

        lfoPrev.fill (0.5f);
        lfoStart.fill (0.5f);
        lfoEnd.fill (0.5f);
        modD0.fill (0.0f);
        modD1.fill (0.0f);
        modActive = false;
    }

    //==========================================================================
    void WarprVoice::start (const StartInfo& info, const VoiceParams& vp, SampleData* ownedSample, const ModContext* mc) noexcept
    {
        reset();

        // D8: voice-owned LFOs start at their phase offset; the latched destinations (ampAttack) see the start values
        if (mc != nullptr && mc->mm != nullptr && ! mc->mm->empty() && mc->params != nullptr)
            startModulation (*mc);

        sample = ownedSample;
        active = true;
        note = info.note;
        age = info.age;
        keyHeld = true;
        // One-Shot TRIGGER latch (Classic playback only: grains have no natural end, so granular One-Shot stays gated).
        sliceLatched = info.slice;
        triggerLatched = sliceLatched || (vp.loopMode == 2 && vp.playMode == 0);     // D11: a slice voice is always a trigger
        velGain = 0.25f + 0.75f * juce::jlimit (0.0f, 1.0f, info.velocity);   // fixed velocity law (A2)
        os = juce::jlimit (1, maxOs, info.os);
        unison = juce::jlimit (1, maxUnison, info.unison);
        oscCostQ = std::max (1, info.oscCostQ);

        if (sliceLatched)
        {
            // UNPITCHED: the note selects the slice, not the pitch - the voice sits at the root note (transpose / fineTune /
            // bend still move it; renderChunk keeps pitchCur on the CURRENT root so a root edit mid-slice cannot detune it)
            pitchCur = (double) vp.rootNote;
            setGlideTarget (pitchCur, 0.0);
        }
        else
        {
            pitchCur = info.glideMs > 0.0 ? info.glideFromSemis : (double) info.note;
            setGlideTarget ((double) info.note, info.glideMs);
        }

        bendCur = vp.bendSemis;
        offsetCur = (float) vp.transpose + vp.fineTuneCents * 0.01f - (float) vp.rootNote;
        spreadCur = vp.unisonSpread;
        detuneCur = vp.unisonDetuneCents;
        startCur = sliceLatched ? info.sliceLo : vp.sampleStart;       // slice region: latched, never chases the parameters
        endCur = sliceLatched ? info.sliceHi : vp.sampleEnd;

        region.configure (*sample, startCur, endCur, regionMode (vp));

        warpAmtCur = vp.warpAmount;
        warpACur = vp.warpA;
        warpBCur = vp.warpB;
        warpMixCur = vp.warpMix;
        warpEnCur = vp.warpEnable ? 1.0f : 0.0f;
        rootCur = (float) vp.rootNote;
        modRatioCur = vp.modOscRatio;
        shaperMorphCur = vp.shaperMorph;
        shaperMixCur = vp.shaperMix;
        shaperEnCur = vp.shaperEnable ? 1.0f : 0.0f;
        granWCur = (vp.playMode == 1 && ! sliceLatched) ? 1.0f : 0.0f;    // a note starts in the current play mode (no fade); slices are Classic
        scanCur = vp.grainScan;
        scanDistCur = vp.grainScanDist;

        const bool reverse = vp.direction == 1;
        const float norm = 1.0f / std::sqrt ((float) unison);
        const double lc = cycleFrames (sample->fileSampleRate, (double) rootCur);

        for (int u = 0; u < unison; ++u)
        {
            float gl, gr;
            balanceGains (spreadCur * unisonPosition (u, unison), gl, gr);
            prevGainL[(size_t) u] = gl * norm;
            prevGainR[(size_t) u] = gr * norm;
            // grain PRNG seed: deterministic from the note counter and the oscillator index (no wall-clock entropy)
            const std::uint32_t seed = static_cast<std::uint32_t> ((info.age + 1u) * 2654435761u)
                                       ^ (static_cast<std::uint32_t> (u + 1) * 0x9E3779B9u);
            osc[(size_t) u].start (region, reverse, u, lc, seed);
        }

        decimator.reset();
        filter.snap (makeFilterTargets (vp, pitchCur));        // adopt the current settings, no ramp from stale values

        env.setParams (mod1 (ParamId::ampDecay, vp.decayMs), mod1 (ParamId::ampSustain, vp.sustain),
                       mod1 (ParamId::ampRelease, vp.releaseMs));
        env.snapSustain();
        env.noteOn (mod1 (ParamId::ampAttack, vp.attackMs));
    }

    //==========================================================================
    // D8 modulation
    //==========================================================================
    void WarprVoice::startModulation (const ModContext& mc) noexcept
    {
        const int count = std::min (mc.lfoCount, (int) kNumLfos);

        for (int i = 0; i < kNumLfos; ++i)
        {
            const bool owned = i < count && ((mc.voiceOwnMask >> i) & 1u) != 0u;
            lfoPrev[(size_t) i] = owned ? lfoStartValue (mc.params[i], mc.tables != nullptr ? mc.tables[i] : nullptr)
                                        : (mc.gEnd != nullptr ? mc.gEnd[i] : 0.5f);
            lfoStart[(size_t) i] = lfoEnd[(size_t) i] = lfoPrev[(size_t) i];
        }

        mc.mm->evaluate (lfoStart.data(), lfoEnd.data(), count, modD0.data(), modD1.data());
        modActive = true;
    }

    /** Advances the voice-owned LFOs by this chunk and evaluates the matrix: modD0 / modD1 = delta at chunk start / end. */
    void WarprVoice::beginModulation (const ModContext* mc) noexcept
    {
        modActive = false;

        if (mc == nullptr || mc->mm == nullptr || mc->mm->empty() || mc->params == nullptr)
            return;

        LfoBankIO io;
        io.params = mc->params;
        io.tables = mc->tables;
        io.mm = mc->mm;
        io.lfoCount = mc->lfoCount;
        io.timing = mc->timing;
        io.ownMask = mc->voiceOwnMask & mc->mm->usedSourceMask();
        io.extStart = mc->gStart;
        io.extEnd = mc->gEnd;

        advanceLfoBank (io, lfoInst.data(), lfoPrev.data(), lfoStart.data(), lfoEnd.data());
        mc->mm->evaluate (lfoStart.data(), lfoEnd.data(), std::min (mc->lfoCount, (int) kNumLfos), modD0.data(), modD1.data());
        modActive = true;
    }

    //==========================================================================
    void WarprVoice::setGlideTarget (double target, double glideMs) noexcept
    {
        pitchTarget = target;

        if (glideMs < 0.5 || juce::exactlyEqual (target, pitchCur))
        {
            pitchCur = target;
            glideLeft = 0;
            return;
        }

        glideLeft = std::max (1, (int) std::lround (glideMs * 0.001 * sampleRate));
        glideRate = (target - pitchCur) / (double) glideLeft;
    }

    void WarprVoice::advanceGlide (int n) noexcept
    {
        if (glideLeft <= 0)
            return;

        const int a = std::min (n, glideLeft);
        pitchCur += glideRate * (double) a;
        glideLeft -= a;

        if (glideLeft == 0)
            pitchCur = pitchTarget;
    }

    void WarprVoice::retarget (int newNote, double glideMs) noexcept
    {
        note = newNote;
        setGlideTarget ((double) newNote, glideMs);
    }

    void WarprVoice::retriggerEnvelope (float attackMs) noexcept
    {
        keyHeld = true;
        pedalHeld = false;
        oneShotReleased = false;
        env.noteOn (attackMs);
        restartLfos();                       // D8: a Mono retrigger is a new note for the RETRIG / ENV LFOs
    }

    void WarprVoice::setVelocity (float velocity) noexcept
    {
        velGain = 0.25f + 0.75f * juce::jlimit (0.0f, 1.0f, velocity);
    }

    void WarprVoice::release() noexcept
    {
        if (env.isActive() && env.getState() != AmpEnvelope::State::Release)
            env.noteOff();
    }

    void WarprVoice::beginFade (int samples) noexcept
    {
        ghost = true;
        keyHeld = false;
        pedalHeld = false;
        fadeTotal = fadeLeft = std::max (1, samples);
    }

    void WarprVoice::kill() noexcept
    {
        active = false;
        ghost = false;
        keyHeld = false;
        pedalHeld = false;
        fadeLeft = fadeTotal = 0;
        env.reset();
    }

    //==========================================================================
    void WarprVoice::render (const VoiceParams& vp, float* outL, float* outR, int numSamples) noexcept
    {
        int done = 0;

        while (done < numSamples && active)
        {
            const int n = std::min (chunkSize, numSamples - done);
            renderChunk (vp, outL + done, outR + done, n, done, nullptr);
            done += n;
        }
    }

    void WarprVoice::renderChunk (const VoiceParams& vp, float* outL, float* outR, int n, int rangeOffset,
                                  const ModContext* mc) noexcept
    {
        const SampleData& sd = *sample;
        const double nD = (double) n;

        // A trigger voice has no key gate, so it must be able to end by itself.  If the loop mode leaves One-Shot or the play
        // mode becomes Granular mid-note, the region read no longer ends: drop the latch and hand the voice back to the keys
        // (a key that was already let go releases now).  Never re-latched mid-note.
        // (A slice voice is exempt: it is a One-Shot Classic read by construction, whatever the parameters say.)
        if (triggerLatched && ! sliceLatched && (vp.loopMode != 2 || vp.playMode != 0))
        {
            triggerLatched = false;

            if (! keyHeld && ! pedalHeld)
                release();
        }

        auto slewCoef = [&] (double seconds) { return (float) (1.0 - std::exp (-nD / (seconds * sampleRate))); };
        const float k3 = slewCoef (0.003), k5 = slewCoef (0.005), k20 = slewCoef (0.020);

        // ---- D8: advance the voice-owned LFOs and evaluate the matrix (modD0 / modD1 = delta at chunk start / end)
        beginModulation (mc);

        // ---- pitch at the start of the chunk, then advance every smoother to the chunk end.  The matrix acts on the
        //      smoothed values: transpose / fineTune / rootNote offsets are evaluated at the chunk start and end (Int
        //      destinations round, so they can step - the per-sample step ramps linearly across the chunk).
        auto pitchModSemis = [&] (bool atEnd) -> double
        {
            if (! modActive)
                return 0.0;

            auto m = [&] (ParamId id, float v) { return atEnd ? mod1 (id, v) : mod0 (id, v); };
            return (double) (m (ParamId::transpose, (float) vp.transpose) - (float) vp.transpose)
                   + 0.01 * (double) (m (ParamId::fineTune, vp.fineTuneCents) - vp.fineTuneCents)
                   - (double) (m (ParamId::rootNote, (float) vp.rootNote) - (float) vp.rootNote);
        };

        if (sliceLatched)
        {
            pitchCur = pitchTarget = (double) vp.rootNote;        // D11: a slice voice is unpitched (note-independent)
            glideLeft = 0;
        }

        const double p0 = pitchCur + (double) bendCur + (double) offsetCur + pitchModSemis (false);

        advanceGlide (n);

        const float offsetTarget = (float) vp.transpose + vp.fineTuneCents * 0.01f - (float) vp.rootNote;
        bendCur += (vp.bendSemis - bendCur) * k3;
        offsetCur += (offsetTarget - offsetCur) * k5;
        spreadCur += (vp.unisonSpread - spreadCur) * k20;
        detuneCur += (vp.unisonDetuneCents - detuneCur) * k20;
        if (! sliceLatched)
        {
            startCur += (vp.sampleStart - startCur) * k20;
            endCur += (vp.sampleEnd - endCur) * k20;
        }

        snapNear (bendCur, vp.bendSemis, 1.0e-6f);
        snapNear (offsetCur, offsetTarget, 1.0e-6f);
        snapNear (spreadCur, vp.unisonSpread, 1.0e-6f);
        snapNear (detuneCur, vp.unisonDetuneCents, 1.0e-5f);

        if (! sliceLatched)
        {
            snapNear (startCur, vp.sampleStart, 1.0e-6f);
            snapNear (endCur, vp.sampleEnd, 1.0e-6f);
        }

        // ---- warp stage parameters: amount / A / B / mix one-pole 10 ms per chunk, enable linear ramp
        //      5 ms, root note 5 ms.  The span ramps linearly from the chunk-start to the chunk-end value.
        const float k10 = slewCoef (0.010);
        const float warpAmt0 = mod0 (ParamId::warpAmount, warpAmtCur), warpA0 = mod0 (ParamId::warpParamA, warpACur),
                    warpB0 = mod0 (ParamId::warpParamB, warpBCur);
        const float warpMix0 = mod0 (ParamId::warpMix, warpMixCur) * warpEnCur;

        warpAmtCur += (vp.warpAmount - warpAmtCur) * k10;
        warpACur += (vp.warpA - warpACur) * k10;
        warpBCur += (vp.warpB - warpBCur) * k10;
        warpMixCur += (vp.warpMix - warpMixCur) * k10;
        snapNear (warpAmtCur, vp.warpAmount, 1.0e-6f);
        snapNear (warpACur, vp.warpA, 1.0e-6f);
        snapNear (warpBCur, vp.warpB, 1.0e-6f);
        snapNear (warpMixCur, vp.warpMix, 1.0e-6f);

        const float enStep = (float) (nD / (0.005 * sampleRate));
        warpEnCur = vp.warpEnable ? std::min (1.0f, warpEnCur + enStep) : std::max (0.0f, warpEnCur - enStep);

        rootCur += ((float) vp.rootNote - rootCur) * k5;
        snapNear (rootCur, (float) vp.rootNote, 1.0e-6f);

        // modOscRatio: multiplicative (log-domain) one-pole, 15 ms
        const float modRatio0 = mod0 (ParamId::modOscRatio, modRatioCur);
        {
            const float ratioTarget = juce::jlimit (0.25f, 16.0f, vp.modOscRatio);
            const float k15 = slewCoef (0.015);
            const float lc = std::log2 (modRatioCur), lt = std::log2 (ratioTarget);
            modRatioCur = std::exp2 (lc + (lt - lc) * k15);
            snapNear (modRatioCur, ratioTarget, 1.0e-5f);
        }

        // rootNote modulation moves the cycle length too (Int destination: rounds, so it can step)
        const float rootEff = modActive ? rootCur + (mod1 (ParamId::rootNote, (float) vp.rootNote) - (float) vp.rootNote) : rootCur;

        WarpSpan warpSpan;
        warpSpan.mode = vp.warpMode;
        warpSpan.a0 = warpAmt0;   warpSpan.a1 = mod1 (ParamId::warpAmount, warpAmtCur);
        warpSpan.A0 = warpA0;     warpSpan.A1 = mod1 (ParamId::warpParamA, warpACur);
        warpSpan.B0 = warpB0;     warpSpan.B1 = mod1 (ParamId::warpParamB, warpBCur);
        warpSpan.mix0 = warpMix0; warpSpan.mix1 = mod1 (ParamId::warpMix, warpMixCur) * warpEnCur;
        warpSpan.active = warpSpan.mix0 > 0.0f || warpSpan.mix1 > 0.0f;
        warpSpan.Lc = cycleFrames (sd.fileSampleRate, (double) rootEff);
        warpSpan.fadeLen = std::max (1, (int) std::lround (0.010 * sampleRate * (double) os));
        warpSpan.os = os;
        warpSpan.osRate = sampleRate * (double) os;
        warpSpan.modSource = vp.modOscSource;
        warpSpan.ratio0 = modRatio0;
        warpSpan.ratio1 = mod1 (ParamId::modOscRatio, modRatioCur);
        warpSpan.curve = vp.shaperCur;               // Phase Remapping reads the shaper curves even while the shaper is off (A4)

        // ---- shaper stage parameters: morph / mix one-pole 10 ms per chunk, enable linear ramp 5 ms.
        //      The A/B-set crossfade weight is a function of ABSOLUTE time since the swap (global, base samples),
        //      so every voice agrees however its chunks are aligned.
        const float shaperMorph0 = mod0 (ParamId::shaperMorph, shaperMorphCur);
        const float shaperMix0 = mod0 (ParamId::shaperMix, shaperMixCur) * shaperEnCur;

        shaperMorphCur += (vp.shaperMorph - shaperMorphCur) * k10;
        shaperMixCur += (vp.shaperMix - shaperMixCur) * k10;
        snapNear (shaperMorphCur, vp.shaperMorph, 1.0e-6f);
        snapNear (shaperMixCur, vp.shaperMix, 1.0e-6f);
        shaperEnCur = vp.shaperEnable ? std::min (1.0f, shaperEnCur + enStep) : std::max (0.0f, shaperEnCur - enStep);

        ShaperSpan shaperSpan;
        shaperSpan.cur = vp.shaperCur;
        shaperSpan.prev = vp.shaperPrev;
        shaperSpan.morph0 = shaperMorph0;  shaperSpan.morph1 = mod1 (ParamId::shaperMorph, shaperMorphCur);
        shaperSpan.mix0 = shaperMix0;      shaperSpan.mix1 = mod1 (ParamId::shaperMix, shaperMixCur) * shaperEnCur;
        shaperSpan.active = shaperSpan.cur != nullptr && (shaperSpan.mix0 > 0.0f || shaperSpan.mix1 > 0.0f);

        if (vp.shaperPrev != nullptr && vp.shaperFadeLen > 0)
        {
            const float invLen = 1.0f / (float) vp.shaperFadeLen;
            shaperSpan.w0 = juce::jlimit (0.0f, 1.0f, (float) (vp.shaperFadeBase + rangeOffset) * invLen);
            shaperSpan.w1 = juce::jlimit (0.0f, 1.0f, (float) (vp.shaperFadeBase + rangeOffset + n) * invLen);
        }

        const double p1 = pitchCur + (double) bendCur + (double) offsetCur + pitchModSemis (true);

        // D8: region bounds and the unison macros carry their modulation (chunk-end value; the gains / read step interpolate)
        // (a slice voice's region is the latched slice: the sampleStart / sampleEnd modulation does not move it)
        const float startEff = sliceLatched ? startCur : mod1 (ParamId::sampleStart, startCur);
        const float endEff = sliceLatched ? endCur : mod1 (ParamId::sampleEnd, endCur);
        const float spreadEff = mod1 (ParamId::unisonSpread, spreadCur), detuneEff = mod1 (ParamId::unisonDetune, detuneCur);

        region.configure (sd, startEff, endEff, regionMode (vp));

        // ---- granular source (D7a): playMode weight is a linear 10 ms ramp (both sources run while 0 < w < 1);
        //      scan / scan distance are one-pole smoothed 20 ms per chunk (the other grain parameters are latched
        //      by the engine at every spawn).
        const float granTarget = (vp.playMode == 1 && ! sliceLatched) ? 1.0f : 0.0f;       // slices stay Classic
        const float granStep = (float) (nD / (0.010 * sampleRate));
        const float granW0 = granWCur;
        granWCur = granTarget > granWCur ? std::min (granTarget, granWCur + granStep)
                                         : std::max (granTarget, granWCur - granStep);

        scanCur += (vp.grainScan - scanCur) * k20;
        scanDistCur += (vp.grainScanDist - scanDistCur) * k20;
        snapNear (scanCur, vp.grainScan, 1.0e-6f);
        snapNear (scanDistCur, vp.grainScanDist, 1.0e-6f);

        GrainSpan grainSpan;
        grainSpan.active = granW0 > 0.0f || granWCur > 0.0f;
        grainSpan.w0 = granW0;
        grainSpan.w1 = granWCur;
        grainSpan.mode = vp.grainMode;
        grainSpan.position = mod1 (ParamId::grainPosition, vp.grainPosition);
        grainSpan.scan = mod1 (ParamId::grainScan, scanCur);
        grainSpan.scanDist = mod1 (ParamId::grainScanDist, scanDistCur);
        grainSpan.scanLoop = vp.grainScanLoop;
        grainSpan.scanKey = vp.grainScanKey;
        grainSpan.sizeMs = mod1 (ParamId::grainSize, vp.grainSizeMs);
        grainSpan.shape = mod1 (ParamId::grainShape, vp.grainShape);
        grainSpan.variation = mod1 (ParamId::grainVariation, vp.grainVariation);
        grainSpan.spread = mod1 (ParamId::grainSpread, vp.grainSpread);
        grainSpan.reverse = mod1 (ParamId::grainReverse, vp.grainReverse);
        grainSpan.density = juce::roundToInt (mod1 (ParamId::grainDensity, (float) vp.grainDensity));
        grainSpan.reverseDir = vp.direction == 1;
        grainSpan.pitchSemis = pitchCur;
        grainSpan.sampleRate = sampleRate;
        grainSpan.hb = hb;
        grainSpan.scratch = &grainScratch;

        // per-grain phase warp (D7b): the grains apply the phase-domain modes themselves, with the same smoothed
        // amount / A / B / mix ramps as the oscillator-level stage (amplitude-domain modes stay in that stage)
        grainSpan.warp.active = warpSpan.active;
        grainSpan.warp.mode = warpSpan.mode;
        grainSpan.warp.a0 = warpSpan.a0;     grainSpan.warp.a1 = warpSpan.a1;
        grainSpan.warp.A0 = warpSpan.A0;     grainSpan.warp.A1 = warpSpan.A1;
        grainSpan.warp.B0 = warpSpan.B0;     grainSpan.warp.B1 = warpSpan.B1;
        grainSpan.warp.mix0 = warpSpan.mix0; grainSpan.warp.mix1 = warpSpan.mix1;
        grainSpan.warp.Lc = warpSpan.Lc;
        grainSpan.warp.modSource = warpSpan.modSource;
        grainSpan.warp.ratio0 = warpSpan.ratio0; grainSpan.warp.ratio1 = warpSpan.ratio1;
        grainSpan.warp.curve = warpSpan.curve;

        // ---- oscillators -> OS-rate bus
        const int nOS = n * os;
        std::fill_n (busL.begin(), nOS, 0.0f);
        std::fill_n (busR.begin(), nOS, 0.0f);

        const double rate = sd.fileSampleRate / sampleRate;
        const float norm = 1.0f / std::sqrt ((float) unison);
        bool allEnded = true;

        for (int u = 0; u < unison; ++u)
        {
            const double det = (double) (detuneEff * unisonPosition (u, unison)) * 0.01;
            const double rho0 = std::exp2 ((p0 + det) / 12.0) * rate;
            const double rho1 = std::exp2 ((p1 + det) / 12.0) * rate;

            float gl, gr;
            balanceGains (spreadEff * unisonPosition (u, unison), gl, gr);
            gl *= norm;
            gr *= norm;

            auto& o = osc[(size_t) u];
            float& pgl = prevGainL[(size_t) u];
            float& pgr = prevGainR[(size_t) u];

            const bool readable = rho0 <= maxRho && rho1 <= maxRho && rho0 > 0.0 && rho1 > 0.0;

            if (readable)
                o.render (sd, region, vp.interpQuality, rho0, rho1, os, pgl, pgr, gl, gr,
                          busL.data(), busR.data(), nOS, &warpSpan, &shaperSpan, &grainSpan);

            pgl = gl;
            pgr = gr;
            // A trigger voice whose oscillator is muted (rate above maxRho) would never reach the region end and, with the
            // key gate ignored, would never free: count the muted oscillator as ended for latched voices only.
            allEnded = allEnded && (o.ended || (triggerLatched && ! readable));
        }

        // One-Shot: when every oscillator has run off its end the voice releases over 5 ms
        // (the fade into the end already removed the step; this frees the voice promptly).
        if (allEnded && region.mode == RegionFold::Mode::OneShot && ! oneShotReleased)
        {
            oneShotReleased = true;
            env.fastRelease (5.0f);
        }

        // ---- sanity guard: a non-finite or runaway bus kills the voice (never reaches the master)
        bool bad = false;

        for (int k = 0; k < nOS; ++k)
        {
            if (! (std::abs (busL[(size_t) k]) < 64.0f) || ! (std::abs (busR[(size_t) k]) < 64.0f))
            {
                bad = true;
                break;
            }
        }

        if (bad)
        {
            killedByGuard = true;
            kill();
            return;
        }

        // ---- ONE decimator per voice (OS rate -> host rate)
        decimator.process (*hb, busL.data(), busR.data(), n, os);

        // ---- amp envelope
        env.setParams (mod1 (ParamId::ampDecay, vp.decayMs), mod1 (ParamId::ampSustain, vp.sustain),
                       mod1 (ParamId::ampRelease, vp.releaseMs));
        env.process (envBuf.data(), n);

        // ---- voice DC block (in place on the base-rate bus)
        for (int i = 0; i < n; ++i)
        {
            const float xl = busL[(size_t) i], xr = busR[(size_t) i];
            const float yl = xl - dcxL + dcCoeff * dcyL;
            const float yr = xr - dcxR + dcCoeff * dcyR;
            dcxL = xl;  dcyL = yl;
            dcxR = xr;  dcyR = yr;
            busL[(size_t) i] = yl;
            busR[(size_t) i] = yr;
        }

        // ---- filter stage (D5a): after the DC blocker, before the envelope.  Key tracking uses the glide-smoothed pitch.
        filter.process (busL.data(), busR.data(), n, makeFilterTargets (vp, pitchCur));

        // ---- envelope, velocity, ghost fade, accumulate
        for (int i = 0; i < n; ++i)
        {
            float g = envBuf[(size_t) i] * velGain;

            if (fadeTotal > 0)
            {
                g *= (float) fadeLeft / (float) fadeTotal;
                if (fadeLeft > 0)
                    --fadeLeft;
            }

            outL[i] += busL[(size_t) i] * g;
            outR[i] += busR[(size_t) i] * g;
        }

        if (! env.isActive() || (fadeTotal > 0 && fadeLeft == 0))
            kill();
    }
}
