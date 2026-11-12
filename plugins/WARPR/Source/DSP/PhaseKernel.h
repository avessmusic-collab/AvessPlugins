#pragma once
/*
  PhaseKernel - the STATEFUL phase / amplitude kernels of the modulation family (warp modes 54-59, phase D6).

  The map modes (PhaseWarp.h evalMap) are pure functions of the cycle clock; the modulation modes also need a modulator
  and feedback state.  `ModState` is that state; one instance lives in the oscillator warp stage (two while a mode
  switch cross-fades) and one per grain head (D7b per-grain warps), so a granular voice carries its own Self FM /
  Cross Mod / modulator state per grain exactly like the Classic oscillator carries it per note.

     54 FM         Delta = I M,   M = the integral of the modulator m (Modulator: see ModOsc.h; unit peak)
                   I = 4 a (0.25 + 1.5 A) cycles;  B = modulator phase offset (cycles)
     55 PM         Delta = I m,   I = 2 a (0.25 + 1.5 A) cycles;  B = self feedback of the modulator onto its own phase
                   (0.25 B (m[n-1] + m[n-2]) / 2 cycles; the first version used 0.5 B m[n-1], which at B = 1 made the
                   modulator itself jump by up to a cycle per sample)
     56 AM         y = x (1 - a (1 - s)),  s = (1 - A) u + A m  (u = (m + 1) / 2 unipolar, A blends toward bipolar),
                   then biased toward 1 by B:  s += B (1 - s)
     57 RM         y = x (1 + a (m_d - 1)) = lerp (x, x m_d, a),  m_d = limit (d m) / limit (d),  d = 0.2 + 4.8 A (tanh-like
                   pre-gain), B unused
     58 Self FM    Delta = I fb[n-1],  I = 1.5 a (0.5 + 0.5 B) <= 1.5 cycles,
                   fb = softLimit (one-pole LP (output)),  LP cut = 100 Hz 2^(8A) (100 Hz .. 25.6 kHz); ignores the modulator
     59 Cross Mod  mutual PM:  carrier Delta = Ic m[n-1],  modulator phase += Im fb[n-1]  (fb = softLimit (output),
                   Ic = 2 a min (1, 2 (1 - A)), Im = 2 a B min (1, 2 A)): A is the carrier : modulator index ratio,
                   B the mutual feedback (B = 0: plain PM with a one-sample-late modulator)
  The Sample source feeds every mode through SampleMod (ModOsc.h) and a caller-supplied `readMod (offsetCycles)` that
  returns the raw modulator read at the modulator playhead shifted by `offsetCycles` root cycles.

  STABILITY (architecture: "|I| <= 1.5 cycles + tanh + NaN guard").  The offsets are bounded by construction
  (|Delta| <= 7 cycles for FM, 2 for PM / Cross, 1.5 for Self FM), the feedback value is softLimited to +-1, every read
  returns bounded sample data, and the FEEDBACK GUARD `feed()` screens the value that closes the loop: a non-finite or
  absurd (> 1e6) output resets the loop state to zero and counts a trip (`ModState::guardTrips`) instead of letting it
  circulate.  A guard that never trips in normal use is exercised by the tests with an injected NaN.

  Everything is allocation-free and double precision.
*/

#include "DSP/ModOsc.h"
#include "DSP/PhaseWarp.h"

#include <algorithm>
#include <cmath>

namespace warpr::phasewarp
{
    struct ModState
    {
        double phi = 0.0;                    // modulator phase of the basic shapes (cycles)
        modosc::SampleMod sm;                // Sample-source follower / integrator
        double fbLp = 0.0;                   // Self FM: one-pole low-passed output
        double fbOut = 0.0;                  // the limited feedback value the NEXT sample uses
        double mPrev = 0.0;                  // Cross Mod: previous modulator value
        double mSelf = 0.0, mSelf2 = 0.0;    // PM: the last two modulator values (self feedback)
        int    guardTrips = 0;               // feedback-guard activations (cumulative, survives reset)
        bool   primed = false;

        /** Forgets the loop and modulator state (the cumulative guard counter is kept). */
        void reset() noexcept
        {
            const int trips = guardTrips;
            *this = ModState {};
            guardTrips = trips;
        }
    };

    /** Per-sample inputs of the modulation kernels. */
    struct ModIn
    {
        double theta = 0.0;                  // carrier cycle clock
        double dTheta = 0.0;                 // its increment per oscillator-rate sample
        double ratio = 1.0;                  // modOscRatio
        int    src = 4;                      // modOscSource 0..4
        const modosc::Coefs* coefs = nullptr;
    };

    inline void primeMod (ModState& st, const ModIn& in) noexcept
    {
        if (! st.primed)
        {
            st.phi = modosc::frac (in.ratio * in.theta);          // locked to the carrier phase at the start
            st.primed = true;
        }
    }

