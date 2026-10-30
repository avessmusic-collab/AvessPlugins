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

    // TensionDSP::LfoShape and LFOEngine::Shape share their numeric order.
    static_assert ((int) TensionDSP::LfoShape::Sine       == (int) LFOEngine::Shape::Sine);
    static_assert ((int) TensionDSP::LfoShape::Triangle   == (int) LFOEngine::Shape::Triangle);
    static_assert ((int) TensionDSP::LfoShape::Saw        == (int) LFOEngine::Shape::Saw);
    static_assert ((int) TensionDSP::LfoShape::ReverseSaw == (int) LFOEngine::Shape::ReverseSaw);
    static_assert ((int) TensionDSP::LfoShape::SampleHold == (int) LFOEngine::Shape::SampleHold);

    inline LFOEngine::Shape toShape (TensionDSP::LfoShape s) noexcept
    {
        return static_cast<LFOEngine::Shape> (static_cast<int> (s));
    }

    // Weighted shape evaluation at the LFO's current phase (single shape == plain evaluation).
    inline float mixValue (const LFOEngine& lfo, const TensionDSP::ShapeMix& m) noexcept
    {
        if (m.count <= 1)
            return lfo.valueFor (toShape (m.shape[0]));

        float acc = 0.0f;
        for (int k = 0; k < m.count; ++k)
            acc += m.weight[(size_t) k] * lfo.valueFor (toShape (m.shape[(size_t) k]));
        return acc;
    }

    constexpr double kCompSeconds      = 1.0;      // Phase 3.5 level compensator time constant
    constexpr double kAmpLfoSlewSec    = 0.0015;
    constexpr double kFilterLfoSlewSec = 0.004;
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

    // ---- Phase 3.3 wet-bus sources ----
    noise.setSeed (0x5EED7E57u);
    noise.prepare (sampleRate);
    reverb.prepare (sampleRate);
    delay.prepare (sampleRate);

    ampDepthSm.reset   (sampleRate, kModSmoothSec);
    inputTrimSm.reset  (sampleRate, kModSmoothSec);

    ampLfoCoef    = (float) (1.0 - std::exp (-1.0 / (kAmpLfoSlewSec * sampleRate)));
    filterLfoCoef = (float) (1.0 - std::exp (-(double) controlBlockSize / (kFilterLfoSlewSec * sampleRate)));

    // Phase 3.5 level compensator: 1 s power trackers (per sample) and 1 s gain slew (per control tick)
    compPowCoef  = (float) (1.0 - std::exp (-1.0 / (kCompSeconds * sampleRate)));
    compGainCoef = (float) (1.0 - std::exp (-(double) controlBlockSize / (kCompSeconds * sampleRate)));

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
    lfoABuf.fill (0.0f);
    lfoDBuf.fill (0.0f);
    ampLfoState    = 0.0f;
    filterLfoState = 0.0f;
    modeBlender.snapTo (riseMode);
    lastTargets = TensionDSP::TensionTargets {};
    ampMix = filterMix = stereoMix = delayMix = TensionDSP::ShapeMix {};
    zeroWetSources();
    compInPow = compWetPow = 0.0f;
    compGain  = 1.0f;

    noise.reset();            // restores the seeds: bit-identical noise after every reset
    reverb.reset();
    delay.reset();
    sendsCleared = true;

    firstBlock    = true;  // next block snaps smoothers to the incoming parameters
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

void TensionEngine::resetSendModules() noexcept
{
    // Preallocated memory is cleared in place; no allocation.
    delay.reset();
    reverb.reset();
    noise.resetFilters();      // the RNG is NOT touched (history independence)
}

void TensionEngine::zeroWetSources() noexcept
{
    nzL.fill (0.0f); nzR.fill (0.0f);
    dlL.fill (0.0f); dlR.fill (0.0f);
    rvL.fill (0.0f); rvR.fill (0.0f);
}

bool TensionEngine::isAtRest() const noexcept
{
    // Tail energy trackers: noise silent (exact 0 gain), delay writes and reverb in/out below -120 dBFS.
    const bool tailsQuiet = noise.isSilent() && delay.isQuiet() && reverb.isQuiet();

    return tailsQuiet
        && tensionSm.getCurrentValue() == 0.0f && tensionSm.getTargetValue() == 0.0f && ! tensionSm.isSmoothing()
        && engageSm.getCurrentValue()  == 0.0f && engageSm.getTargetValue()  == 0.0f && ! engageSm.isSmoothing()
        && safetySm.getCurrentValue()  == 0.0f && safetySm.getTargetValue()  == 0.0f && ! safetySm.isSmoothing();
}

