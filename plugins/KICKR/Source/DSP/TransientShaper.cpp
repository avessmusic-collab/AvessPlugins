#include "DSP/TransientShaper.h"
#include "Utilities/DSPUtils.h"

#include <cmath>

namespace kickr
{
    namespace
    {
        /** One-pole smoothing coefficient for a `ms` time-constant at `fs`.
            Used both as `env = target + coef * (env - target)` (followers) and
            for the applied-gain smoother. */
        float onePoleCoef (float ms, double fs) noexcept
        {
            const float t = juce::jmax (0.01f, ms) * 0.001f;
            return std::exp (-1.0f / (t * static_cast<float> (juce::jmax (1.0, fs))));
        }
    }

    void TransientShaper::prepare (double fsOversampled) noexcept
    {
        fs = juce::jmax (1.0, fsOversampled);

        fastAtk  = onePoleCoef (1.0f,   fs);
        fastRel  = onePoleCoef (20.0f,  fs);
        slowAtk  = onePoleCoef (15.0f,  fs);
        slowRel  = onePoleCoef (150.0f, fs);
        gainCoef = onePoleCoef (3.0f,   fs);   // ~3 ms zipper guard on the applied gain

        reset();
    }

    void TransientShaper::reset() noexcept
    {
        fastEnv      = 0.0f;
        slowEnv      = 0.0f;
        smoothedGain = 1.0f;   // unity until the first block sets a target
    }

    void TransientShaper::resetFollowers() noexcept
    {
        fastEnv = 0.0f;
        slowEnv = 0.0f;
        // smoothedGain deliberately untouched — see header.
    }

    void TransientShaper::updateOversampledRate (double newFsOversampled) noexcept
    {
        fs = juce::jmax (1.0, newFsOversampled);

        fastAtk  = onePoleCoef (1.0f,   fs);
        fastRel  = onePoleCoef (20.0f,  fs);
        slowAtk  = onePoleCoef (15.0f,  fs);
        slowRel  = onePoleCoef (150.0f, fs);
        gainCoef = onePoleCoef (3.0f,   fs);
        // fastEnv / slowEnv / smoothedGain kept
    }

    void TransientShaper::setParams (float attackBipolar, float sustainBipolar) noexcept
    {
        attackAmt  = juce::jlimit (-1.0f, 1.0f, attackBipolar);
        sustainAmt = juce::jlimit (-1.0f, 1.0f, sustainBipolar);
    }

    float TransientShaper::computeGain (float detector) noexcept
    {
        const float rect = std::abs (detector);

        const float fc = (rect > fastEnv) ? fastAtk : fastRel;
        fastEnv = rect + fc * (fastEnv - rect);

        const float sc = (rect > slowEnv) ? slowAtk : slowRel;
        slowEnv = rect + sc * (slowEnv - rect);

        dsputils::flushDenormal (fastEnv);
        dsputils::flushDenormal (slowEnv);

        const bool  inTransient = fastEnv > slowEnv * kThresh;
        const float targetDb    = (inTransient ? attackAmt : sustainAmt) * kMaxGainDb;
        const float targetGain  = dsputils::dbToGain (targetDb);

        smoothedGain = targetGain + gainCoef * (smoothedGain - targetGain);
        dsputils::flushDenormal (smoothedGain);

        return smoothedGain;
    }

    float TransientShaper::processSample (float x) noexcept
    {
        return dsputils::sanitize (x * computeGain (x));
    }

    void TransientShaper::processStereo (float& l, float& r) noexcept
    {
        const float g = computeGain (0.5f * (l + r));
        l = dsputils::sanitize (l * g);
        r = dsputils::sanitize (r * g);
    }
}
