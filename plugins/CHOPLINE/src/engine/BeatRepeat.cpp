// CHOPLINE — BeatRepeat implementation. See header for the algorithm.

#include "BeatRepeat.h"

#include <cmath>

namespace chopline::engine
{

namespace
{
    inline float sanitize (float v) noexcept
    {
        return std::isfinite (v) ? v : 0.0f;
    }
} // namespace

void BeatRepeat::prepare (double newSampleRate, int maxBlockSize)
{
    juce::ignoreUnused (maxBlockSize);
    rate = newSampleRate > 0.0 ? newSampleRate : 48000.0;

    // 4 bars at 30 BPM (32 s) at the session rate — the spec §7 figure at
    // 192 kHz. Allocated here, never on the audio thread.
    const auto newLen = static_cast<juce::int64> (std::ceil (ringSeconds * rate));
    if (newLen != ringLen)
    {
        ringLen = newLen;
        ringL.assign (static_cast<size_t> (ringLen), 0.0f);
        ringR.assign (static_cast<size_t> (ringLen), 0.0f);
    }
    else
    {
        std::fill (ringL.begin(), ringL.end(), 0.0f);
        std::fill (ringR.begin(), ringR.end(), 0.0f);
    }

    fadeLen = juce::jmax (8, static_cast<int> (std::llround (rate * 0.003))); // 3 ms

    reset();
}

void BeatRepeat::reset() noexcept
{
    absPos = 0;
    repeating = false;
    holdWas = false;
    slotIndex = 0;
    slotRemaining = 0;
    gateRemaining = 0;
    gateTotal = 1;
    elapsed = 0;
    cur = {};
    prev = {};
    fadePos = 0;
    dryGain = 1.0f;
    repGain = 0.0f;
    fIc1[0] = fIc1[1] = fIc2[0] = fIc2[1] = 0.0;
    lastFc = -1.0;
    uiActive.store (false, std::memory_order_relaxed);
}

float BeatRepeat::readRing (const Reader& r, int channel) const noexcept
{
    const auto& ring = channel == 0 ? ringL : ringR;
    auto pos = r.pos;
    if (pos < 0.0)
        pos = 0.0;
    const auto maxPos = static_cast<double> (absPos); // never read the future
    if (pos > maxPos)
        pos = maxPos;

    const auto i0 = static_cast<juce::int64> (pos);
    const auto frac = pos - static_cast<double> (i0);
    const auto j0 = static_cast<size_t> (i0 % ringLen);
    const auto j1 = static_cast<size_t> ((i0 + 1) % ringLen);
    if (frac <= 0.0)
        return ring[j0]; // integer read path (slot 0 capture pass is bit-exact)
    return static_cast<float> ((1.0 - frac) * static_cast<double> (ring[j0])
                               + frac * static_cast<double> (ring[j1]));
}

void BeatRepeat::updateFilterCoeffs (double fc) noexcept
{
    if (juce::exactlyEqual (fc, lastFc))
        return;
    lastFc = fc;
    const auto g = std::tan (juce::MathConstants<double>::pi
                             * juce::jlimit (10.0, rate * 0.49, fc) / rate);
    fK = 1.0 / 0.707;
    fA1 = 1.0 / (1.0 + g * (g + fK));
    fA2 = g * fA1;
    fA3 = g * fA2;
}

void BeatRepeat::startRepeat (const Params& p, double samplesPerBar) noexcept
{
    gridSamples = juce::jmax (static_cast<double> (2 * fadeLen),
                              gridBars (p.gridIndex) * samplesPerBar);
    sliceStart = absPos;
    sliceLen = gridSamples;
    slotIndex = 0;
    elapsed = 0;
    gateTotal = juce::jmax<juce::int64> (1,
        static_cast<juce::int64> (std::llround (gateBars (p.gateIndex) * samplesPerBar)));
    // Hold-driven repeats run while held (gate 0); Auto repeats run the gate.
    gateRemaining = (p.hold || p.freeze) ? 0 : gateTotal;
    repeating = true;

    if (cur.active)
    {
        prev = cur;
        fadePos = 0;
    }
    cur.active = true;
    cur.pos = static_cast<double> (sliceStart);
    cur.rate = 1.0;
    cur.reverse = false;
    cur.gain = 1.0f;
    slotRemaining = static_cast<juce::int64> (std::llround (gridSamples));

    fIc1[0] = fIc1[1] = fIc2[0] = fIc2[1] = 0.0;
    lastFc = -1.0;
}

void BeatRepeat::startSlot (const Params& p, bool advance) noexcept
{
    if (advance)
        ++slotIndex;

    const auto progress = juce::jlimit (0.0, 1.0,
        static_cast<double> (elapsed) / static_cast<double> (gateTotal));
    const auto accel01 = static_cast<double> (juce::jlimit (0.0f, 100.0f, p.accelPct)) / 100.0;

    auto lenRatio = 1.0;
    if (p.rndGrid && advance)
    {
        const auto gi = juce::jlimit (0, numGridChoices - 1,
                                      p.gridIndex + rng.nextInt (3) - 1);
        lenRatio = gridBars (gi) / gridBars (p.gridIndex);
    }

    // Stutter accel (spec §7): the repeat size shrinks over the gate, up to 8×.
    auto slotLen = gridSamples * lenRatio * std::pow (2.0, -3.0 * accel01 * progress);
    slotLen = juce::jmax (static_cast<double> (2 * fadeLen), slotLen);
    slotRemaining = juce::jmax<juce::int64> (1,
        static_cast<juce::int64> (std::llround (slotLen)));

    const auto readRate = std::pow (2.0,
        -static_cast<double> (slotIndex) * static_cast<double> (p.pitchDecaySt) / 12.0);
    const auto reversed = slotIndex >= 1
        && (p.reverseMode == 2 || (p.reverseMode == 1 && (slotIndex & 1) != 0));
    const auto decay01 = juce::jlimit (0.0f, 1.0f, p.decayPct / 100.0f);
    const auto gain = static_cast<float> (
        std::pow (static_cast<double> (1.0f - decay01), static_cast<double> (slotIndex)));

    prev = cur;
    fadePos = 0;

    cur.active = true;
    cur.rate = readRate;
    cur.reverse = reversed;
    cur.gain = gain;
    const auto content = juce::jmin (sliceLen, slotLen * readRate);
    cur.pos = reversed ? static_cast<double> (sliceStart) + content - 1.0
                       : static_cast<double> (sliceStart);
}

void BeatRepeat::beginStop() noexcept
{
    repeating = false;
    if (cur.active)
    {
        prev = cur;   // the repeat tail fades out over fadeLen
        fadePos = 0;
        cur.active = false;
    }
}

void BeatRepeat::process (juce::AudioBuffer<float>& buffer, const Params& p,
                          double barPhase, double samplesPerBar) noexcept
{
    const auto numSamples = buffer.getNumSamples();
    if (numSamples <= 0 || ringLen <= 0 || buffer.getNumChannels() == 0)
        return;

    auto* L = buffer.getWritePointer (0);
    auto* R = buffer.getNumChannels() > 1 ? buffer.getWritePointer (1) : L;

    const auto off = p.mode == 2;
    const auto holdNow = ! off && (p.hold || p.freeze);
    const auto autoOn = ! off && p.mode == 0;

    if (off && repeating)
        beginStop();

    // Fast path: fully idle (no repeat, no tail, gains settled, nothing can
    // trigger) — just capture into the ring, dry stays bit-identical.
    if (! repeating && ! cur.active && ! prev.active && ! holdNow && ! autoOn
        && repGain <= 0.0f && dryGain >= 1.0f)
    {
        for (int i = 0; i < numSamples; ++i)
        {
            const auto idx = static_cast<size_t> (absPos % ringLen);
            ringL[idx] = sanitize (L[i]);
            ringR[idx] = sanitize (R[i]);
            ++absPos;
        }
        uiActive.store (false, std::memory_order_relaxed);
        return;
    }

    samplesPerBar = juce::jmax (64.0, samplesPerBar);
    const auto barsPerSample = 1.0 / samplesPerBar;

    // Samples until the next boundary of `sizeBars` (offset `offsetBars`),
    // 0 = a boundary sits on the current sample (half-sample tolerance).
    const auto samplesToBoundary = [&] (double phaseNow, double sizeBars,
                                        double offsetBars) noexcept -> juce::int64
    {
        const auto rel = (phaseNow - offsetBars) / sizeBars;
        const auto relEps = 0.5 * barsPerSample / sizeBars;
        const auto next = std::ceil (rel - relEps);
        const auto ds = (next * sizeBars + offsetBars - phaseNow) * samplesPerBar;
        return juce::jmax<juce::int64> (0, static_cast<juce::int64> (std::llround (ds)));
    };

    const auto gainStep = 1.0f / static_cast<float> (fadeLen);

    int n = 0;
    while (n < numSamples)
    {
        const auto phaseNow = barPhase + static_cast<double> (n) * barsPerSample;

        // Slot boundary bookkeeping first.
        if (repeating && slotRemaining <= 0)
        {
            const auto keepGoing = holdNow || gateRemaining > 0;
            if (keepGoing)
                startSlot (p, ! p.freeze); // freeze pins the current slot
            else
                beginStop();
        }

        auto seg = static_cast<juce::int64> (numSamples - n);

        // Manual / freeze arm: trigger at the next grid boundary.
        if (! repeating && holdNow)
        {
            const auto toGrid = samplesToBoundary (phaseNow, gridBars (p.gridIndex), 0.0);
            if (toGrid <= 0)
                startRepeat (p, samplesPerBar);
            else
                seg = juce::jmin (seg, toGrid);
        }

        // Auto: probability roll at every interval(+offset) boundary.
        if (autoOn)
        {
            const auto iv = intervalBars (p.intervalIndex);
            const auto ivOffset = iv * static_cast<double> (juce::jlimit (0, 15, p.offsetSteps))
                                / 16.0;
            auto toInterval = samplesToBoundary (phaseNow, iv, ivOffset);
            if (toInterval <= 0)
            {
                const auto fire = rng.nextFloat() * 100.0f
                                  < juce::jlimit (0.0f, 100.0f, p.chancePct);
                if (fire && ! (repeating && holdNow))
                    startRepeat (p, samplesPerBar); // (re)capture at the boundary
                toInterval = juce::jmax<juce::int64> (1,
                    static_cast<juce::int64> (std::llround (iv * samplesPerBar)));
            }
            seg = juce::jmin (seg, toInterval);
        }

        if (repeating)
        {
            seg = juce::jmin (seg, slotRemaining);
            if (p.filterMode != 0)
                seg = juce::jmin<juce::int64> (seg, 64); // smooth sweep updates
        }
        seg = juce::jmax<juce::int64> (1, seg);

        // Sweep filter cutoff for this segment (repeat path only).
        if (repeating && p.filterMode != 0)
        {
            const auto progress = juce::jlimit (0.0, 1.0,
                static_cast<double> (elapsed) / static_cast<double> (gateTotal));
            const auto depth = static_cast<double> (juce::jlimit (0.0f, 100.0f, p.sweepPct))
                             / 100.0;
            const auto fc = p.filterMode == 1
                ? juce::jlimit (80.0, 20000.0, 20000.0 * std::pow (2.0, -8.0 * depth * progress))
                : juce::jlimit (20.0, 12000.0, 20.0 * std::pow (2.0, 9.0 * depth * progress));
            updateFilterCoeffs (fc);
        }

        // Capture only while not repeating or during the slot-0 live pass
        // (plus its 3 ms crossfade continuation into slot 1). Repeats never
        // overwrite the pinned slice, so a hold or freeze longer than the
        // 32 s ring keeps its content intact; writes resume on the next
        // (re)capture.
        const auto writeEnabled = ! repeating || slotIndex == 0
                               || (slotIndex == 1 && prev.active);

        // Gain targets (ramped per sample over the crossfade window).
        const auto repTarget = (repeating || cur.active)
            ? ((p.mixMode == 0 && slotIndex == 0 && repeating) ? 0.0f : 1.0f)
            : 0.0f;
        const auto dryTarget = p.mixMode == 0 ? 1.0f
                             : p.mixMode == 1 ? (repeating ? 0.0f : 1.0f)
                                              : 0.0f;

        for (juce::int64 i = 0; i < seg; ++i)
        {
            const auto idx = static_cast<int> (n + i);
            const auto inL = sanitize (L[idx]);
            const auto inR = sanitize (R[idx]);

            if (writeEnabled)
            {
                const auto w = static_cast<size_t> (absPos % ringLen);
                ringL[w] = inL;
                ringR[w] = inR;
            }

            auto repL = 0.0f, repR = 0.0f;
            if (cur.active)
            {
                auto scale = cur.gain;
                if (prev.active) // crossfade the new slot in against the old
                    scale *= static_cast<float> (fadePos) * gainStep;
                repL += readRing (cur, 0) * scale;
                repR += readRing (cur, 1) * scale;
                cur.pos += cur.reverse ? -cur.rate : cur.rate;
                if (cur.reverse && cur.pos < static_cast<double> (sliceStart))
                    cur.pos = static_cast<double> (sliceStart);
            }
            if (prev.active)
            {
                const auto x = 1.0f - static_cast<float> (fadePos) * gainStep;
                repL += readRing (prev, 0) * prev.gain * x;
                repR += readRing (prev, 1) * prev.gain * x;
                prev.pos += prev.reverse ? -prev.rate : prev.rate;
                if (prev.reverse && prev.pos < static_cast<double> (sliceStart))
                    prev.pos = static_cast<double> (sliceStart);
                if (++fadePos >= fadeLen)
                    prev.active = false;
            }

            if (p.filterMode != 0 && (repeating || cur.active || prev.active))
            {
                // TPT state-variable filter on the repeat path.
                for (int ch = 0; ch < 2; ++ch)
                {
                    const auto x = static_cast<double> (ch == 0 ? repL : repR);
                    const auto v3 = x - fIc2[ch];
                    const auto v1 = fA1 * fIc1[ch] + fA2 * v3;
                    const auto v2 = fIc2[ch] + fA2 * fIc1[ch] + fA3 * v3;
                    fIc1[ch] = 2.0 * v1 - fIc1[ch];
                    fIc2[ch] = 2.0 * v2 - fIc2[ch];
                    const auto y = p.filterMode == 1 ? v2 : x - fK * v1 - v2;
                    if (ch == 0)
                        repL = static_cast<float> (y);
                    else
                        repR = static_cast<float> (y);
                }
            }

            dryGain += juce::jlimit (-gainStep, gainStep, dryTarget - dryGain);
            repGain += juce::jlimit (-gainStep, gainStep, repTarget - repGain);

            L[idx] = inL * dryGain + sanitize (repL) * repGain;
            if (R != L)
                R[idx] = inR * dryGain + sanitize (repR) * repGain;

            ++absPos;
        }

        if (repeating)
        {
            slotRemaining -= seg;
            elapsed += seg;
            if (! p.freeze)
                gateRemaining = juce::jmax<juce::int64> (0, gateRemaining - seg);
        }
        n += static_cast<int> (seg);
    }

    // Denormal/NaN guard on the sweep filter states.
    for (int ch = 0; ch < 2; ++ch)
    {
        if (! std::isfinite (fIc1[ch]) || ! std::isfinite (fIc2[ch]))
            fIc1[ch] = fIc2[ch] = 0.0;
    }

    holdWas = holdNow;
    uiActive.store (repeating || holdNow, std::memory_order_relaxed);
}

} // namespace chopline::engine