void TensionEngine::resetFeedbackModules() noexcept
{
    // Series-module states (filter / saturator / crossover) plus the delay lines, the reverb and their
    // filters are cleared so a poisoned IIR / feedback state can never persist (preallocated memory,
    // no allocation). The noise RNG is left alone (only its filters are cleared).
    resetSeriesModules();
    resetSendModules();
    seriesCleared = false;
    sendsCleared  = false;
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
        modeBlender.snapTo (riseMode);          // no crossfade on the first block / after a reset
    }
    else
    {
        modeBlender.setTarget (riseMode);       // a changed mode crossfades over 600 ms (stepped per control tick)
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
    telemetry.noiseLevel.store      (noise.getLevel(),            std::memory_order_relaxed);
    telemetry.lfoPhaseA.store       (lfoA.getPhase(),             std::memory_order_relaxed);
}

void TensionEngine::controlTick()
{
    // Once per control period (absolute sample grid, so independent of host block size).
    const float t = tensionSm.getCurrentValue();

    // Rise-mode crossfade: advance the 600 ms blend by one control period, evaluate every profile
    // that still carries weight and lerp the whole targets struct.
    modeBlender.step ((float) ((double) controlBlockSize / sampleRate));
    const auto targets = modeBlender.evaluate (t);
    lastTargets = targets;

    ampMix    = modeBlender.shapeMix (&TensionDSP::LfoShapes::amp);
    filterMix = modeBlender.shapeMix (&TensionDSP::LfoShapes::filter);
    stereoMix = modeBlender.shapeMix (&TensionDSP::LfoShapes::stereo);
    delayMix  = modeBlender.shapeMix (&TensionDSP::LfoShapes::delay);

    engageSm.setTargetValue (snapZero (targets.engagement));
    safetySm.setTargetValue (snapZero (targets.safety));

    // Modules snap to their targets on the first block and while the engine is at rest (so a
    // re-engagement never ramps in from stale values).
    const bool snap = snapModules || isAtRest();
    snapModules = false;

    // ---- Phase 3.5 level compensator: slew the wet-bus gain towards "limit / measured" (1 s) ----
    if (levelCompEnabled)
    {
        float target = 1.0f;

        if (compInPow > kCompMinInputPower)
        {
            const float limit = std::pow (10.0f, kCompMaxOverInputDb / 10.0f);       // power ratio
            const float ratio = compWetPow / compInPow;
            if (ratio > limit)
                target = std::sqrt (limit / ratio);
        }

        compGain += compGainCoef * (target - compGain);
        if (target == 1.0f && std::abs (compGain - 1.0f) < 1.0e-5f)
            compGain = 1.0f;                                   // exact identity again
    }
    else
    {
        compGain = 1.0f;
    }

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

    // Filter LFO: per-mode shape (Aggressive = S&H), sampled once per control period and smoothed
    // (4 ms) so plateau changes never zipper the cutoff. Amp LFO state is re-seeded on a snap.
    const float filterRaw = mixValue (lfoA, filterMix);

    if (snap)
    {
        filterLfoState = filterRaw;
        ampLfoState    = mixValue (lfoA, ampMix);
    }
    else
    {
        filterLfoState += filterLfoCoef * (filterRaw - filterLfoState);
    }

    filter.setTargets (ft, filterLfoState, snap, controlBlockSize);
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

    // ---- Phase 3.3 wet-bus sources: noise (parallel add), delay + reverb (sends from proc) ----
    if (sendsEnabled())
    {
        // Runaway guard: halves the delay feedback and the reverb send (smoothed inside OutputSafety).
        const float fbClamp = safety.getFeedbackClamp();

        NoiseGenerator::Targets nt;
        nt.gain        = targets.noiseGain;
        nt.brightness  = targets.noiseBrightness;
        nt.hpfHz       = targets.noiseHpfHz;
        nt.topHz       = targets.noiseTopHz;
        nt.filteredAmt = targets.noiseFilteredAmt;
        nt.tremDepth   = targets.lfoAmpDepth;
        noise.setTargets (nt, snap, controlBlockSize);

        ReverbEngine::Targets rt;
        rt.sendGain    = targets.reverbSend * fbClamp;
        rt.returnGain  = targets.reverbReturn;
        rt.roomSize    = targets.reverbRoom;
        rt.damping     = targets.reverbDamping;
        rt.width       = targets.reverbWidth;
        rt.preDelayMs  = targets.reverbPreDelayMs;
        rt.returnLpfHz = targets.reverbReturnLpfHz;
        rt.sendHpfHz   = targets.reverbSendHpfHz;
        reverb.setTargets (rt, snap, controlBlockSize);

        DelayEngine::Targets dt;
        dt.sendGain    = targets.delaySend;
        dt.returnGain  = targets.delayReturn;
        dt.feedback    = targets.delayFeedback * fbClamp;
        dt.fbLpfHz     = targets.delayFbLpfHz;
        dt.delayMs     = (float) DelayEngine::computeDelayMs (syncEnabled, bpm, (int) riseMode);
        dt.lfoDepthMs  = targets.delayLfoDepthMs;
        delay.setTargets (dt, snap, controlBlockSize);
    }
    else
    {
        // Phase 3.1 / 3.2 test hooks: the three sources stay neutral (all gains exactly 0).
        noise.setTargets  (NoiseGenerator::Targets {}, true, controlBlockSize);
        reverb.setTargets (ReverbEngine::Targets {},   true, controlBlockSize);
        delay.setTargets  (DelayEngine::Targets {},    true, controlBlockSize);
    }
}

