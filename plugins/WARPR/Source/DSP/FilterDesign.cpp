#include "DSP/FilterDesign.h"

#include <algorithm>
#include <cmath>

namespace warpr
{
    namespace
    {
        constexpr double kPi = 3.14159265358979323846;

        /** Q tables of the cascades, highest-Q section LAST (it receives the resonance boost).
            Butterworth values are the spec's; the "Bessel-like" column is the matching maximally-flat-delay Q set
            (morph 0 -> Butterworth, 1 -> Bessel-like).  The Bessel frequency scaling is deliberately NOT applied:
            every section stays at the same fc so the cutoff knob keeps meaning "about -3 dB" at morph 0 and a
            slightly lower corner at morph 1 (documented, tested only for the Butterworth end). */
        struct CascadeSpec
        {
            bool   onePole;
            int    numSvf;
            double butter[4];
            double bessel[4];
        };

        constexpr CascadeSpec kCascades[6] = {
            { true,  0, { 0.0, 0.0, 0.0, 0.0 },                 { 0.0, 0.0, 0.0, 0.0 } },                  //  6 dB
            { false, 1, { 0.70710678, 0.0, 0.0, 0.0 },          { 0.57735027, 0.0, 0.0, 0.0 } },           // 12 dB
            { true,  1, { 1.0, 0.0, 0.0, 0.0 },                 { 0.69104, 0.0, 0.0, 0.0 } },              // 18 dB
            { false, 2, { 0.54119610, 1.30656296, 0.0, 0.0 },   { 0.52193, 0.80554, 0.0, 0.0 } },          // 24 dB
            { false, 3, { 0.51763809, 0.70710678, 1.93185165, 0.0 }, { 0.51032, 0.61119, 1.02331, 0.0 } }, // 36 dB
            { false, 4, { 0.50979558, 0.60134489, 0.89997622, 2.56291545 }, { 0.50599, 0.55961, 0.71085, 1.22576 } }   // 48 dB
        };

        double clamp01 (double x) noexcept { return x < 0.0 ? 0.0 : (x > 1.0 ? 1.0 : x); }
        double lerp (double a, double b, double t) noexcept { return a + (b - a) * t; }
        double gOf (double fc, double fs) noexcept { return std::tan (kPi * fc / fs); }

        void setOnePole (FilterSection& s, double g, double m0, double m2) noexcept
        {
            s.kind = FilterSection::Kind::OnePole;
            s.g = g;
            s.k = 1.0;
            s.a1 = g / (1.0 + g);
            s.a2 = 0.0;
            s.a3 = 0.0;
            s.m0 = m0;
            s.m1 = 0.0;
            s.m2 = m2;
        }

        void setSvf (FilterSection& s, double g, double k, double m0, double m1, double m2) noexcept
        {
            s.kind = FilterSection::Kind::Svf;
            s.g = g;
            s.k = k;
            s.a1 = 1.0 / (1.0 + g * (g + k));
            s.a2 = g * s.a1;
            s.a3 = g * s.a2;
            s.m0 = m0;
            s.m1 = m1;
            s.m2 = m2;
        }

        FilterSection& next (FilterDesign& d) noexcept
        {
            return d.sec[static_cast<std::size_t> (d.numSections++)];
        }

        /** Low-pass / high-pass cascade.  HP taps: one-pole  x - lp ;  SVF  x - k bp - lp. */
        void buildCascade (FilterDesign& d, const CascadeSpec& spec, bool highpass, double morph, double boost, double g) noexcept
        {
            if (spec.onePole)
                setOnePole (next (d), g, highpass ? 1.0 : 0.0, highpass ? -1.0 : 1.0);

            for (int i = 0; i < spec.numSvf; ++i)
            {
                double q = lerp (spec.butter[i], spec.bessel[i], morph);

                if (i == spec.numSvf - 1)
                    q *= boost;

                const double k = 1.0 / q;

                if (highpass)
                    setSvf (next (d), g, k, 1.0, -k, -1.0);
                else
                    setSvf (next (d), g, k, 0.0, 0.0, 1.0);
            }
        }
    }

    //==========================================================================
    namespace filterdesign
    {
        double clampCutoff (double hz, double fs) noexcept
        {
            return std::min (std::max (hz, kMinCutoffHz), kMaxCutoffFraction * fs);
        }

        double resonanceBoost (double res) noexcept
        {
            return 1.0 + 14.0 * std::pow (clamp01 (res), 1.5);
        }

