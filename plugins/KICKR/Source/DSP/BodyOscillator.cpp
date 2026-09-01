#include "DSP/BodyOscillator.h"
#include "Utilities/DSPUtils.h"

#include <cmath>

namespace kickr
{
    void BodyOscillator::prepare (double fsOS) noexcept
    {
        fsOversampled = juce::jmax (1.0, fsOS);
        morphSm.reset (fsOversampled, kMorphSmoothSeconds);   // also snaps current -> target
        shapeCoef = static_cast<float> (std::exp (-1.0 / (kShapeDecaySeconds * fsOversampled)));
        reset();
        setFrequency (frequencyHz);
    }

    void BodyOscillator::reset() noexcept
    {
        phase    = 0.0;
        shapeEnv = 0.0f;   // silent voice: no shaping armed until the next noteOn
        morphSm.setCurrentAndTargetValue (morphSm.getTargetValue());   // a fresh/stolen voice starts AT the current morph
    }

    void BodyOscillator::updateOversampledRate (double newFsOversampled) noexcept
    {
        fsOversampled = juce::jmax (1.0, newFsOversampled);
        setFrequency (frequencyHz);   // recompute the increment only; phase is kept
        shapeCoef = static_cast<float> (std::exp (-1.0 / (kShapeDecaySeconds * fsOversampled)));
        // Re-rate the morph ramp for the new fsOversampled. This snaps current -> target,
        // which is only a discontinuity if a morph ramp was mid-flight at the exact OS
        // switch — and that instant is already covered by KickEngine's OS-switch fade.
        morphSm.reset (fsOversampled, kMorphSmoothSeconds);
    }

    void BodyOscillator::setFrequency (float hz) noexcept
    {
        frequencyHz    = hz;
        phaseIncrement = juce::MathConstants<double>::twoPi
                       * static_cast<double> (hz) / fsOversampled;
    }

    float BodyOscillator::renderSample() noexcept
    {
        float s;

        // Effective skew this sample: smoothed knob value x the attack-only envelope.
        // When the knob target is 0 and the ramp has settled, getNextValue() returns
        // exactly 0.0f -> k == 0 -> the original pure-sine path (bit-identical output).
        const float morphAmt = morphSm.getNextValue();
        const float k        = morphAmt * shapeEnv * kMaxSkew;

        shapeEnv *= shapeCoef;
        dsputils::flushDenormal (shapeEnv);

        if (k <= 0.0f)
        {
            s = static_cast<float> (std::sin (phase));
        }
        else
        {
            // CZ-style "saw" phase distortion (see header): run the phase fast through the
            // first d of the cycle and slow through the rest, then read a sine. k <= kMaxSkew
            // (< 1) keeps d strictly positive.
            const double t = phase / juce::MathConstants<double>::twoPi;   // [0, 1) — phase is wrapped
            const double d = 0.5 * (1.0 - static_cast<double> (k));
            const double warped = t < d ? t * (0.5 / d)
                                        : 0.5 + (t - d) * (0.5 / (1.0 - d));
            s = static_cast<float> (std::sin (juce::MathConstants<double>::twoPi * warped));
        }

        phase += phaseIncrement;
        if (phase >= juce::MathConstants<double>::twoPi)
            phase -= juce::MathConstants<double>::twoPi;
        else if (phase < 0.0)
            phase += juce::MathConstants<double>::twoPi;

        return dsputils::sanitize (s);
    }
}