void TensionEngine::runWetSources (int pos, int count) noexcept
{
    if (! sendsEnabled())
    {
        zeroWetSources();
        return;
    }

    const float* inL = procBuf.getReadPointer (0) + pos;
    const float* inR = procBuf.getReadPointer (1) + pos;

    noise.process  (nzL.data(), nzR.data(), count, lfoABuf.data());       // amplitude-destination LFO
    delay.process  (inL, inR, dlL.data(), dlR.data(), count, lfoDBuf.data());   // delay-destination LFO
    reverb.process (inL, inR, rvL.data(), rvR.data(), count);

    // A tripped loop guard (non-finite in a feedback path) is forwarded to OutputSafety, which
    // resets the feedback modules at the next block boundary.
    const bool delayFailed  = delay.consumeFailure();
    const bool reverbFailed = reverb.consumeFailure();

    if (delayFailed || reverbFailed)
        safety.requestReset();
}

void TensionEngine::runSeriesChain (int pos, int count) noexcept
{
    float*       pL = procBuf.getWritePointer (0) + pos;
    float*       pR = procBuf.getWritePointer (1) + pos;
    const float* dL = dryBuf.getReadPointer (0) + pos;
    const float* dR = dryBuf.getReadPointer (1) + pos;

    // Per-destination LFO values for this chunk (shapes mixed with the mode-blend weights); both LFOs advance.
    fillLfoBuffers (count);

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
        const float la    = lfoABuf[(size_t) i];
        const float depth = ampDepthSm.getNextValue();
        const float g     = 1.0f - depth * 0.5f * (1.0f - la);
        pL[i] *= g;
        pR[i] *= g;
    }

    // 4. ADAA saturation, 5. stereo width (stereo-destination lfoB)
    saturation.process (pL, pR, count);
    stereoWidth.process (pL, pR, count, lfoBBuf.data());
}

void TensionEngine::fillLfoBuffers (int count) noexcept
{
    // Called once per chunk BEFORE the audio is touched (the LFO values do not depend on it).
    // amp : mode-mixed shape through a 1.5 ms one-pole (a reverse-saw reset is a ramp, never a step)
    // stereo / delay : mode-mixed shapes of lfoB.
    for (int i = 0; i < count; ++i)
    {
        ampLfoState += ampLfoCoef * (mixValue (lfoA, ampMix) - ampLfoState);

        lfoABuf[(size_t) i] = ampLfoState;
        lfoBBuf[(size_t) i] = mixValue (lfoB, stereoMix);
        lfoDBuf[(size_t) i] = mixValue (lfoB, delayMix);

        lfoA.advanceOne();
        lfoB.advanceOne();
    }
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

            if (! sendsCleared)
            {
                resetSendModules();
                sendsCleared = true;
            }

            lfoA.advance (chunk);       // LFO phase keeps running (keeps beat alignment in sync mode)
            lfoB.advance (chunk);
            noise.skip (chunk);         // RNG advances every sample (automation-history independence)
            zeroWetSources();
        }
        else
        {
            seriesCleared = false;
            sendsCleared  = false;
            runSeriesChain (pos, chunk);
            runWetSources (pos, chunk);
        }

        for (int i = pos; i < pos + chunk; ++i)
        {
            const int k = i - pos;
            const float e   = engageSm.getNextValue();
            const float s   = safetySm.getNextValue();
            const float mix = mixSm.getNextValue();
            const float g   = trimSm.getNextValue();

            const float dl = dL[i];
            const float dr = dR[i];

            // wet bus: dry*(1-e) + proc*e + noise + delayReturn + reverbReturn.
            // The additive sources are skipped when their sum is exactly 0 so that t = 0 stays
            // bit-exact (dry + 0.0f would turn a -0.0 sample into +0.0).
            float wl = wetBus (dl, pL[i], e);
            float wr = wetBus (dr, pR[i], e);

            const float xl = nzL[(size_t) k] + dlL[(size_t) k] + rvL[(size_t) k];
            const float xr = nzR[(size_t) k] + dlR[(size_t) k] + rvR[(size_t) k];
            if (xl != 0.0f) wl += xl;
            if (xr != 0.0f) wr += xr;

            // Phase 3.5 optional level compensator (default OFF): trackers see the pre-compensation
            // wet bus; the slewed gain is exactly 1.0 (skipped) whenever it is idle.
            if (levelCompEnabled)
            {
                const float cdl = finiteOrZero (dl), cdr = finiteOrZero (dr);
                const float cwl = finiteOrZero (wl), cwr = finiteOrZero (wr);
                compInPow  += compPowCoef * (0.5f * (cdl * cdl + cdr * cdr) - compInPow);
                compWetPow += compPowCoef * (0.5f * (cwl * cwl + cwr * cwr) - compWetPow);

                if (compGain != 1.0f)
                {
                    wl *= compGain;
                    wr *= compGain;
                }
            }

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