        double bandQ (double res) noexcept
        {
            return 0.5 * std::pow (40.0, clamp01 (res));
        }

        double shelfPeakGainDb (double res) noexcept
        {
            return std::min (18.0, std::max (-18.0, 36.0 * (clamp01 (res) - 0.5)));
        }

        double keyTrackedCutoff (double cutoffHz, double keyTrack, double pitchSemis) noexcept
        {
            return cutoffHz * std::exp2 (keyTrack * (pitchSemis - 60.0) / 12.0);
        }

        bool isOpenLowpass (int type, double cutoffHz, double res, double driveDb) noexcept
        {
            return type >= 0 && type < kNumLowpassTypes && cutoffHz >= kOpenCutoffHz && res < 0.01 && driveDb < 0.01;
        }

        void make (int type, double cutoffHz, double res, double morph, double fs, FilterDesign& d) noexcept
        {
            d = FilterDesign {};
            res = clamp01 (res);
            morph = clamp01 (morph);

            const double fc = clampCutoff (cutoffHz, fs);
            const double g = gOf (fc, fs);

            if (type >= 0 && type <= 5)                                     // LP 6 .. LP 48
            {
                buildCascade (d, kCascades[type], false, morph, resonanceBoost (res), g);
            }
            else if (type >= 6 && type <= 11)                               // HP 6 .. HP 48
            {
                buildCascade (d, kCascades[type - 6], true, morph, resonanceBoost (res), g);
            }
            else if (type == 12)                                            // BP 6
            {
                // HP corner at fc/s, LP corner at fc*s, s = 8^morph (1x..8x).  Normalised to 0 dB at fc: at s = 1 the
                // two corners coincide and the raw pair peaks at -6 dB, which would make the morph knob also a gain knob.
                const double s = std::pow (8.0, morph);
                setOnePole (next (d), gOf (clampCutoff (fc / s, fs), fs), 1.0, -1.0);
                setOnePole (next (d), gOf (clampCutoff (fc * s, fs), fs), 0.0, 1.0);

                const double mag = std::abs (response (d, fc, fs));
                d.gain = mag > 1.0e-9 ? 1.0 / mag : 1.0;
            }
            else if (type == 13)                                            // BP 12
            {
                // v1 has peak gain Q.  y = k^(1-morph) v1: morph 0 = constant-peak (0 dB at fc, the topology column),
                // morph 1 = constant-skirt (peak gain Q).  Ambiguity A-D5-1: the spec lists "constant-skirt <->
                // constant-peak" without a direction; the default (morph 0) is the normalised topology.
                const double k = 1.0 / bandQ (res);
                setSvf (next (d), g, k, 0.0, std::pow (k, 1.0 - morph), 0.0);
            }
            else if (type == 14)                                            // BP 24
            {
                const double k = 1.0 / bandQ (res);
                const double spread = std::exp2 (0.5 * morph);
                setSvf (next (d), gOf (clampCutoff (fc / spread, fs), fs), k, 0.0, k, 0.0);
                setSvf (next (d), gOf (clampCutoff (fc * spread, fs), fs), k, 0.0, k, 0.0);
            }
            else if (type == 15)                                            // Notch: x - k bp  (exactly zero at fc)
            {
                const double k = 1.0 / bandQ (res);
                setSvf (next (d), g, k, 1.0, -k, 0.0);
            }
            else if (type == 16)                                            // Var Notch: x - depth * (k bp)
            {
                const double k = 1.0 / bandQ (res);
                setSvf (next (d), g, k, 1.0, -morph * k, 0.0);
            }
            else if (type == 17)                                            // Peak (bell)
            {
                // Cytomic bell: k = 1/(Q A), y = x + k (A^2 - 1) v1  ->  gain A^2 = 10^(dB/20) at fc.
                const double a = std::pow (10.0, shelfPeakGainDb (res) / 40.0);
                const double q = 4.0 * std::pow (0.3 / 4.0, morph);
                const double k = 1.0 / (q * a);
                setSvf (next (d), g, k, 1.0, k * (a * a - 1.0), 0.0);
            }
            else if (type == 18)                                            // Low shelf
            {
                // The corner is the shelf's mid-gain point: the integrators are scaled by 1/sqrt(A) (Cytomic).
                const double a = std::pow (10.0, shelfPeakGainDb (res) / 40.0);
                const double k = 1.0 / (0.5 * std::pow (4.0, morph));
                setSvf (next (d), g / std::sqrt (a), k, 1.0, k * (a - 1.0), a * a - 1.0);
            }
            else if (type == 19)                                            // High shelf
            {
                const double a = std::pow (10.0, shelfPeakGainDb (res) / 40.0);
                const double k = 1.0 / (0.5 * std::pow (4.0, morph));
                setSvf (next (d), g * std::sqrt (a), k, a * a, k * (1.0 - a) * a, 1.0 - a * a);
            }
            // else: not implemented in this phase -> identity design
        }

