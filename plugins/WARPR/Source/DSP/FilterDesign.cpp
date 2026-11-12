#include "DSP/FilterDesign.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <cmath>
#include <complex>

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

        //======================================================================
        // D5b helpers
        //======================================================================

        /** Smooth 0..1 step of width 0.3 centred on x = 0: exactly 0 / 1 for |x| >= 0.15, so a stage-count threshold that
            sits 0.5 away from every integer morph position is fully on / off there, and fades while the (already
            10 ms smoothed) morph passes through it (no click when the stage count changes). */
        double stageWeight (double x) noexcept
        {
            const double t = clamp01 (x / 0.3 + 0.5);
            return t * t * (3.0 - 2.0 * t);
        }

        /** Phase delay (samples) of the loop one-pole  a / (1 - (1 - a) z^-1)  at angular frequency w. */
        double lpPhaseDelay (double a, double w) noexcept
        {
            if (a >= 1.0)
                return 0.0;

            const double b = 1.0 - a;
            return std::atan2 (b * std::sin (w), 1.0 - b * std::cos (w)) / w;
        }

        /** Phase delay (samples) of the Lagrange3 interpolator for a total delay `delay` at angular frequency w.  The phase
            is taken relative to the nominal delay so the large wrapped angle of long delays never matters. */
        double lagrangePhaseDelay (double delay, double w) noexcept
        {
            int base = 0;
            double c[4] = {};
            filterdesign::lagrange3 (delay, base, c);

            std::complex<double> h (0.0, 0.0);
            for (int k = 0; k < 4; ++k)
                h += c[k] * std::polar (1.0, -w * static_cast<double> (base - 1 + k));

            const double r = std::arg (h * std::polar (1.0, w * delay));
            return delay - r / w;
        }

        /** Comb.  Positive: teeth at k fc (loop phase delay fs/fc).  Negative: teeth at the ODD multiples of fc (loop phase
            delay fs/(2 fc)), so "fc = fundamental" holds for both ("odd harmonics, hollow").  The loop delay is solved so
            that the TOTAL loop phase delay (Lagrange3 + damping one-pole) hits the target exactly at fc: the comb is in
            tune whatever the damping. */
        void buildComb (FilterDesign& d, bool negative, double cutoffHz, double res, double morph, double fs) noexcept
        {
            const double fc = std::min (std::max (cutoffHz, filterdesign::kCombMinHz), filterdesign::kMaxCutoffFraction * fs);
            const double g = filterdesign::kCombMaxFeedback * res;
            const double aDark = 1.0 - std::exp (-2.0 * kPi * 500.0 / fs);           // morph 1: ~500 Hz loop corner
            const double a = std::pow (aDark, morph);                                 // morph 0: exactly 1 (transparent loop)
            const double target = negative ? 0.5 * fs / fc : fs / fc;
            const double w0 = 2.0 * kPi * fc / fs;

            double delay = target;

            if (g > 1.0e-9)
            {
                const double pdLp = lpPhaseDelay (a, w0);
                delay = std::max (2.0, target - pdLp);

                for (int it = 0; it < 2; ++it)
                    delay = std::max (2.0, delay + (target - (lagrangePhaseDelay (delay, w0) + pdLp)));
            }

            d.topo = FilterDesign::Topo::Comb;
            d.comb.delay = std::min (std::max (delay, 2.0), fs / filterdesign::kCombMinHz + 2.0);
            d.comb.sign = negative ? -1.0 : 1.0;
            d.comb.g = g;
            d.comb.a = a;
            d.comb.c = std::sqrt (1.0 - g * g);
        }

        //---- formants -------------------------------------------------------------------------------------------
        /** Five-formant vowel table A E I O U: Csound's public "tenor" set (centre Hz, amplitude dB, bandwidth Hz). */
        struct VowelFormant { double f, ampDb, bw; };

        constexpr VowelFormant kVowels[5][5] = {
            { { 650.0, 0.0, 80.0 },  { 1080.0, -6.0, 90.0 },   { 2650.0, -7.0, 120.0 },  { 2900.0, -8.0, 130.0 },  { 3250.0, -22.0, 140.0 } },    // A
            { { 400.0, 0.0, 70.0 },  { 1700.0, -14.0, 80.0 },  { 2600.0, -12.0, 100.0 }, { 3200.0, -14.0, 120.0 }, { 3580.0, -20.0, 120.0 } },    // E
            { { 290.0, 0.0, 40.0 },  { 1870.0, -15.0, 90.0 },  { 2800.0, -18.0, 100.0 }, { 3250.0, -20.0, 120.0 }, { 3540.0, -30.0, 120.0 } },    // I
            { { 400.0, 0.0, 40.0 },  { 800.0, -10.0, 80.0 },   { 2600.0, -12.0, 100.0 }, { 2800.0, -12.0, 120.0 }, { 3000.0, -26.0, 120.0 } },    // O
            { { 350.0, 0.0, 40.0 },  { 600.0, -20.0, 80.0 },   { 2700.0, -17.0, 100.0 }, { 2900.0, -14.0, 120.0 }, { 3300.0, -26.0, 120.0 } }     // U
        };

        /** Parallel band-pass of centre f, quality q and linear peak gain `amp` (0 dB peak x amp at f exactly). */
        void addBand (FilterDesign& d, double f, double q, double amp, double fs) noexcept
        {
            const double fc = std::min (std::max (f, 20.0), filterdesign::kMaxCutoffFraction * fs);
            setSvf (next (d), gOf (fc, fs), 1.0 / q, 0.0, amp / q, 0.0);
        }

        /** Narrowness factor: res 0 = 4x wider than the table, 0.5 = the table, 1 = 4x narrower. */
        double narrowness (double res) noexcept { return std::pow (4.0, 2.0 * res - 1.0); }

        /** Global formant transposition in octaves from the cutoff: 1 kHz = 0, 20 kHz = +1, 20 Hz = -1 (log-linear each side). */
        double vowelTranspose (double cutoffHz) noexcept
        {
            const double r = cutoffHz / 1000.0;
            return r >= 1.0 ? std::min (1.0, std::log2 (r) / std::log2 (20.0))
                            : std::max (-1.0, std::log2 (r) / std::log2 (50.0));
        }

        /** The five vowel formants at vowel position x (0 = A .. 4 = U, fractional = interpolated: log frequency, Q, dB). */
        void buildVowelBank (FilterDesign& d, double x, double cutoffHz, double res, double fs) noexcept
        {
            x = std::min (4.0, std::max (0.0, x));
            const int i0 = std::min(static_cast<int> (x), 3);
            const double t = x - static_cast<double> (i0);
            const double shift = std::exp2 (vowelTranspose (cutoffHz));
            const double narrow = narrowness (res);

            d.topo = FilterDesign::Topo::Parallel;

            for (int j = 0; j < 5; ++j)
            {
                const VowelFormant& a = kVowels[i0][j];
                const VowelFormant& b = kVowels[i0 + 1][j];
                const double f = std::exp (lerp (std::log (a.f), std::log (b.f), t)) * shift;
                const double q = lerp (a.f / a.bw, b.f / b.bw, t) * narrow;
                const double amp = std::pow (10.0, lerp (a.ampDb, b.ampDb, t) / 20.0);
                addBand (d, f, q, amp, fs);
            }
        }

        //======================================================================
        // D5c helpers: nonlinear family (types 28-33)
        //======================================================================

        /** Global feedback of the ladders: k = 4.2 res.  The TPT ladder's linear stability limit is exactly k = 4 (four equal
            poles, -180 degrees and 1/4 magnitude at fc), so the last 5% of the knob (res > 0.952) self-oscillates; the
            saturators bound the oscillation. */
        constexpr double kLadderFeedback = 4.2;

        /** Largest feedback the DISPLAY uses: just below the limit, so a self-oscillating setting draws a very tall (clamped)
            peak instead of the unstable pole's meaningless magnitude. */
        constexpr double kLadderDisplayFeedback = 3.98;

        void buildNonlinear (FilterDesign& d, int type, double fc, double res, double morph, double driveDb,
                             double fs, bool localOs) noexcept
        {
            NonlinearSpec& nl = d.nl;
            nl = NonlinearSpec {};
            d.topo = FilterDesign::Topo::Nonlinear;

            const double drive = std::pow (10.0, std::min (24.0, std::max (0.0, driveDb)) / 20.0);
            nl.os2x = localOs && type != 32;
            nl.rateFactor = nl.os2x ? 2.0 : 1.0;
            const double fsCore = fs * nl.rateFactor;

            if (type >= 28 && type <= 30)                                   // ladders
            {
                nl.kind = NonlinearSpec::Kind::Ladder;
                nl.highpass = type == 29;
                nl.g = gOf (fc, fsCore);
                nl.bigG = nl.g / (1.0 + nl.g);
                nl.k = kLadderFeedback * res;
                nl.comp = nl.highpass ? 1.0 : 1.0 + 0.5 * nl.k;               // keeps the bass from collapsing at high resonance
                nl.tapBlend = type == 30 ? 1.0 : morph;                       // 28 / 29: 2-pole (0) <-> 4-pole (1); 30 is always 4-pole
                nl.loopScale = type == 30 ? drive * (1.0 + 15.0 * morph) : drive;
            }
            else if (type == 31)                                            // Scream
            {
                nl.kind = NonlinearSpec::Kind::Scream;
                const double q = 0.7 * std::pow (40.0, res);                 // 0.7 .. 28
                const double k = 1.0 / q;
                setSvf (next (d), gOf (fc, fsCore), k, 0.0, k, 0.0);        // peak-normalised band-pass
                nl.inGain = std::pow (10.0, 2.0 * morph) * drive;           // 0 .. 40 dB, times the drive
                nl.fbAmount = 0.95 * morph;
            }
            else if (type == 32)                                            // S&H (base rate, aliasing by design)
            {
                nl.kind = NonlinearSpec::Kind::SampleHold;
                const double open = 0.499 * fs;                              // pre-LP effectively open at res 0
                const double hold = std::max (fc, 20.0);
                const double corner = open * std::pow (std::min (1.0, 0.4 * hold / open), res);
                const double g = gOf (corner, fs);
                setOnePole (next (d), g, 0.0, 1.0);
                setOnePole (next (d), g, 0.0, 1.0);
                nl.holdStep = hold / fs;
                nl.interp = morph;
            }
            else                                                            // 33: Ring Mod
            {
                nl.kind = NonlinearSpec::Kind::RingMod;
                nl.carrierStep = fc / fsCore;
                nl.carrierMorph = morph;
                const double open = 0.499 * fsCore;
                const double floorHz = std::min (open, std::max (40.0, 0.5 * fc));
                const double corner = open * std::pow (floorHz / open, res);
                const double g = gOf (corner, fsCore);
                nl.smoothG = g / (1.0 + g);
            }
        }

        std::complex<double> sectionH (const FilterSection& s, double w) noexcept
        {
            const std::complex<double> sv (0.0, w / s.g);

            if (s.kind == FilterSection::Kind::Svf)
            {
                const std::complex<double> den = sv * sv + s.k * sv + 1.0;
                return s.m0 + s.m1 * (sv / den) + s.m2 * (1.0 / den);
            }

            if (s.kind == FilterSection::Kind::OnePole)
                return s.m0 + s.m2 * (1.0 / (1.0 + sv));

            return { 1.0, 0.0 };
        }

        /** Small-signal (every saturator at unit slope) transfer function of the nonlinear family, see FilterResponse. */
        std::complex<double> nonlinearResponse (const FilterDesign& d, double hz, double fs) noexcept
        {
            const NonlinearSpec& nl = d.nl;
            const double f = std::min (hz, 0.49999 * fs);
            const double w = std::tan (kPi * f / (fs * nl.rateFactor));

            switch (nl.kind)
            {
                case NonlinearSpec::Kind::Ladder:
                {
                    const std::complex<double> h1 = 1.0 / (1.0 + std::complex<double> (0.0, w / nl.g));
                    const std::complex<double> h2 = h1 * h1, h4 = h2 * h2;
                    const double k = std::min (nl.k, kLadderDisplayFeedback);
                    const std::complex<double> den = 1.0 + k * h4;

                    std::complex<double> num;
                    if (nl.highpass)
                    {
                        const std::complex<double> q = 1.0 - h1, q2 = q * q;
                        num = (1.0 - nl.tapBlend) * q2 + nl.tapBlend * q2 * q2;
                    }
                    else
                        num = (1.0 - nl.tapBlend) * h2 + nl.tapBlend * h4;

                    return nl.comp * num / den;
                }

                case NonlinearSpec::Kind::Scream:
                    return sectionH (d.sec[0], w);

                case NonlinearSpec::Kind::SampleHold:
                    return sectionH (d.sec[0], w) * sectionH (d.sec[1], w);

                case NonlinearSpec::Kind::RingMod:
                case NonlinearSpec::Kind::None:
                    break;
            }

            return { 1.0, 0.0 };
        }

        /** Vowel position of the STEPPED Vowel type: the morph knob picks the nearest vowel (plateau), and only a short
            window (0.4 of a vowel step) around every midpoint is interpolated, so the 10 ms morph smoother turns a vowel
            change into an ~8 ms glide of the formants instead of a click. */
        double steppedVowelPosition (double morph) noexcept
        {
            constexpr double w = 0.4;
            const double v = 4.0 * morph;
            const double n = std::floor (v);
            const double frac = v - n;
            const double lo = 0.5 - 0.5 * w, hi = 0.5 + 0.5 * w;

            if (frac <= lo)
                return n;
            if (frac >= hi)
                return std::min (4.0, n + 1.0);

            const double t = (frac - lo) / w;
            return n + t * t * (3.0 - 2.0 * t);
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

        void make (int type, double cutoffHz, double res, double morph, double fs, FilterDesign& d,
                   double driveDb, bool localOversampling) noexcept
        {
            // Header reset only (a FilterDesign carries 16 sections; unused ones are never read).
            d.topo = FilterDesign::Topo::Series;
            d.numSections = 0;
            d.gain = 1.0;
            d.fb = 0.0;
            d.outDry = 0.0;
            d.outWet = 1.0;
            d.comb = CombSpec {};
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
            else if (type == 20 || type == 21)                              // Comb + / Comb -
            {
                buildComb (d, type == 21, cutoffHz, res, morph, fs);
            }
            else if (type == 22)                                            // All Pass: 2nd order <-> 1st order
            {
                // Parallel blend (1-m) AP2 + m AP1, both +1 at DC.  AP2 = x - 2 k bp (phase -pi at fc), AP1 = 2 lp - x
                // (phase -pi/2 at fc): |H(fc)| = sqrt((1-m)^2 + m^2), exactly 1 at either end of the morph.
                const double k = 1.0 / bandQ (res);
                d.topo = FilterDesign::Topo::Parallel;
                setSvf (next (d), g, k, 1.0 - morph, -2.0 * k * (1.0 - morph), 0.0);
                setOnePole (next (d), g, -morph, 2.0 * morph);
            }
            else if (type == 23)                                            // Serial AP: N = 2 + round(14 morph), stage i at fc 2^(i s)
            {
                // Spec: fc * 2^(i * spread), res = spread.  Ambiguity A-D5b-2: spread (octaves per stage) = 0.5 res.
                const double spreadOct = 0.5 * res;
                const double u = 14.0 * morph;

                for (int i = 0; i < FilterDesign::maxSections; ++i)
                {
                    const double w = i < 2 ? 1.0 : stageWeight (u - (static_cast<double> (i) - 1.5));

                    if (w <= 0.0)
                        continue;

                    const double fi = clampCutoff (fc * std::exp2 (static_cast<double> (i) * spreadOct), fs);
                    setOnePole (next (d), gOf (fi, fs), 1.0 - 2.0 * w, 2.0 * w);        // x + w (AP1(x) - x)
                }
            }
            else if (type == 24)                                            // Formant: F1 = fc, F2 = F1 (1.5 + 2 morph), fixed F3
            {
                const double n = narrowness (res);
                d.topo = FilterDesign::Topo::Parallel;
                addBand (d, fc, 8.0 * n, 1.0, fs);
                addBand (d, fc * (1.5 + 2.0 * morph), 10.0 * n, 0.5011872336, fs);         // -6 dB
                addBand (d, 2800.0, 12.0 * n, 0.2511886432, fs);                            // -12 dB, fixed
            }
            else if (type == 25)                                            // Vowel (stepped)
            {
                buildVowelBank (d, steppedVowelPosition (morph), fc, res, fs);
            }
            else if (type == 26)                                            // Morph Formant (interpolated A-E-I-O-U)
            {
                buildVowelBank (d, 4.0 * morph, fc, res, fs);
            }
            else if (type == 27)                                            // Phaser: 2..12 all-pass stages, bipolar feedback
            {
                // Stage i at fc 2^((i - 5.5) / 2): half-octave spacing, geometrically centred on fc; the stage PAIRS (5,6),
                // (4,7), ... switch on outward as the morph rises (pair p is on from 5 morph = p - 0.5), so adding stages never
                // moves the existing ones.  Two stages per notch -> one octave between notches.
                const double u = 5.0 * morph;

                for (int i = 0; i < 12; ++i)
                {
                    const int pair = static_cast<int> (std::abs (static_cast<double> (i) - 5.5));
                    const double w = pair == 0 ? 1.0 : stageWeight (u - (static_cast<double> (pair) - 0.5));

                    if (w <= 0.0)
                        continue;

                    const double fi = clampCutoff (fc * std::exp2 ((static_cast<double> (i) - 5.5) * 0.5), fs);
                    setOnePole (next (d), gOf (fi, fs), 1.0 - 2.0 * w, 2.0 * w);
                }

                // res 0.5 = no feedback; |fb| <= 0.95 keeps the loop (unit-magnitude chain x |fb|) contractive.  The dry sum
                // makes the notches; the output is trimmed by 1/sqrt(peak) with peak = (1 + 1/(1-|fb|))/2 so strong
                // feedback does not shout (resonant peaks still reach +10 dB at the extremes).
                d.fb = 1.9 * (res - 0.5);
                d.outDry = 0.5;
                d.outWet = 0.5;
                d.gain = 1.0 / std::sqrt (0.5 * (1.0 + 1.0 / (1.0 - std::abs (d.fb))));
            }
            else if (type >= 28 && type <= 33)                              // nonlinear family (D5c)
            {
                buildNonlinear (d, type, fc, res, morph, driveDb, fs, localOversampling);
            }
            // else: out-of-range index -> identity design
        }

        std::complex<double> response (const FilterDesign& d, double hz, double fs) noexcept
        {
            const double f = std::min (hz, 0.49999 * fs);
            const double w = std::tan (kPi * f / fs);
            const double om = 2.0 * kPi * f / fs;                     // angular frequency in rad/sample

            if (d.topo == FilterDesign::Topo::Nonlinear)
                return nonlinearResponse (d, hz, fs);

            // ---- comb: H = c / (1 - sign g Hd Hlp)
            if (d.topo == FilterDesign::Topo::Comb)
            {
                const CombSpec& cs = d.comb;
                int base = 0;
                double c[4] = {};
                lagrange3 (cs.delay, base, c);

                std::complex<double> hd (0.0, 0.0);
                for (int k = 0; k < 4; ++k)
                    hd += c[k] * std::polar (1.0, -om * static_cast<double> (base - 1 + k));

                const std::complex<double> zInv = std::polar (1.0, -om);
                const std::complex<double> hlp = cs.a / (1.0 - (1.0 - cs.a) * zInv);
                return cs.c / (1.0 - cs.sign * cs.g * hd * hlp);
            }

            auto sectionResponse = [w] (const FilterSection& s) -> std::complex<double>
            {
                const std::complex<double> sv (0.0, w / s.g);

                if (s.kind == FilterSection::Kind::Svf)
                {
                    const std::complex<double> den = sv * sv + s.k * sv + 1.0;
                    return s.m0 + s.m1 * (sv / den) + s.m2 * (1.0 / den);
                }

                if (s.kind == FilterSection::Kind::OnePole)
                    return s.m0 + s.m2 * (1.0 / (1.0 + sv));

                return { 1.0, 0.0 };
            };

            // ---- parallel bank: the branches add
            if (d.topo == FilterDesign::Topo::Parallel)
            {
                std::complex<double> sum (0.0, 0.0);

                for (int i = 0; i < d.numSections; ++i)
                    sum += sectionResponse (d.sec[static_cast<std::size_t> (i)]);

                return sum * d.gain;
            }

            // ---- series chain (D5a types, serial all-pass, phaser)
            const bool plain = juce::exactlyEqual (d.fb, 0.0) && juce::exactlyEqual (d.outDry, 0.0) && juce::exactlyEqual (d.outWet, 1.0);
            std::complex<double> h (plain ? d.gain : 1.0, 0.0);

            for (int i = 0; i < d.numSections; ++i)
                h *= sectionResponse (d.sec[static_cast<std::size_t> (i)]);

            if (plain)
                return h;

            // chain A with the one-sample-delayed feedback: Hc = A / (1 - fb z^-1 A);  y = gain (dry x + wet Hc)
            const std::complex<double> zInv = std::polar (1.0, -om);
            const std::complex<double> wet = h / (1.0 - d.fb * zInv * h);
            return d.gain * (d.outDry + d.outWet * wet);
        }

        int combBufferLength (double fs) noexcept
        {
            return static_cast<int> (std::ceil (fs / kCombMinHz)) + 12;
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
