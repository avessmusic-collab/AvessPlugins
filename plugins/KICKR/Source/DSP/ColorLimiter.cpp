#include "DSP/ColorLimiter.h"

#include <cmath>
#include <vector>

namespace kickr
{
    void ColorLimiter::prepare (double fsOversampled)
    {
        delayCap = kMaxDelay;
        delayL.assign (static_cast<size_t> (delayCap), 0.0f);
        delayR.assign (static_cast<size_t> (delayCap), 0.0f);
        dqCap = kMaxDelay;
        dqVal.assign (static_cast<size_t> (dqCap), 1.0f);
        dqIdx.assign (static_cast<size_t> (dqCap), 0);
        switchScratch.assign (static_cast<size_t> (2 * kMaxDelay), 0.0f);

        lookaheadSamples = 0;   // prepare(): no held content to carry over -> plain reset below
        updateOversampledRate (fsOversampled);
    }

    void ColorLimiter::updateOversampledRate (double newFsOversampled) noexcept
    {
        const double oldFs   = fs;
        const int    oldLook = lookaheadSamples;
        fs = juce::jmax (1.0, newFsOversampled);

        loudGain.reset (fs, 0.02);
        ceilSm.reset   (fs, 0.02);

        splitCoef = static_cast<float> (1.0 - std::exp (-juce::MathConstants<double>::twoPi * kSplitHz / fs));

        // Re-derive rate-dependent coefficients from whatever params were last set.
        Params keep;
        keep.enabled      = enabled;
        keep.loudnessDb   = dsputils::gainToDb (loudGain.getTargetValue());
        keep.ceilingDb    = dsputils::gainToDb (ceilSm.getTargetValue());
        keep.lookaheadMs  = lookaheadMsSet;
        keep.releaseMs    = releaseMsSet;
        keep.saturation01 = sat;
        keep.color01      = colorSet;
        setParams (keep);

        if (delayCap < 3)
            return;

        if (oldLook <= 0 || std::abs (oldFs - fs) < 1.0e-9)
        {
            reset();
            return;
        }

        // OS switch mid-note (audio thread): the delay line holds `oldLook` samples written
        // at the OLD rate. Clearing it (the first version) left `lookahead` samples of
        // silence that popped back in AFTER the engine's 64-sample switch fade — a step
        // (caught by the OS-switching slew test). Instead, resample the held content to
        // the new rate so the read-out stays continuous: newLook samples <- linear
        // interpolation over the last oldLook samples. Bounded O(newLook), no allocation.
        const int newLook = lookaheadSamples;
        for (int ch = 0; ch < 2; ++ch)
        {
            auto& d = ch == 0 ? delayL : delayR;
            float* tmp = switchScratch.data() + (ch == 0 ? 0 : kMaxDelay);
            for (int k = 0; k < newLook; ++k)
            {
                // position in the old content: 0 = oldest held sample, oldLook-1 = newest
                const double pos = static_cast<double> (k) * (static_cast<double> (oldLook - 1) / juce::jmax (1.0, static_cast<double> (newLook - 1)));
                const int    i0  = static_cast<int> (std::floor (pos));
                const float  f   = static_cast<float> (pos - static_cast<double> (i0));
                const int    a   = (writePos - oldLook + i0 + delayCap) % delayCap;
                const int    b   = (a + 1) % delayCap;
                tmp[k] = d[static_cast<size_t> (a)] + f * (d[static_cast<size_t> (b)] - d[static_cast<size_t> (a)]);
            }
            for (int k = 0; k < newLook; ++k)
                d[static_cast<size_t> ((writePos - newLook + k + delayCap) % delayCap)] = tmp[k];
        }

        // The seam between the held (old-rate) content and the first new-rate sample
        // carries whatever time-shift the oversampler's own latency change introduced.
        // Fade the held content out over its last `junctionFade` samples and the new
        // content in over the same length — a 64-base-sample-equivalent, capped at the
        // look-ahead.
        junctionFade    = juce::jmax (1, juce::jmin (newLook, static_cast<int> (std::lround (fs * 64.0 / 48000.0))));
        heldRemaining   = newLook;
        fadeInRemaining = 0;

        // Window-min restarts (its indices are in samples at the old rate); the applied
        // gain `env` is kept so the gain itself doesn't jump.
        dqFront = 0; dqSize = 0; counter = 0;
        lpPost[0] = lpPost[1] = 0.0f;
        loudGain.setCurrentAndTargetValue (loudGain.getTargetValue());
        ceilSm.setCurrentAndTargetValue (ceilSm.getTargetValue());
    }

    void ColorLimiter::reset() noexcept
    {
        std::fill (delayL.begin(), delayL.end(), 0.0f);
        std::fill (delayR.begin(), delayR.end(), 0.0f);
        writePos = 0;
        dqFront  = 0;
        dqSize   = 0;
        counter  = 0;
        env      = 1.0f;
        heldRemaining = fadeInRemaining = junctionFade = 0;
        lpPost[0] = lpPost[1] = 0.0f;
        loudGain.setCurrentAndTargetValue (loudGain.getTargetValue());
        ceilSm.setCurrentAndTargetValue (ceilSm.getTargetValue());
    }

