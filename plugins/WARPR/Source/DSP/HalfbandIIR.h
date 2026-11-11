#pragma once
/*
  HalfbandIIR + OsDecimator (architecture C13).

  A decimate-by-2 stage built from the two-path polyphase ALL-PASS half-band structure.  The
  coefficients come from juce::dsp::FilterDesign<float>::designIIRLowpassHalfBandPolyphaseAllpassMethod
  (the same public design JUCE's own Oversampling stage uses); the processing loops are our own
  (stereo, in place, fixed storage, no allocation) and mirror
  juce::dsp::Oversampling2TimesPolyphaseIIR::processSamplesDown:

      direct  path : cascade of first-order all-pass sections on the EVEN input samples
      delayed path : cascade on the ODD input samples, then one sample of delay
      output       : 0.5 * (delayed(n-1) + direct(n))        (unity DC gain)

  Design target: transition width 0.10 (stopband edge at 0.6 x the OUTPUT rate, passband edge at
  0.4 x), -105 dB stopband.  The same design is used for every stage of the 2x / 4x / 8x
  cascade (per-stage requirement passband <= 0.1 dB to 0.4 fs, stopband <= -90 dB above 0.6 fs
  holds per stage and for the cascade; verified by WARPR_Tests, not assumed).

  Reported latency stays 0 (instrument): the group delay is part of the note.
*/

#include <array>
#include <cstddef>

namespace warpr
{
    struct HalfbandCoeffs
    {
        static constexpr int maxStages = 32;

        int numStages = 0;
        int directStages = 0;                         // first `directStages` alphas belong to the direct path
        std::array<float, (size_t) maxStages> alpha {};

        /** Designed once (thread-safe static).  Call from prepare() so the audio thread never pays for it. */
        static const HalfbandCoeffs& get();
    };

    struct HalfbandStageState
    {
        std::array<float, (size_t) HalfbandCoeffs::maxStages> vL {}, vR {};
        float delayL = 0.0f, delayR = 0.0f;

        void reset() noexcept
        {
            vL.fill (0.0f);
            vR.fill (0.0f);
            delayL = delayR = 0.0f;
        }
    };

    /** One decimate-by-2 stage, IN PLACE: reads l[0 .. 2*outLen), writes l[0 .. outLen). */
    inline void decimateStage (const HalfbandCoeffs& c, HalfbandStageState& s,
                               float* l, float* r, int outLen) noexcept
    {
        const int total = c.numStages;
        const int direct = c.directStages;

        for (int i = 0; i < outLen; ++i)
        {
            float inL = l[2 * i], inR = r[2 * i];

            for (int n = 0; n < direct; ++n)
            {
                const float a = c.alpha[(size_t) n];
                const float oL = a * inL + s.vL[(size_t) n];
                const float oR = a * inR + s.vR[(size_t) n];
                s.vL[(size_t) n] = inL - a * oL;
                s.vR[(size_t) n] = inR - a * oR;
                inL = oL;
                inR = oR;
            }

            const float directL = inL, directR = inR;

            inL = l[2 * i + 1];
            inR = r[2 * i + 1];

            for (int n = direct; n < total; ++n)
            {
                const float a = c.alpha[(size_t) n];
                const float oL = a * inL + s.vL[(size_t) n];
                const float oR = a * inR + s.vR[(size_t) n];
                s.vL[(size_t) n] = inL - a * oL;
                s.vR[(size_t) n] = inR - a * oR;
                inL = oL;
                inR = oR;
            }

            l[i] = (s.delayL + directL) * 0.5f;
            r[i] = (s.delayR + directR) * 0.5f;
            s.delayL = inL;
            s.delayR = inR;
        }
    }

    /** Cascade for OS factors 1 / 2 / 4 / 8 (up to three stages), one instance per voice. */
    class OsDecimator
    {
    public:
        void reset() noexcept
        {
            for (auto& s : stages)
                s.reset();
        }

        /** In place: `numBase * factor` samples in, `numBase` samples out (factor 1 is a no-op). */
        void process (const HalfbandCoeffs& c, float* l, float* r, int numBase, int factor) noexcept
        {
            int len = numBase * factor;
            size_t stage = 0;

            for (int f = factor; f > 1 && stage < stages.size(); f >>= 1)
            {
                len >>= 1;
                decimateStage (c, stages[stage], l, r, len);
                ++stage;
            }
        }

    private:
        std::array<HalfbandStageState, 3> stages {};
    };
}
