// CHOPLINE — MasterFX implementation. See header for the chain + rationale.

#include "MasterFX.h"

#include "../state/Parameters.h" // delayTimeBeats (shared with the UI)

#include <cmath>

namespace chopline::engine
{

namespace
{
    inline float sanitize (float v) noexcept { return std::isfinite (v) ? v : 0.0f; }
} // namespace

// ---- shared shaping ---------------------------------------------------------------

float MasterFX::shapeSample (int type, float x, float drive01) noexcept
{
    const auto g = driveGain (drive01);
    switch (type)
    {
        case 0: return std::tanh (g * x);                       // Soft clip
        case 1: return juce::jlimit (-1.0f, 1.0f, g * x);       // Hard clip
        case 2:                                                  // Tape (v17: tanh + DC)
        {
            constexpr float b = 0.15f;
            return std::tanh (g * x + b) - std::tanh (b);
        }
        case 3: return std::sin (juce::MathConstants<float>::halfPi * g * x); // Fold
        case 4:                                                  // Bitcrush (1–16 bits)
        {
            const auto bits = 16.0f - 15.0f * juce::jlimit (0.0f, 1.0f, drive01);
            const auto levels = std::exp2 (bits - 1.0f);
            return std::round (x * levels) / levels;
        }
        default: return x; // Downsample is a rate reducer, handled separately
    }
}

float MasterFX::autoGainFor (int type, float drive01) noexcept
{
    if (type >= 4)
        return 1.0f; // crushers keep level by construction
    constexpr float ref = 0.5f;
    const auto shaped = std::abs (shapeSample (type, ref, drive01));
    return shaped > 1.0e-6f ? ref / shaped : 1.0f;
}

// ---- lifecycle ----------------------------------------------------------------------

void MasterFX::prepare (double newSampleRate, int maxBlockSize)
{
    rate = newSampleRate > 0.0 ? newSampleRate : 48000.0;
    maxBlock = juce::jmax (16, maxBlockSize);

    juce::dsp::ProcessSpec spec { rate, static_cast<juce::uint32> (maxBlock), 2 };

    svf.prepare (spec);
    svfType = -1;
    fScratch.setSize (2, maxBlock);

    // 2× oversampling, IIR polyphase: best latency (sub-sample) for a live
    // instrument; spec §7 asks for 2× minimum.
    oversampler = std::make_unique<juce::dsp::Oversampling<float>> (
        2, 1, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true);
    oversampler->initProcessing (static_cast<size_t> (maxBlock));
    latencySamples = static_cast<int> (std::llround (oversampler->getLatencyInSamples()));
    dScratch.setSize (2, maxBlock);

    delayLen = static_cast<int> (std::ceil (rate * 4.0)) + 4; // ≥ 2 beats at 40 BPM
    delayRingL.assign (static_cast<size_t> (delayLen), 0.0f);
    delayRingR.assign (static_cast<size_t> (delayLen), 0.0f);
    delayWrite = 0;

    reverb.setSampleRate (rate);
    reverb.reset();
    reverbWasActive = false;
    lastVerbDecay = lastVerbDamp = -1.0f;
    vScratch.setSize (2, maxBlock);

    const auto fast = 0.015; // bypass crossfades
    const auto slow = 0.020; // audible parameters
    fOn.reset (rate, fast);
    cutoff.reset (rate, slow);
    reso.reset (rate, slow);
    dOn.reset (rate, fast);
    drive.reset (rate, slow);
    distMix.reset (rate, slow);
    delayTime.reset (rate, 0.1); // tempo glide
    delayFb.reset (rate, slow);
    delayRtn.reset (rate, slow);
    wOn.reset (rate, fast);
    width.reset (rate, slow);
    monoFreq.reset (rate, slow);
    oOn.reset (rate, fast);
    outGain.reset (rate, slow);
    ceiling.reset (rate, slow);

    limRelease = static_cast<float> (1.0 - std::exp (-1.0 / (0.080 * rate)));

    reset();
}

void MasterFX::reset() noexcept
{
    svf.reset();
    if (oversampler != nullptr)
        oversampler->reset();
    std::fill (delayRingL.begin(), delayRingL.end(), 0.0f);
    std::fill (delayRingR.begin(), delayRingR.end(), 0.0f);
    delayWrite = 0;
    reverb.reset();
    reverbWasActive = false;
    dsHoldL = dsHoldR = 0.0f;
    dsCounter = 0;
    wIc1 = wIc2 = 0.0;
    limEnv = 1.0f;
    snapSmoothers = true;
    grDb.store (0.0f, std::memory_order_relaxed);
    outPeak.store (0.0f, std::memory_order_relaxed);
}

// ---- process ------------------------------------------------------------------------------

void MasterFX::process (juce::AudioBuffer<float>& main,
                        const juce::AudioBuffer<float>& delaySend,
                        const juce::AudioBuffer<float>& verbSend,
                        const MasterParams& p, double bpm) noexcept
{
    const auto n = main.getNumSamples();
    if (n <= 0 || main.getNumChannels() == 0)
        return;

    // Targets for this block (smoothed; snap on the very first block).
    fOn.setTargetValue (p.filterOn ? 1.0f : 0.0f);
    cutoff.setTargetValue (juce::jlimit (20.0f, juce::jmin (20000.0f,
                               static_cast<float> (rate * 0.49)), p.cutoffHz));
    reso.setTargetValue (juce::jlimit (0.0f, 100.0f, p.resoPct) / 100.0f);
    dOn.setTargetValue (p.distOn ? 1.0f : 0.0f);
    drive.setTargetValue (juce::jlimit (0.0f, 100.0f, p.drivePct) / 100.0f);
    distMix.setTargetValue (juce::jlimit (0.0f, 100.0f, p.mixPct) / 100.0f);

    const auto bpmSafe = juce::jlimit (20.0, 999.0, bpm > 1.0 ? bpm : 120.0);
    const auto delaySamples = params::delayTimeBeats (p.delayTimeIndex) * 60.0 / bpmSafe * rate;
    delayTime.setTargetValue (juce::jlimit (1.0f, static_cast<float> (delayLen - 2),
                                            static_cast<float> (delaySamples)));
    delayFb.setTargetValue (juce::jlimit (0.0f, 95.0f, p.delayFbPct) / 100.0f);
    delayRtn.setTargetValue (p.delayOn ? juce::jlimit (0.0f, 100.0f, p.delayRtnPct) / 100.0f
                                       : 0.0f);
    wOn.setTargetValue (p.widthOn ? 1.0f : 0.0f);
    width.setTargetValue (juce::jlimit (0.0f, 200.0f, p.widthPct) / 100.0f);
    monoFreq.setTargetValue (juce::jlimit (20.0f, 300.0f, p.monoHz));
    oOn.setTargetValue (p.outOn ? 1.0f : 0.0f);
    outGain.setTargetValue (juce::Decibels::decibelsToGain (
        juce::jlimit (-24.0f, 12.0f, p.outDb)));
    ceiling.setTargetValue (juce::Decibels::decibelsToGain (
        juce::jlimit (-12.0f, 0.0f, p.ceilingDb)));

    if (snapSmoothers)
    {
        snapSmoothers = false;
        for (auto* s : { &fOn, &cutoff, &reso, &dOn, &drive, &distMix, &delayTime,
                         &delayFb, &delayRtn, &wOn, &width, &monoFreq, &oOn,
                         &outGain, &ceiling })
            s->setCurrentAndTargetValue (s->getTargetValue());
    }

    processFilter (main, p, n);
    processDistortion (main, p, n);
    processDelay (main, delaySend, p, bpmSafe, n);
    processReverb (main, verbSend, p, n);
    processWidth (main, p, n);
    processOutput (main, p, n);

    outPeak.store (main.getMagnitude (0, n), std::memory_order_relaxed);
}

// ---- modules -----------------------------------------------------------------------------

void MasterFX::processFilter (juce::AudioBuffer<float>& main, const MasterParams& p, int n) noexcept
{
    const auto idle = ! fOn.isSmoothing() && fOn.getCurrentValue() <= 0.0f;
    if (idle)
    {
        cutoff.skip (n);
        reso.skip (n);
        svfType = -1; // forces a reset when re-enabled
        return;
    }

    if (svfType != p.filterType)
    {
        svfType = p.filterType;
        svf.setType (svfType == 1 ? juce::dsp::StateVariableTPTFilterType::bandpass
                   : svfType == 2 ? juce::dsp::StateVariableTPTFilterType::highpass
                                  : juce::dsp::StateVariableTPTFilterType::lowpass);
        svf.reset();
    }

    auto* l = main.getWritePointer (0);
    auto* r = main.getNumChannels() > 1 ? main.getWritePointer (1) : l;

    for (int start = 0; start < n; start += 32)
    {
        const auto chunk = juce::jmin (32, n - start);
        const auto fc = cutoff.getCurrentValue();
        const auto q = 0.707f + 7.3f * reso.getCurrentValue(); // 0 % → Butterworth
        cutoff.skip (chunk);
        reso.skip (chunk);
        svf.setCutoffFrequency (fc);
        svf.setResonance (q);

        for (int i = start; i < start + chunk; ++i)
        {
            const auto wetL = svf.processSample (0, l[i]);
            const auto wetR = r != l ? svf.processSample (1, r[i]) : wetL;
            const auto m = fOn.getNextValue();
            l[i] += (sanitize (wetL) - l[i]) * m;
            if (r != l)
                r[i] += (sanitize (wetR) - r[i]) * m;
        }
    }
}

void MasterFX::processDistortion (juce::AudioBuffer<float>& main, const MasterParams& p,
                                  int n) noexcept
{
    const auto idle = ! dOn.isSmoothing() && dOn.getCurrentValue() <= 0.0f;
    if (idle)
    {
        drive.skip (n);
        distMix.skip (n);
        return;
    }

    // Wet path into dScratch.
    for (int ch = 0; ch < 2; ++ch)
        dScratch.copyFrom (ch, 0, main, juce::jmin (ch, main.getNumChannels() - 1), 0, n);

    if (p.distType == 5)
    {
        // Downsample: base rate, intentionally NOT oversampled (aliasing wanted).
        const auto d01 = drive.getCurrentValue();
        drive.skip (n);
        const auto holdLen = 1 + static_cast<int> (std::floor (d01 * 63.0f));
        auto* l = dScratch.getWritePointer (0);
        auto* r = dScratch.getWritePointer (1);
        for (int i = 0; i < n; ++i)
        {
            if (dsCounter <= 0)
            {
                dsHoldL = l[i];
                dsHoldR = r[i];
                dsCounter = holdLen;
            }
            l[i] = dsHoldL;
            r[i] = dsHoldR;
            --dsCounter;
        }
    }
    else
    {
        juce::dsp::AudioBlock<float> block (dScratch);
        auto sub = block.getSubBlock (0, static_cast<size_t> (n));
        auto os = oversampler->processSamplesUp (sub);

        const auto osLen = static_cast<int> (os.getNumSamples());
        auto* l = os.getChannelPointer (0);
        auto* r = os.getChannelPointer (1);
        for (int start = 0; start < osLen; start += 128)
        {
            const auto chunk = juce::jmin (128, osLen - start);
            const auto d01 = drive.getCurrentValue();
            drive.skip (juce::jmax (1, chunk / 2)); // oversampled domain → ×2
            const auto ag = p.autoGain ? autoGainFor (p.distType, d01) : 1.0f;
            for (int i = start; i < start + chunk; ++i)
            {
                l[i] = sanitize (shapeSample (p.distType, l[i], d01) * ag);
                r[i] = sanitize (shapeSample (p.distType, r[i], d01) * ag);
            }
        }
        oversampler->processSamplesDown (sub);
    }

    auto* l = main.getWritePointer (0);
    auto* r = main.getNumChannels() > 1 ? main.getWritePointer (1) : l;
    const auto* wl = dScratch.getReadPointer (0);
    const auto* wr = dScratch.getReadPointer (1);
    for (int i = 0; i < n; ++i)
    {
        const auto m = dOn.getNextValue() * distMix.getNextValue();
        l[i] += (sanitize (wl[i]) - l[i]) * m;
        if (r != l)
            r[i] += (sanitize (wr[i]) - r[i]) * m;
    }
}

void MasterFX::processDelay (juce::AudioBuffer<float>& main,
                             const juce::AudioBuffer<float>& sendBus,
                             const MasterParams& p, double bpm, int n) noexcept
{
    juce::ignoreUnused (p, bpm);
    if (delayLen <= 4)
        return;

    // Idle fast path (like the other modules): return settled at 0 → skip the
    // per-sample ring work entirely; a fresh activation starts from a clean
    // ring so stale echoes from before the bypass never replay.
    const auto idle = ! delayRtn.isSmoothing() && delayRtn.getCurrentValue() <= 1.0e-4f;
    if (idle)
    {
        delayTime.skip (n);
        delayFb.skip (n);
        delayRtn.skip (n);
        delayWasActive = false;
        return;
    }
    if (! delayWasActive)
    {
        std::fill (delayRingL.begin(), delayRingL.end(), 0.0f);
        std::fill (delayRingR.begin(), delayRingR.end(), 0.0f);
        delayWasActive = true;
    }

    auto* l = main.getWritePointer (0);
    auto* r = main.getNumChannels() > 1 ? main.getWritePointer (1) : l;
    const auto* sl = sendBus.getReadPointer (0);
    const auto* sr = sendBus.getNumChannels() > 1 ? sendBus.getReadPointer (1) : sl;
    const auto sendLen = sendBus.getNumSamples();

    for (int i = 0; i < n; ++i)
    {
        const auto t = delayTime.getNextValue();
        auto readPos = static_cast<double> (delayWrite) - static_cast<double> (t);
        if (readPos < 0.0)
            readPos += static_cast<double> (delayLen);
        const auto i0 = static_cast<int> (readPos);
        const auto frac = static_cast<float> (readPos - static_cast<double> (i0));
        const auto i1 = i0 + 1 >= delayLen ? 0 : i0 + 1;
        const auto outL = delayRingL[static_cast<size_t> (i0)] * (1.0f - frac)
                        + delayRingL[static_cast<size_t> (i1)] * frac;
        const auto outR = delayRingR[static_cast<size_t> (i0)] * (1.0f - frac)
                        + delayRingR[static_cast<size_t> (i1)] * frac;

        const auto fb = delayFb.getNextValue();
        const auto inL = i < sendLen ? sl[i] : 0.0f;
        const auto inR = i < sendLen ? sr[i] : 0.0f;
        // NaN/Inf guard + hard bound on the feedback path (spec §8).
        delayRingL[static_cast<size_t> (delayWrite)] =
            juce::jlimit (-4.0f, 4.0f, sanitize (inL + outL * fb));
        delayRingR[static_cast<size_t> (delayWrite)] =
            juce::jlimit (-4.0f, 4.0f, sanitize (inR + outR * fb));
        if (++delayWrite >= delayLen)
            delayWrite = 0;

        const auto rtn = delayRtn.getNextValue();
        if (r != l)
        {
            l[i] += outL * rtn;
            r[i] += outR * rtn;
        }
        else
        {
            l[i] += (outL + outR) * 0.5f * rtn; // mono main: fold the return
        }
    }
}

void MasterFX::processReverb (juce::AudioBuffer<float>& main,
                              const juce::AudioBuffer<float>& sendBus,
                              const MasterParams& p, int n) noexcept
{
    const auto rtnTarget = p.verbOn ? juce::jlimit (0.0f, 100.0f, p.verbRtnPct) / 100.0f : 0.0f;
    const auto active = rtnTarget > 0.0001f || verbRtnCurrent > 0.0001f;
    if (! active)
    {
        reverbWasActive = false;
        verbRtnCurrent = rtnTarget;
        return;
    }

    if (! reverbWasActive)
    {
        reverb.reset();
        reverbWasActive = true;
    }

    const auto decay01 = juce::jlimit (0.0f, 100.0f, p.verbDecayPct) / 100.0f;
    const auto damp01 = juce::jlimit (0.0f, 100.0f, p.verbDampPct) / 100.0f;
    if (! juce::exactlyEqual (decay01, lastVerbDecay)
        || ! juce::exactlyEqual (damp01, lastVerbDamp))
    {
        lastVerbDecay = decay01;
        lastVerbDamp = damp01;
        juce::Reverb::Parameters rp;
        rp.roomSize = 0.25f + 0.72f * decay01; // small plate character
        rp.damping = damp01;
        rp.wetLevel = 1.0f;
        rp.dryLevel = 0.0f;
        rp.width = 1.0f;
        rp.freezeMode = 0.0f;
        reverb.setParameters (rp);
    }

    const auto sendLen = juce::jmin (n, sendBus.getNumSamples());
    for (int ch = 0; ch < 2; ++ch)
    {
        vScratch.clear (ch, 0, n);
        if (sendLen > 0)
            vScratch.copyFrom (ch, 0, sendBus, juce::jmin (ch, sendBus.getNumChannels() - 1),
                               0, sendLen);
    }
    // NaN guard on the reverb (feedback network) input.
    for (int ch = 0; ch < 2; ++ch)
    {
        auto* d = vScratch.getWritePointer (ch);
        for (int i = 0; i < n; ++i)
            d[i] = sanitize (d[i]);
    }
    reverb.processStereo (vScratch.getWritePointer (0), vScratch.getWritePointer (1), n);

    auto* l = main.getWritePointer (0);
    auto* r = main.getNumChannels() > 1 ? main.getWritePointer (1) : l;
    const auto* wl = vScratch.getReadPointer (0);
    const auto* wr = vScratch.getReadPointer (1);
    const auto step = 1.0f / static_cast<float> (juce::jmax (1.0, 0.02 * rate)); // ~20 ms
    for (int i = 0; i < n; ++i)
    {
        verbRtnCurrent += juce::jlimit (-step, step, rtnTarget - verbRtnCurrent);
        if (r != l)
        {
            l[i] += sanitize (wl[i]) * verbRtnCurrent;
            r[i] += sanitize (wr[i]) * verbRtnCurrent;
        }
        else
        {
            l[i] += (sanitize (wl[i]) + sanitize (wr[i])) * 0.5f * verbRtnCurrent;
        }
    }
}

void MasterFX::processWidth (juce::AudioBuffer<float>& main, const MasterParams& p, int n) noexcept
{
    juce::ignoreUnused (p);
    const auto idle = ! wOn.isSmoothing() && wOn.getCurrentValue() <= 0.0f;
    if (idle || main.getNumChannels() < 2)
    {
        width.skip (n);
        monoFreq.skip (n);
        wIc1 = wIc2 = 0.0;
        return;
    }

    auto* l = main.getWritePointer (0);
    auto* r = main.getWritePointer (1);

    for (int start = 0; start < n; start += 32)
    {
        const auto chunk = juce::jmin (32, n - start);
        const auto mono = monoFreq.getCurrentValue();
        monoFreq.skip (chunk);
        const auto useMono = mono > 20.5f;
        double a1 = 0.0, a2 = 0.0, a3 = 0.0;
        if (useMono)
        {
            const auto g = std::tan (juce::MathConstants<double>::pi
                                     * juce::jlimit (20.0, rate * 0.45,
                                                     static_cast<double> (mono)) / rate);
            a1 = 1.0 / (1.0 + g * (g + wK));
            a2 = g * a1;
            a3 = g * a2;
        }

        for (int i = start; i < start + chunk; ++i)
        {
            const auto m = 0.5f * (l[i] + r[i]);
            auto s = 0.5f * (l[i] - r[i]);
            if (useMono)
            {
                // TPT SVF high-pass on the side channel → elliptical bass mono.
                const auto x = static_cast<double> (s);
                const auto v3 = x - wIc2;
                const auto v1 = a1 * wIc1 + a2 * v3;
                const auto v2 = wIc2 + a2 * wIc1 + a3 * v3;
                wIc1 = 2.0 * v1 - wIc1;
                wIc2 = 2.0 * v2 - wIc2;
                s = static_cast<float> (x - wK * v1 - v2);
            }
            const auto w = width.getNextValue();
            const auto wetL = m + w * s;
            const auto wetR = m - w * s;
            const auto blend = wOn.getNextValue();
            l[i] += (sanitize (wetL) - l[i]) * blend;
            r[i] += (sanitize (wetR) - r[i]) * blend;
        }
    }

    if (! std::isfinite (wIc1) || ! std::isfinite (wIc2))
        wIc1 = wIc2 = 0.0;
}

void MasterFX::processOutput (juce::AudioBuffer<float>& main, const MasterParams& p, int n) noexcept
{
    juce::ignoreUnused (p);
    const auto idle = ! oOn.isSmoothing() && oOn.getCurrentValue() <= 0.0f;
    if (idle)
    {
        outGain.skip (n);
        ceiling.skip (n);
        limEnv = 1.0f;
        grDb.store (0.0f, std::memory_order_relaxed);
        return;
    }

    auto* l = main.getWritePointer (0);
    auto* r = main.getNumChannels() > 1 ? main.getWritePointer (1) : l;

    auto envMin = 1.0f;
    for (int i = 0; i < n; ++i)
    {
        const auto g = outGain.getNextValue();
        const auto ceil = juce::jmax (1.0e-4f, ceiling.getNextValue());

        auto wetL = l[i] * g;
        auto wetR = r != l ? r[i] * g : wetL;

        // Safety limiter: instant attack, smooth release, clip-guarded.
        const auto peak = juce::jmax (std::abs (wetL), std::abs (wetR));
        const auto target = peak > ceil ? ceil / peak : 1.0f;
        if (target < limEnv)
            limEnv = target;                       // instant attack
        else
            limEnv += (target - limEnv) * limRelease; // ~80 ms release
        envMin = juce::jmin (envMin, limEnv);

        wetL = juce::jlimit (-ceil, ceil, wetL * limEnv);
        wetR = juce::jlimit (-ceil, ceil, wetR * limEnv);

        const auto blend = oOn.getNextValue();
        if (blend >= 1.0f)
        {
            l[i] = wetL;
            if (r != l)
                r[i] = wetR;
        }
        else
        {
            l[i] += (wetL - l[i]) * blend;
            if (r != l)
                r[i] += (wetR - r[i]) * blend;
        }
    }

    grDb.store (envMin >= 1.0f ? 0.0f
                               : juce::Decibels::gainToDecibels (envMin, -60.0f),
                std::memory_order_relaxed);
}

} // namespace chopline::engine
