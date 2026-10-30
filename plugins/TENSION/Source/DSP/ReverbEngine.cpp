#include "ReverbEngine.h"

#include <algorithm>
#include <cmath>

#include "OutputSafety.h"

void ReverbEngine::prepare (double newSampleRate)
{
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 44100.0;
    maxHz      = (float) (0.45 * sampleRate);

    juce::dsp::ProcessSpec spec;
    spec.sampleRate       = sampleRate;
    spec.maximumBlockSize = (juce::uint32) kChunk;
    spec.numChannels      = 2;

    reverb.prepare (spec);                       // modern API (Pattern #17)

    preDelay.setMaximumDelayInSamples ((int) std::ceil (kMaxPreDelaySec * sampleRate) + 8);
    preDelay.prepare (spec);

    sendHpf.prepare (spec);
    returnLpf.prepare (spec);
    sendHpf.setType (juce::dsp::StateVariableTPTFilterType::highpass);
    returnLpf.setType (juce::dsp::StateVariableTPTFilterType::lowpass);
    sendHpf.setResonance (0.70710678f);
    returnLpf.setResonance (0.70710678f);

    work.setSize (2, kChunk, false, true, false);

    sendSm.reset  (sampleRate, kSendSmoothSec);
    roomSm.reset  (sampleRate, kParamSmoothSec);
    dampSm.reset  (sampleRate, kParamSmoothSec);
    widthSm.reset (sampleRate, kParamSmoothSec);
    hpfSm.reset   (sampleRate, kFilterSmoothSec);
    lpfSm.reset   (sampleRate, kFilterSmoothSec);
    retSm.prepare (sampleRate, kReturnAttackSec, kReturnReleaseSec);

    preCoef   = 1.0 - std::exp (-1.0 / (kParamSmoothSec * sampleRate));
    quietHold = std::max (kChunk, (int) (kQuietHoldSec * sampleRate));

    reset();
}

void ReverbEngine::clearState() noexcept
{
    reverb.reset();
    preDelay.reset();
    sendHpf.reset();
    returnLpf.reset();
    work.clear();
}

void ReverbEngine::reset()
{
    clearState();

    const Targets d {};
    sendSm.setCurrentAndTargetValue  (0.0f);
    roomSm.setCurrentAndTargetValue  (d.roomSize);
    dampSm.setCurrentAndTargetValue  (d.damping);
    widthSm.setCurrentAndTargetValue (d.width);
    hpfSm.setCurrentAndTargetValue   (d.sendHpfHz);
    lpfSm.setCurrentAndTargetValue   (d.returnLpfHz);
    retSm.setCurrentAndTarget (0.0f);

    preSamples = preTarget = 0.0;
    quietRun   = quietHold;               // empty == quiet
    failed.store (false, std::memory_order_relaxed);

    sendHpf.setCutoffFrequency (d.sendHpfHz);
    returnLpf.setCutoffFrequency (d.returnLpfHz);

    juce::dsp::Reverb::Parameters p;
    p.roomSize = d.roomSize; p.damping = d.damping; p.width = d.width;
    p.wetLevel = 1.0f; p.dryLevel = 0.0f; p.freezeMode = 0.0f;
    reverb.setParameters (p);
}

