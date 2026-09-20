#include "PitchModule.h"
#include <cstdlib>
#include <limits>

namespace drop
{

//==============================================================================
void GranularGrainEngine::prepare(const juce::dsp::ProcessSpec& spec, float grainLengthMs)
{
    sampleRate = spec.sampleRate > 0.0 ? spec.sampleRate : 44100.0;
    grainLengthSamples = juce::jmax(4.0, (double) grainLengthMs * 0.001 * sampleRate);

    // ~15% of grain length (few-ms-scale for typical 20-40ms grains,
    // matching the "few-ms equal-power crossfade" convention used
    // elsewhere in this codebase, e.g. StutterModule's kFadeMs) - short
    // enough that the majority of each voice's lifetime is a clean, single-
    // voice read (see the class doc comment's phase-interference analysis
    // for why this matters).
    crossfadeLength = juce::jmax(1.0, grainLengthSamples * 0.15);

    // Worst-case delay = kMaxRatio*grainLengthSamples (age==0 retrigger,
    // see the causality proof in the header) plus a safety margin.
    const double maxDelaySamples = (double) kMaxRatio * grainLengthSamples * 1.25 + 8.0;
    delayLine.setMaximumDelayInSamples((int) std::ceil(maxDelaySamples));
    delayLine.prepare(spec);

    ratioSmoothed.reset(sampleRate, kRatioSmoothingSeconds);

    reset();
}

void GranularGrainEngine::reset()
{
    delayLine.reset();
    ratioSmoothed.setCurrentAndTargetValue(1.0f);

    voices[0] = Voice {};
    voices[1] = Voice {};
    activeVoice = 0;
    crossfadePos = -1.0;
    crossfadingThisSample = false;
    cachedDelayActive = cachedDelayOther = 0.0f;
    cachedGainActive = 1.0f;
    cachedGainOther = 0.0f;

    for (auto& s : syncBaseWindow) s = 0.0f;
    for (auto& s : syncTailWindow) s = 0.0f;
}

void GranularGrainEngine::beginSample() noexcept
{
    const float ratio = ratioSmoothed.getNextValue();
    const double maxDelay = juce::jmax(0.0, (double) delayLine.getMaximumDelayInSamples() - 4.0);

    auto& active = voices[(size_t) activeVoice];
    auto& other = voices[(size_t) (1 - activeVoice)];

    active.age += 1.0;

    // Start a crossfade shortly before the active voice's lifetime ends
    // (mirrors StutterModule's dual-voice crossfade trigger condition).
    if (crossfadePos < 0.0 && active.age >= grainLengthSamples - crossfadeLength)
    {
        const double baseline = (double) ratio * grainLengthSamples;
        const double activeDelayNow = juce::jlimit(0.0, maxDelay, computeDelayAmt(active));

        other.age = 0.0;
        other.ratioAtRetrigger = ratio; // latch for the new voice's whole lifetime
        // Round 3: pitch-synchronous retrigger search (run ONCE per
        // crossfade event, here) - snaps the new voice's start position to
        // best phase-match the outgoing voice's current content, instead of
        // the blind nominal `baseline`. Falls back to 0.0 (nominal,
        // unsynchronized - identical to round-2 behavior) when no reliable
        // periodicity is found. See computeSyncOffset()'s comment below.
        other.syncOffset = computeSyncOffset(baseline, activeDelayNow);
        crossfadePos = 0.0;
    }

    crossfadingThisSample = (crossfadePos >= 0.0);
    if (crossfadingThisSample)
        other.age += 1.0;

    cachedDelayActive = (float) juce::jlimit(0.0, maxDelay, computeDelayAmt(active));

    if (crossfadingThisSample)
    {
        cachedDelayOther = (float) juce::jlimit(0.0, maxDelay, computeDelayAmt(other));

        const float x = (float) juce::jlimit(0.0, 1.0, crossfadePos / crossfadeLength);
        cachedGainActive = std::cos(x * juce::MathConstants<float>::halfPi);
        cachedGainOther = std::sin(x * juce::MathConstants<float>::halfPi);

        crossfadePos += 1.0;
        if (crossfadePos >= crossfadeLength)
        {
            crossfadePos = -1.0;
            activeVoice = 1 - activeVoice; // `other` (the newly-triggered voice) becomes active
        }
    }
    else
    {
        cachedGainActive = 1.0f;
        cachedGainOther = 0.0f;
    }
}

float GranularGrainEngine::processChannel(int channel, float input) noexcept
{
    delayLine.pushSample(channel, input);

    // juce::dsp::DelayLine gotcha (see class doc comment): `delayInSamples`
    // is resolved against the line's own internal per-channel readPos,
    // which only advances when `updateReadPointer` is true. Exactly ONE tap
    // per processChannel() call must pass `true` (advancing readPos once,
    // matching pushSample's own once-per-sample advance); any other taps in
    // the SAME sample use `false` and read using that same not-yet-advanced
    // readPos - genuine multi-tap reads relative to a consistent "now".
    if (!crossfadingThisSample)
        return cachedGainActive * delayLine.popSample(channel, cachedDelayActive, true);

    const float a = cachedGainActive * delayLine.popSample(channel, cachedDelayActive, false);
    const float b = cachedGainOther * delayLine.popSample(channel, cachedDelayOther, true);
    return a + b;
}

double GranularGrainEngine::computeSyncOffset(double baseline, double activeDelayNow) noexcept
{
    // Pitch-synchronous retrigger search (round 3 - see class doc comment
    // for the full derivation of WHY this is needed). Two bounded steps,
    // both peeking channel 0's delay line ONLY (updateReadPointer=false
    // throughout - these are non-consuming "look-ahead-into-history" reads;
    // per the DelayLine gotcha documented on processChannel(), this never
    // touches readPos, so it cannot disturb the readPos/writePos lockstep
    // invariant that multi-tap reads there depend on. Channel-0-only is a
    // deliberate lightweight simplification: stereo content is typically
    // highly phase-correlated for tonal/pitched material, so one channel's
    // estimate is a reasonable, cheap proxy for both).
    //
    // Fixed worst-case cost (no allocation, preallocated scratch, bounded
    // loops - RT-safe): step 1 is at most
    // (kSyncMaxLagSamples-kSyncMinLagSamples+1) * kSyncAnalysisWindow
    // popSample calls (~221*48=10,608); step 2 is at most
    // (kSyncMaxLagSamples+1) * kSyncMatchWindow (~241*24=5,784). Total
    // ~16,500 popSample calls (each a handful of array reads/multiplies),
    // once per crossfade event (~every grainLengthSamples-crossfadeLength
    // samples, e.g. ~25ms @48kHz/30ms grains) - a small fraction of one
    // audio block's time budget even in the worst case, and negligible when
    // amortized over the crossfade period.
    const double maxDelayForRead = juce::jmax(0.0, (double) delayLine.getMaximumDelayInSamples() - 4.0);

    // --- Step 1: bounded-lag normalized autocorrelation -> T0 estimate ---
    // Base window x[i] read from a fixed reference point (kSyncMaxLagSamples
    // samples back) so that BOTH x[i] and the shifted x[i+lag] (for every
    // lag up to kSyncMaxLagSamples) stay comfortably >= 0 and within the
    // delay line's bounds - verified: min read delay =
    // kSyncMaxLagSamples - (kSyncAnalysisWindow-1) + kSyncMinLagSamples > 0
    // for the constants chosen (240 - 47 + 20 = 213 > 0).
    const double baseReadDelay = juce::jlimit(0.0, maxDelayForRead, (double) kSyncMaxLagSamples);
    for (int i = 0; i < kSyncAnalysisWindow; ++i)
        syncBaseWindow[i] = delayLine.popSample(0, (float) juce::jlimit(0.0, maxDelayForRead, baseReadDelay - (double) i), false);

    double baseEnergy = 0.0;
    for (int i = 0; i < kSyncAnalysisWindow; ++i)
        baseEnergy += (double) syncBaseWindow[i] * (double) syncBaseWindow[i];

    int bestLag = -1;
    double bestCorr = 0.0;

    if (baseEnergy > 1.0e-8) // near-silent analysis window -> no reliable period, skip the search entirely
    {
        for (int lag = kSyncMinLagSamples; lag <= kSyncMaxLagSamples; ++lag)
        {
            double crossSum = 0.0, shiftedEnergy = 0.0;
            for (int i = 0; i < kSyncAnalysisWindow; ++i)
            {
                const double shiftedDelay = juce::jlimit(0.0, maxDelayForRead, baseReadDelay - (double) i + (double) lag);
                const float shiftedSample = delayLine.popSample(0, (float) shiftedDelay, false);
                crossSum += (double) syncBaseWindow[i] * (double) shiftedSample;
                shiftedEnergy += (double) shiftedSample * (double) shiftedSample;
            }

            if (shiftedEnergy > 1.0e-8)
            {
                const double normCorr = crossSum / std::sqrt(baseEnergy * shiftedEnergy);
                if (normCorr > bestCorr)
                {
                    bestCorr = normCorr;
                    bestLag = lag;
                }
            }
        }
    }

    // Reliability gate: no confident periodicity -> fall back to the
    // nominal (unsynchronized) offset, identical to round-2 behavior.
    // Noise/transient/silent material is UNAFFECTED by this whole method.
    if (bestLag < 0 || bestCorr < (double) kSyncMinReliableCorrelation)
        return 0.0;

    const double t0Est = (double) bestLag;

    // --- Step 2: search +/-T0_est/2 around `baseline` for the offset that
    // best phase-matches the OUTGOING voice's CURRENT tail. ---
    const int searchRadius = (int) juce::jlimit(1.0, (double) kSyncMaxLagSamples, t0Est * 0.5);

    for (int i = 0; i < kSyncMatchWindow; ++i)
        syncTailWindow[i] = delayLine.popSample(0, (float) juce::jlimit(0.0, maxDelayForRead, activeDelayNow + (double) i), false);

    double tailEnergy = 0.0;
    for (int i = 0; i < kSyncMatchWindow; ++i)
        tailEnergy += (double) syncTailWindow[i] * (double) syncTailWindow[i];

    if (tailEnergy <= 1.0e-8) // outgoing voice currently reading near-silence -> no reliable match target
        return 0.0;

    double bestOffset = 0.0;
    double bestMatchCorr = -1.0;

    for (int off = -searchRadius; off <= searchRadius; ++off)
    {
        const double candidateBase = juce::jlimit(0.0, maxDelayForRead, baseline + (double) off);

        double crossSum = 0.0, candEnergy = 0.0;
        for (int i = 0; i < kSyncMatchWindow; ++i)
        {
            const float candSample = delayLine.popSample(0, (float) juce::jlimit(0.0, maxDelayForRead, candidateBase + (double) i), false);
            crossSum += (double) syncTailWindow[i] * (double) candSample;
            candEnergy += (double) candSample * (double) candSample;
        }

        if (candEnergy > 1.0e-8)
        {
            const double normCorr = crossSum / std::sqrt(tailEnergy * candEnergy);
            if (normCorr > bestMatchCorr)
            {
                bestMatchCorr = normCorr;
                bestOffset = (double) off;
            }
        }
    }

    if (bestMatchCorr < (double) kSyncMinReliableCorrelation)
        return 0.0; // no confidently-matching offset found - fall back to nominal

    return bestOffset;
}

//==============================================================================
int PitchModule::quantizeSemitones(int raw) noexcept
{
    // implementation-spec.md PITCHMODULE: "Musical ranges: 0, +1, +2, +3,
    // +5, +7, +12 semitones and reasonable downward equivalents where
    // supported" - symmetric downward set used here.
    static constexpr int kAllowed[] = { -12, -7, -5, -3, -2, -1, 0, 1, 2, 3, 5, 7, 12 };

    int best = 0;
    int bestDiff = std::numeric_limits<int>::max();
    for (auto v : kAllowed)
    {
        const int diff = std::abs(v - raw);
        if (diff < bestDiff)
        {
            bestDiff = diff;
            best = v;
        }
    }
    return best;
}

float PitchModule::shapeProgress(PitchMode m, float p) noexcept
{
    switch (m)
    {
        case PitchMode::Rise: return p;        // 0 -> +target
        case PitchMode::Fall: return -p;       // 0 -> -target
        case PitchMode::RiseAndFall:
            // 0 -> +target (first half) -> -target (second half), continuous at p==0.5 (both branches == 1.0).
            return (p < 0.5f) ? (p * 2.0f) : (1.0f - (p - 0.5f) * 4.0f);
    }
    return p;
}

void PitchModule::prepare(const juce::dsp::ProcessSpec& spec)
{
    engine.prepare(spec, kGrainLengthMs);
    dryBuffer.setSize((int) juce::jmax(1u, spec.numChannels), (int) juce::jmax(1u, spec.maximumBlockSize));
    weightSmoothed.reset(spec.sampleRate, 0.02);
    reset();
}

void PitchModule::reset()
{
    engine.reset();
    weightSmoothed.setCurrentAndTargetValue(0.0f);
}

void PitchModule::process(juce::dsp::AudioBlock<float> block, float weight, float progress) noexcept
{
    const int numSamples = (int) block.getNumSamples();
    const int numChannels = (int) block.getNumChannels();
    if (numSamples <= 0 || numChannels <= 0)
        return;

    weight = juce::jlimit(0.0f, 1.0f, weight);
    progress = juce::jlimit(0.0f, 1.0f, progress);

    for (int ch = 0; ch < numChannels; ++ch)
        dryBuffer.copyFrom(ch, 0, block.getChannelPointer((size_t) ch), numSamples);

    const float shaped = shapeProgress(mode, progress);
    const float semitonesNow = shaped * (float) semitoneTarget;
    engine.setRatio(ratioForSemitones(semitonesNow));

    weightSmoothed.setTargetValue(weight);

    for (int i = 0; i < numSamples; ++i)
    {
        engine.beginSample();
        const float wetMix = weightSmoothed.getNextValue() * kMaxWetAtWeight1;

        for (int ch = 0; ch < numChannels; ++ch)
        {
            const float dry = dryBuffer.getSample(ch, i);
            const float wet = engine.processChannel(ch, dry);
            block.getChannelPointer((size_t) ch)[i] = dry + (wet - dry) * wetMix;
        }
    }
}

} // namespace drop
