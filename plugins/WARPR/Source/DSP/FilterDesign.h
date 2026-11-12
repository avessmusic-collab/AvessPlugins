#pragma once
/*
  FilterDesign - the coefficient generators of the LINEAR filter families (types 0-27, architecture section G)
  AND the analytic magnitude response computed from those very coefficients (FilterResponse).

  One code path serves the audio and the display: FilterStage runs the FilterDesign sections sample by sample,
  FilterResponse evaluates the same sections' transfer function, so the response display is the real DSP and the
  D5 test (impulse-response FFT vs analytic) compares two independent evaluations of one coefficient set.

  Building blocks (all trapezoidal / zero-delay-feedback, "TPT"; state and coefficients in double):
      OnePole  lp = G (x - s) + s ,  s' = 2 lp - s ,  G = g / (1 + g)
      Svf      Zavalishin / Cytomic form:  v3 = x - ic2,  v1 = a1 ic1 + a2 v3,  v2 = ic2 + a2 ic1 + a3 v3,
               ic1' = 2 v1 - ic1,  ic2' = 2 v2 - ic2   with  a1 = 1 / (1 + g (g + k)),  a2 = g a1,  a3 = g a2.
               v1 is the band-pass with peak gain 1/k (k = 1/Q), v2 the low-pass.
  Every section outputs y = m0 x + m1 v1 + m2 v2 (one-pole: v2 = lp, m1 = 0), which covers LP / HP / BP / notch /
  peak / shelves / all-pass with ONE recurrence.  A design (topology) is one of

      Series    up to 16 sections in series times a scalar gain (D5a types, serial all-pass, phaser chain);
                the phaser adds a one-sample-delayed feedback fb of the chain output into its input and an output
                that sums the dry input:  y = gain (outDry x + outWet chain)           [types 0-19, 23, 27]
      Parallel  the section outputs are SUMMED (formant banks, the 2nd/1st-order all-pass blend)  [22, 24-26]
      Comb      delay-line comb (D5b): Lagrange3 fractional delay, one-pole loop damping, signed feedback  [20, 21]

  Why the response is exact: a TPT integrator is the bilinear transform with the cutoff pre-warped (g = tan(pi fc / fs)),
  so section transfer functions are the analog prototypes evaluated at  s = j tan(pi f / fs) / g.  The comb is a pure
  z-domain recurrence: H = c / (1 - sign g Hd(z) Hlp(z)) with Hd the Lagrange3 taps and Hlp the loop one-pole.

  Cutoff clamp [10 Hz, 0.45 fs] (combs: [20 Hz, 0.45 fs]).  Resonance, morph and the per-type Q tables are documented
  in FilterDesign.cpp.

  "Open" low-pass convention: a low-pass type whose cutoff is at the top of its range (>= 19999.5 Hz) with no resonance
  and no drive is NOT a filter at all (the audible band is untouched anyway).  FilterStage ramps it out over the 5 ms
  enable ramp and FilterResponse shows a flat line, so the default patch (LP 24 @ 20 kHz) is bit-exact transparent
  and costs nothing.  The rule applies to the LOW-PASS types 0-5 ONLY (a comb or formant at 20 kHz is not "open").
*/

#include "DSP/FilterTypeTable.h"

#include <array>
#include <complex>

namespace warpr
{
    struct FilterSection
    {
        enum class Kind : unsigned char { Off, OnePole, Svf };

        Kind   kind = Kind::Off;
        double g = 0.0;                          // tan(pi fc / fs) of THIS section (shelves pre-scale it)
        double k = 1.0;                          // 1 / Q (Svf)
        double a1 = 0.0, a2 = 0.0, a3 = 0.0;     // Svf: TPT coefficients; OnePole: a1 = G = g / (1 + g)
        double m0 = 0.0, m1 = 0.0, m2 = 0.0;     // y = m0 x + m1 v1 + m2 v2
    };

    /** Comb (types 20 / 21):  v = c x + sign g sat(lp(y[n - D])),  lp: one-pole, coefficient a (1 = transparent). */
    struct CombSpec
    {
        double delay = 2.0;                      // loop delay D in samples, >= 2 (Lagrange3 taps floor(D)-1 .. floor(D)+2)
        double sign = 1.0;                       // +1 positive feedback, -1 negative feedback
        double g = 0.0;                          // feedback magnitude 0 .. 0.97
        double a = 1.0;                          // loop damping coefficient (1 = bright, small = dark)
        double c = 1.0;                          // input gain sqrt (1 - g^2): white noise keeps its level
    };

    struct FilterDesign
    {
        enum class Topo : unsigned char { Series, Parallel, Comb };

