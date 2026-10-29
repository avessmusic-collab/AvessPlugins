#include "TensionEngine.h"

#include <algorithm>
#include <cmath>

namespace
{
    constexpr double kTensionSmoothSec = 0.020;
    constexpr double kEngageSmoothSec  = 0.015;
    constexpr double kMixSmoothSec     = 0.020;
    constexpr double kTrimSmoothSec    = 0.020;

    // Exact-zero snapping: tiny targets become exactly 0 so t = 0 is bit-exact once settled.
    inline float snapZero (float v) noexcept
    {
        return std::abs (v) < 1.0e-6f ? 0.0f : v;
    }

    // wet = dry*(1-e) + proc*e, written so e == 0 returns the dry sample untouched
    // and e == 1 returns proc untouched.
    inline float wetBus (float d, float p, float e) noexcept
    {
        if (e == 0.0f) return d;
        if (e == 1.0f) return p;
        return d + e * (p - d);
    }

    // out = dry*(1-mix) + wet*mix, same exactness guarantees.
    inline float mixBus (float d, float w, float mix) noexcept
    {
        if (mix == 0.0f || w == d) return d;
        if (mix == 1.0f)           return w;
        return d + mix * (w - d);
    }

    inline float finiteOrZero (float x) noexcept
    {
        return OutputSafety::isFinite (x) ? x : 0.0f;
    }
}

void TensionEngine::prepare (double newSampleRate, int maxBlockSize, int /*numChannels*/)
{
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 44100.0;
    maxBlock   = std::max (1, maxBlockSize);

    // Internally always 2 channels.
    dryBuf.setSize  (2, maxBlock, false, true, false);
    procBuf.setSize (2, maxBlock, false, true, false);
    wetBuf.setSize  (2, maxBlock, false, true, false);

    tensionSm.reset (sampleRate, kTensionSmoothSec);
    engageSm.reset  (sampleRate, kEngageSmoothSec);
    safetySm.reset  (sampleRate, kEngageSmoothSec);
    mixSm.reset     (sampleRate, kMixSmoothSec);
    trimSm.reset    (sampleRate, kTrimSmoothSec);

    safety.prepare (sampleRate);

    prepared = true;
    reset();
}

void TensionEngine::reset()
{
    dryBuf.clear();
    procBuf.clear();
    wetBuf.clear();

    tensionSm.setCurrentAndTargetValue (0.0f);
    engageSm.setCurrentAndTargetValue  (0.0f);
    safetySm.setCurrentAndTargetValue  (0.0f);
    mixSm.setCurrentAndTargetValue     (1.0f);
    trimSm.setCurrentAndTargetValue    (1.0f);

    safety.reset();

    firstBlock = true;      // next block snaps smoothers to the incoming parameters
    controlPos = 0;
    peakHold   = 0.0f;
}

bool TensionEngine::isAtRest() const noexcept
{
    // Phase 3.3 adds: && tail energy trackers < -120 dBFS
    const bool tailsQuiet = true;

    return tailsQuiet
        && tensionSm.getCurrentValue() == 0.0f && tensionSm.getTargetValue() == 0.0f && ! tensionSm.isSmoothing()
        && engageSm.getCurrentValue()  == 0.0f && engageSm.getTargetValue()  == 0.0f && ! engageSm.isSmoothing()
        && safetySm.getCurrentValue()  == 0.0f && safetySm.getTargetValue()  == 0.0f && ! safetySm.isSmoothing();
}

void TensionEngine::resetFeedbackModules() noexcept
{
    // Phase 3.3+: clear delay lines, reverb, filter states (preallocated memory, no allocation).
    feedbackResetCount.fetch_add (1, std::memory_order_relaxed);
}

