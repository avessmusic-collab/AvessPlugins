#include "TensionEngine.h"

#include <algorithm>
#include <cmath>

namespace
{
    constexpr double kTensionSmoothSec = 0.020;
    constexpr double kEngageSmoothSec  = 0.015;
    constexpr double kMixSmoothSec     = 0.020;
    constexpr double kTrimSmoothSec    = 0.020;
    constexpr double kModSmoothSec     = 0.050;

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

    // ---- Phase 3.2 series modules ----
    lfoA.setSeed (0xA5F1C3D7u);
    lfoA.setShape (LFOEngine::Shape::Sine);
    lfoA.setPhaseOffset (0.0f);
    lfoA.setRateScale (1.0f);
    lfoA.prepare (sampleRate);

    lfoB.setSeed (0x3C6EF372u);
    lfoB.setShape (LFOEngine::Shape::Sine);
    lfoB.setPhaseOffset (0.25f);          // 90 degrees
    lfoB.setRateScale (0.75f);            // rate ratio 0.75 of A
    lfoB.prepare (sampleRate);

    filter.prepare (sampleRate);
    saturation.prepare (sampleRate);
    stereoWidth.prepare (sampleRate);

    ampDepthSm.reset   (sampleRate, kModSmoothSec);
    inputTrimSm.reset  (sampleRate, kModSmoothSec);

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

    lfoA.reset();
    lfoB.reset();
    resetSeriesModules();
    ampDepthSm.setCurrentAndTargetValue  (0.0f);
    inputTrimSm.setCurrentAndTargetValue (1.0f);
    lfoBBuf.fill (0.0f);

    firstBlock    = true;   // next block snaps smoothers to the incoming parameters
    snapModules   = true;
    seriesCleared = false;
    prevPlaying   = false;
    controlPos = 0;
    peakHold   = 0.0f;
}

void TensionEngine::resetSeriesModules() noexcept
{
    filter.reset();
    saturation.reset();
    stereoWidth.reset();
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
    // Phase 3.2: series-module states (filter / saturator / crossover) are cleared so a poisoned IIR
    // state can never persist. Phase 3.3+ adds delay lines and reverb (preallocated, no allocation).
    resetSeriesModules();
    seriesCleared = false;
    snapModules   = true;     // module smoothers re-snap to their targets at the next control tick
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
        snapModules = true;
    }
    else
    {
        tensionSm.setTargetValue (tensionTarget);
        mixSm.setTargetValue     (mixTarget);
        trimSm.setTargetValue    (trimTarget);
    }

    // One-shot LFO re-alignment on transport start (sync mode only; phase is otherwise integrated).
    const bool startedPlaying = hostTransport.isPlaying && ! prevPlaying;
    prevPlaying = hostTransport.isPlaying;

    if (startedPlaying && params.sync && hostTransport.hasPpq)
    {
        lfoA.alignToPpq (hostTransport.ppq);
        lfoB.alignToPpq (hostTransport.ppq);
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
    telemetry.lfoPhaseA.store       (lfoA.getPhase(),             std::memory_order_relaxed);
}

void TensionEngine::controlTick()
{
    // Once per control period (absolute sample grid, so independent of host block size).
    const float t = tensionSm.getCurrentValue();
    const auto targets = TensionDSP::evaluateTensionMapping (t, riseMode);

    engageSm.setTargetValue (snapZero (targets.engagement));
    safetySm.setTargetValue (snapZero (targets.safety));

    // Modules snap to their targets on the first block and while the engine is at rest (so a
    // re-engagement never ramps in from stale values).
    const bool snap = snapModules || isAtRest();
    snapModules = false;

    // ---- LFOs: tempo fallback 120 BPM; rate slewed in the log domain ----
    const double bpm = (transport.hasBpm && transport.bpm > 0.0) ? transport.bpm : 120.0;

    if (syncEnabled)
    {
        lfoA.setSyncTarget (bpm, (double) targets.lfoCyclesPerBeat);
        lfoB.setSyncTarget (bpm, (double) targets.lfoCyclesPerBeat);
    }
    else
    {
        lfoA.setTargetRateHz (targets.lfoRateHz);
        lfoB.setTargetRateHz (targets.lfoRateHz);
    }

    if (snap)
    {
        lfoA.snapRateToTarget();
        lfoB.snapRateToTarget();
    }

    lfoA.controlStep (controlBlockSize);
    lfoB.controlStep (controlBlockSize);

    // ---- series modules (coefficients / targets change here and only here) ----
    FilterEngine::Targets ft;
    ft.hpHz        = targets.hpCutoffHz;
    ft.bpBlend     = targets.bpBlend;
    ft.bpCenterHz  = targets.bpCenterHz;
    ft.bpQ         = targets.bpQ;
    ft.lfoDepthOct = targets.lfoFilterDepthOct;

    filter.setTargets (ft, lfoA.value(), snap, controlBlockSize);
    saturation.setTargets (targets.satDrive, targets.satAmount, snap);
    stereoWidth.setTargets (targets.widthFactor, targets.lowSideCollapse, targets.lfoMoveDepth, snap);

    if (snap)
    {
        ampDepthSm.setCurrentAndTargetValue  (targets.lfoAmpDepth);
        inputTrimSm.setCurrentAndTargetValue (targets.inputTrim);
    }
    else
    {
        ampDepthSm.setTargetValue  (targets.lfoAmpDepth);
        inputTrimSm.setTargetValue (targets.inputTrim);
    }
}

