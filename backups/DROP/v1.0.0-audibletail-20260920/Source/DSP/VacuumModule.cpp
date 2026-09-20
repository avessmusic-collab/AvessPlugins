#include "VacuumModule.h"
#include <cmath>

namespace drop
{

void VacuumModule::prepare(const juce::dsp::ProcessSpec& spec)
{
    sampleRate = spec.sampleRate > 0.0 ? spec.sampleRate : 44100.0;
    const double twoPiFcOverFs = 2.0 * juce::MathConstants<double>::pi * (double) kCrossoverHz / sampleRate;
    crossoverCoeff = (float) (1.0 - std::exp(-twoPiFcOverFs));
    reset();
}

void VacuumModule::reset()
{
    lowTrackState[0] = lowTrackState[1] = 0.0f;
}

float VacuumModule::computeGain(double elapsedSamples, double totalWindowSamples, float depthAmount01) noexcept
{
    depthAmount01 = juce::jlimit(0.0f, 1.0f, depthAmount01);
    const float floorDb = juce::jmap(depthAmount01, 0.0f, 1.0f, kMaxFloorDb, kMinFloorDb);

    if (totalWindowSamples <= 0.0 || elapsedSamples <= 0.0 || elapsedSamples >= totalWindowSamples)
        return 1.0f; // outside the window: fully transparent, continuous hand-off to/from neighboring processing

    const double t = juce::jlimit(0.0, 1.0, elapsedSamples / totalWindowSamples);
    const double fadeOutEnd = (double) kFadeOutFraction;
    const double holdEnd = fadeOutEnd + (double) kHoldFraction;

    // POST-1.0 (2026-09-19, user-directed: "make the vacume volume in db").
    // DOCUMENTED SUPERSESSION of the raised-cosine AMPLITUDE fades: measured
    // in decibels a cosine amplitude fade hangs near full level and then
    // plunges (the same perceptual class as the superseded tail-envelope
    // and noise-rise shapes, both already converted to dB-linear). The
    // collapse/return are now DECIBEL-LINEAR ramps between 0dB and the
    // depth-mapped floor (kMaxFloorDb..kMinFloorDb - the floor itself was
    // always specified in dB): every equal slice of the fade drops/raises
    // the same number of dB, the perceptually even "volume in dB" fade.
    // Value-continuity at the seams is exact (both branches hit floorDb at
    // their hold-side ends and 0dB at the window edges); the discrete
    // per-sample steps at the seams sit AT the floor level (<= -12dB) and
    // are orders of magnitude below the click-free bounds (verified by the
    // Phase 3.3 continuity tests).
    if (t < fadeOutEnd)
    {
        const double x = juce::jlimit(0.0, 1.0, t / juce::jmax(1.0e-9, fadeOutEnd));
        return juce::Decibels::decibelsToGain(floorDb * (float) x);
    }

    if (t < holdEnd)
        return juce::Decibels::decibelsToGain(floorDb);

    const double x = juce::jlimit(0.0, 1.0, (t - holdEnd) / juce::jmax(1.0e-9, (double) kFadeInFraction));
    return juce::Decibels::decibelsToGain(floorDb * (float) (1.0 - x));
}

void VacuumModule::process(juce::dsp::AudioBlock<float> block, double elapsedSamplesAtBlockStart,
                            double totalWindowSamples, float depthAmount01) noexcept
{
    const int numSamples = (int) block.getNumSamples();
    const int numChannels = (int) block.getNumChannels();
    if (numSamples <= 0 || numChannels <= 0)
        return;

    depthAmount01 = juce::jlimit(0.0f, 1.0f, depthAmount01);
    const float floorGain = juce::Decibels::decibelsToGain(juce::jmap(depthAmount01, 0.0f, 1.0f, kMaxFloorDb, kMinFloorDb));

    for (int i = 0; i < numSamples; ++i)
    {
        const double elapsed = elapsedSamplesAtBlockStart + (double) i;
        const float gain = computeGain(elapsed, totalWindowSamples, depthAmount01);

        // 0 at unity gain, 1 at the envelope's floor - drives the low/high/
        // width hooks below so they track the SAME smooth, sample-accurate
        // envelope (never an independently-switched, discontinuous state).
        const float collapseAmount = (floorGain < 0.999f)
                                    ? juce::jlimit(0.0f, 1.0f, (1.0f - gain) / (1.0f - floorGain))
                                    : 0.0f;

        float processed[2] = { 0.0f, 0.0f };
        const int chCount = juce::jmin(numChannels, 2);
        for (int ch = 0; ch < chCount; ++ch)
        {
            auto* data = block.getChannelPointer((size_t) ch);
            const float x = data[i];

            lowTrackState[ch] += crossoverCoeff * (x - lowTrackState[ch]);
            const float lp = lowTrackState[ch];
            const float hp = x - lp;

            // architecture.md: "Optionally also collapses low-frequency
            // content... and/or high-frequency content... depending on
            // Style flags" - Phase 3.3 placeholder magnitudes (real
            // per-Style flags/gating land in Phase 3.5); continuous by
            // construction since collapseAmount(t) is itself C1-continuous.
            const float lowAttenuation = 1.0f - 0.3f * collapseAmount;
            const float highAttenuation = 1.0f - 0.5f * collapseAmount;
            processed[ch] = (lp * lowAttenuation + hp * highAttenuation) * gain;
        }

        if (chCount >= 2)
        {
            const float mid = 0.5f * (processed[0] + processed[1]);
            const float side = 0.5f * (processed[0] - processed[1]);
            const float widthGain = 1.0f - 0.4f * collapseAmount; // side gain pulled toward 0 as the vacuum deepens
            const float shapedSide = side * widthGain;
            processed[0] = mid + shapedSide;
            processed[1] = mid - shapedSide;
        }

        for (int ch = 0; ch < chCount; ++ch)
            block.getChannelPointer((size_t) ch)[i] = processed[ch];

        for (int ch = 2; ch < numChannels; ++ch) // defensive: not expected for this mono/stereo-only plugin
            block.getChannelPointer((size_t) ch)[i] *= gain;
    }
}

} // namespace drop
