#include "FilterEngine.h"

#include <algorithm>
#include <cmath>

void FilterEngine::prepare (double newSampleRate)
{
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 44100.0;
    maxHz = (float) (kMaxHzRel * sampleRate);

    juce::dsp::ProcessSpec spec;
    spec.sampleRate       = sampleRate;
    spec.maximumBlockSize = 32;
    spec.numChannels      = 2;

    hpFilter.prepare (spec);
    bpFilter.prepare (spec);

    hpFilter.setType (juce::dsp::StateVariableTPTFilterType::highpass);
    bpFilter.setType (juce::dsp::StateVariableTPTFilterType::bandpass);
    hpFilter.setResonance (0.70710678f);

    hpSm.reset (sampleRate, kHpSmoothSec);
    bpSm.reset (sampleRate, kBpSmoothSec);
    qSm.reset  (sampleRate, kQSmoothSec);
    blendSm.reset (sampleRate, kHpSmoothSec);
    depthSm.reset (sampleRate, kHpSmoothSec);

    reset();
}

void FilterEngine::reset()
{
    hpFilter.reset();
    bpFilter.reset();

    hpSm.setCurrentAndTargetValue (kMinHz);
    bpSm.setCurrentAndTargetValue (300.0f);
    qSm.setCurrentAndTargetValue  (0.9f);
    blendSm.setCurrentAndTargetValue (0.0f);
    depthSm.setCurrentAndTargetValue (0.0f);

    blendNow = 0.0f;
    bpNorm   = 1.0f / 0.9f;
    hpFilter.setCutoffFrequency (kMinHz);
    bpFilter.setCutoffFrequency (300.0f);
    bpFilter.setResonance (0.9f);
}

void FilterEngine::setTargets (const Targets& t, float lfo, bool snap, int controlSamples)
{
    const float hp    = std::clamp (t.hpHz,       kMinHz, maxHz);
    const float bp    = std::clamp (t.bpCenterHz, kMinHz, maxHz);
    const float q     = std::clamp (t.bpQ,        kMinQ,  kMaxQ);
    const float blend = std::clamp (t.bpBlend,    0.0f,   1.0f);
    const float depth = std::clamp (t.lfoDepthOct, 0.0f,  4.0f);

    if (snap)
    {
        hpSm.setCurrentAndTargetValue (hp);
        bpSm.setCurrentAndTargetValue (bp);
        qSm.setCurrentAndTargetValue  (q);
        blendSm.setCurrentAndTargetValue (blend);
        depthSm.setCurrentAndTargetValue (depth);
    }
    else
    {
        hpSm.setTargetValue (hp);
        bpSm.setTargetValue (bp);
        qSm.setTargetValue  (q);
        blendSm.setTargetValue (blend);
        depthSm.setTargetValue (depth);

        hpSm.skip (controlSamples);
        bpSm.skip (controlSamples);
        qSm.skip  (controlSamples);
        blendSm.skip (controlSamples);
        depthSm.skip (controlSamples);
    }

    const float mod = std::exp2 (depthSm.getCurrentValue() * std::clamp (lfo, -1.0f, 1.0f));

    appliedHp = std::clamp (hpSm.getCurrentValue() * mod, kMinHz, maxHz);
    appliedBp = std::clamp (bpSm.getCurrentValue() * mod, kMinHz, maxHz);
    appliedQ  = std::clamp (qSm.getCurrentValue(), kMinQ, kMaxQ);

    hpFilter.setCutoffFrequency (appliedHp);
    bpFilter.setCutoffFrequency (appliedBp);
    bpFilter.setResonance (appliedQ);

    blendNow = blendSm.getCurrentValue();
    bpNorm   = 1.0f / appliedQ;
}

void FilterEngine::process (float* l, float* r, int n)
{
    if (blendNow > 0.0f)
    {
        for (int i = 0; i < n; ++i)
        {
            const float hl = hpFilter.processSample (0, l[i]);
            const float hr = hpFilter.processSample (1, r[i]);
            l[i] = hl + blendNow * (bpFilter.processSample (0, hl) * bpNorm);
            r[i] = hr + blendNow * (bpFilter.processSample (1, hr) * bpNorm);
        }
    }
    else
    {
        // keep the BP states following the HP output so an emerging blend does not start from stale state
        for (int i = 0; i < n; ++i)
        {
            const float hl = hpFilter.processSample (0, l[i]);
            const float hr = hpFilter.processSample (1, r[i]);
            (void) bpFilter.processSample (0, hl);
            (void) bpFilter.processSample (1, hr);
            l[i] = hl;
            r[i] = hr;
        }
    }
}
