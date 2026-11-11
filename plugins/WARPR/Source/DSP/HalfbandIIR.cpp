#include "DSP/HalfbandIIR.h"

#include <juce_dsp/juce_dsp.h>

namespace warpr
{
    const HalfbandCoeffs& HalfbandCoeffs::get()
    {
        static const HalfbandCoeffs instance = []
        {
            HalfbandCoeffs c;

            const auto structure = juce::dsp::FilterDesign<float>::designIIRLowpassHalfBandPolyphaseAllpassMethod (0.10f, -105.0f);

            int n = 0;

            for (int i = 0; i < structure.directPath.size() && n < maxStages; ++i)
                c.alpha[(size_t) n++] = structure.directPath.getObjectPointer (i)->coefficients[0];

            c.directStages = n;

            // delayedPath[0] is the pure one-sample delay element; the all-pass sections start at 1
            for (int i = 1; i < structure.delayedPath.size() && n < maxStages; ++i)
                c.alpha[(size_t) n++] = structure.delayedPath.getObjectPointer (i)->coefficients[0];

            c.numStages = n;
            return c;
        }();

        return instance;
    }
}
