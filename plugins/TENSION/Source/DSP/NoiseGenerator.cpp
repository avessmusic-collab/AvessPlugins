#include "NoiseGenerator.h"

#include <algorithm>
#include <cmath>

float NoiseGenerator::pinkRaw (float w, PinkState& s) noexcept
{
    // Paul Kellett's refined cascaded one-pole pink-noise approximation.
    s.b0 = 0.99886f * s.b0 + w * 0.0555179f;
    s.b1 = 0.99332f * s.b1 + w * 0.0750759f;
    s.b2 = 0.96900f * s.b2 + w * 0.1538520f;
    s.b3 = 0.86650f * s.b3 + w * 0.3104856f;
    s.b4 = 0.55000f * s.b4 + w * 0.5329522f;
    s.b5 = -0.7616f * s.b5 - w * 0.0168980f;
    const float pink = s.b0 + s.b1 + s.b2 + s.b3 + s.b4 + s.b5 + s.b6 + w * 0.5362f;
    s.b6 = w * 0.115926f;
    return pink;
}

float NoiseGenerator::pinkNormalisation() noexcept
{
    // Gain that makes the pink stream's RMS equal the white RMS. Measured once, deterministically,
    // from a fixed-seed run (called from prepare(), never from the audio path).
    static const float norm = []
    {
        std::uint32_t s = 0x2545F491u;
        PinkState ps;
        double acc = 0.0;
        constexpr int total = 1 << 18, warm = 4096;

        for (int i = 0; i < total; ++i)
        {
            const float p = pinkRaw (whiteFromState (xorshift32 (s)), ps);
            if (i >= warm)
                acc += (double) p * (double) p;
        }

        return (float) ((double) kWhiteRms / std::sqrt (acc / (double) (total - warm)));
    }();

    return norm;
}

void NoiseGenerator::prepare (double newSampleRate)
{
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 44100.0;
    maxHz      = (float) (0.45 * sampleRate);
    pinkNorm   = pinkNormalisation();
    rateNorm   = (float) std::sqrt (sampleRate / 48000.0);     // Phase 3.6: rate-independent noise density (== 1.0f at 48 kHz)

    juce::dsp::ProcessSpec spec;
    spec.sampleRate       = sampleRate;
    spec.maximumBlockSize = 32;
    spec.numChannels      = 2;

    hpf.prepare (spec);
    lpf.prepare (spec);
    bpf.prepare (spec);

    hpf.setType (juce::dsp::StateVariableTPTFilterType::highpass);
    lpf.setType (juce::dsp::StateVariableTPTFilterType::lowpass);
    bpf.setType (juce::dsp::StateVariableTPTFilterType::bandpass);
    hpf.setResonance (0.70710678f);
    lpf.setResonance (0.70710678f);
    bpf.setResonance (kBpQ);

    gainSm.reset   (sampleRate, kGainSmoothSec);
    brightSm.reset (sampleRate, kToneSmoothSec);
    filtSm.reset   (sampleRate, kToneSmoothSec);
    tremSm.reset   (sampleRate, 0.050);
    hpfSm.reset    (sampleRate, kToneSmoothSec);
    topSm.reset    (sampleRate, kToneSmoothSec);

    reset();
}

void NoiseGenerator::setSeed (std::uint32_t masterSeed) noexcept
{
    seedL = masterSeed != 0u ? masterSeed : 1u;
    seedR = seedL ^ kSeedXor;
    if (seedR == 0u) seedR = 1u;
}

void NoiseGenerator::resetFilters()
{
    pinkL = PinkState {};
    pinkR = PinkState {};
    hpf.reset();
    lpf.reset();
    bpf.reset();
}