    void ColorLimiter::setParams (const Params& p) noexcept
    {
        enabled        = p.enabled;
        lookaheadMsSet = p.lookaheadMs;
        releaseMsSet   = p.releaseMs;
        colorSet       = p.color01;

        loudGain.setTargetValue (dsputils::dbToGain (juce::jlimit (0.0f, 24.0f, p.loudnessDb)));
        ceilSm.setTargetValue   (dsputils::dbToGain (juce::jlimit (-24.0f, 0.0f, p.ceilingDb)));

        const int newLook = juce::jmin (delayCap - 2, lookaheadSamplesFor (p.lookaheadMs, fs));
        lookaheadSamples  = juce::jmax (1, newLook);
        // Attack: reach the window-minimum in ~3 time constants over the look-ahead.
        attackCoef  = static_cast<float> (1.0 - std::exp (-3.0 / (double) lookaheadSamples));
        releaseCoef = static_cast<float> (1.0 - std::exp (-1.0 / (juce::jlimit (1.0f, 1000.0f, p.releaseMs) * 0.001 * fs)));

        sat       = juce::jlimit (0.0f, 1.0f, p.saturation01);
        drive     = 1.0f + (kMaxDrive - 1.0f) * sat;
        driveNorm = 1.0f / std::tanh (drive);
        tiltHi    = dsputils::dbToGain ((juce::jlimit (0.0f, 1.0f, p.color01) - 0.5f) * 2.0f * kColorTiltDb);
    }

    float ColorLimiter::saturate (float x, int ch) noexcept
    {
        if (sat <= 0.0f)
            return x;

        // Peak-normalised tanh shaper; the residual is the generated harmonic content.
        const float shaped = std::tanh (drive * x) * driveNorm;
        const float resid  = shaped - x;

        // COLOR: tilt the residual only — split @ kSplitHz, lows x 1/tiltHi, highs x tiltHi.
        lpPost[ch] += splitCoef * (resid - lpPost[ch]);
        dsputils::flushDenormal (lpPost[ch]);
        const float low  = lpPost[ch];
        const float high = resid - low;
        const float colored = low / tiltHi + high * tiltHi;

        return x + sat * colored;   // SAT = 0 -> exactly x (early-out above anyway)
    }

    void ColorLimiter::pushPeak (float gReq) noexcept
    {
        // Monotonic deque (increasing values front->back) over the last `lookaheadSamples`
        // entries; front is the window minimum.
        while (dqSize > 0)
        {
            const int back = (dqFront + dqSize - 1) % dqCap;
            if (dqVal[static_cast<size_t> (back)] >= gReq)
                --dqSize;
            else
                break;
        }
        const int slot = (dqFront + dqSize) % dqCap;
        dqVal[static_cast<size_t> (slot)] = gReq;
        dqIdx[static_cast<size_t> (slot)] = counter;
        ++dqSize;

        // Drop entries that have left the window.
        while (dqSize > 0 && dqIdx[static_cast<size_t> (dqFront)] <= counter - lookaheadSamples)
        {
            dqFront = (dqFront + 1) % dqCap;
            --dqSize;
        }
        ++counter;
    }

    void ColorLimiter::process (float& l, float& r) noexcept
    {
        if (! enabled || delayCap < 3)
        {
            loudGain.getNextValue(); ceilSm.getNextValue();   // keep smoothers ticking
            return;
        }

        const float ceil = ceilSm.getNextValue();
        const float lg   = loudGain.getNextValue();

        // 1 + 2: loudness, saturation.
        float xl = saturate (l * lg, 0);
        float xr = saturate (r * lg, 1);

        // 3: gain computer on the INPUT (future) sample, window-min over the look-ahead.
        const float peak = juce::jmax (std::abs (xl), std::abs (xr));
        const float gReq = peak > ceil ? ceil / peak : 1.0f;
        pushPeak (gReq);
        const float target = dqSize > 0 ? dqVal[static_cast<size_t> (dqFront)] : 1.0f;

        env += (target < env ? attackCoef : releaseCoef) * (target - env);
        dsputils::flushDenormal (env);

        // Delay line: write now, read `lookaheadSamples` back.
        delayL[static_cast<size_t> (writePos)] = xl;
        delayR[static_cast<size_t> (writePos)] = xr;
        const int readPos = (writePos - lookaheadSamples + delayCap) % delayCap;
        writePos = (writePos + 1) % delayCap;

        float yl = delayL[static_cast<size_t> (readPos)] * env;
        float yr = delayR[static_cast<size_t> (readPos)] * env;

        // OS-switch seam fade (normally inactive).
        if (heldRemaining > 0)
        {
            if (heldRemaining <= junctionFade)
            {
                const float g = static_cast<float> (heldRemaining) / static_cast<float> (junctionFade + 1);
                yl *= g; yr *= g;
            }
            if (--heldRemaining == 0)
                fadeInRemaining = junctionFade;
        }
        else if (fadeInRemaining > 0)
        {
            const float g = 1.0f - static_cast<float> (fadeInRemaining) / static_cast<float> (junctionFade + 1);
            yl *= g; yr *= g;
            --fadeInRemaining;
        }

        // 4: hard clamp at the ceiling — the guarantee (adds nothing below it).
        yl = juce::jlimit (-ceil, ceil, yl);
        yr = juce::jlimit (-ceil, ceil, yr);

        l = dsputils::sanitize (yl);
        r = dsputils::sanitize (yr);
    }
}