        static constexpr int maxSections = 16;

        Topo topo = Topo::Series;
        int numSections = 0;                     // 0 (Series) = identity
        std::array<FilterSection, static_cast<std::size_t> (maxSections)> sec {};
        double gain = 1.0;
        double fb = 0.0;                         // Series: feedback of the previous chain output into the chain input
        double outDry = 0.0, outWet = 1.0;       // Series: y = gain (outDry x + outWet chain)
        CombSpec comb;                           // Comb
    };

    namespace filterdesign
    {
        inline constexpr double kMinCutoffHz       = 10.0;
        inline constexpr double kMaxCutoffFraction = 0.45;
        inline constexpr double kOpenCutoffHz      = 19999.5;     // "fully open" threshold of the low-pass types
        inline constexpr double kCombMinHz         = 20.0;        // lowest comb fundamental (delay line capacity fs / 20)
        inline constexpr double kCombMaxFeedback   = 0.97;        // g = 0.97 res
        inline constexpr double kCombSoftLimit     = 6.0;         // the in-loop limiter is the identity below this level

        /** [10 Hz, 0.45 fs]. */
        double clampCutoff (double hz, double fs) noexcept;

        /** Resonance boost on the highest-Q section: 1 + 14 res^1.5 (architecture "Common controls"). */
        double resonanceBoost (double res) noexcept;

        /** Band-pass / notch Q for res 0..1: 0.5 * 40^res = 0.5 .. 20, exponential so equal knob steps are equal musical steps. */
        double bandQ (double res) noexcept;

        /** Peak / shelf gain in dB for res 0..1: 36 (res - 0.5) clamped to +-18 (res 0.5 = 0 dB). */
        double shelfPeakGainDb (double res) noexcept;

        /** fc * 2^(keyTrack * (pitch - 60) / 12): keyTrack 1 follows the played pitch one-to-one (unclamped). */
        double keyTrackedCutoff (double cutoffHz, double keyTrack, double pitchSemis) noexcept;

        /** True when the "open low-pass" convention (file header) applies (low-pass types 0-5 only). */
        bool isOpenLowpass (int type, double cutoffHz, double res, double driveDb) noexcept;

        /** Fills `out` for types 0-27; any other index gives the identity design (numSections 0, gain 1). */
        void make (int type, double cutoffHz, double res, double morph, double fs, FilterDesign& out) noexcept;

        /** Complex frequency response of a design at `hz`. */
        std::complex<double> response (const FilterDesign& d, double hz, double fs) noexcept;

        /** Delay-line capacity (samples per channel) a comb needs at `fs`: fs / 20 Hz plus the interpolator margin. */
        int combBufferLength (double fs) noexcept;

        /** Third-order Lagrange fractional delay.  `delay` >= 2.  base = floor (delay); c[0..3] weight the samples that are
            base-1, base, base+1, base+2 samples old.  Shared by the audio loop and the analytic response. */
        inline void lagrange3 (double delay, int& base, double* c) noexcept
        {
            base = static_cast<int> (delay);
            const double f = delay - static_cast<double> (base);
            c[0] = -f * (f - 1.0) * (f - 2.0) * (1.0 / 6.0);
            c[1] = (f + 1.0) * (f - 1.0) * (f - 2.0) * 0.5;
            c[2] = -(f + 1.0) * f * (f - 2.0) * 0.5;
            c[3] = (f + 1.0) * f * (f - 1.0) * (1.0 / 6.0);
        }
    }

    //==========================================================================
    /** Magnitude response for the editor's response display.  Exact for every linear type (0-27); unimplemented types
        (D5c) and the "open low-pass" case return a flat 0 dB.  Drive does not change the small-signal response (the
        saturator is normalised to unity small-signal gain); mix is not part of the drawn curve.  Results are clamped to
        [-48, +24] dB (the display range). */
    struct FilterResponse
    {
        static constexpr double kFloorDb = -48.0;
        static constexpr double kCeilDb  = 24.0;

        static double magnitudeDbAt (int type, double cutoffHz, double res, double driveDb, double morph,
                                     double fs, double hz) noexcept;

        static void magnitudeDb (int type, double cutoffHz, double res, double driveDb, double morph, double fs,
                                 const double* freqsHz, int n, float* dbOut) noexcept;

        /** n points log-spaced from loHz to hiHz inclusive (the editor feed: 256 points, 20 Hz .. 20 kHz). */
        static void magnitudeDbLogSpaced (int type, double cutoffHz, double res, double driveDb, double morph,
                                          double fs, float* dbOut, int n, double loHz = 20.0, double hiHz = 20000.0) noexcept;
    };
}