void TensionEngine::process (juce::AudioBuffer<float>& buffer, const EngineParams& params, const HostTransport& hostTransport)
{
    if (! prepared)
        return;

    const int numCh = std::min (buffer.getNumChannels(), 2);
    const int total = buffer.getNumSamples();

    if (numCh < 1 || total <= 0)
        return;

    transport    = hostTransport;
    syncEnabled  = params.sync;
    riseMode     = TensionDSP::riseModeFromIndex (params.riseMode);

    // A scrub event in a previous block resets feedback modules at this block boundary.
    if (safety.consumeResetRequest())
        resetFeedbackModules();

    const float tensionTarget = snapZero (std::clamp (params.tension, 0.0f, 1.0f));
    const float mixTarget     = snapZero (std::clamp (params.mix, 0.0f, 1.0f));
    const float trimTarget    = juce::Decibels::decibelsToGain (std::clamp (params.outputDb, -24.0f, 12.0f));

    if (firstBlock)
    {
        // Deterministic restore: no ramp from stale values.
        const auto targets = TensionDSP::evaluateTensionMapping (tensionTarget, riseMode);

        tensionSm.setCurrentAndTargetValue (tensionTarget);
        engageSm.setCurrentAndTargetValue  (snapZero (targets.engagement));
        safetySm.setCurrentAndTargetValue  (snapZero (targets.safety));
        mixSm.setCurrentAndTargetValue     (mixTarget);
        trimSm.setCurrentAndTargetValue    (trimTarget);
        controlPos = 0;
        firstBlock = false;
    }
    else
    {
        tensionSm.setTargetValue (tensionTarget);
        mixSm.setTargetValue     (mixTarget);
        trimSm.setTargetValue    (trimTarget);
    }

    statOutPeak  = 0.0f;
    statWetSq    = 0.0;
    statWetCount = 0;

    float* ch0 = buffer.getWritePointer (0);
    float* ch1 = numCh > 1 ? buffer.getWritePointer (1) : nullptr;

    for (int offset = 0; offset < total; offset += maxBlock)
    {
        const int n = std::min (maxBlock, total - offset);
        processSegment (ch0 + offset, ch1 != nullptr ? ch1 + offset : nullptr, n);
    }

    // ---- Telemetry (relaxed, once per block) ----
    const float decay = (float) std::exp (-(double) total / (peakDecayTimeSec * sampleRate));
    peakHold = std::max (statOutPeak, peakHold * decay);

    const float wetRms = statWetCount > 0 ? (float) std::sqrt (statWetSq / (double) statWetCount) : 0.0f;

    telemetry.tensionSmoothed.store (tensionSm.getCurrentValue(), std::memory_order_relaxed);
    telemetry.outPeak.store         (peakHold,                    std::memory_order_relaxed);
    telemetry.wetRms.store          (wetRms,                      std::memory_order_relaxed);
    telemetry.noiseLevel.store      (0.0f,                        std::memory_order_relaxed); // Phase 3.3
    telemetry.lfoPhaseA.store       (0.0f,                        std::memory_order_relaxed); // Phase 3.2
}

void TensionEngine::controlTick()
{
    // Once per control period (absolute sample grid, so independent of host block size).
    const float t = tensionSm.getCurrentValue();
    const auto targets = TensionDSP::evaluateTensionMapping (t, riseMode);

    engageSm.setTargetValue (snapZero (targets.engagement));
    safetySm.setTargetValue (snapZero (targets.safety));

    // Phases 3.2+: push the remaining destination targets into each module here.
}

void TensionEngine::processSegment (float* out0, float* out1, int n)
{
    float* dL = dryBuf.getWritePointer (0);
    float* dR = dryBuf.getWritePointer (1);
    float* pL = procBuf.getWritePointer (0);
    float* pR = procBuf.getWritePointer (1);
    float* wL = wetBuf.getWritePointer (0);
    float* wR = wetBuf.getWritePointer (1);

    const bool mono = (out1 == nullptr);

    // 1. Capture dry (never altered afterwards). Mono is duplicated to both channels.
    juce::FloatVectorOperations::copy (dL, out0, n);
    juce::FloatVectorOperations::copy (dR, mono ? out0 : out1, n);

    // 2. Series chain (identity stubs this phase). Skipped entirely at rest.
    if (! isAtRest())
    {
        juce::FloatVectorOperations::copy (pL, dL, n);
        juce::FloatVectorOperations::copy (pR, dR, n);

        if (stubProcGain != 1.0f)
        {
            juce::FloatVectorOperations::multiply (pL, stubProcGain, n);
            juce::FloatVectorOperations::multiply (pR, stubProcGain, n);
        }
    }
    // else: engagement e == 0 for the whole segment, proc is never read.

    // 3. Bus + safety + trim, in control-rate chunks (<= 32 samples).
    int pos = 0;

    while (pos < n)
    {
        if (controlPos == 0)
            controlTick();

        const int chunk = std::min (n - pos, controlBlockSize - controlPos);

        for (int i = pos; i < pos + chunk; ++i)
        {
            const float e   = engageSm.getNextValue();
            const float s   = safetySm.getNextValue();
            const float mix = mixSm.getNextValue();
            const float g   = trimSm.getNextValue();

            const float dl = dL[i];
            const float dr = dR[i];

            // wet bus (noise / delay / reverb returns are zero in Phase 3.1)
            const float wl = wetBus (dl, pL[i], e);
            const float wr = wetBus (dr, pR[i], e);
            wL[i] = wl;
            wR[i] = wr;

            // telemetry + runaway guard on the wet bus
            const float fl = finiteOrZero (wl);
            const float fr = finiteOrZero (wr);
            statWetSq += (double) fl * fl + (double) fr * fr;
            statWetCount += 2;
            safety.updateGuard (std::max (std::abs (fl), std::abs (fr)));

            // MIX -> OutputSafety -> output trim (AFTER safety)
            float yl = safety.processSample (mixBus (dl, wl, mix), s);
            float yr = safety.processSample (mixBus (dr, wr, mix), s);

            if (g != 1.0f)
            {
                yl *= g;
                yr *= g;
            }

            statOutPeak = std::max (statOutPeak, std::max (std::abs (yl), std::abs (yr)));

            if (mono)
            {
                out0[i] = 0.5f * (yl + yr);
            }
            else
            {
                out0[i] = yl;
                out1[i] = yr;
            }
        }

        tensionSm.skip (chunk);

        controlPos = (controlPos + chunk) % controlBlockSize;
        pos += chunk;
    }
}
