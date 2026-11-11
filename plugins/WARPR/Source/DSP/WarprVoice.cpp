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
    }

    //==========================================================================
    void WarprVoice::prepare (double newSampleRate, const HalfbandCoeffs& coeffs) noexcept
    {
        sampleRate = newSampleRate;
        hb = &coeffs;
        dcCoeff = (float) (1.0 - 6.283185307179586 * 5.0 / newSampleRate);
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

        const bool reverse = vp.direction == 1;
        const float norm = 1.0f / std::sqrt ((float) unison);

        for (int u = 0; u < unison; ++u)
        {
            float gl, gr;
            balanceGains (spreadCur * unisonPosition (u, unison), gl, gr);
            prevGainL[(size_t) u] = gl * norm;
            prevGainR[(size_t) u] = gr * norm;
            osc[(size_t) u].start (region, reverse, u);
        }

        decimator.reset();

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
            renderChunk (vp, outL + done, outR + done, n);
            done += n;
        }
    }

    void WarprVoice::renderChunk (const VoiceParams& vp, float* outL, float* outR, int n) noexcept
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
                          busL.data(), busR.data(), nOS);

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

        // ---- DC block, envelope, velocity, ghost fade, accumulate
        for (int i = 0; i < n; ++i)
        {
            const float xl = busL[(size_t) i], xr = busR[(size_t) i];
            const float yl = xl - dcxL + dcCoeff * dcyL;
            const float yr = xr - dcxR + dcCoeff * dcyR;
            dcxL = xl;  dcyL = yl;
            dcxR = xr;  dcyR = yr;

            float g = envBuf[(size_t) i] * velGain;

            if (fadeTotal > 0)
            {
                g *= (float) fadeLeft / (float) fadeTotal;
                if (fadeLeft > 0)
                    --fadeLeft;
            }

            outL[i] += yl * g;
            outR[i] += yr * g;
        }

        if (! env.isActive() || (fadeTotal > 0 && fadeLeft == 0))
            kill();
    }
}
