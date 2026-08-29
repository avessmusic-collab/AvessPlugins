#include "DSP/Waveshaper.h"
#include "Utilities/DSPUtils.h"

#include <cmath>

namespace kickr
{
    namespace
    {
        inline float sgn (float v) noexcept { return v < 0.0f ? -1.0f : 1.0f; }
    }

    void Waveshaper::prepare (double fsOversampled) noexcept
    {
        fs = juce::jmax (1.0, fsOversampled);

        // One-pole "ms += a*(x*x - ms)" for a ~kRmsWindowSamples window.
        rmsAlpha = 1.0f / static_cast<float> (juce::jmax (1, kRmsWindowSamples));

        // One-pole "makeup += a*(target - makeup)" for ~kMakeupSmoothMs.
        const float tSamples = juce::jmax (1.0f, 0.001f * kMakeupSmoothMs
                                                 * static_cast<float> (fs));
        makeupAlpha = 1.0f - std::exp (-1.0f / tSamples);

        reset();
    }

    void Waveshaper::reset() noexcept
    {
        msIn      = 0.0f;
        msOut     = 0.0f;
        makeup    = 1.0f;
        shCounter = 0;
        shHeld    = 0.0f;
        primed    = false;
        warmup    = 0;
    }

    void Waveshaper::updateOversampledRate (double newFsOversampled) noexcept
    {
        fs = juce::jmax (1.0, newFsOversampled);

        const float tSamples = juce::jmax (1.0f, 0.001f * kMakeupSmoothMs
                                                 * static_cast<float> (fs));
        makeupAlpha = 1.0f - std::exp (-1.0f / tSamples);
        // msIn / msOut / makeup / shCounter / shHeld / primed / warmup kept
    }

    void Waveshaper::setParams (float drive, float character, float driveMix) noexcept
    {
        driveRaw = juce::jlimit (0.0f, 1.0f, drive);
        gDrive   = dsputils::dbToGain (driveRaw * kDriveMaxDb);
        mix      = juce::jlimit (0.0f, 1.0f, driveMix);

        const float c = juce::jlimit (0.0f, 1.0f, character) * 6.0f;
        float       s = std::floor (c);
        if (s > 5.0f) s = 5.0f;
        seg     = static_cast<int> (s);
        segFrac = c - s;   // 0..1 ; 0 at character = k/6 exactly

        if (! primed)
        {
            const float m0 = kCurveMakeup[static_cast<size_t> (seg)];
            const float m1 = kCurveMakeup[static_cast<size_t> (seg + 1)];
            makeup = m0 + segFrac * (m1 - m0);
            warmup = kRmsWindowSamples;   // hold `makeup` frozen until the meters fill
            primed = true;
        }
    }

    float Waveshaper::curve (int index, float u) noexcept
    {
        switch (index)
        {
            case 0:   // tanh
                return std::tanh (u);

            case 1:   // cubic
                return (std::abs (u) < 1.0f) ? (u - u * u * u / 3.0f)
                                             : (sgn (u) * (2.0f / 3.0f));

            case 2:   // asymmetric (adds even harmonics + DC -> DC-blocked downstream)
                return (u <= 0.0f) ? std::tanh (u) : (1.0f - std::exp (-u));

            case 3:   // soft-clip (quintic) — smooth knee, 1.25*(1 - 0.2) = 1.0 at |u| = 1
                return (std::abs (u) < 1.0f)
                           ? (1.25f * (u - 0.2f * u * u * u * u * u))
                           : sgn (u);

            case 4:   // hard-clip
                return juce::jlimit (-1.0f, 1.0f, u);

            case 5:   // foldback (triangle fold, bounded)
            {
                float y = u;
                for (int k = 0; k < 8 && std::abs (y) > 1.0f; ++k)
                    y = 2.0f * sgn (y) - y;
                return juce::jlimit (-1.0f, 1.0f, y);
            }

            default:  // 6 — bitcrush / decimate
            {
                const float bits = 12.0f - 10.0f * driveRaw;                 // lerp(12, 2, drive)
                const float L    = std::exp2 (bits - 1.0f);                  // 2^(bits-1)
                int         D    = static_cast<int> (std::lround (1.0f + 7.0f * driveRaw)); // lerp(1, 8)
                if (D < 1) D = 1;

                if (shCounter <= 0)
                {
                    // Quantise, then bound to +/-1 like every other curve so a large
                    // driven input (gDrive up to ~63x) can't leave the shaper at 60x.
                    shHeld    = juce::jlimit (-1.0f, 1.0f, std::round (u * L) / L);
                    shCounter = D;
                }
                --shCounter;

                dsputils::flushDenormal (shHeld);
                return shHeld;
            }
        }
    }

    float Waveshaper::processSample (float x) noexcept
    {
        const float xClean = x;                 // PRE-drive-gain -> driveMix = 0 is unity
        const float u      = gDrive * x;

        // Equal-gain (linear) crossfade of the two bracketing transfer functions.
        const float a = curve (seg,     u);
        const float b = curve (seg + 1, u);
        float yShaped = (1.0f - segFrac) * a + segFrac * b;
        yShaped = dsputils::sanitize (yShaped);

        // Adaptive short-window RMS makeup. Reference = the CLEAN pre-drive-gain input
        // (not `u`): the makeup restores the shaper output to the clean signal's loudness,
        // so sweeping `character` — and cranking `drive` — never jumps in level. Measured
        // on `u` instead, the ratio blows past the +/-12 dB clamp at any real drive and
        // the compensation stops working (that was the character-sweep failure).
        msIn  += rmsAlpha * (xClean * xClean - msIn);
        msOut += rmsAlpha * (yShaped * yShaped - msOut);
        dsputils::flushDenormal (msIn);
        dsputils::flushDenormal (msOut);

        const float target = juce::jlimit (kMakeupMin, kMakeupMax,
                                           std::sqrt (msIn / juce::jmax (msOut, 1.0e-9f)));

        if (warmup > 0)
            --warmup;                            // hold `makeup` at the primed value
        else
            makeup += makeupAlpha * (target - makeup);

        dsputils::flushDenormal (makeup);

        const float shaped = makeup * yShaped;

        // Parallel blend clean <-> shaped. driveMix == 0.0f -> xClean exactly.
        return dsputils::sanitize (xClean + mix * (shaped - xClean));
    }
}