void TensionEngine::runSeriesChain (int pos, int count) noexcept
{
    float*       pL = procBuf.getWritePointer (0) + pos;
    float*       pR = procBuf.getWritePointer (1) + pos;
    const float* dL = dryBuf.getReadPointer (0) + pos;
    const float* dR = dryBuf.getReadPointer (1) + pos;

    if (stubProcGain != 1.0f || ! seriesChainEnabled)
    {
        // Phase 3.1 identity / bus-algebra test mode: modules bypassed (LFOs still advance).
        juce::FloatVectorOperations::copy (pL, dL, count);
        juce::FloatVectorOperations::copy (pR, dR, count);

        if (stubProcGain != 1.0f)
        {
            juce::FloatVectorOperations::multiply (pL, stubProcGain, count);
            juce::FloatVectorOperations::multiply (pR, stubProcGain, count);
        }

        lfoA.advance (count);
        lfoB.advance (count);
        return;
    }

    // Series input: dry copy with non-finite samples scrubbed so a NaN/Inf can never poison the
    // IIR states (the dry bus itself is untouched; OutputSafety scrubs the output).
    for (int i = 0; i < count; ++i)
    {
        pL[i] = finiteOrZero (dL[i]);
        pR[i] = finiteOrZero (dR[i]);
    }

    // 1. input drive trim (identity in the 3.2 stub)
    if (inputTrimSm.isSmoothing() || inputTrimSm.getCurrentValue() != 1.0f)
    {
        for (int i = 0; i < count; ++i)
        {
            const float g = inputTrimSm.getNextValue();
            pL[i] *= g;
            pR[i] *= g;
        }
    }

    // 2. HP + BP-sweep pre-filter (coefficients fixed for this chunk)
    filter.process (pL, pR, count);

    // 3. amplitude tremolo (lfoA): g in [1 - depth, 1], a smooth gain, never a gate
    for (int i = 0; i < count; ++i)
    {
        const float la    = lfoA.next();
        lfoBBuf[(size_t) i] = lfoB.next();

        const float depth = ampDepthSm.getNextValue();
        const float g     = 1.0f - depth * 0.5f * (1.0f - la);
        pL[i] *= g;
        pR[i] *= g;
    }

    // 4. ADAA saturation, 5. stereo width (lfoB)
    saturation.process (pL, pR, count);
    stereoWidth.process (pL, pR, count, lfoBBuf.data());
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

    // 2./3. Series chain + bus + safety + trim, in control-rate chunks (<= 32 samples) on the
    //       absolute control grid. The chain is skipped while the engine is at rest (e == 0:
    //       proc is never read); module states are cleared once per rest period.
    int pos = 0;

    while (pos < n)
    {
        if (controlPos == 0)
            controlTick();

        const int chunk = std::min (n - pos, controlBlockSize - controlPos);

        if (isAtRest())
        {
            if (! seriesCleared)
            {
                resetSeriesModules();
                seriesCleared = true;
            }

            lfoA.advance (chunk);       // LFO phase keeps running (keeps beat alignment in sync mode)
            lfoB.advance (chunk);
        }
        else
        {
            seriesCleared = false;
            runSeriesChain (pos, chunk);
        }

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
