#include "DSP/BodyOscillator.h"
#include "Utilities/DSPUtils.h"

#include <cmath>

namespace kickr
{
    void BodyOscillator::prepare (double fsOS) noexcept
    {
        fsOversampled = juce::jmax (1.0, fsOS);
        reset();
        setFrequency (frequencyHz);
    }

    void BodyOscillator::reset() noexcept
    {
        phase = 0.0;
    }

    void BodyOscillator::setFrequency (float hz) noexcept
    {
        frequencyHz    = hz;
        phaseIncrement = juce::MathConstants<double>::twoPi
                       * static_cast<double> (hz) / fsOversampled;
    }

    float BodyOscillator::renderSample() noexcept
    {
        const auto s = static_cast<float> (std::sin (phase));

        phase += phaseIncrement;
        if (phase >= juce::MathConstants<double>::twoPi)
            phase -= juce::MathConstants<double>::twoPi;
        else if (phase < 0.0)
            phase += juce::MathConstants<double>::twoPi;

        return dsputils::sanitize (s);
    }
}