void ReverbEngine::setTargets (const Targets& t, bool snap, int controlSamples)
{
    const float send  = std::clamp (t.sendGain,   0.0f, 1.0f);
    const float ret   = t.returnGain < 1.0e-6f ? 0.0f : std::min (t.returnGain, 2.0f);   // exact-zero snapping
    const float room  = std::clamp (t.roomSize,   0.0f, 1.0f);
    const float damp  = std::clamp (t.damping,    0.0f, 1.0f);
    const float width = std::clamp (t.width,      0.0f, 1.0f);
    const float pre   = std::clamp (t.preDelayMs, 0.0f, (float) (kMaxPreDelaySec * 1000.0));
    const float lpf   = std::clamp (t.returnLpfHz, 500.0f, maxHz);
    const float hpf   = std::clamp (t.sendHpfHz,   20.0f,  maxHz);

    preTarget = (double) pre * 0.001 * sampleRate;

    if (snap)
    {
        sendSm.setCurrentAndTargetValue  (send);
        roomSm.setCurrentAndTargetValue  (room);
        dampSm.setCurrentAndTargetValue  (damp);
        widthSm.setCurrentAndTargetValue (width);
        hpfSm.setCurrentAndTargetValue   (hpf);
        lpfSm.setCurrentAndTargetValue   (lpf);
        retSm.setCurrentAndTarget (ret);
        preSamples = preTarget;
    }
    else
    {
        sendSm.setTargetValue  (send);
        roomSm.setTargetValue  (room);
        dampSm.setTargetValue  (damp);
        widthSm.setTargetValue (width);
        hpfSm.setTargetValue   (hpf);
        lpfSm.setTargetValue   (lpf);
        retSm.setTarget (ret);

        roomSm.skip (controlSamples);
        dampSm.skip (controlSamples);
        widthSm.skip (controlSamples);
        hpfSm.skip (controlSamples);
        lpfSm.skip (controlSamples);
    }

    sendHpf.setCutoffFrequency (hpfSm.getCurrentValue());
    returnLpf.setCutoffFrequency (lpfSm.getCurrentValue());

    // Once per control period, with smoothed values. Always 100% wet internally.
    juce::dsp::Reverb::Parameters p;
    p.roomSize   = roomSm.getCurrentValue();
    p.damping    = dampSm.getCurrentValue();
    p.width      = widthSm.getCurrentValue();
    p.wetLevel   = 1.0f;
    p.dryLevel   = 0.0f;
    p.freezeMode = 0.0f;                  // never engaged
    reverb.setParameters (p);
}

void ReverbEngine::process (const float* inL, const float* inR, float* outL, float* outR, int n) noexcept
{
    for (int done = 0; done < n; done += kChunk)
    {
        const int c = std::min (kChunk, n - done);
        processChunk (inL + done, inR + done, outL + done, outR + done, c);
    }
}

void ReverbEngine::processChunk (const float* inL, const float* inR, float* outL, float* outR, int n) noexcept
{
    float* wl = work.getWritePointer (0);
    float* wr = work.getWritePointer (1);

    float inActivity = 0.0f;

    for (int i = 0; i < n; ++i)
    {
        const float sw = sendSm.getNextValue();
        const float xl = sendHpf.processSample (0, inL[i] * sw);
        const float xr = sendHpf.processSample (1, inR[i] * sw);

        preSamples += preCoef * (preTarget - preSamples);
        const float d = std::max (1.0f, (float) preSamples);      // pop-then-push: min 1 sample

        const float pl = preDelay.popSample (0, d);
        const float pr = preDelay.popSample (1, d);
        preDelay.pushSample (0, xl);
        preDelay.pushSample (1, xr);

        wl[i] = pl;
        wr[i] = pr;
        inActivity = std::max (inActivity, std::max (std::abs (xl), std::abs (xr)));
    }

    // The reverb is ALWAYS processed (tails keep evolving).
    {
        float* chans[2] = { wl, wr };
        juce::dsp::AudioBlock<float> block (chans, 2, (size_t) n);
        juce::dsp::ProcessContextReplacing<float> ctx (block);
        reverb.process (ctx);
    }

    bool bad = false;
    for (int i = 0; i < n; ++i)
        if (! OutputSafety::isFinite (wl[i]) || ! OutputSafety::isFinite (wr[i]) || ! OutputSafety::isFinite (inActivity))
            bad = true;

    if (bad)
    {
        clearState();
        failed.store (true, std::memory_order_relaxed);

        for (int i = 0; i < n; ++i)
            outL[i] = outR[i] = 0.0f;

        quietRun = 0;
        return;
    }

    float outActivity = 0.0f;

    for (int i = 0; i < n; ++i)
    {
        const float yl = returnLpf.processSample (0, wl[i]);
        const float yr = returnLpf.processSample (1, wr[i]);
        const float rg = retSm.next();

        outL[i] = yl * rg;
        outR[i] = yr * rg;
        outActivity = std::max (outActivity, std::max (std::abs (yl), std::abs (yr)));
    }

    if (inActivity < kQuietThreshold && outActivity < kQuietThreshold)
        quietRun = std::min (quietRun + n, quietHold + kChunk);
    else
        quietRun = 0;
}
