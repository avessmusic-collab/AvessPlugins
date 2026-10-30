#include "DelayEngine.h"

#include <algorithm>
#include <cmath>

#include "OutputSafety.h"

double DelayEngine::computeDelayMs (bool sync, double bpm, int riseModeIndex) noexcept
{
    const int mode = std::clamp (riseModeIndex, 0, 2);

    if (! sync)
    {
        constexpr double fixedMs[3] = { 560.0, 375.0, 190.0 };
        return fixedMs[mode];
    }

    // dotted 1/4 = 1.5 beats, dotted 1/8 = 0.75 beats, 1/16 triplet = 1/6 beat
    constexpr double beats[3] = { 1.5, 0.75, 1.0 / 6.0 };

    const double safeBpm = std::clamp (bpm > 0.0 ? bpm : 120.0, 20.0, 400.0);
    double ms = 60000.0 / safeBpm * beats[mode];

    while (ms > kMaxDelaySec * 1000.0)
        ms *= 0.5;

    return ms;
}

void DelayEngine::prepare (double newSampleRate)
{
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 44100.0;
    maxHz      = (float) (0.45 * sampleRate);

    const double bufferRate = std::max (sampleRate, kBufferRate);
    const int maxSamples = (int) std::ceil (kBufferSec * bufferRate) + 8;     // 2.5 s @ 192 kHz per line

    juce::dsp::ProcessSpec mono { sampleRate, 32, 1 };
    lineL.setMaximumDelayInSamples (maxSamples);
    lineR.setMaximumDelayInSamples (maxSamples);
    lineL.prepare (mono);
    lineR.prepare (mono);

    juce::dsp::ProcessSpec stereo { sampleRate, 32, 2 };
    fbHpf.prepare (stereo);
    fbLpf.prepare (stereo);
    fbHpf.setType (juce::dsp::StateVariableTPTFilterType::highpass);
    fbLpf.setType (juce::dsp::StateVariableTPTFilterType::lowpass);
    fbHpf.setResonance (0.70710678f);
    fbLpf.setResonance (0.70710678f);
    fbHpf.setCutoffFrequency (kFbHpfHz);

    sendSm.reset  (sampleRate, kSendSmoothSec);
    fbSm.reset    (sampleRate, kFeedbackSmoothSec);
    depthSm.reset (sampleRate, 0.050);
    lpfSm.reset   (sampleRate, 0.060);
    retSm.prepare (sampleRate, kReturnAttackSec, kReturnReleaseSec);

    slewCoef        = 1.0 - std::exp (-1.0 / (kTimeSlewSec * sampleRate));
    velCoef         = 1.0 - std::exp (-1.0 / (kSlewVelSmoothSec * sampleRate));
    maxDelaySamples = (float) (kMaxDelaySec * sampleRate) + 1.0f;
    quietHold       = std::max (32, (int) (kQuietHoldSec * sampleRate));

    reset();
}

void DelayEngine::resetLoop() noexcept
{
    lineL.reset();
    lineR.reset();
    fbHpf.reset();
    fbLpf.reset();
}

void DelayEngine::reset()
{
    resetLoop();

    sendSm.setCurrentAndTargetValue  (0.0f);
    fbSm.setCurrentAndTargetValue    (0.0f);
    depthSm.setCurrentAndTargetValue (0.0f);
    lpfSm.setCurrentAndTargetValue   (9000.0f);
    retSm.setCurrentAndTarget (0.0f);

    fbHpf.setCutoffFrequency (kFbHpfHz);
    fbLpf.setCutoffFrequency (9000.0f);

    timeSnap = true;                 // next setTargets() jumps straight to the target time
    quietRun = quietHold;            // empty == quiet
    failed.store (false, std::memory_order_relaxed);
}

