#include "DSP/LfoEngine.h"

namespace warpr
{
    double lfoShapePhase (int mode, int dir, double offset, double progress, bool& envDone) noexcept
    {
        const double q = offset + progress;

        if (mode == (int) LfoMode::Env)
        {
            double total = q;

            if (total >= 1.0)
            {
                total = 1.0;
                envDone = true;
            }

            return dir == (int) LfoDir::Reverse ? 1.0 - total : total;     // Ping-Pong plays as Forward
        }

        const double k = std::floor (q);
        const double f = q - k;

        if (dir == (int) LfoDir::Reverse)
            return 1.0 - f;

        if (dir == (int) LfoDir::PingPong)
            return (static_cast<long long> (k) & 1LL) != 0 ? 1.0 - f : f;

        return f;
    }

    float lfoStartValue (const LfoBlockParams& p, const LfoShapeTable* table) noexcept
    {
        bool done = false;
        const double ph = lfoShapePhase (p.mode, p.dir, (double) p.phaseDeg / 360.0, 0.0, done);
        return table != nullptr ? table->eval (ph) : 0.5f;
    }

    float advanceLfo (LfoInstance& s, const LfoBlockParams& p, const LfoShapeTable* table, const LfoChunkTiming& t) noexcept
    {
        const bool sync = p.sync == 1;
        const double fs = t.fs > 0.0 ? t.fs : 44100.0;
        double bpc = 1.0, hz = 1.0, periodSec = 1.0;

        if (sync)
        {
            bpc = lfoBeatsPerCycle (lfoDivisionIndex (p.rate), t.barBeats);
            periodSec = bpc * 60.0 / std::max (1.0, t.bpm);
        }
        else
        {
            hz = lfoRateHz (p.rate);
            periodSec = 1.0 / hz;
        }

        if (p.mode == (int) LfoMode::Free && sync)
        {
            s.progress = t.ppq1 / bpc;                                       // grid-locked: recomputed from the host position
        }
        else
        {
            s.progress += sync ? (t.ppq1 - t.ppq0) / bpc : (double) t.n * hz / fs;

            if (s.progress > 1.0e6 && p.mode != (int) LfoMode::Env)          // keep precision; 1e6 is even and integral
                s.progress -= 1.0e6;
        }

        bool done = s.envDone;
        const double ph = lfoShapePhase (p.mode, p.dir, (double) p.phaseDeg / 360.0, s.progress, done);
        s.envDone = done;
        s.phase = (float) ph;

        const float x = table != nullptr ? table->eval (ph) : 0.5f;

        if (s.fresh)
        {
            s.slewed = s.out = x;
            s.fresh = false;
            return x;
        }

        const double nD = (double) t.n;
        const float kSlew = (float) (1.0 - std::exp (-nD / (0.002 * fs)));
        s.slewed += (x - s.slewed) * kSlew;

        if (p.smooth > 0.0f)
        {
            const double tau = (double) p.smooth * (double) p.smooth * std::min (1.0, 0.5 * periodSec);

            if (tau > 1.0e-6)
            {
                const float kSm = (float) (1.0 - std::exp (-nD / (tau * fs)));
                s.out += (s.slewed - s.out) * kSm;
                return s.out;
            }
        }

        s.out = s.slewed;
        return s.out;
    }

    //==========================================================================
    void advanceLfoBank (const LfoBankIO& io, LfoInstance* inst, float* prevOut, float* start, float* end) noexcept
    {
        const int count = io.lfoCount < kNumLfos ? io.lfoCount : kNumLfos;

        // every LFO's start value: owned = its previous end, others = the external start; end defaults to the same
        for (int j = 0; j < kNumLfos; ++j)
        {
            if (j >= count)
            {
                start[j] = end[j] = 0.5f;
            }
            else if (((io.ownMask >> j) & 1u) != 0u)
            {
                start[j] = end[j] = prevOut[j];
            }
            else
            {
                start[j] = io.extStart[j];
                end[j] = io.extEnd[j];
            }
        }

        float srcMod[kNumLfos];

        for (int i = 0; i < count; ++i)
        {
            if (((io.ownMask >> i) & 1u) == 0u)
                continue;

            LfoBlockParams p = io.params[i];

            if (io.mm != nullptr && ((io.mm->lfoDestinationMask() >> i) & 1u) != 0u)
            {
                // LFO-on-LFO: index-ordered, lower sources are same-chunk, the rest one chunk late
                for (int j = 0; j < kNumLfos; ++j)
                    srcMod[j] = j >= count ? 0.0f : 2.0f * (j < i ? end[j] : start[j]) - 1.0f;

                const ParamRange* ranges = paramRanges();
                const int iRate = lfoParamIndex (i, 3), iPhase = lfoParamIndex (i, 4), iSmooth = lfoParamIndex (i, 5);

                p.rate     = applyModulation (ranges[iRate],   p.rate,     io.mm->delta (iRate,   srcMod));
                p.phaseDeg = applyModulation (ranges[iPhase],  p.phaseDeg, io.mm->delta (iPhase,  srcMod));
                p.smooth   = applyModulation (ranges[iSmooth], p.smooth,   io.mm->delta (iSmooth, srcMod));
            }

            end[i] = advanceLfo (inst[i], p, io.tables[i], io.timing);
            prevOut[i] = end[i];
        }
    }
}
