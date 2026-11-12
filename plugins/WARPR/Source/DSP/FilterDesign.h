#pragma once
/*
  FilterDesign - the coefficient generators of the standard filter family (types 0-19, architecture section G)
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
  peak / shelves with ONE recurrence.  A design is up to four such sections in series times a scalar gain.

  Why the response is exact: a TPT integrator is the bilinear transform with the cutoff pre-warped (g = tan(pi fc / fs)),
  so section transfer functions are the analog prototypes evaluated at  s = j tan(pi f / fs) / g.

  Cutoff clamp [10 Hz, 0.45 fs].  Resonance, morph and the per-type Q tables are documented in FilterDesign.cpp.

  "Open" low-pass convention: a low-pass type whose cutoff is at the top of its range (>= 19999.5 Hz) with no resonance
  and no drive is NOT a filter at all (the audible band is untouched anyway).  FilterStage ramps it out over the 5 ms
  enable ramp and FilterResponse shows a flat line, so the default patch (LP 24 @ 20 kHz) is bit-exact transparent
  and costs nothing.
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

    struct FilterDesign
    {
        static constexpr int maxSections = 4;

        int numSections = 0;                     // 0 = identity
        std::array<FilterSection, static_cast<std::size_t> (maxSections)> sec {};
        double gain = 1.0;
    };

    namespace filterdesign
    {
        inline constexpr double kMinCutoffHz       = 10.0;
        inline constexpr double kMaxCutoffFraction = 0.45;
        inline constexpr double kOpenCutoffHz      = 19999.5;     // "fully open" threshold of the low-pass types

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

        /** True when the "open low-pass" convention (file header) applies. */
        bool isOpenLowpass (int type, double cutoffHz, double res, double driveDb) noexcept;

        /** Fills `out` for types 0-19; any other index gives the identity design (numSections 0, gain 1). */
        void make (int type, double cutoffHz, double res, double morph, double fs, FilterDesign& out) noexcept;

        /** Complex frequency response of a design at `hz`. */
        std::complex<double> response (const FilterDesign& d, double hz, double fs) noexcept;
    }

    //==========================================================================
    /** Magnitude response for the editor's response display.  Linear types only in D5a; unimplemented types and the
        "open low-pass" case return a flat 0 dB.  Drive does not change the small-signal response (the saturator is
        normalised to unity small-signal gain).  Results are clamped to [-48, +24] dB (the display range). */
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