void DelayEngine::setTargets (const Targets& t, bool snap, int controlSamples)
{
    const float send  = std::clamp (t.sendGain,   0.0f, 1.0f);
    const float ret   = t.returnGain < 1.0e-6f ? 0.0f : std::min (t.returnGain, 2.0f);   // exact-zero snapping
    const float fb    = std::clamp (t.feedback,   0.0f, kHardFeedbackCap);              // HARD CAP 0.85
    const float lpf   = std::clamp (t.fbLpfHz,    200.0f, maxHz);
    const float depth = std::clamp (t.lfoDepthMs, 0.0f, 2.0f);
    const float ms    = std::clamp (t.delayMs,    1.0f, (float) (kMaxDelaySec * 1000.0));

    targetDelay = (double) ms * 0.001 * sampleRate;

    if (snap || timeSnap)
    {
        curDelay = targetDelay;
        slewVel  = 0.0;
        timeSnap = false;
    }

    if (snap)
    {
        sendSm.setCurrentAndTargetValue  (send);
        fbSm.setCurrentAndTargetValue    (fb);
        depthSm.setCurrentAndTargetValue (depth);
        lpfSm.setCurrentAndTargetValue   (lpf);
        retSm.setCurrentAndTarget (ret);
    }
    else
    {
        sendSm.setTargetValue  (send);
        fbSm.setTargetValue    (fb);
        depthSm.setTargetValue (depth);
        lpfSm.setTargetValue   (lpf);
        retSm.setTarget (ret);

        lpfSm.skip (controlSamples);
    }

    fbLpf.setCutoffFrequency (lpfSm.getCurrentValue());
}

void DelayEngine::process (const float* inL, const float* inR, float* outL, float* outR, int n, const float* lfo) noexcept
{
    const float msToSamples = (float) (0.001 * sampleRate);

    for (int i = 0; i < n; ++i)
    {
        const float sw  = sendSm.getNextValue();
        const float fb  = std::min (fbSm.getNextValue(), kHardFeedbackCap);
        const float rg  = retSm.next();
        const float dep = depthSm.getNextValue();

        // tape glide, per sample: the 250 ms one-pole slew gives the desired delay velocity, which is
        // clamped to +-0.9 samples/sample (read speed never drops below 0.1: no stall / reversal) and
        // low-passed (20 ms) so the read speed has no step at the moment of a time jump (a plain
        // one-pole starts with a velocity STEP = a slope kink in the output).
        const double wantVel = std::clamp (slewCoef * (targetDelay - curDelay), -kMaxSlewRate, kMaxSlewRate);
        slewVel  += velCoef * (wantVel - slewVel);
        curDelay += slewVel;

        const float wob = lfo != nullptr ? lfo[i] * dep * msToSamples : 0.0f;
        const float dL  = std::clamp ((float) curDelay + wob, 2.0f, maxDelaySamples);
        const float dR  = std::clamp ((float) curDelay - wob, 2.0f, maxDelaySamples);

        // each line is popped exactly once per sample, BEFORE the push (Pattern #25)
        const float yL = lineL.popSample (0, dL, true);
        const float yR = lineR.popSample (0, dR, true);

        // cross feedback: R out -> L in, L out -> R in
        float fL = fbLpf.processSample (0, fbHpf.processSample (0, yR));
        float fR = fbLpf.processSample (1, fbHpf.processSample (1, yL));
        fL = std::tanh (fL);
        fR = std::tanh (fR);

        const float in = 0.5f * (inL[i] + inR[i]) * sw;
        float wL = in + fb * fL;
        float wR = fb * fR;

        if (! OutputSafety::isFinite (wL) || ! OutputSafety::isFinite (wR)
            || ! OutputSafety::isFinite (yL) || ! OutputSafety::isFinite (yR))
        {
            resetLoop();
            failed.store (true, std::memory_order_relaxed);
            quietRun = 0;
            outL[i] = 0.0f;
            outR[i] = 0.0f;
            continue;
        }

        if (std::abs (wL) < 1.0e-20f) wL = 0.0f;       // denormal flush on the feedback write
        if (std::abs (wR) < 1.0e-20f) wR = 0.0f;

        lineL.pushSample (0, wL);
        lineR.pushSample (0, wR);

        outL[i] = yL * rg;
        outR[i] = yR * rg;

        if (std::abs (wL) < kQuietThreshold && std::abs (wR) < kQuietThreshold)
            quietRun = std::min (quietRun + 1, quietHold + 1);
        else
            quietRun = 0;
    }
}
