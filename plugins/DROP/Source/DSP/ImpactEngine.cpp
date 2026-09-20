#include "ImpactEngine.h"
#include <cmath>
#include <new>

namespace drop
{

void ImpactEngine::prepare(const juce::dsp::ProcessSpec& spec)
{
    sampleRate = spec.sampleRate > 0.0 ? spec.sampleRate : 44100.0;

    subPhaseIncrement = (float) (2.0 * juce::MathConstants<double>::pi * (double) kSubFrequencyHz / sampleRate);

    juce::dsp::ProcessSpec monoSpec { spec.sampleRate, spec.maximumBlockSize, 1 };
    subBandpass.coefficients = juce::dsp::IIR::Coefficients<float>::makeBandPass(sampleRate, kSubFrequencyHz, kSubBandpassQ);
    subBandpass.prepare(monoSpec);
    burstBandpass.coefficients = juce::dsp::IIR::Coefficients<float>::makeBandPass(sampleRate, kBurstFrequencyHz, kBurstBandpassQ);
    burstBandpass.prepare(monoSpec);

    const double twoPiFcOverFs = 2.0 * juce::MathConstants<double>::pi * (double) kLowClipCrossoverHz / sampleRate;
    lowClipCoeff = (float) (1.0 - std::exp(-twoPiFcOverFs));

    // DEPTH-AUDIT FIX (2026-09-20, offline-determinism): juce::dsp::Reverb's
    // internal parameter smoothers survive reset() - a re-prepared instance
    // therefore rendered its first impact-reverb ramp differently from a
    // fresh one (measured 0.0015 max diff between two identical offline
    // renders, diverging exactly at Impact). Reconstructing the object
    // makes every prepare() bit-identical to a fresh instance.
    // (shortTailReverb is a drop::ReverbModule; its prepare() performs the
    // determinism-grade fresh reconstruction of the inner juce Reverb.)
    shortTailReverb.prepare(spec);

    reset();
}

void ImpactEngine::reset()
{
    subPhase = 0.0f;
    subBandpass.reset();
    burstBandpass.reset();
    lowClipState[0] = lowClipState[1] = 0.0f;
    shortTailReverb.reset();
}

void ImpactEngine::setSeed(uint32_t masterSeed) noexcept
{
    burstRng.seed(masterSeed ^ kSeedXorConstant);
}

float ImpactEngine::nextBurstNoise() noexcept
{
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
    return dist(burstRng);
}

float ImpactEngine::computeSubEnvelope(double elapsedSamples, double sampleRateIn) noexcept
{
    if (elapsedSamples < 0.0 || sampleRateIn <= 0.0)
        return 0.0f;

    const double attackSamples = kSubAttackSeconds * sampleRateIn;
    const double decaySamples = kSubDecaySeconds * sampleRateIn;

    if (elapsedSamples < attackSamples)
        return (float) juce::jlimit(0.0, 1.0, elapsedSamples / juce::jmax(1.0e-9, attackSamples));

    const double decayElapsed = elapsedSamples - attackSamples;
    if (decayElapsed >= decaySamples)
        return 0.0f;

    // Raised-cosine decay to zero (C0-continuous with the attack branch at
    // the join: both evaluate to 1.0 there); a standard, smooth,
    // fast-attack/short-decay percussive envelope shape.
    return (float) (0.5 * (1.0 + std::cos(juce::MathConstants<double>::pi * (decayElapsed / decaySamples))));
}

float ImpactEngine::computeBurstEnvelope(double elapsedSamples, double sampleRateIn) noexcept
{
    if (elapsedSamples < 0.0 || sampleRateIn <= 0.0)
        return 0.0f;

    const double burstSamples = kBurstDurationSeconds * sampleRateIn;
    if (elapsedSamples >= burstSamples)
        return 0.0f;

    const double t = juce::jlimit(0.0, 1.0, elapsedSamples / juce::jmax(1.0e-9, burstSamples));
    return (float) (0.5 * (1.0 + std::cos(juce::MathConstants<double>::pi * t)));
}

ImpactEngine::DriveValues ImpactEngine::computeDriveValues(double elapsedSamplesAtSegmentStart, double totalWindowSamples, float impactAmount01) noexcept
{
    DriveValues d;
    impactAmount01 = juce::jlimit(0.0f, 1.0f, impactAmount01);
    if (totalWindowSamples <= 0.0)
        return d;

    const double t = juce::jlimit(0.0, 1.0, elapsedSamplesAtSegmentStart / totalWindowSamples);
    const double attackShape = (t < kDriveAttackFraction)
        ? 0.5 * (1.0 - std::cos(juce::MathConstants<double>::pi * (t / kDriveAttackFraction)))
        : 1.0;

    d.widthExpansionAmount = impactAmount01 * (float) attackShape;
    d.saturationOverlayAmount = kSaturationOverlayScale * impactAmount01 * (float) attackShape;
    return d;
}

float ImpactEngine::generateSubHitSample(double elapsedSamples, float impactAmount01) noexcept
{
    const float env = computeSubEnvelope(elapsedSamples, sampleRate);
    subPhase += subPhaseIncrement;
    if (subPhase >= juce::MathConstants<float>::twoPi)
        subPhase -= juce::MathConstants<float>::twoPi;
    const float raw = std::sin(subPhase) * env * impactAmount01 * kSubLevel;
    return subBandpass.processSample(raw);
}

void ImpactEngine::renderSubHitOnly(float* monoOut, int numSamples, double elapsedSamplesAtSegmentStart, float impactAmount01) noexcept
{
    if (monoOut == nullptr || numSamples <= 0)
        return;

    impactAmount01 = juce::jlimit(0.0f, 1.0f, impactAmount01);
    for (int i = 0; i < numSamples; ++i)
        monoOut[i] = generateSubHitSample(elapsedSamplesAtSegmentStart + (double) i, impactAmount01);
}

void ImpactEngine::process(juce::dsp::AudioBlock<float> block, double elapsedSamplesAtSegmentStart,
                            double totalWindowSamples, float impactAmount01,
                            float reverbSendWeightOverride, float accentLevel01) noexcept
{
    juce::ignoreUnused(totalWindowSamples); // see header doc comment - every envelope here self-terminates from its own (much shorter) duration

    const int numSamples = (int) block.getNumSamples();
    const int numChannels = (int) block.getNumChannels();
    if (numSamples <= 0 || numChannels <= 0)
        return;

    impactAmount01 = juce::jlimit(0.0f, 1.0f, impactAmount01);
    accentLevel01 = juce::jlimit(0.0f, 1.0f, accentLevel01);
    const int chCount = juce::jmin(numChannels, 2);

    for (int i = 0; i < numSamples; ++i)
    {
        const double elapsed = elapsedSamplesAtSegmentStart + (double) i;

        const float subSample = generateSubHitSample(elapsed, impactAmount01);
        const float burstEnv = computeBurstEnvelope(elapsed, sampleRate);
        const float burstRaw = nextBurstNoise() * burstEnv * impactAmount01 * kBurstLevel;
        const float burstSample = burstBandpass.processSample(burstRaw);

        // Mono-summed additive content (LOW-FREQUENCY PROTECTION - added
        // identically to every channel to avoid any phase risk at 40-80Hz).
        // accentLevel01 scales the accents AFTER the bandpass filters (both
        // linear, so pre/post is equivalent) - the filters keep running so
        // their internal state stays continuous if a Style change mid-tail
        // ever re-enables accents.
        const float additive = (subSample + burstSample) * accentLevel01;

        for (int ch = 0; ch < chCount; ++ch)
        {
            auto* data = block.getChannelPointer((size_t) ch);
            const float summed = data[i] + additive;

            // Low-end soft-clip safety on the SUMMED result (this sample's
            // existing bus content PLUS this sample's sub/burst addition).
            //
            // Stage 3 DSP Phase 3.4 bugfix: the original approach
            // reconstructed the sample as tanh(low) + high, where `low`/
            // `high` came from a one-pole crossover split. That does NOT
            // actually bound the result: a simple one-pole crossover is not
            // phase-neutral for source content close to its ~120Hz cutoff
            // (e.g. bass around 40-80Hz), so a non-trivial fraction of a
            // loud low-frequency signal's own amplitude ends up in the
            // UNCLIPPED `high` residual, 90-ish degrees out of phase with
            // `low` - at some sample positions `tanh(low)` and `high` add
            // constructively and the reconstructed output can exceed the
            // original (un-clipped) amplitude, i.e. the "safety" was
            // measurably amplifying instead of bounding.
            //
            // Fix: derive a single scalar GAIN from the low band's level
            // and apply it to the WHOLE summed sample (bus + sub-hit +
            // burst), rather than reconstructing from separately-treated
            // bands. |tanh(x)/x| <= 1 for all real x, so
            // |output| = |summed| * |gain| <= |summed| ALWAYS - genuinely
            // bounded by construction, independent of crossover phase
            // behavior. tanh(x)/x is continuous (limit 1 as x->0), so this
            // introduces no threshold discontinuity; a final hard jlimit()
            // is a defensive absolute ceiling for the low-frequency-content
            // case specifically (LOW-FREQUENCY PROTECTION: "Impact low-end
            // must be controlled").
            lowClipState[ch] += lowClipCoeff * (summed - lowClipState[ch]);
            const float lowAbs = std::abs(lowClipState[ch]);
            const float gain = (lowAbs > 1.0e-6f) ? (std::tanh(lowAbs) / lowAbs) : 1.0f;
            data[i] = juce::jlimit(-2.0f, 2.0f, summed * gain);
        }
    }

    // Dedicated short-tail reverb (architecture.md: small room / high
    // damping -> a punchy, bounded tail, not "an indistinct wash").
    // POST-1.0 bugfix: sendWeight defaults to impactAmount01 (identical to
    // the pre-bugfix single-weight processWithParams() call) unless the
    // caller explicitly overrides it (Recovery carryover - see this
    // method's header doc comment) - `reverbSendWeightOverride < 0` is the
    // sentinel meaning "no override, use legacy behavior".
    const float reverbSendWeight = (reverbSendWeightOverride >= 0.0f) ? reverbSendWeightOverride : impactAmount01;
    juce::dsp::Reverb::Parameters shortTailParams;
    shortTailParams.roomSize = kShortTailRoomSize;
    shortTailParams.damping = kShortTailDamping;
    shortTailParams.width = 1.0f;
    shortTailParams.freezeMode = 0.0f;
    shortTailReverb.processWithParamsSendReturn(block, reverbSendWeight, impactAmount01, shortTailParams);

    // The low-end soft-clip above bounds the summed (bus + sub + burst)
    // signal, but the short-tail reverb runs AFTER it and its early
    // reflections can re-amplify past the ceiling (observed: 4.08 peak from
    // a 2.0-clamped signal in the Phase 3.4 safety test). Safety ceilings
    // must be the LAST stage in the chain - re-apply the hard bound to the
    // final output so the guarantee holds unconditionally.
    for (size_t ch = 0; ch < block.getNumChannels(); ++ch)
    {
        auto* data = block.getChannelPointer(ch);
        for (size_t i = 0; i < block.getNumSamples(); ++i)
            data[i] = juce::jlimit(-2.0f, 2.0f, data[i]);
    }
}

} // namespace drop
