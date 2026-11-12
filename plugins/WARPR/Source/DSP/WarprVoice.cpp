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

        /** Filter targets for a chunk: the cutoff follows the voice's glide-smoothed note pitch by `keyTrack`
            (1 = one-to-one, anchored at note 60).  The stage clamps the result to [10 Hz, 0.45 fs]. */
        inline FilterTargets filterTargetsFor (const VoiceParams& vp, double pitchSemis) noexcept
        {
            FilterTargets ft;
            ft.enable = vp.filterEnable;
            ft.type = vp.filterType;
            ft.cutoffHz = (float) filterdesign::keyTrackedCutoff ((double) vp.filterCutoffHz, (double) vp.filterKeyTrack, pitchSemis);
            ft.res = vp.filterRes;
            ft.driveDb = vp.filterDriveDb;
            ft.morph = vp.filterMorph;
            ft.mix = vp.filterMix;
            return ft;
        }
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
        killedByGuard = false;
        note = -1;
        age = 0;
        velGain = 1.0f;
        glideLeft = 0;
        fadeLeft = fadeTotal = 0;
        bendCur = offsetCur = 0.0f;
        dcxL = dcxR = dcyL = dcyR = 0.0f;
        region = RegionFold {};

        for (auto& o : osc)
            o = UnisonOsc {};

        prevGainL.fill (0.0f);
        prevGainR.fill (0.0f);
        decimator.reset();
        filter.reset();
        env.reset();
    }

    //==========================================================================
    void WarprVoice::start (const StartInfo& info, const VoiceParams& vp, SampleData* ownedSample) noexcept
    {
        reset();

        sample = ownedSample;
        active = true;
        note = info.note;
        age = info.age;
        keyHeld = true;
        velGain = 0.25f + 0.75f * juce::jlimit (0.0f, 1.0f, info.velocity);   // fixed velocity law (A2)
        os = juce::jlimit (1, maxOs, info.os);
        unison = juce::jlimit (1, maxUnison, info.unison);

        pitchCur = info.glideMs > 0.0 ? info.glideFromSemis : (double) info.note;
        setGlideTarget ((double) info.note, info.glideMs);

        bendCur = vp.bendSemis;
        offsetCur = (float) vp.transpose + vp.fineTuneCents * 0.01f - (float) vp.rootNote;
        spreadCur = vp.unisonSpread;
        detuneCur = vp.unisonDetuneCents;
        startCur = vp.sampleStart;
        endCur = vp.sampleEnd;

        region.configure (*sample, startCur, endCur, modeFromParam (vp.loopMode));

        warpAmtCur = vp.warpAmount;
        warpACur = vp.warpA;
        warpBCur = vp.warpB;
        warpMixCur = vp.warpMix;
        warpEnCur = vp.warpEnable ? 1.0f : 0.0f;
        rootCur = (float) vp.rootNote;
        shaperMorphCur = vp.shaperMorph;
        shaperMixCur = vp.shaperMix;
        shaperEnCur = vp.shaperEnable ? 1.0f : 0.0f;

        const bool reverse = vp.direction == 1;
        const float norm = 1.0f / std::sqrt ((float) unison);
        const double lc = cycleFrames (sample->fileSampleRate, (double) rootCur);

        for (int u = 0; u < unison; ++u)
        {
            float gl, gr;
            balanceGains (spreadCur * unisonPosition (u, unison), gl, gr);
            prevGainL[(size_t) u] = gl * norm;
            prevGainR[(size_t) u] = gr * norm;
            osc[(size_t) u].start (region, reverse, u, lc);
        }

        decimator.reset();
        filter.snap (filterTargetsFor (vp, pitchCur));         // adopt the current settings, no ramp from stale values

        env.setParams (vp.decayMs, vp.sustain, vp.releaseMs);
        env.snapSustain();
        env.noteOn (vp.attackMs);
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
            renderChunk (vp, outL + done, outR + done, n, done);
            done += n;
        }
    }

    void WarprVoice::renderChunk (const VoiceParams& vp, float* outL, float* outR, int n, int rangeOffset) noexcept
    {
        const SampleData& sd = *sample;
        const double nD = (double) n;

        auto slewCoef = [&] (double seconds) { return (float) (1.0 - std::exp (-nD / (seconds * sampleRate))); };
        const float k3 = slewCoef (0.003), k5 = slewCoef (0.005), k20 = slewCoef (0.020);

        // ---- pitch at the start of the chunk, then advance every smoother to the chunk end
        const double p0 = pitchCur + (double) bendCur + (double) offsetCur;

        advanceGlide (n);

        const float offsetTarget = (float) vp.transpose + vp.fineTuneCents * 0.01f - (float) vp.rootNote;
        bendCur += (vp.bendSemis - bendCur) * k3;
        offsetCur += (offsetTarget - offsetCur) * k5;
        spreadCur += (vp.unisonSpread - spreadCur) * k20;
        detuneCur += (vp.unisonDetuneCents - detuneCur) * k20;
        startCur += (vp.sampleStart - startCur) * k20;
        endCur += (vp.sampleEnd - endCur) * k20;

        snapNear (bendCur, vp.bendSemis, 1.0e-6f);
        snapNear (offsetCur, offsetTarget, 1.0e-6f);
        snapNear (spreadCur, vp.unisonSpread, 1.0e-6f);
        snapNear (detuneCur, vp.unisonDetuneCents, 1.0e-5f);
        snapNear (startCur, vp.sampleStart, 1.0e-6f);
        snapNear (endCur, vp.sampleEnd, 1.0e-6f);

        // ---- warp stage parameters: amount / A / B / mix one-pole 10 ms per chunk, enable linear ramp
        //      5 ms, root note 5 ms.  The span ramps linearly from the chunk-start to the chunk-end value.
        const float k10 = slewCoef (0.010);
        const float warpAmt0 = warpAmtCur, warpA0 = warpACur, warpB0 = warpBCur;
        const float warpMix0 = warpMixCur * warpEnCur;

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

        WarpSpan warpSpan;
        warpSpan.mode = vp.warpMode;
        warpSpan.a0 = warpAmt0;   warpSpan.a1 = warpAmtCur;
        warpSpan.A0 = warpA0;     warpSpan.A1 = warpACur;
        warpSpan.B0 = warpB0;     warpSpan.B1 = warpBCur;
        warpSpan.mix0 = warpMix0; warpSpan.mix1 = warpMixCur * warpEnCur;
        warpSpan.active = warpSpan.mix0 > 0.0f || warpSpan.mix1 > 0.0f;
        warpSpan.Lc = cycleFrames (sd.fileSampleRate, (double) rootCur);
        warpSpan.fadeLen = std::max (1, (int) std::lround (0.010 * sampleRate * (double) os));
        warpSpan.os = os;
        warpSpan.osRate = sampleRate * (double) os;

        // ---- shaper stage parameters: morph / mix one-pole 10 ms per chunk, enable linear ramp 5 ms.
        //      The A/B-set crossfade weight is a function of ABSOLUTE time since the swap (global, base samples),
        //      so every voice agrees however its chunks are aligned.
        const float shaperMorph0 = shaperMorphCur;
        const float shaperMix0 = shaperMixCur * shaperEnCur;

        shaperMorphCur += (vp.shaperMorph - shaperMorphCur) * k10;
        shaperMixCur += (vp.shaperMix - shaperMixCur) * k10;
        snapNear (shaperMorphCur, vp.shaperMorph, 1.0e-6f);
        snapNear (shaperMixCur, vp.shaperMix, 1.0e-6f);
        shaperEnCur = vp.shaperEnable ? std::min (1.0f, shaperEnCur + enStep) : std::max (0.0f, shaperEnCur - enStep);

        ShaperSpan shaperSpan;
        shaperSpan.cur = vp.shaperCur;
        shaperSpan.prev = vp.shaperPrev;
        shaperSpan.morph0 = shaperMorph0;  shaperSpan.morph1 = shaperMorphCur;
        shaperSpan.mix0 = shaperMix0;      shaperSpan.mix1 = shaperMixCur * shaperEnCur;
        shaperSpan.active = shaperSpan.cur != nullptr && (shaperSpan.mix0 > 0.0f || shaperSpan.mix1 > 0.0f);

        if (vp.shaperPrev != nullptr && vp.shaperFadeLen > 0)
        {
            const float invLen = 1.0f / (float) vp.shaperFadeLen;
            shaperSpan.w0 = juce::jlimit (0.0f, 1.0f, (float) (vp.shaperFadeBase + rangeOffset) * invLen);
            shaperSpan.w1 = juce::jlimit (0.0f, 1.0f, (float) (vp.shaperFadeBase + rangeOffset + n) * invLen);
        }

        const double p1 = pitchCur + (double) bendCur + (double) offsetCur;

        region.configure (sd, startCur, endCur, modeFromParam (vp.loopMode));

        // ---- oscillators -> OS-rate bus
        const int nOS = n * os;
        std::fill_n (busL.begin(), nOS, 0.0f);
        std::fill_n (busR.begin(), nOS, 0.0f);

        const double rate = sd.fileSampleRate / sampleRate;
        const float norm = 1.0f / std::sqrt ((float) unison);
        bool allEnded = true;

        for (int u = 0; u < unison; ++u)
        {
            const double det = (double) (detuneCur * unisonPosition (u, unison)) * 0.01;
            const double rho0 = std::exp2 ((p0 + det) / 12.0) * rate;
            const double rho1 = std::exp2 ((p1 + det) / 12.0) * rate;

            float gl, gr;
            balanceGains (spreadCur * unisonPosition (u, unison), gl, gr);
            gl *= norm;
            gr *= norm;

            auto& o = osc[(size_t) u];
            float& pgl = prevGainL[(size_t) u];
            float& pgr = prevGainR[(size_t) u];

            if (rho0 <= maxRho && rho1 <= maxRho && rho0 > 0.0 && rho1 > 0.0)
                o.render (sd, region, vp.interpQuality, rho0, rho1, os, pgl, pgr, gl, gr,
                          busL.data(), busR.data(), nOS, &warpSpan, &shaperSpan);

            pgl = gl;
            pgr = gr;
            allEnded = allEnded && o.ended;
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
        env.setParams (vp.decayMs, vp.sustain, vp.releaseMs);
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
        filter.process (busL.data(), busR.data(), n, filterTargetsFor (vp, pitchCur));

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