    inline void advanceMod (ModState& st, const ModIn& in) noexcept
    {
        st.phi = modosc::frac (st.phi + in.ratio * in.dTheta);
    }

    /** The phase-domain modulation kernels 54, 55, 58, 59.  `readMod (double offsetCycles) -> double` is only called
        for the Sample source.  Neutral PhaseOut (and no state change) at a <= 0. */
    template <class ReadMod>
    inline PhaseOut evalMod (int mode, double a, double A, double B, const ModIn& in, ModState& st, ReadMod&& readMod) noexcept
    {
        PhaseOut o;

        if (a <= 0.0)
            return o;

        primeMod (st, in);

        const double dphi = in.ratio * in.dTheta;
        const bool sample = in.src >= 4;
        const int src = in.src < 0 ? 0 : (in.src > 3 ? 3 : in.src);
        const modosc::Coefs& cf = *in.coefs;

        switch (mode)
        {
            case 54:
            {
                double M = 0.0;

                if (sample)
                {
                    double m = 0.0;
                    st.sm.process (readMod (B), dphi, cf, m, M);
                }
                else
                {
                    M = modosc::integral (src, st.phi + B, dphi);
                }

                o.d0 = 4.0 * a * (0.25 + 1.5 * A) * M;
                break;
            }

            case 55:
            {
                const double fb = 0.25 * B * 0.5 * (st.mSelf + st.mSelf2);       // operator-style feedback: mean of the last two values, <= 1/4 cycle
                double m = 0.0;

                if (sample)
                {
                    double M = 0.0;
                    st.sm.process (readMod (fb), dphi, cf, m, M);
                }
                else
                {
                    m = modosc::shape (src, st.phi + fb, dphi);
                }

                st.mSelf2 = st.mSelf;
                st.mSelf = m;
                o.d0 = 2.0 * a * (0.25 + 1.5 * A) * m;
                break;
            }

            case 58:
                o.d0 = 1.5 * a * (0.5 + 0.5 * B) * st.fbOut;
                break;

            case 59:
            {
                const double Ic = 2.0 * a * std::min (1.0, 2.0 * (1.0 - A));
                const double Im = 2.0 * a * B * std::min (1.0, 2.0 * A);
                const double off = Im * st.fbOut;
                double m = 0.0;

                if (sample)
                {
                    double M = 0.0;
                    st.sm.process (readMod (off), dphi, cf, m, M);
                }
                else
                {
                    m = modosc::shape (src, st.phi + off, dphi);
                }

                o.d0 = Ic * st.mPrev;
                st.mPrev = m;
                break;
            }

            default:
                break;
        }

        advanceMod (st, in);
        return o;
    }

    /** The amplitude-domain modulation kernels 56 (AM) and 57 (RM): the gain applied to the dry value.
        Exactly 1.0 (and no state change) at a <= 0. */
    template <class ReadMod>
    inline double evalAmpGain (int mode, double a, double A, double B, const ModIn& in, ModState& st, ReadMod&& readMod) noexcept
    {
        if (a <= 0.0)
            return 1.0;

        primeMod (st, in);

        const double dphi = in.ratio * in.dTheta;
        double m = 0.0;

        if (in.src >= 4)
        {
            double M = 0.0;
            st.sm.process (readMod (0.0), dphi, *in.coefs, m, M);
        }
        else
        {
            m = modosc::shape (in.src < 0 ? 0 : in.src, st.phi, dphi);
        }

        advanceMod (st, in);

        if (mode == 56)
        {
            const double u = 0.5 * (m + 1.0);
            double s = (1.0 - A) * u + A * m;
            s += B * (1.0 - s);
            return 1.0 - a * (1.0 - s);
        }

        const double d = 0.2 + 4.8 * A;
        const double md = modosc::softLimit (d * m) / modosc::softLimit (d);
        return 1.0 + a * (md - 1.0);
    }

    /** Feedback closing of Self FM (58) and Cross Mod (59): `y` is the kernel's wet output (mono mix, before the dry /
        wet mix).  The FEEDBACK GUARD lives here: a non-finite or absurd value resets the loop and counts a trip. */
    inline void feed (int mode, double A, double y, double osRate, ModState& st) noexcept
    {
        if (mode != 58 && mode != 59)
            return;

        if (! (std::abs (y) < 1.0e6))
        {
            ++st.guardTrips;
            st.fbLp = 0.0;
            st.fbOut = 0.0;
            st.mPrev = 0.0;
            return;
        }

        double v = y;

        if (mode == 58)
        {
            const double x = 6.283185307179586 * 100.0 * fastmath::exp2 (8.0 * A) / std::max (osRate, 1000.0);
            const double k = x / (1.0 + x);
            st.fbLp += k * (y - st.fbLp);
            v = st.fbLp;
        }

        st.fbOut = modosc::softLimit (v);
    }
}