        std::complex<double> response (const FilterDesign& d, double hz, double fs) noexcept
        {
            const double w = std::tan (kPi * std::min (hz, 0.49999 * fs) / fs);
            std::complex<double> h (d.gain, 0.0);

            for (int i = 0; i < d.numSections; ++i)
            {
                const FilterSection& s = d.sec[static_cast<std::size_t> (i)];
                const std::complex<double> sv (0.0, w / s.g);

                if (s.kind == FilterSection::Kind::Svf)
                {
                    const std::complex<double> den = sv * sv + s.k * sv + 1.0;
                    h *= s.m0 + s.m1 * (sv / den) + s.m2 * (1.0 / den);
                }
                else if (s.kind == FilterSection::Kind::OnePole)
                {
                    h *= s.m0 + s.m2 * (1.0 / (1.0 + sv));
                }
            }

            return h;
        }
    }

    //==========================================================================
    double FilterResponse::magnitudeDbAt (int type, double cutoffHz, double res, double driveDb, double morph,
                                          double fs, double hz) noexcept
    {
        if (type < 0 || type >= kNumFilterTypes || ! kFilterTypes[static_cast<std::size_t> (type)].implemented
            || filterdesign::isOpenLowpass (type, cutoffHz, res, driveDb))
            return 0.0;

        FilterDesign d;
        filterdesign::make (type, cutoffHz, res, morph, fs, d);
        const double mag = std::abs (filterdesign::response (d, hz, fs));
        const double db = 20.0 * std::log10 (std::max (mag, 1.0e-12));
        return std::min (kCeilDb, std::max (kFloorDb, db));
    }

    void FilterResponse::magnitudeDb (int type, double cutoffHz, double res, double driveDb, double morph, double fs,
                                      const double* freqsHz, int n, float* dbOut) noexcept
    {
        if (dbOut == nullptr || freqsHz == nullptr)
            return;

        const bool flat = type < 0 || type >= kNumFilterTypes || ! kFilterTypes[static_cast<std::size_t> (type)].implemented
                       || filterdesign::isOpenLowpass (type, cutoffHz, res, driveDb);

        if (flat)
        {
            for (int i = 0; i < n; ++i)
                dbOut[i] = 0.0f;
            return;
        }

        FilterDesign d;
        filterdesign::make (type, cutoffHz, res, morph, fs, d);

        for (int i = 0; i < n; ++i)
        {
            const double mag = std::abs (filterdesign::response (d, freqsHz[i], fs));
            const double db = 20.0 * std::log10 (std::max (mag, 1.0e-12));
            dbOut[i] = static_cast<float> (std::min (kCeilDb, std::max (kFloorDb, db)));
        }
    }

    void FilterResponse::magnitudeDbLogSpaced (int type, double cutoffHz, double res, double driveDb, double morph,
                                               double fs, float* dbOut, int n, double loHz, double hiHz) noexcept
    {
        if (dbOut == nullptr || n <= 0)
            return;

        const bool flat = type < 0 || type >= kNumFilterTypes || ! kFilterTypes[static_cast<std::size_t> (type)].implemented
                       || filterdesign::isOpenLowpass (type, cutoffHz, res, driveDb);

        if (flat)
        {
            for (int i = 0; i < n; ++i)
                dbOut[i] = 0.0f;
            return;
        }

        FilterDesign d;
        filterdesign::make (type, cutoffHz, res, morph, fs, d);
        const double ratio = n > 1 ? std::pow (hiHz / loHz, 1.0 / static_cast<double> (n - 1)) : 1.0;
        double f = loHz;

        for (int i = 0; i < n; ++i)
        {
            const double mag = std::abs (filterdesign::response (d, f, fs));
            const double db = 20.0 * std::log10 (std::max (mag, 1.0e-12));
            dbOut[i] = static_cast<float> (std::min (kCeilDb, std::max (kFloorDb, db)));
            f *= ratio;
        }
    }
}
