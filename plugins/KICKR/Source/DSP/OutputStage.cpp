#include "DSP/OutputStage.h"

#include <cmath>

namespace kickr
{
    void OutputStage::prepare (double fsOversampled) noexcept
    {
        fs = juce::jmax (1.0, fsOversampled);

        juce::dsp::ProcessSpec spec;
        spec.sampleRate       = fs;
        spec.maximumBlockSize = static_cast<juce::uint32> (32);   // per-sample processing
        spec.numChannels      = static_cast<juce::uint32> (2);

        for (Filter* f : { &lowL, &lowR, &midL, &midR, &highL, &highR })
            f->prepare (spec);

        crossover.prepare (spec);
        crossover.setType (juce::dsp::LinkwitzRileyFilterType::lowpass);
        crossover.setCutoffFrequency (kMonoCrossoverHz);

        limiterCeil = dsputils::dbToGain (kLimiterCeilingDb);

        // In-region DC blocker, ~5 Hz corner at fsOversampled (below the 25 Hz sub).
        const float dcR = std::exp (-2.0f * juce::MathConstants<float>::pi * 5.0f
                                    / static_cast<float> (fs));
        for (auto& b : dcBlock) b.R = dcR;

        mixGain.reset (fs, 0.01);
        outGain.reset (fs, 0.01);

        // Flat tone by default -> exact unity pass-through (0 dB shelf / bell).
        lowDbCached = midDbCached = highDbCached = 0.0f;
        refreshTone();

        reset();
    }

    void OutputStage::reset() noexcept
    {
        for (Filter* f : { &lowL, &lowR, &midL, &midR, &highL, &highR })
            f->reset();

        crossover.reset();
        for (auto& b : dcBlock) b.reset();

        mixGain.setCurrentAndTargetValue (1.0f);
        outGain.setCurrentAndTargetValue (dsputils::dbToGain (0.0f));
    }

    void OutputStage::updateOversampledRate (double newFsOversampled) noexcept
    {
        fs = juce::jmax (1.0, newFsOversampled);

        juce::dsp::ProcessSpec spec;
        spec.sampleRate       = fs;
        spec.maximumBlockSize = static_cast<juce::uint32> (32);
        spec.numChannels      = static_cast<juce::uint32> (2);

        crossover.prepare (spec);   // same numChannels -> no allocation; integrators clear
        crossover.setType (juce::dsp::LinkwitzRileyFilterType::lowpass);
        crossover.setCutoffFrequency (kMonoCrossoverHz);

        const float dcR = std::exp (-2.0f * juce::MathConstants<float>::pi * 5.0f
                                    / static_cast<float> (fs));
        for (auto& b : dcBlock)
            b.R = dcR;   // keep x1 / y1

        mixGain.reset (fs, 0.01);
        outGain.reset (fs, 0.01);

        refreshTone();   // recompute the 3 biquads vs the new fs, in-place (IIR state kept)
    }

    void OutputStage::refreshTone() noexcept
    {
        const auto low  = ArrayCoeffs::makeLowShelf  (fs, kLowShelfHz,  kShelfQ,
                                                      dsputils::dbToGain (lowDbCached));
        const auto mid  = ArrayCoeffs::makePeakFilter (fs, kMidBellHz,   kMidBellQ,
                                                       dsputils::dbToGain (midDbCached));
        const auto high = ArrayCoeffs::makeHighShelf (fs, kHighShelfHz, kShelfQ,
                                                      dsputils::dbToGain (highDbCached));

        *lowL.coefficients  = low;
        *lowR.coefficients  = low;
        *midL.coefficients  = mid;
        *midR.coefficients  = mid;
        *highL.coefficients = high;
        *highR.coefficients = high;
    }

    void OutputStage::setParams (float lowDb, float midDb, float highDb,
                                 float outputWidth01, float outputDb,
                                 bool limiterEnabled, float mix01) noexcept
    {
        // In-place biquad recompute only on a real change (no per-block heap otherwise).
        if (std::abs (lowDb  - lowDbCached)  > 0.01f
            || std::abs (midDb  - midDbCached)  > 0.01f
            || std::abs (highDb - highDbCached) > 0.01f)
        {
            lowDbCached  = lowDb;
            midDbCached  = midDb;
            highDbCached = highDb;
            refreshTone();
        }

        widthFactor = 2.0f * juce::jlimit (0.0f, 1.0f, outputWidth01);
        limiterOn   = limiterEnabled;

        outGain.setTargetValue (dsputils::dbToGain (juce::jlimit (-24.0f, 12.0f, outputDb)));
        // Equal-power processed<->silence blend (dsputils::equalPowerRise — shared with the
        // retrigger crossfade's formula so a future retune only needs one edit).
        mixGain.setTargetValue (dsputils::equalPowerRise (mix01));
    }

    void OutputStage::processTone (float& l, float& r) noexcept
    {
        l = highL.processSample (midL.processSample (lowL.processSample (l)));
        r = highR.processSample (midR.processSample (lowR.processSample (r)));

        lowL.snapToZero();  lowR.snapToZero();
        midL.snapToZero();  midR.snapToZero();
        highL.snapToZero(); highR.snapToZero();

        l = dsputils::sanitize (l);
        r = dsputils::sanitize (r);
    }

    void OutputStage::processOutput (float& l, float& r) noexcept
    {
        // 1. Linkwitz-Riley split @ 130 Hz.
        float loL = 0.0f, hiL = 0.0f, loR = 0.0f, hiR = 0.0f;
        crossover.processSample (0, l, loL, hiL);
        crossover.processSample (1, r, loR, hiR);

        // Low band -> forced mono.
        const float lowM = 0.5f * (loL + loR);

        // High band -> M/S width (side scaled by 2*outputWidth).
        const float mid  = 0.5f * (hiL + hiR);
        const float side = 0.5f * (hiL - hiR) * widthFactor;

        float outL = lowM + (mid + side);
        float outR = lowM + (mid - side);

        // 2. Mix — equal-power blend processed <-> silence.
        const float mg = mixGain.getNextValue();
        outL *= mg;
        outR *= mg;

        // 3. Output gain — after Mix.
        const float og = outGain.getNextValue();
        outL *= og;
        outR *= og;

        // 4. DC blocker — in-region, BEFORE the limiter. (A DC blocker after the limiter
        //    turns each edge of a heavily-clipped kick into a spike above the ceiling.)
        outL = dcBlock[0].process (outL);
        outR = dcBlock[1].process (outR);

        // 5. Safety limiter — zero-latency soft-clip, truly the last stage, so the
        //    -0.5 dBFS ceiling is guaranteed. tanh() asymptotes to `limiterCeil`.
        if (limiterOn)
        {
            outL = limiterCeil * std::tanh (outL / limiterCeil);
            outR = limiterCeil * std::tanh (outR / limiterCeil);
        }

        crossover.snapToZero();

        l = dsputils::sanitize (outL);
        r = dsputils::sanitize (outR);
    }
}