void NoiseGenerator::reset()
{
    stateL = seedL;
    stateR = seedR;
    resetFilters();

    const Targets d {};
    gainSm.setCurrentAndTargetValue   (0.0f);
    brightSm.setCurrentAndTargetValue (d.brightness);
    filtSm.setCurrentAndTargetValue   (d.filteredAmt);
    tremSm.setCurrentAndTargetValue   (d.tremDepth);
    hpfSm.setCurrentAndTargetValue    (d.hpfHz);
    topSm.setCurrentAndTargetValue    (d.topHz);

    hpf.setCutoffFrequency (d.hpfHz);
    lpf.setCutoffFrequency (d.topHz);
    bpf.setCutoffFrequency (d.topHz);
}

void NoiseGenerator::setTargets (const Targets& t, bool snap, int controlSamples)
{
    const float gain   = t.gain < 1.0e-6f ? 0.0f : std::min (t.gain, 4.0f);   // exact-zero snapping
    const float bright = std::clamp (t.brightness,  0.0f, 1.0f);
    const float filt   = std::clamp (t.filteredAmt, 0.0f, 2.0f);
    const float trem   = std::clamp (t.tremDepth,   0.0f, 1.0f);
    const float hp     = std::clamp (t.hpfHz, 20.0f, maxHz);
    const float top    = std::clamp (t.topHz, 20.0f, maxHz);

    if (snap)
    {
        gainSm.setCurrentAndTargetValue   (gain);
        brightSm.setCurrentAndTargetValue (bright);
        filtSm.setCurrentAndTargetValue   (filt);
        tremSm.setCurrentAndTargetValue   (trem);
        hpfSm.setCurrentAndTargetValue    (hp);
        topSm.setCurrentAndTargetValue    (top);
    }
    else
    {
        gainSm.setTargetValue   (gain);
        brightSm.setTargetValue (bright);
        filtSm.setTargetValue   (filt);
        tremSm.setTargetValue   (trem);
        hpfSm.setTargetValue    (hp);
        topSm.setTargetValue    (top);

        hpfSm.skip (controlSamples);      // cutoffs are applied once per control period
        topSm.skip (controlSamples);
    }

    hpf.setCutoffFrequency (std::clamp (hpfSm.getCurrentValue(), 20.0f, maxHz));
    const float topNow = std::clamp (topSm.getCurrentValue(), 20.0f, maxHz);
    lpf.setCutoffFrequency (topNow);
    bpf.setCutoffFrequency (topNow);
}

void NoiseGenerator::skip (int n) noexcept
{
    for (int i = 0; i < n; ++i)
    {
        (void) xorshift32 (stateL);
        (void) xorshift32 (stateR);
    }
}

void NoiseGenerator::renderRawWhiteForTesting (float* l, float* r, int n) noexcept
{
    for (int i = 0; i < n; ++i)
    {
        l[i] = whiteFromState (xorshift32 (stateL));
        r[i] = whiteFromState (xorshift32 (stateR));
    }
}

void NoiseGenerator::process (float* outL, float* outR, int n, const float* lfo) noexcept
{
    for (int i = 0; i < n; ++i)
    {
        // Both streams are ALWAYS drawn (automation-history independence).
        const float wL = whiteFromState (xorshift32 (stateL));
        const float wR = whiteFromState (xorshift32 (stateR));
        const float pL = pinkRaw (wL, pinkL) * pinkNorm;
        const float pR = pinkRaw (wR, pinkR) * pinkNorm;

        const float g     = gainSm.getNextValue();
        const float depth = tremSm.getNextValue();
        const float br    = brightSm.getNextValue();
        const float fa    = filtSm.getNextValue();
        const float la    = lfo != nullptr ? lfo[i] : 1.0f;
        const float amp   = g * (1.0f - depth * 0.5f * (1.0f - la)) * kRmsNorm * rateNorm;

        // pink -> white blend, rising HPF, LPF top shaper, + resonant band-pass of the white stream
        float xL = pL + br * (wL - pL);
        float xR = pR + br * (wR - pR);
        xL = lpf.processSample (0, hpf.processSample (0, xL));
        xR = lpf.processSample (1, hpf.processSample (1, xR));
        xL += fa * bpf.processSample (0, wL);
        xR += fa * bpf.processSample (1, wR);

        outL[i] = xL * amp;
        outR[i] = xR * amp;
    }
}
