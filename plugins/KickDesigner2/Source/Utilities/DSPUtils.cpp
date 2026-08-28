#include "Utilities/DSPUtils.h"

namespace kd2::dsputils
{
    void sanitizeBuffer (juce::AudioBuffer<float>& buffer) noexcept
    {
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        {
            auto* data = buffer.getWritePointer (ch);
            for (int i = 0; i < buffer.getNumSamples(); ++i)
                data[i] = sanitize (data[i]);
        }
    }
}
