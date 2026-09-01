#include "DSP/BodyOscillator.h"
#include "Utilities/DSPUtils.h"

#include <cmath>

namespace kickr
{
    void BodyOscillator::prepare (double fsOS) noexcept
    {
        fsOversampled = juce::jmax (1.0, fsOS);
        morphSm.reset (fsOversampled, kMorphSmoothSeconds);   // also snaps current -> target
        reset();
        setFrequency (frequencyHz);
    }

    void BodyOscillator::reset() noexcept
    {
        phase = 0.0;
        morphSm.setCurrentAndTargetValue (morphSm.getTargetValue());   // a fresh/stolen voice starts AT the current morph
    }

    void BodyOscillator::updateOversampledRate (double newFsOversampled) noexcept
    {
        fsOversampled = juce::jmax (1.0, newFsOversampled);
        setFrequency (frequencyHz);   // recompute the increment only; phase is kept
        // Re-rate the ramp for the new fsOversampled. This snaps current -> target, which is
        // only a discontinuity if a morph ramp was mid-flight at the exact OS switch — and
        // that instant is already covered by KickEngine's OS-switch output fade.
        morphSm.reset (fsOversampled, kMorphSmoothSeconds);
    }

    void BodyOscillator::setFrequency (float hz) noexcept
    {
        frequencyHz    = hz;
        phaseIncrement = juce::MathConstants<double>::twoPi
                       * static_cast<double> (hz) / fsOversampled;
    }

    // All three shapes are odd, phase-aligned to std::sin: value 0 with a RISING slope at
    // t01 == 0 (and, for triangle/sine, again — falling — at t01 == 0.5), so morphing
    // between any two adjacent shapes never introduces a phase-alignment discontinuity.
    float BodyOscillator::triangleAt (double t01) noexcept
    {
        // Rises 0->1 over [0, 0.25], falls 1->-1 over [0.25, 0.75], rises -1->0 over [0.75, 1].
        const double x = t01 - std::floor (t01);
        double v;
        if (x < 0.25)       v = 4.0 * x;
        else if (x < 0.75)  v = 2.0 - 4.0 * x;
        else                v = 4.0 * x - 4.0;
        return static_cast<float> (v);
    }

    float BodyOscillator::sawAt (double t01) noexcept
    {
        // Rising ramp with its discontinuity moved to t01 == 0.5 (not 0), so the zero
        // crossing at t01 == 0 is rising, matching sine/triangle.
        const double x = t01 - std::floor (t01);
        const double shifted = x + 0.5;
        const double wrapped = shifted - std::floor (shifted);
        return static_cast<float> (2.0 * wrapped - 1.0);
    }

    float BodyOscillator::squareAt (double t01) noexcept
    {
        const double x = t01 - std::floor (t01);
        return x < 0.5 ? 1.0f : -1.0f;
    }

    float BodyOscillator::renderSample() noexcept
    {
        float s;

        // Per-sample smoothed morph position. When the target is 0 and the ramp has
        // settled, getNextValue() returns exactly 0.0f -> the original pure-sine path.
        const float morphAmt = morphSm.getNextValue();

        if (morphAmt <= 0.0f)
        {
            s = static_cast<float> (std::sin (phase));
        }
        else
        {
            const double t01 = phase / juce::MathConstants<double>::twoPi;

            // 3-segment crossfade: Sine->Triangle->Saw->Square across morphAmt [0,1].
            const float seg = morphAmt * 3.0f;
            if (seg < 1.0f)
            {
                const float u = seg;
                s = juce::jmap (u, 0.0f, 1.0f, static_cast<float> (std::sin (phase)), triangleAt (t01));
            }
            else if (seg < 2.0f)
            {
                const float u = seg - 1.0f;
                s = juce::jmap (u, 0.0f, 1.0f, triangleAt (t01), sawAt (t01));
            }
            else
            {
                const float u = juce::jmin (1.0f, seg - 2.0f);
                s = juce::jmap (u, 0.0f, 1.0f, sawAt (t01), squareAt (t01));
            }
        }

        phase += phaseIncrement;
        if (phase >= juce::MathConstants<double>::twoPi)
            phase -= juce::MathConstants<double>::twoPi;
        else if (phase < 0.0)
            phase += juce::MathConstants<double>::twoPi;

        return dsputils::sanitize (s);
    }
}
