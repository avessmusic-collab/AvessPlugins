/*
    WARPR - phase D6 test block: phase & time (44-49), Phase Remapping (50), sync (51-53), the FM family (54-59),
    the modulator (ModOsc.h), the speed-aware mip selection and the per-grain versions of all of them.

    Called from RunTests.cpp main():   failures += runPhaseD6Tests (full);

    Contents (plan.md "Phase D6" test criteria + T1/T2)
      1. mode table         rows 44-59: implemented, names, families, kinds, flags
      2. modulator          shapes (range, BLEP / BLAMP alias gain), integrals (dM/dphi = kappa m, zero mean, unit peak),
                            SampleMod bounds, soft limiter
      3. phase maps         neutral at a = 0, anchor values and continuity of g(theta), monotony, the second tap / window of
                            the extras, evalMap == delta for the old rows, Phase Remapping against the curve
      4. Hard Sync          analytic output, BLEP on vs off: alias power measured and printed
      5. A3 alias gate      FM / PM / PD / Hard Sync: 8x at least 20 dB below 1x (printed table incl. 2x / 4x)
      6. T1 cube            all 16 modes x signals x OS x (a, A, B) x modulator source: finite, bounded, a = 0 bit-exact,
                            discontinuity within the per-mode bound (flagged modes use their own)
      7. Self FM / Cross Mod  full A / B / a cube at 1x / 8x bounded + finite, feedback guard exercised (injected NaN)
      8. mip correctness    mipIndexForMult / ReadParamsCache, a 15 kHz modulator read at 4x does not fold (class level and
                            through the oscillator), measured read speed of FM
      9. Phase Remapping    identity curve = bit-exact identity (oscillator and processor), a real curve changes the sound
     10. stereo / mono, determinism, zero allocations, mode switching, a-sweeps
     11. per-grain         analytic grain output for the map modes and FM / PM, identities, all modes x schedulers x OS
     12. processor         all 16 modes through the voice path (DC, bounds), minOS enforcement, parameter plumbing,
                           zero allocations (Classic and granular), determinism

    Hand-derived / hand-guessed gates carry a comment; measured numbers are printed next to the assertion.
*/

#include "PluginProcessor.h"
#include "DSP/GrainEngine.h"
#include "DSP/HalfbandIIR.h"
#include "DSP/ModOsc.h"
#include "DSP/PhaseKernel.h"
#include "DSP/PhaseWarp.h"
#include "DSP/RegionFold.h"
#include "DSP/SampleReader.h"
#include "DSP/ShaperEngine.h"
#include "DSP/ShaperPresetLibrary.h"
#include "DSP/UnisonOsc.h"
#include "DSP/WarpModeTable.h"
#include "DSP/WarpStage.h"
#include "Parameters/ParamTable.h"
#include "Tests/AllocHook.h"
#include "Tests/OfflineRender.h"
#include "Tests/Scanners.h"
#include "Tests/WarpTestKit.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// Bit-exact float equality is the point of several assertions in this file (a = 0 identity, determinism, L == R).
JUCE_BEGIN_IGNORE_WARNINGS_GCC_LIKE ("-Wfloat-equal")

int runPhaseD6Tests (bool full);

namespace
{
    using namespace warpr::tests::kit;
    namespace pw = warpr::phasewarp;
    namespace mo = warpr::modosc;

    int failures = 0;
    int printedDetails = 0;

    constexpr double kFs = 48000.0;

    void check (bool cond, const char* name)
    {
        std::printf ("  %s  %s\n", cond ? "PASS" : "FAIL", name);
        if (! cond) ++failures;
    }

    /** Prints a failure detail line (bounded, so a systematic failure does not flood the log). */
   #define DETAIL(...) do { if (printedDetails++ < 60) std::printf (__VA_ARGS__); } while (false)

    void setParam (PluginProcessor& p, const char* id, float value)
    {
        if (auto* prm = p.apvts.getParameter (id))
            prm->setValueNotifyingHost (prm->convertTo0to1 (value));
        else
        {
            std::printf ("  FAIL  unknown parameter '%s'\n", id);
            ++failures;
        }
    }

    const char* modeName (int mode) { return warpr::kWarpModes[ix (mode)].name; }

    //==========================================================================
    // configuration + class-level renderer helpers
    //==========================================================================
    struct D6Cfg
    {
        int   mode = 17;
        float a = 0.0f, A = 0.5f, B = 0.0f, mix = 1.0f;
        int   src = 4;
        float ratio = 1.0f;
        const warpr::ShaperLutSet* curve = nullptr;
        bool  blep = true;
    };

    D6Cfg cfgOf (int mode, float a, float A, float B, int src = 4, float ratio = 1.0f, float mix = 1.0f)
    {
        D6Cfg c;
        c.mode = mode;
        c.a = a;
        c.A = A;
        c.B = B;
        c.src = src;
        c.ratio = ratio;
        c.mix = mix;
        return c;
    }

    inline auto d6Span (const D6Cfg& c)
    {
        return [c] (int, warpr::WarpSpan& sp)
        {
            sp.active = true;
            sp.mode = c.mode;
            sp.a0 = sp.a1 = c.a;
            sp.A0 = sp.A1 = c.A;
            sp.B0 = sp.B1 = c.B;
            sp.mix0 = sp.mix1 = c.mix;
            sp.modSource = c.src;
            sp.ratio0 = sp.ratio1 = c.ratio;
            sp.curve = c.curve;
            sp.blep = c.blep;
            return true;
        };
    }

    /** In-band alias level (dB) of an oscillator-rate render: energy below the BASE Nyquist that is not at a harmonic of f0. */
    double aliasOf (const std::vector<float>& x, double fs, int os, double f0)
    {
        const int order = 15 + (os == 1 ? 0 : (os == 2 ? 1 : (os == 4 ? 2 : 3)));
        const size_t n = (size_t) 1 << order;

        // The DC offset is removed from the analysed window first: a time-warped wave has a non-zero mean (PD, FM, sync), and its
        // Hann leakage into bins 1-3 is neither harmonic nor foldback - it is what produced the OS-independent "alias floor"
        // (-11 dB for PD, -17 dB for FM) in the first run.
        std::vector<float> tail (x.end() - (std::ptrdiff_t) n, x.end());
        double mean = 0.0;
        for (float v : tail)
            mean += (double) v;
        mean /= (double) n;
        for (auto& v : tail)
            v -= (float) mean;

        return warpr::tests::aliasDb (tail.data(), (int) n, fs * (double) os, f0, order, 8, 0.5 * fs);
    }

    constexpr int kBin = 2053;                                          // f0 = fs * 2053 / 32768 = 3007.0 Hz (aliases miss the harmonics)
    const double kF0 = kFs * (double) kBin / 32768.0;
    constexpr int kAliasBase = 34816;                                   // base samples per alias render (>= 2^15 + settle, multiple of 32)

    /** Largest |g'(theta)| of a map mode (numerical). */
    double maxSlopeMap (int mode, double a, double A, double B, double dTheta)
    {
        double m = 1.0;
        constexpr double h = 1.0e-4;

        for (double t = 0.0; t + h <= 1.0; t += h)
        {
            const double g0 = t + pw::delta (mode, a, A, B, t, dTheta);
            const double g1 = (t + h) + pw::delta (mode, a, A, B, t + h, dTheta);
            m = std::max (m, std::abs ((g1 - g0) / h));
        }

        return m;
    }

    D6Cfg hardSyncNaive()
    {
        D6Cfg c = cfgOf (52, 0.4f, 0.5f, 0.0f);
        c.blep = false;
        return c;
    }

    //==========================================================================
    // 1. mode table
    //==========================================================================
    void testModeTable()
    {
        std::printf ("\n[D6] WarpModeTable rows 44-59: implemented, names, families, kinds, flags\n");

        int bad = 0, nameBad = 0, famBad = 0, kindBad = 0, osBad = 0;

        for (int i = 44; i < 60; ++i)
        {
            const auto& m = warpr::kWarpModes[ix (i)];

            if (! m.implemented) ++bad;
            if (std::string (m.name) != std::string (warpr::choices::warpMode[i])) ++nameBad;

            const warpr::WarpFamily fam = i <= 50 ? warpr::WarpFamily::Phm : (i <= 53 ? warpr::WarpFamily::Syn : warpr::WarpFamily::Mod);
            if (m.family != fam) ++famBad;

            const warpr::WarpKind kind = (i == 56 || i == 57) ? warpr::WarpKind::Amp : warpr::WarpKind::Phase;
            if (m.kind != kind) ++kindBad;

            if (m.minOS != 1 && m.minOS != 2 && m.minOS != 4 && m.minOS != 8) ++osBad;
        }

        check (bad == 0 && warpr::kNumImplementedWarpModes == 60, "rows 44-59 are implemented; kNumImplementedWarpModes == 60");
        check (nameBad == 0, "names equal choices::warpMode[44..59]");
        check (famBad == 0, "families: PHM 44-50, SYN 51-53, MOD 54-59");
        check (kindBad == 0, "kinds: AM / RM (56, 57) are amplitude-domain, every other D6 row moves the read position");
        check (osBad == 0, "minOS hints are 1, 2, 4 or 8");
        check (warpr::warpModeInfo (49).stepFlag && warpr::warpModeInfo (51).stepFlag && warpr::warpModeInfo (52).stepFlag
                   && warpr::warpModeInfo (53).stepFlag && ! warpr::warpModeInfo (54).stepFlag && ! warpr::warpModeInfo (55).stepFlag,
               "intended-discontinuity flags: Phase Quantization (hard staircase) and the three sync rows; FM / PM are continuous");
    }

    //==========================================================================
    // 2. modulator
    //==========================================================================
    void testModulator()
    {
        std::printf ("\n[D6] ModOsc: shapes, integrals, BLEP / BLAMP, Sample follower, soft limiter\n");

        // ---- range and anchor values (no band-limiting: dphi 0)
        {
            bool inRange = true;
            double worstSine = 0.0;

            for (int src = 0; src < 4; ++src)
                for (int i = 0; i < 4096; ++i)
                {
                    const double phi = (double) i / 4096.0;
                    const double m = mo::shape (src, phi, 0.0);
                    inRange = inRange && std::abs (m) <= 1.0 + 1.0e-8;   // the polynomial sine peaks at 1 + ~7e-10
                                                                           // (same order as its accuracy gate below)

                    if (src == 0)
                        worstSine = std::max (worstSine, std::abs (m - std::sin (2.0 * kPi * phi)));
                }

            std::printf ("        (shape range ok: %s, worst |Sine - std::sin| = %.3e)\n", inRange ? "yes" : "NO", worstSine);
            check (inRange && worstSine < 1.0e-8, "shapes stay within [-1, 1] without band-limiting; Sine == sin (2 pi phi)");

            const bool anchors = std::abs (mo::shape (1, 0.25, 0.0) - 1.0) < 1.0e-12 && std::abs (mo::shape (1, 0.75, 0.0) + 1.0) < 1.0e-12
                                 && std::abs (mo::shape (1, 0.0, 0.0)) < 1.0e-12 && std::abs (mo::shape (2, 0.0, 0.0)) < 1.0e-12
                                 && std::abs (mo::shape (2, 0.25, 0.0) - 0.5) < 1.0e-12 && mo::shape (3, 0.1, 0.0) > 0.99 && mo::shape (3, 0.6, 0.0) < -0.99;
            check (anchors, "sine-aligned anchors: Triangle +1 at 1/4, -1 at 3/4; Saw 0 at 0 and rising; Square +1 on [0, 1/2)");

            bool boundedBlep = true;
            for (int src = 1; src < 4; ++src)
                for (int i = 0; i < 4096; ++i)
                    boundedBlep = boundedBlep && std::abs (mo::shape (src, (double) i / 4096.0, 0.05)) <= 1.3;
            check (boundedBlep, "band-limited shapes (dphi 0.05) overshoot by at most 0.3");
        }

        // ---- dM/dphi = kappa m away from the steps; zero mean; unit peak
        {
            const double kappa[4] = { 2.0 * kPi, 8.0, 6.0, 4.0 };
            double worstDeriv = 0.0, worstMean = 0.0, worstPeak = 0.0;

            for (int src = 0; src < 4; ++src)
            {
                double mean = 0.0, peak = 0.0;
                constexpr int n = 20000;
                constexpr double h = 1.0e-6;

                for (int i = 0; i < n; ++i)
                {
                    const double phi = ((double) i + 0.5) / (double) n;
                    const double M = mo::integral (src, phi, 0.0);
                    mean += M / (double) n;
                    peak = std::max (peak, std::abs (M));

                    // skip the neighbourhood of m's discontinuities (Saw at phi 1/2, Square at 0 and 1/2)
                    const bool nearStep = (src == 2 && std::abs (phi - 0.5) < 0.002) || (src == 3 && (phi < 0.002 || phi > 0.998 || std::abs (phi - 0.5) < 0.002));
                    if (! nearStep)
                    {
                        const double d = (mo::integral (src, phi + h, 0.0) - mo::integral (src, phi - h, 0.0)) / (2.0 * h);
                        worstDeriv = std::max (worstDeriv, std::abs (d - kappa[src] * mo::shape (src, phi, 0.0)) / kappa[src]);
                    }
                }

                worstMean = std::max (worstMean, std::abs (mean));
                worstPeak = std::max (worstPeak, std::abs (peak - 1.0));
            }

            std::printf ("        (dM/dphi - kappa m: worst %.2e relative | mean of M: worst %.2e | peak - 1: worst %.2e)\n", worstDeriv, worstMean, worstPeak);
            check (worstDeriv < 1.0e-3, "integral: dM/dphi == kappa m (2 pi, 8, 6, 4) away from the steps");
            check (worstMean < 1.0e-4 && worstPeak < 1.0e-3, "integral: zero mean, peak normalised to 1");
        }

        // ---- BLEP / BLAMP: alias power of a band-limited shape vs the naive one (1x, exact FFT bin f0)
        {
            const int order = 15, n = 1 << order;
            const double dphi = kF0 / kFs;
            std::vector<float> naive ((size_t) n + 2048), blep (naive.size());

            std::printf ("        BLEP / BLAMP at %.1f Hz, 48 kHz: ", kF0);
            for (int src = 1; src < 4; ++src)
            {
                for (size_t i = 0; i < naive.size(); ++i)
                {
                    const double phi = mo::frac ((double) i * dphi);
                    naive[i] = (float) mo::shape (src, phi, 0.0);
                    blep[i] = (float) mo::shape (src, phi, dphi);
                }

                const double an = warpr::tests::aliasDb (naive.data(), (int) naive.size(), kFs, kF0, order);
                const double ab = warpr::tests::aliasDb (blep.data(), (int) blep.size(), kFs, kF0, order);
                std::printf ("%s naive %.1f / band-limited %.1f dB (%+.1f)   ", src == 1 ? "Tri" : (src == 2 ? "Saw" : "Sqr"), an, ab, an - ab);

                // HAND-GUESSED gates: polyBLEP gains 20+ dB on a 3 kHz saw / square; BLAMP on a triangle (already 1/f^2) a few dB
                char label[200];
                std::snprintf (label, sizeof (label), "%s: polyBLEP / BLAMP lowers the 1x alias power (%.1f dB, gate %.0f)",
                               src == 1 ? "Triangle" : (src == 2 ? "Saw" : "Square"), an - ab, src == 1 ? 3.0 : 10.0);
                check (an - ab >= (src == 1 ? 3.0 : 10.0), label);
            }
            std::printf ("\n");
        }

        // ---- Sample-source follower: bounded for anything, finite after silence
        {
            mo::SampleMod sm;
            const auto cf = mo::coefsFor (96000.0);
            juce::Random rng (42);
            bool bounded = true;
            double m = 0.0, M = 0.0;

            for (int i = 0; i < 20000; ++i)
            {
                const double x = i < 10000 ? 8.0 * ((double) rng.nextFloat() * 2.0 - 1.0) : 0.0;
                sm.process (x, 0.003, cf, m, M);
                bounded = bounded && std::abs (m) <= 1.0 + 1.0e-12 && std::abs (M) <= 1.0 + 1.0e-12 && std::isfinite (m) && std::isfinite (M);
            }

            // a unit sine at the nominal modulator frequency gives M with unit amplitude (= the analytic shapes' scale)
            mo::SampleMod s2;
            double peakM = 0.0;
            // (the integrator starts with a DC transient of up to 2 that the 2 s peak follower remembers: measure after 7 s)
            for (int i = 0; i < 768000; ++i)
            {
                const double dphi = 0.003;
                s2.process (std::sin (2.0 * kPi * dphi * (double) i), dphi, cf, m, M);
                if (i > 672000)
                    peakM = std::max (peakM, std::abs (M));
            }

            std::printf ("        (Sample follower: sine at the nominal frequency -> peak |M| %.3f)\n", peakM);
            check (bounded, "SampleMod: |m| <= 1 and |M| <= 1 for loud noise, then silence (peak followers, floor 0.1)");
            check (peakM > 0.85 && peakM <= 1.0 + 1.0e-12, "SampleMod: a tone at the nominal modulator frequency integrates to ~unit amplitude");
        }

        // ---- soft limiter
        {
            bool odd = true, mono = true, bounded = true;
            double prev = -2.0;

            for (double x = -6.0; x <= 6.0; x += 0.01)
            {
                const double y = mo::softLimit (x);
                odd = odd && std::abs (mo::softLimit (-x) + y) < 1.0e-12;
                mono = mono && y >= prev - 1.0e-12;
                bounded = bounded && std::abs (y) <= 1.0;
                prev = y;
            }

            check (odd && mono && bounded && mo::softLimit (3.0) == 1.0 && mo::softLimit (-4.0) == -1.0 && std::abs (mo::softLimit (0.01) - 0.01) < 1.0e-5,
                   "softLimit: odd, monotone, within +-1, exactly +-1 from |x| = 3, slope 1 at 0");
        }
    }

    //==========================================================================
    // 3. phase maps
    //==========================================================================
    void testPhaseMaps()
    {
        std::printf ("\n[D6] phase maps 44-53: neutral at a = 0, anchors, continuity, monotony, extras\n");
        const double dT = 220.0 / (kFs * 2.0);
        const pw::MapCtx noCx;

        // ---- neutral at a = 0 for every D6 row, evalMap == delta for the old rows
        {
            int bad = 0, oldBad = 0;

            for (int mode = 44; mode <= 53; ++mode)
                for (double A : { 0.0, 0.5, 1.0 })
                    for (double B : { 0.0, 1.0 })
                        for (double t : { 0.0, 0.3, 0.7, 0.999 })
                        {
                            const auto o = pw::evalMap (mode, 0.0, A, B, t, dT, noCx);
                            if (! o.identity() || o.g1 != 0.0f || o.amp != 1.0f)
                                ++bad;
                        }

            for (int mode : { 0, 1, 2, 3, 7, 9, 14 })
                for (double t : { 0.0, 0.2, 0.5, 0.77, 0.999 })
                {
                    const auto o = pw::evalMap (mode, 0.7, 0.4, 0.6, t, dT, noCx);
                    if (o.d0 != pw::delta (mode, 0.7, 0.4, 0.6, t, dT) || o.g1 != 0.0f || o.amp != 1.0f || o.g0 != 1.0f)
                        ++oldBad;
                }

            check (bad == 0, "evalMap is the exact neutral PhaseOut at warpAmount 0 for all rows 44-53");
            check (oldBad == 0, "evalMap reproduces delta() bit-exactly (single tap) for the old rows 0-3, 7, 9, 14");
        }

        // ---- anchors (hand-derived)
        {
            auto g = [&] (int mode, double a, double A, double B, double t) { return pw::phaseMap (mode, a, A, B, t, dT); };
            bool ok = true;

            ok = ok && std::abs (g (44, 1.0, 0.0, 0.0, 0.02) - 0.5) < 1.0e-9 && std::abs (g (44, 1.0, 0.0, 0.0, 1.0) - 1.0) < 1.0e-9;     // d = 0.02
            ok = ok && std::abs (g (45, 0.5, 1.0, 0.0, 0.0) - 0.475) < 1.0e-12 && std::abs (g (45, 0.5, 1.0, 0.0, 1.0) - 1.0) < 1.0e-12;    // z 0.525, s 0.475
            ok = ok && std::abs (g (46, 1.0 / 7.0, 0.0, 0.0, 0.25) - 0.5) < 1.0e-9 && std::abs (g (46, 1.0 / 7.0, 0.0, 0.0, 0.75) - 0.5) < 1.0e-9;   // c = 2
            ok = ok && std::abs (g (46, 0.5, 1.0, 0.0, 0.1) - 0.5) < 1.0e-9;                                                            // c 4.5 snapped to 5
            ok = ok && std::abs (g (47, 3.0 / 7.0, 0.0, 0.0, 0.25) - 1.0) < 1.0e-9 && std::abs (g (47, 3.0 / 7.0, 0.0, 0.0, 0.5)) < 1.0e-9
                       && std::abs (g (47, 3.0 / 7.0, 0.0, 0.0, 0.75) - 1.0) < 1.0e-9                                                    // k = 4 half-cycles
                       && std::abs (g (47, 1.0 / 7.0, 0.0, 0.0, 0.25) - 0.5) < 1.0e-9 && std::abs (g (47, 1.0 / 7.0, 0.0, 0.0, 0.5) - 1.0) < 1.0e-9; // k = 2: one ping-pong
            ok = ok && std::abs (g (48, 0.8, 0.5, 0.0, 0.0)) < 1.0e-12 && std::abs (g (48, 0.8, 0.5, 0.7, 1.0) - 1.0) < 1.0e-9;       // g(0) = 0, g(1) = 1
            ok = ok && std::abs (g (51, 0.5, 0.5, 0.0, 0.2) - 0.9) < 1.0e-12 && std::abs (g (52, 0.5, 0.5, 0.0, 1.0) - 4.5) < 1.0e-12; // r = 4.5
            check (ok, "hand-derived anchors: PD breakpoint, Stretch window, Compression c snap, Mirror ping-pong, Skew ends, sync ratio r = 1 + a (7 + 2 (A - 0.5))");
        }

        // ---- continuity of the cycle map across the wrap (mod one cycle) and range / monotony
        {
            int contBad = 0, rangeBad = 0, monoBad = 0, cases = 0;

            for (double a : { 0.05, 0.3, 0.6, 1.0 })
                for (double A : { 0.0, 0.5, 1.0 })
                    for (double B : { 0.0, 0.5, 1.0 })
                    {
                        ++cases;

                        for (int mode : { 44, 45, 46, 47, 48, 49 })
                        {
                            const bool resonant = mode == 44 && B > 0.0;
                            double prev = pw::phaseMap (mode, a, A, B, 0.0, dT);
                            bool mono = true, inRange = true;

                            for (double t = 0.0005; t <= 1.0; t += 0.0005)
                            {
                                const double v = pw::phaseMap (mode, a, A, B, t, dT);
                                inRange = inRange && v >= -1.0e-9 && v <= 1.0 + 1.0e-9;
                                // monotone inside a sub-cycle: only the modes that never reverse (not Mirror) and without a wrapping frac
                                if ((mode == 45 || mode == 48 || mode == 49 || (mode == 44 && B <= 0.0)) && v < prev - 1.0e-9)
                                    mono = false;
                                prev = v;
                            }

                            if (! inRange) ++rangeBad;
                            if (! mono) ++monoBad;

                            // continuity across the wrap, modulo one cycle, for the maps that promise it
                            // (Mirror is excluded: a fractional k blends an odd and an even ping-pong map, see mirrorMap; integer k below)
                            if (mode == 48 || mode == 49 || (mode == 44 && ! resonant))
                            {
                                const double e = pw::phaseMap (mode, a, A, B, 1.0 - 1.0e-9, dT) - pw::phaseMap (mode, a, A, B, 0.0, dT);
                                const double dist = std::abs (e - std::round (e));
                                if (dist > 1.0e-5)
                                {
                                    ++contBad;
                                    DETAIL ("        wrap discontinuity %.2e: mode %d a=%.2f A=%.1f B=%.1f\n", dist, mode, a, A, B);
                                }
                            }
                        }
                    }

            std::printf ("        (%d parameter combinations)\n", cases);
            check (rangeBad == 0, "g stays inside [0, 1] for 44-49");
            check (monoBad == 0, "g is monotone for Phase Distortion (B = 0), Stretch, Skew and Quantization");
            check (contBad == 0, "g(1) == g(0) mod 1 for Phase Distortion (B = 0), Skew, Quantization");

            int mirrorBad = 0;
            for (int k = 1; k <= 8; ++k)                                       // integer k = 1 + 7a: every ping-pong map is wrap-continuous
                for (double B : { 0.0 })
                {
                    const double a = (double) (k - 1) / 7.0;
                    const double e = pw::phaseMap (47, a, 0.5, B, 1.0 - 1.0e-9, dT) - pw::phaseMap (47, a, 0.5, B, 0.0, dT);
                    if (std::abs (e - std::round (e)) > 1.0e-5)
                        ++mirrorBad;
                }
            check (mirrorBad == 0, "Phase Mirror: g(1) == g(0) mod 1 at every integer k = 1 + 7a (k = 1 is the identity)");
        }

        // ---- Skew: strictly monotone everywhere
        {
            int bad = 0;
            for (double a : { 0.2, 0.6, 1.0 })
                for (double A : { 0.0, 0.3, 0.5, 0.8, 1.0 })
                    for (double B : { 0.0, 0.5, 1.0 })
                    {
                        double prev = -1.0;
                        for (double t = 0.0; t <= 1.0; t += 0.001)
                        {
                            const double v = pw::phaseMap (48, a, A, B, t, dT);
                            if (v <= prev) { ++bad; break; }
                            prev = v;
                        }
                    }
            check (bad == 0, "Phase Skew is strictly monotone for every (a, A, B) (k + k2 < 1 by construction)");
        }

        // ---- Phase Quantization: a hard staircase at A -> 0 (multiples of 1/N)
        {
            bool ok = true;
            int distinct = 0;
            double last = -1.0;

            for (double a : { 0.3, 0.6, 1.0 })
            {
                const double n = std::max (2.0, std::floor (std::exp2 (10.0 - 9.0 * a) + 0.5));
                last = -1.0;
                distinct = 0;

                for (double t = 0.0; t < 1.0; t += 0.0007)
                {
                    const double v = pw::phaseMap (49, a, 1.0e-9, 0.0, t, dT) * n;
                    ok = ok && std::abs (v - std::round (v)) < 1.0e-6;
                    if (std::abs (v - last) > 0.5) { ++distinct; last = v; }
                }
            }

            std::printf ("        (Phase Quantization a = 1, A -> 0: %d level changes, all multiples of 1/N: %s)\n", distinct, ok ? "yes" : "NO");
            check (ok && distinct == 3,"Phase Quantization at smoothness ~0 takes only multiples of 1/N (N = round (2^(10 - 9a))): N = 2 at a = 1 gives the levels 0, 1/2, 1");
        }

        // ---- the extras: second tap, window, Soft Sync parity, BLEP weights
        {
            // Phase Stretch edge cross-fade: at theta -> 1 the output is the tap at g - z (equal power: g0^2 + g1^2 = 1)
            const auto s0 = pw::evalMap (45, 0.5, 1.0, 1.0, 0.99, dT, noCx);
            const auto s1 = pw::evalMap (45, 0.5, 1.0, 1.0, 1.0 - 1.0e-9, dT, noCx);
            const auto s2 = pw::evalMap (45, 0.5, 1.0, 1.0, 0.1, dT, noCx);
            check (s0.g1 > 0.0f && std::abs (s0.g0 * s0.g0 + s0.g1 * s0.g1 - 1.0f) < 1.0e-5f && s1.g1 > 0.999f && s2.g1 == 0.0f,
                   "Phase Stretch: B > 0 cross-fades equal-power into the read before the window start over the last 0.5 B of the cycle");

            // Compression sub-cycle cross-fade only near complete sub-cycle ends
            const auto c0 = pw::evalMap (46, 1.0 / 7.0, 0.0, 1.0, 0.49, dT, noCx);             // c = 2: sub-cycle ends at 0.5
            const auto c1 = pw::evalMap (46, 1.0 / 7.0, 0.0, 1.0, 0.2, dT, noCx);
            check (c0.g1 > 0.0f && c1.g1 == 0.0f, "Phase Compression: B > 0 cross-fades only inside the tail of a complete sub-cycle");

            // Phase Distortion window
            const auto w0 = pw::evalMap (44, 0.5, 0.5, 1.0, 0.5, dT, noCx);
            const auto w1 = pw::evalMap (44, 0.5, 0.5, 0.0, 0.5, dT, noCx);
            check (std::abs (w0.amp - 0.5f) < 1.0e-5f && w1.amp == 1.0f, "Phase Distortion: resonance window (1 - theta)^B on the output, none at B = 0");

            // Soft Sync: continuous across the wrap (B = 0), reflection parity
            const double r = pw::syncRatio (0.5, 0.5);
            pw::MapCtx even, odd;
            odd.parity = 1;
            const double gEvenEnd = r;                                                       // g (1) of an even cycle
            const double gOddStart = pw::evalMap (53, 0.5, 0.5, 0.0, 0.0, dT, odd).d0;       // g (0) = d (0) + 0
            const double gOddEnd = 1.0 + pw::evalMap (53, 0.5, 0.5, 0.0, 1.0, dT, odd).d0;  // g (1) = d (1) + 1
            const double gEvenStart = pw::evalMap (53, 0.5, 0.5, 0.0, 0.0, dT, even).d0;
            check (std::abs (gOddStart - gEvenEnd) < 1.0e-9 && std::abs (gOddEnd) < 1.0e-9 && std::abs (gEvenStart) < 1.0e-12,
                   "Soft Sync (B = 0): even cycles run r theta, odd cycles run r (1 - theta): the slave position is continuous across every master wrap");

            // BLEP weights: partition of unity, <= 1/2, mirror image around the event
            bool ok = true;
            const double dTheta = 0.01;
            for (double th : { 0.0, 0.002, 0.0099, 0.9901, 0.998 })
            {
                const auto o = pw::evalMap (52, 0.5, 0.5, 0.0, th, dTheta, noCx);
                ok = ok && o.g1 > 0.0f && o.g1 <= 0.5f + 1.0e-6f && std::abs (o.g0 + o.g1 - 1.0f) < 1.0e-6f;
            }
            const auto mid = pw::evalMap (52, 0.5, 0.5, 0.0, 0.5, dTheta, noCx);
            const auto eventPre = pw::evalMap (52, 0.5, 0.5, 0.0, 1.0 - 1.0e-12, dTheta, noCx);
            const auto eventPost = pw::evalMap (52, 0.5, 0.5, 0.0, 0.0, dTheta, noCx);
            pw::MapCtx noBlep;
            noBlep.blep = false;
            const auto off = pw::evalMap (52, 0.5, 0.5, 0.0, 0.0, dTheta, noBlep);
            check (ok && mid.g1 == 0.0f && std::abs (eventPre.g1 - 0.5f) < 1.0e-4f && std::abs (eventPost.g1 - 0.5f) < 1.0e-4f && off.g1 == 0.0f,
                   "Hard Sync BLEP: weights 0.5 (1 - x)^2 / 0.5 (x + 1)^2 on the other trajectory within one sample of the reset, 1/2 at the event, none elsewhere / with blep off");
        }

        // ---- Phase Remapping against the curve
        {
            const auto idSet = warpr::ShaperLutSet::build (warpr::ShaperCurve::identity(), warpr::ShaperCurve::identity(), true);
            const auto hc = warpr::shaperpresets::generate (1);
            const auto mixSet = warpr::ShaperLutSet::build (warpr::ShaperCurve::identity(), hc, true);

            pw::MapCtx idCx, mixCx, nullCx;
            idCx.curve = idSet.get();
            mixCx.curve = mixSet.get();

            bool idExact = true, nullExact = true, mathOk = true;
            double worst = 0.0;

            for (double a : { 0.3, 1.0 })
                for (double A : { 0.0, 0.5, 1.0 })
                    for (double t = 0.0; t <= 1.0; t += 0.0137)
                    {
                        idExact = idExact && pw::evalMap (50, a, A, 0.0, t, dT, idCx).identity();
                        nullExact = nullExact && pw::evalMap (50, a, A, 0.0, t, dT, nullCx).identity();

                        // curve blend A: 0 = curve A (identity), 1 = curve B (Hard Clip)
                        const double x = 2.0 * t - 1.0;
                        const double expect = 0.5 * a * (A * (mixSet->fAt (1, x) - x));
                        const double got = pw::evalMap (50, a, A, 0.0, t, dT, mixCx).d0;
                        worst = std::max (worst, std::abs (got - expect));
                        mathOk = mathOk && std::abs (got - expect) < 1.0e-12;
                    }

            std::printf ("        (Phase Remapping vs 0.5 a A (f_B - x): worst %.2e)\n", worst);
            check (idExact && nullExact, "Phase Remapping with the identity curve (or no curve) is the exact neutral PhaseOut");
            check (mathOk, "Phase Remapping: Delta = 0.5 a (blend (A) of the stored remainders) at x = 2 theta - 1");
        }
    }

    //==========================================================================
    // 4. Hard Sync: analytic output + BLEP measured
    //==========================================================================
    void testHardSync()
    {
        std::printf ("\n[D6] Hard Sync: analytic output, BLEP residual measured\n");
        auto sine = makeCycle (2048, 0.5f);

        // analytic: y = 0.5 sin (2 pi r theta) with BLEP off (theta = frac (hz t / ...), r = 1 + a (7 + 2 (A - 0.5)))
        {
            D6Cfg c = cfgOf (52, 0.4f, 0.5f, 0.0f);
            c.blep = false;
            const double hz = 220.0;
            const auto run = renderOsc (*sine, hz, kFs, 2, 6400, d6Span (c));
            const double r = pw::syncRatio (0.4, 0.5);
            const double dTheta = hz / (kFs * 2.0);
            double worst = 0.0;

            for (size_t i = 256; i < run.L.size(); ++i)
            {
                const double th = mo::frac ((double) i * dTheta);
                worst = std::max (worst, std::abs ((double) run.L[i] - 0.5 * std::sin (2.0 * kPi * r * th)));
            }

            std::printf ("        (analytic 0.5 sin (2 pi r theta), r = %.2f: worst error %.2e)\n", r, worst);
            // HAND-DERIVED: sinc8 interpolation of a 2048-frame sine at <= 9.4 frames per sample is accurate to ~1e-4; 2e-3 leaves room for the mip level
            check (worst < 2.0e-3, "Hard Sync (BLEP off): the oscillator output equals 0.5 sin (2 pi r theta)");
        }

        // BLEP residual: alias power at 1x with and without, and the largest step
        {
            double an = 0.0, ab = 0.0, stepOff = 0.0, stepOn = 0.0;
            for (int pass = 0; pass < 2; ++pass)
            {
                D6Cfg c = cfgOf (52, 0.4f, 0.5f, 0.0f);
                c.blep = pass == 1;
                const auto run = renderOsc (*sine, kF0, kFs, 1, kAliasBase, d6Span (c));
                const double al = aliasOf (run.L, kFs, 1, kF0);
                const double st = maxStepOf (run.L, 4096);
                (pass == 0 ? an : ab) = al;
                (pass == 0 ? stepOff : stepOn) = st;
            }

            std::printf ("        (3 kHz master, r = 3.8, 1x: alias %.1f dB naive / %.1f dB with BLEP = %.1f dB better; max step %.3f -> %.3f)\n",
                         an, ab, an - ab, stepOff, stepOn);
            // HAND-GUESSED: the 2-sample polyBLEP corrects the reset STEP only; the slope break of the reset (1/f^2 tail) stays, which
            // caps the gain (the first run measured 7.4 dB with a DC-contaminated meter). Gate 6 dB; the polyBLEP saw gains 14 dB.
            check (an - ab >= 6.0, "Hard Sync BLEP lowers the 1x alias power by >= 6 dB");
            check (stepOn < stepOff, "the BLEP spreads the reset step: the largest sample-to-sample step is smaller than the naive reset");
        }

        // B > 0 replaces the BLEP by a smoothstep fade into the post-reset trajectory: no step at all
        {
            D6Cfg c = cfgOf (52, 0.4f, 0.5f, 1.0f);
            const auto run = renderOsc (*sine, 220.0, kFs, 2, 6400, d6Span (c));
            c.B = 0.0f;
            const auto hard = renderOsc (*sine, 220.0, kFs, 2, 6400, d6Span (c));
            std::printf ("        (220 Hz, 2x: max step B = 0 (BLEP) %.4f, B = 1 (0.15-cycle fade) %.4f)\n", maxStepOf (hard.L, 512), maxStepOf (run.L, 512));
            check (maxStepOf (run.L, 512) < maxStepOf (hard.L, 512), "Hard Sync B > 0: the reset-edge fade leaves a smaller step than the BLEP'd hard reset");
        }
    }

    //==========================================================================
    // 5. A3 alias gate
    //==========================================================================
    void testAliasGateA3()
    {
        std::printf ("\n[D6] T2 / A3: FM / PM / PD / Hard Sync at 8x are >= 20 dB below 1x (f0 %.1f Hz, in-band alias meter)\n", kF0);
        auto saw = warpr::SampleData::createDefaultWave();
        auto sine = makeCycle (2048, 0.5f);

        struct Case { const char* name; const warpr::SampleData* sd; D6Cfg cfg; };
        // operating points: strong enough to alias heavily at 1x, mild enough that the spectrum fits below 8 x fs/2.
        // FM / PM: sine modulator at ratio 1 (a harmonic ratio: every sideband is a harmonic of f0, so the meter sees only fold-back)
        const Case cases[] = {
            { "FM  (a 0.5, A 0.25, I 1.25 cycles)", saw.get(),  cfgOf (54, 0.5f, 0.25f, 0.0f, 0, 1.0f) },
            { "PM  (a 1.0, A 0.25, I 1.25 cycles)", saw.get(),  cfgOf (55, 1.0f, 0.25f, 0.0f, 0, 1.0f) },
            { "PD  (a 0.5, d = 0.26)",              saw.get(),  cfgOf (44, 0.5f, 0.5f, 0.0f) },
            // Hard Sync is gated on its NAIVE path (BLEP off): A3 is the oversampling claim. With the BLEP on, the residual below
            // 8x is dominated by the uncorrected slope break of the reset, so the OS gain is smaller by construction; the BLEP is
            // gated separately below (efficacy at 1x, never worse than naive at 8x).
            { "Hard Sync (a 0.4, r 3.8, BLEP off)", sine.get(), hardSyncNaive() }
        };

        int bad = 0;

        for (const auto& cs : cases)
        {
            double lvl[4] {};
            int k = 0;

            for (int os : { 1, 2, 4, 8 })
            {
                const auto run = renderOsc (*cs.sd, kF0, kFs, os, kAliasBase, d6Span (cs.cfg));
                lvl[k++] = aliasOf (run.L, kFs, os, kF0);
            }

            const double gain = lvl[0] - lvl[3];
            std::printf ("        %-38s alias 1x %7.1f | 2x %7.1f | 4x %7.1f | 8x %7.1f dB  -> 8x is %.1f dB below 1x (gate 20)\n",
                         cs.name, lvl[0], lvl[1], lvl[2], lvl[3], gain);

            if (gain < 20.0)
            {
                ++bad;
                DETAIL ("        A3 FAILED for %s: %.1f dB\n", cs.name, gain);
            }
        }

        check (bad == 0, "A3: FM, PM, Phase Distortion and Hard Sync (naive reset) alias at 8x is at least 20 dB below the 1x level");

        // BLEP efficacy across OS: at every factor the BLEP'd reset is no worse than the naive one
        {
            bool notWorse = true;
            for (int os : { 1, 8 })
            {
                D6Cfg on = hardSyncNaive();
                on.blep = true;
                const double lOn = aliasOf (renderOsc (*sine, kF0, kFs, os, kAliasBase, d6Span (on)).L, kFs, os, kF0);
                const double lOff = aliasOf (renderOsc (*sine, kF0, kFs, os, kAliasBase, d6Span (hardSyncNaive())).L, kFs, os, kF0);
                std::printf ("        Hard Sync %dx: naive %7.1f dB, BLEP %7.1f dB (%+.1f)\n", os, lOff, lOn, lOff - lOn);
                notWorse = notWorse && lOn <= lOff + 1.0;
            }
            check (notWorse, "Hard Sync BLEP is never worse than the naive reset at 1x or 8x (within 1 dB)");
        }
    }

    //==========================================================================
    // 6. T1 universal cube
    //==========================================================================
    double discBound (int mode, const D6Cfg& c, double nat, double hz, int os)
    {
        const auto& info = warpr::warpModeInfo (mode);

        if (info.stepFlag || info.seamJump)
            return 2.0;                                                     // intended discontinuity: full-swing bound

        const double a = (double) c.a, A = (double) c.A, B = (double) c.B, ratio = (double) c.ratio;

        switch (mode)
        {
            case 47:
            case 48:
                return 2.0 * nat * maxSlopeMap (mode, a, A, B, hz / (kFs * (double) os)) + 0.01;

            case 54:
            {
                if (c.src > 3) return 2.0;
                const double kappa[4] = { 2.0 * kPi, 8.0, 6.0, 4.0 };
                const double I = 4.0 * a * (0.25 + 1.5 * A);
                return std::min (2.5, 2.0 * nat * (1.0 + I * kappa[c.src] * ratio) + 0.01);
            }

            case 55:
            {
                if (c.src > 1 || c.B > 0.0f) return 2.0;                     // Saw / Square / Sample step; B > 0 = modulator self-feedback (not analytic)
                const double kappa[2] = { 2.0 * kPi, 4.0 };
                const double I = 2.0 * a * (0.25 + 1.5 * A);
                return std::min (2.5, 2.0 * nat * (1.0 + I * kappa[c.src] * ratio) + 0.01);
            }

            case 56:
            case 57:
                return 1.5;                                                  // gain follows the modulator (at most 2 x 0.5 FS, BLEP overshoot slack)

            default:
                return 2.0;                                                  // Self FM / Cross Mod: chaotic feedback, full-swing bound
        }
    }

    void testUniversal (bool full)
    {
        std::printf ("\n[D6] T1 universal checks: 16 modes x 4 signals x OS x (a, A, B) x modulator source%s\n", full ? "  [full cube]" : "  [quick cube]");

        auto sine = makeCycle (2048, 0.5f);
        auto saw = warpr::SampleData::createDefaultWave();
        auto noise = makeNoise();

        struct Sig { const warpr::SampleData* sd; double hz; const char* name; bool smooth; std::vector<int> osQuick, osFull; };
        const std::vector<Sig> sigs {
            { sine.get(),  100.0,    "sine 100 Hz",      true,  { 1, 2 }, { 1, 2, 8 } },
            { sine.get(),  5000.0,   "sine 5 kHz",       false, { 1, 8 }, { 1, 2, 8 } },
            { saw.get(),   110.0,    "saw cycle 110 Hz", false, { 2 },    { 1, 2, 8 } },
            { noise.get(), rootHz(), "noise",            false, { 1, 8 }, { 1, 2, 8 } }
        };

        const std::vector<double> aList = full ? std::vector<double> { 0.0, 0.5, 1.0 } : std::vector<double> { 0.0, 1.0 };
        const std::vector<double> AList = full ? std::vector<double> { 0.0, 0.5, 1.0 } : std::vector<double> { 0.5 };
        const std::vector<double> bList { 0.0, 1.0 };
        const int totalBase = full ? 7200 : 3200;

        int runs = 0, nonFinite = 0, unbounded = 0, identityBad = 0, discBad = 0;
        double worstPeak = 0.0;

        for (int mode = 44; mode <= 59; ++mode)
        {
            const auto& info = warpr::warpModeInfo (mode);
            const bool usesMod = info.family == warpr::WarpFamily::Mod && mode != 58;
            const std::vector<int> srcs = usesMod ? (full ? std::vector<int> { 0, 3, 4 } : std::vector<int> { 0, 4 }) : std::vector<int> { 4 };
            const std::vector<float> ratios = usesMod && full ? std::vector<float> { 1.0f, 3.7f } : std::vector<float> { 1.0f };

            for (const auto& sig : sigs)
                for (int os : (full ? sig.osFull : sig.osQuick))
                {
                    const auto base = renderOsc (*sig.sd, sig.hz, kFs, os, totalBase, noWarp());
                    const double nat = maxStepOf (base.L);

                    for (int src : srcs)
                        for (float ratio : ratios)
                            for (double A : AList)
                                for (double a : aList)
                                    for (double B : bList)
                                    {
                                        if (a == 0.0 && B > 0.0 && ! full)
                                            continue;

                                        ++runs;
                                        const D6Cfg c = cfgOf (mode, (float) a, (float) A, (float) B, src, ratio);
                                        const auto run = renderOsc (*sig.sd, sig.hz, kFs, os, totalBase, d6Span (c));

                                        if (! allFinite (run.L) || ! allFinite (run.R))
                                        {
                                            ++nonFinite;
                                            DETAIL ("        non-finite: mode %d (%s) %s OS %d a=%.1f A=%.1f B=%.1f src %d ratio %.1f\n", mode, info.name, sig.name, os, a, A, B, src, (double) ratio);
                                            continue;
                                        }

                                        const double pk = std::max (peakOf (run.L), peakOf (run.R));
                                        worstPeak = std::max (worstPeak, pk);
                                        if (pk > 4.0)
                                        {
                                            ++unbounded;
                                            DETAIL ("        unbounded %.2f: mode %d (%s) %s OS %d a=%.1f A=%.1f B=%.1f src %d\n", pk, mode, info.name, sig.name, os, a, A, B, src);
                                        }

                                        if (a == 0.0 && ! (bitEqual (base.L, run.L) && bitEqual (base.R, run.R)))
                                        {
                                            ++identityBad;
                                            DETAIL ("        a=0 not bit-exact dry (null %.1f dB): mode %d (%s) %s OS %d\n", nullDb (base.L, run.L), mode, info.name, sig.name, os);
                                        }

                                        if (sig.smooth)
                                        {
                                            const double bound = discBound (mode, c, nat, sig.hz, os);
                                            const double step = maxStepOf (run.L);
                                            if (step > bound)
                                            {
                                                ++discBad;
                                                DETAIL ("        discontinuity %.4f > %.4f: mode %d (%s) OS %d a=%.1f A=%.1f B=%.1f src %d ratio %.1f\n",
                                                        step, bound, mode, info.name, os, a, A, B, src, (double) ratio);
                                            }
                                        }
                                    }
                }
        }

        std::printf ("        (%d renders, worst |y| %.3f)\n", runs, worstPeak);
        check (nonFinite == 0, "no NaN / inf for any D6 mode, signal, OS factor, parameter or modulator source");
        check (unbounded == 0, "|y| <= 4 FS before the master for every combination");
        check (identityBad == 0, "a == 0: bit-exact dry for all 16 D6 modes (stronger than the -80 dB null)");
        check (discBad == 0, "smooth input: max step within the per-mode bound (flagged modes use their own full-swing bound)");
    }

    //==========================================================================
    // stage harness: WarpOscState::process driven sample by sample
    //==========================================================================
    struct StageRun
    {
        std::vector<float> y;
        double maxSpeed = 0.0;
        int guardTrips = 0;
    };

    /** One-sine-cycle sample (2048 frames, root 60) played at 220 Hz, 1x; `poisonAt` >= 0 makes the carrier read return NaN once. */
    StageRun runStage (int mode, double a, double A, double B, int src, double ratio, int n, int poisonAt = -1)
    {
        const auto sd = makeCycle (2048, 0.8f);
        warpr::RegionFold rg;
        rg.configure (*sd, 0.0f, 1.0f, warpr::RegionFold::Mode::Forward);

        const double hz = 220.0, lc = 2048.0;
        const double step = hz / rootHz() * sd->fileSampleRate / kFs;                    // frames per sample
        const auto rp = warpr::makeReadParams (*sd, step, 0, rg.wrapTaps, rg.lo, rg.hi);

        warpr::WarpOscState st;
        st.reset (0.0);

        warpr::WarpSpan sp;
        sp.active = true;
        sp.mode = mode;
        sp.Lc = lc;
        sp.os = 1;
        sp.osRate = kFs;
        sp.fadeLen = 480;
        sp.modSource = src;

        int idx = 0;
        auto readAt = [&] (double p, float& l, float& r) noexcept
        {
            warpr::readFrame (rp, p, l, r);

            if (idx == poisonAt)
                l = r = std::numeric_limits<float>::quiet_NaN();
        };
        auto readMod = [&] (double p, float& l, float& r) noexcept { warpr::readFrame (rp, p, l, r); };

        StageRun out;
        out.y.reserve ((size_t) n);
        double pos = 0.0;
        int dir = 1;

        for (idx = 0; idx < n; ++idx)
        {
            float oL = 0.0f, oR = 0.0f;
            st.process (sp, (float) a, (float) A, (float) B, 1.0f, (float) ratio, pos, dir, step, pos, dir, rg, false, readAt, readAt, readMod, oL, oR);
            out.y.push_back (oL);
            out.maxSpeed = std::max (out.maxSpeed, st.speedRatio());
            st.advanceTheta (step / lc);
            rg.advance (pos, dir, step);
        }

        out.guardTrips = st.guardTrips();
        return out;
    }

    //==========================================================================
    // 7. Self FM / Cross Mod stability
    //==========================================================================
    void testFeedbackStability (bool full)
    {
        std::printf ("\n[D6] Self FM / Cross Mod: bounded + NaN-free over the A / B / a cube at 1x / 8x, feedback guard exercised\n");

        auto saw = warpr::SampleData::createDefaultWave();
        auto noise = makeNoise();
        auto sine = makeCycle (2048, 0.5f);

        const std::vector<double> grid = full ? std::vector<double> { 0.0, 0.25, 0.5, 0.75, 1.0 } : std::vector<double> { 0.0, 0.5, 1.0 };
        const int totalBase = full ? 4800 : 2400;

        struct Sig { const warpr::SampleData* sd; double hz; const char* name; };
        const Sig sigs[] = { { saw.get(), 110.0, "saw 110 Hz" }, { noise.get(), rootHz(), "noise" }, { sine.get(), 5000.0, "sine 5 kHz" } };

        int runs = 0, nonFinite = 0, unbounded = 0;
        double worstPeak = 0.0;

        for (int mode : { 58, 59 })
            for (const auto& sig : sigs)
                for (int os : { 1, 8 })
                    for (double a : grid)
                        for (double A : grid)
                            for (double B : grid)
                                for (int src : { 0, 4 })
                                {
                                    if (mode == 58 && src == 0)
                                        continue;                                  // Self FM ignores the modulator

                                    ++runs;
                                    const auto run = renderOsc (*sig.sd, sig.hz, kFs, os, totalBase, d6Span (cfgOf (mode, (float) a, (float) A, (float) B, src, 1.0f)));
                                    const double pk = std::max (peakOf (run.L), peakOf (run.R));
                                    worstPeak = std::max (worstPeak, pk);

                                    if (! allFinite (run.L) || ! allFinite (run.R))
                                    {
                                        ++nonFinite;
                                        DETAIL ("        non-finite: mode %d %s OS %d a=%.2f A=%.2f B=%.2f\n", mode, sig.name, os, a, A, B);
                                    }
                                    else if (pk > 4.0)
                                    {
                                        ++unbounded;
                                        DETAIL ("        unbounded %.2f: mode %d %s OS %d a=%.2f A=%.2f B=%.2f\n", pk, mode, sig.name, os, a, A, B);
                                    }
                                }

        std::printf ("        (%d renders, worst |y| %.3f)\n", runs, worstPeak);
        check (nonFinite == 0, "Self FM / Cross Mod: finite over the whole a / A / B cube at 1x and 8x (saw, noise, 5 kHz sine; sine and Sample modulators)");
        check (unbounded == 0, "Self FM / Cross Mod: |y| <= 4 FS over the whole cube (offsets bounded: |I| <= 1.5 cycles, softLimit feedback)");

        // normal operation never trips the guard (stage level, strongest settings)
        int normalTrips = 0;
        for (int mode : { 58, 59 })
            for (double B : { 0.0, 1.0 })
                normalTrips += runStage (mode, 1.0, 0.5, B, 4, 1.0, 20000).guardTrips;
        check (normalTrips == 0, "the feedback guard stays silent in normal operation (a = 1, B in {0, 1}, 20000 samples)");

        // injected NaN: the guard trips, resets the loop, the output recovers
        for (int mode : { 58, 59 })
        {
            const auto run = runStage (mode, 1.0, 0.5, 1.0, 0, 1.0, 4000, 1000);
            bool finiteAfter = true;
            double pk = 0.0;
            for (size_t i = 1002; i < run.y.size(); ++i)
            {
                finiteAfter = finiteAfter && std::isfinite (run.y[i]);
                pk = std::max (pk, (double) std::abs (run.y[i]));
            }

            char label[200];
            std::snprintf (label, sizeof (label), "%s: an injected NaN trips the feedback guard (%d trip(s)) and the output is finite again 2 samples later (peak %.2f)",
                           modeName (mode), run.guardTrips, pk);
            check (run.guardTrips >= 1 && finiteAfter && pk <= 1.0, label);
        }

        // the guard function itself: absurd / non-finite values reset the loop
        {
            pw::ModState st;
            st.fbOut = 0.7;
            st.fbLp = 0.4;
            st.mPrev = 0.3;
            pw::feed (58, 0.5, std::numeric_limits<double>::infinity(), kFs, st);
            const bool afterInf = st.guardTrips == 1 && st.fbOut == 0.0 && st.fbLp == 0.0 && st.mPrev == 0.0;
            pw::feed (59, 0.5, 1.0e9, kFs, st);
            const bool afterHuge = st.guardTrips == 2;
            pw::feed (59, 0.5, 2.5, kFs, st);
            const bool normal = st.guardTrips == 2 && st.fbOut > 0.9 && st.fbOut <= 1.0;
            check (afterInf && afterHuge && normal, "pw::feed: inf / 1e9 trip the guard and clear the loop; a normal value is softLimited and does not trip");
        }
    }

    //==========================================================================
    // 8. mip correctness + read speed
    //==========================================================================
    warpr::SampleData::Ptr makeTone (double hz, int frames, double fileSr)
    {
        juce::AudioBuffer<float> b (1, frames);
        for (int i = 0; i < frames; ++i)
            b.setSample (0, i, 0.8f * (float) std::sin (2.0 * kPi * hz * (double) i / fileSr));
        return warpr::SampleData::create (b, fileSr, "tone", true, false);
    }

    void testMipCorrectness()
    {
        std::printf ("\n[D6] speed-aware mip selection: index mapping, modulator content does not fold, measured read speed\n");

        // mapping
        {
            bool ok = warpr::mipIndexForMult (1.0) == 0 && warpr::mipIndexForMult (1.08) == 0 && warpr::mipIndexForMult (0.3) == 0
                      && warpr::mipIndexForMult (1.1) == 1 && warpr::mipIndexForMult (1.5) == 2 && warpr::mipIndexForMult (2.0) == 2
                      && warpr::mipIndexForMult (2.01) == 3 && warpr::mipIndexForMult (16.0) == 8 && warpr::mipIndexForMult (1000.0) == 8;
            bool covers = true, monotone = true;
            int prev = 0;

            for (double m = 1.0; m <= 16.0; m *= 1.013)
            {
                const int i = warpr::mipIndexForMult (m);
                monotone = monotone && i >= prev;
                prev = i;
                if (m >= 1.09)                                                                       // (below 1.09 index 0 is the design: <= 9 % under)
                    covers = covers && std::exp2 (0.5 * (double) i) >= m * (1.0 - 1.0e-9);         // the effective speed is never underestimated
            }

            check (ok && monotone && covers, "mipIndexForMult: 0 below 1.09, half-octave steps rounded up, index capped at 8 (= 16x speed); 2^(i/2) >= mult for every mult in [1.09, 16]");

            auto tone = makeTone(1000.0, 4800, kFs);
            warpr::ReadParamsCache cache;
            cache.reset (*tone, 6.0, 0, false, 0.0, 4800.0);
            const auto direct = warpr::makeReadParams (*tone, 6.0, 0, false, 0.0, 4800.0);
            const auto& c0 = cache.get (0);
            const auto& c4 = cache.get (4);
            const auto direct4 = warpr::makeReadParams (*tone, 6.0 * 4.0, 0, false, 0.0, 4800.0);
            check (c0.lv[0].ch0 == direct.lv[0].ch0 && c0.wHigh == direct.wHigh && c4.lv[0].ch0 == direct4.lv[0].ch0 && c4.wHigh == direct4.wHigh
                       && &cache.get (4) == &c4,
                   "ReadParamsCache: slot 0 equals makeReadParams (rho) (so unwarped reads are bit-identical), slot i builds rho 2^(i/2) once");
        }

        // class level: a 15 kHz modulator read at 4x (ratio 4) is removed by the mip level, not folded
        {
            auto tone = makeTone (15000.0, 48000, kFs);
            const double rhoMod = 4.0;
            const auto withMip = warpr::makeReadParams (*tone, rhoMod, 0, true, 0.0, 48000.0);
            const auto noMip = warpr::makeReadParams (*tone, 1.0, 0, true, 0.0, 48000.0);          // what an unaware modulator read would use

            double eMip = 0.0, eNaive = 0.0;
            constexpr int n = 8192;
            for (int i = 0; i < n; ++i)
            {
                float l, r;
                warpr::readFrame (withMip, 100.0 + (double) i * rhoMod, l, r);
                eMip += (double) l * (double) l;
                warpr::readFrame (noMip, 100.0 + (double) i * rhoMod, l, r);
                eNaive += (double) l * (double) l;
            }

            const double db = 10.0 * std::log10 (std::max (eMip, 1.0e-30) / eNaive);
            std::printf ("        (15 kHz tone read at 4 frames per step: %.1f dB with the speed-matched mip vs the unaware read)\n", db);
            check (db < -60.0, "a 15 kHz modulator read at 4x speed is removed by the mip level (< -60 dB), the unaware read folds it to full amplitude");
        }

        // through the oscillator: AM with a Sample modulator at ratio 4 on a 15 kHz carrier leaves a single clean line
        {
            auto tone = makeTone (15000.0, 48000, kFs);
            D6Cfg c = cfgOf (56, 1.0f, 0.0f, 0.0f, 4, 4.0f);                                      // AM, unipolar (A 0), full depth, Sample source, ratio 4
            const auto run = renderOsc (*tone, rootHz(), kFs, 1, kAliasBase, d6Span (c));
            const double al = aliasOf (run.L, kFs, 1, 15000.0);
            D6Cfg c1 = c;
            c1.ratio = 1.0f;                                                                      // ratio 1: the modulator is the carrier's own 15 kHz tone
            const auto run1 = renderOsc (*tone, rootHz(), kFs, 1, kAliasBase, d6Span (c1));
            std::printf ("        (AM, 15 kHz carrier: Sample modulator ratio 4 -> inharmonic power %.1f dB re the line; ratio 1 (legit AM by the 15 kHz tone) %.1f dB)\n",
                         al, aliasOf (run1.L, kFs, 1, 15000.0));
            check (al < -50.0, "through the oscillator: the ratio-4 Sample modulator contributes no folded 12 kHz sidebands (inharmonic power < -50 dB)");
        }

        // measured read speed of FM: analytic peak 1 + I kappa ratio
        {
            const double a = 0.25, A = 0.5;                                        // I = 4 a (0.25 + 1.5 A) = 1 cycle
            const double expected = 1.0 + 1.0 * 2.0 * kPi * 1.0;
            const auto run = runStage (54, a, A, 0.0, 0, 1.0, 6000);
            std::printf ("        (FM I = 1 cycle, sine modulator ratio 1: measured peak read speed %.2f, analytic 1 + 2 pi = %.2f)\n", run.maxSpeed, expected);
            check (run.maxSpeed > 0.9 * expected && run.maxSpeed < 1.05 * expected, "FM: the stage measures the warped read speed (peak within -10 % / +5 % of 1 + I kappa ratio)");
        }
    }

    //==========================================================================
    // 9. Phase Remapping identity
    //==========================================================================
    void testPhaseRemapping()
    {
        std::printf ("\n[D6] Phase Remapping: identity curve = bit-exact identity\n");
        constexpr double fs = kFs;
        auto sine = makeCycle (2048, 0.5f);
        auto saw = warpr::SampleData::createDefaultWave();

        const auto idSet = warpr::ShaperLutSet::build (warpr::ShaperCurve::identity(), warpr::ShaperCurve::identity(), true);
        const auto hc = warpr::shaperpresets::generate (1);
        const auto hcSet = warpr::ShaperLutSet::build (hc, hc, true);

        int idBad = 0, nullBad = 0, zeroBad = 0;

        for (const auto* sd : { sine.get(), saw.get() })
        {
            const auto base = renderOsc (*sd, 220.0, fs, 2, 6400, noWarp());

            for (float a : { 0.5f, 1.0f })
                for (float A : { 0.0f, 0.5f, 1.0f })
                {
                    D6Cfg c = cfgOf (50, a, A, 0.0f);
                    c.curve = idSet.get();
                    const auto withId = renderOsc (*sd, 220.0, fs, 2, 6400, d6Span (c));
                    c.curve = nullptr;
                    const auto withNull = renderOsc (*sd, 220.0, fs, 2, 6400, d6Span (c));
                    if (! bitEqual (base.L, withId.L) || ! bitEqual (base.R, withId.R)) ++idBad;
                    if (! bitEqual (base.L, withNull.L)) ++nullBad;
                }

            D6Cfg z = cfgOf (50, 0.0f, 0.5f, 0.0f);
            z.curve = hcSet.get();
            if (! bitEqual (base.L, renderOsc (*sd, 220.0, fs, 2, 6400, d6Span (z)).L)) ++zeroBad;
        }

        check (idBad == 0, "identity curve set: Phase Remapping renders bit-identical to the unwarped read (a in {0.5, 1}, A in {0, 0.5, 1})");
        check (nullBad == 0, "no curve set published: Phase Remapping is the identity as well");
        check (zeroBad == 0, "a Hard Clip curve at warpAmount 0 is still the exact identity");

        // a real curve changes the sound, finitely, and the A blend moves between the two curves
        {
            const auto idc = warpr::ShaperCurve::identity();
            const auto mixSet = warpr::ShaperLutSet::build (idc, hc, true);
            D6Cfg c = cfgOf (50, 1.0f, 1.0f, 0.0f);
            c.curve = mixSet.get();
            const auto base = renderOsc (*saw, 220.0, fs, 2, 6400, noWarp());
            const auto b1 = renderOsc (*saw, 220.0, fs, 2, 6400, d6Span (c));
            c.A = 0.0f;
            const auto b0 = renderOsc (*saw, 220.0, fs, 2, 6400, d6Span (c));
            std::printf ("        (Hard Clip curve B vs dry: null %.1f dB at A = 1, %.1f dB at A = 0 (curve A = identity))\n", nullDb (base.L, b1.L), nullDb (base.L, b0.L));
            check (allFinite (b1.L) && nullDb (base.L, b1.L) > -20.0 && bitEqual (base.L, b0.L), "Phase Remapping with a Hard Clip curve B changes the sound (A = 1); A = 0 selects curve A = identity");
        }

        // processor level: the default shaper curves are identity, so warpMode 50 at full amount is bit-exact dry
        {
            PluginProcessor p;
            setParam (p, "ampSustain", 1.0f);
            setParam (p, "ampAttack", 1.0f);
            setParam (p, "warpMode", 50.0f);
            setParam (p, "warpAmount", 1.0f);
            const auto midi = warpr::tests::singleNote (60, 0.8f, fs, 0.8);
            const auto on = warpr::tests::render (p, midi, fs, 256, 0.5);
            setParam (p, "warpAmount", 0.0f);
            const auto off = warpr::tests::render (p, midi, fs, 256, 0.5);
            bool same = on.getNumSamples() == off.getNumSamples();
            for (int ch = 0; ch < 2 && same; ++ch)
                for (int i = 0; i < on.getNumSamples(); ++i)
                    if (on.getSample (ch, i) != off.getSample (ch, i)) { same = false; break; }
            check (same, "processor: Phase Remapping at warpAmount 1 with the default (identity) shaper curves is bit-identical to warpAmount 0");
        }
    }

    //==========================================================================
    // 10. stereo / mono, determinism, allocations, switching, sweeps
    //==========================================================================
    void testStereoDeterminismAllocations()
    {
        std::printf ("\n[D6] stereo sources, determinism, zero allocations (class level)\n");
        constexpr double fs = kFs;

        auto same = makeStereoCycle (2048, true);
        auto diff = makeStereoCycle (2048, false);
        auto mono = makeCycle (2048, 0.5f);
        auto saw = warpr::SampleData::createDefaultWave();

        int asym = 0, notIndep = 0, nonFinite = 0, monoBad = 0, nondet = 0;

        for (int mode = 44; mode <= 59; ++mode)
            for (int src : { 0, 4 })
            {
                const D6Cfg c1 = cfgOf (mode, 0.6f, 0.5f, 0.5f, src);
                const auto a = renderOsc (*same, 100.0, fs, 2, 4800, d6Span (c1));
                if (! bitEqual (a.L, a.R)) { ++asym; DETAIL ("        L != R for identical channels: mode %d (%s) src %d\n", mode, modeName (mode), src); }

                const D6Cfg c2 = cfgOf (mode, 0.3f, 0.5f, 0.5f, src);
                const auto b = renderOsc (*diff, 100.0, fs, 2, 4800, d6Span (c2));
                if (! allFinite (b.L) || ! allFinite (b.R)) ++nonFinite;
                if (bitEqual (b.L, b.R)) { ++notIndep; DETAIL ("        L == R for different channels: mode %d (%s) src %d\n", mode, modeName (mode), src); }

                D6Cfg c3 = cfgOf (mode, 0.7f, 0.5f, 0.5f, src);
                c3.mix = 0.6f;
                const auto m = renderOsc (*mono, 100.0, fs, 2, 4800, d6Span (c3));
                if (! bitEqual (m.L, m.R)) ++monoBad;

                const D6Cfg c4 = cfgOf (mode, 0.8f, 0.4f, 0.6f, src);
                const auto d1 = renderOsc (*saw, 110.0, fs, 2, 6400, d6Span (c4));
                const auto d2 = renderOsc (*saw, 110.0, fs, 2, 6400, d6Span (c4));
                if (! bitEqual (d1.L, d2.L) || ! bitEqual (d1.R, d2.R)) { ++nondet; DETAIL ("        non-deterministic: mode %d (%s)\n", mode, modeName (mode)); }
            }

        check (asym == 0, "identical L / R channels stay bit-identical through every D6 mode (shared read offset, mid feedback)");
        check (notIndep == 0 && nonFinite == 0, "different channels stay independent and finite");
        check (monoBad == 0, "mono source: L == R exactly through every D6 mode (also with warpMix < 1)");
        check (nondet == 0, "two identical renders are bit-equal for all 16 modes (modulator sources Sine and Sample)");

        // allocations: every D6 mode, switches, ramps (a, ratio, mix), both modulator sources
        warpr::RegionFold rg;
        rg.configure (*saw, 0.0f, 1.0f, warpr::RegionFold::Mode::Forward);
        const double lc = saw->fileSampleRate / rootHz();
        warpr::UnisonOsc osc;
        osc.start (rg, false, 0, lc);
        std::vector<float> l (64), r (64);
        const double rho = 110.0 / rootHz() * saw->fileSampleRate / fs;

        const auto curveSet = warpr::ShaperLutSet::build (warpr::ShaperCurve::identity(), warpr::shaperpresets::generate (1), true);

        std::size_t allocations = 0;
        {
            warpr::tests::ScopedAllocCount counter;
            for (int ci = 0; ci < 1200; ++ci)
            {
                warpr::WarpSpan sp;
                sp.active = true;
                sp.mode = 44 + (ci / 6) % 16;
                sp.a0 = (float) (ci % 13) / 13.0f;  sp.a1 = (float) ((ci + 1) % 13) / 13.0f;
                sp.A0 = sp.A1 = (float) (ci % 7) / 6.0f;
                sp.B0 = sp.B1 = (float) (ci % 5) / 4.0f;
                sp.mix0 = 1.0f;  sp.mix1 = (ci % 5 == 0) ? 0.6f : 1.0f;
                sp.Lc = lc;
                sp.os = 2;
                sp.osRate = fs * 2.0;
                sp.fadeLen = 960;
                sp.modSource = (ci / 40) % 5;
                sp.ratio0 = 1.0f + (float) (ci % 9) * 0.3f;  sp.ratio1 = 1.0f + (float) ((ci + 1) % 9) * 0.3f;
                sp.curve = curveSet.get();
                osc.render (*saw, rg, 0, rho, rho, 2, 1.0f, 1.0f, 1.0f, 1.0f, l.data(), r.data(), 64, &sp);
            }
            allocations = counter.count();
        }
        check (allocations == 0, "UnisonOsc::render with the D6 modes (all 16, source / mode switches, ramps, curve set) performs zero allocations");
    }

    void testSwitchAndSweep()
    {
        std::printf ("\n[D6] mode switching (10 ms cross-fade) and a-sweeps\n");
        constexpr double fs = kFs;
        auto sine = makeCycle (2048, 0.5f);

        const int pairs[][2] = { { 17, 54 }, { 54, 0 }, { 0, 55 }, { 48, 3 }, { 3, 48 }, { 55, 54 }, { 30, 48 }, { 25, 55 } };
        constexpr int totalBase = 9600, switchChunk = 150;
        int stepBad = 0, tailBad = 0;
        double worstStep = 0.0;

        for (const auto& pr : pairs)
        {
            const int from = pr[0], to = pr[1];
            auto sw = [&] (int ci, warpr::WarpSpan& sp)
            {
                sp.active = true;
                sp.mode = ci < switchChunk ? from : to;
                sp.a0 = sp.a1 = 0.35f;
                sp.A0 = sp.A1 = 0.5f;
                sp.B0 = sp.B1 = 0.0f;                      // (B > 0 would add PM self-feedback state that need not converge)
                sp.mix0 = sp.mix1 = 1.0f;
                sp.modSource = 0;
                sp.ratio0 = sp.ratio1 = 1.0f;
                return true;
            };

            const auto run = renderOsc (*sine, 100.0, fs, 2, totalBase, sw);
            const auto pureTo = renderOsc (*sine, 100.0, fs, 2, totalBase, d6Span (cfgOf (to, 0.35f, 0.5f, 0.0f, 0)));
            const double step = maxStepOf (run.L);
            worstStep = std::max (worstStep, step);

            if (step > 0.1) { ++stepBad; DETAIL ("        switch %d -> %d: max step %.4f\n", from, to, step); }

            const size_t tail = run.L.size() * 3 / 4;
            if (nullDb (pureTo.L, run.L, tail) > -100.0)
            {
                ++tailBad;
                DETAIL ("        switch %d -> %d: not converged (null %.1f dB)\n", from, to, nullDb (pureTo.L, run.L, tail));
            }
        }

        std::printf ("        (%d pairs, worst max step %.4f FS)\n", (int) (sizeof (pairs) / sizeof (pairs[0])), worstStep);
        check (stepBad == 0, "mode switch mid-note between D6 / D2 / D3 modes: max step <= 0.1 FS");
        check (tailBad == 0, "after the 10 ms fade the output equals the new mode rendered alone (-100 dB null)");

        int bad = 0;
        for (int mode : { 48, 54, 55 })
        {
            double staticMax = 0.0;
            for (double a : { 0.0, 0.25, 0.5, 0.75, 1.0 })
                staticMax = std::max (staticMax, maxStepOf (renderOsc (*sine, 100.0, fs, 2, 4800, d6Span (cfgOf (mode, (float) a, 0.5f, 0.0f, 0))).L));

            constexpr int chunks = 150;
            auto sweep = [mode] (int ci, warpr::WarpSpan& sp)
            {
                sp.active = true;
                sp.mode = mode;
                sp.a0 = (float) ci / (float) chunks;
                sp.a1 = (float) (ci + 1) / (float) chunks;
                sp.A0 = sp.A1 = 0.5f;
                sp.B0 = sp.B1 = 0.0f;
                sp.mix0 = sp.mix1 = 1.0f;
                sp.modSource = 0;
                sp.ratio0 = sp.ratio1 = 1.0f;
                return true;
            };

            const double sweepMax = maxStepOf (renderOsc (*sine, 100.0, fs, 2, 4800, sweep).L);
            if (sweepMax > 1.5 * staticMax + 0.01)
            {
                ++bad;
                DETAIL ("        zipper: mode %d sweep max step %.4f vs static %.4f\n", mode, sweepMax, staticMax);
            }
        }
        check (bad == 0, "a-sweep 0 -> 1 over 100 ms: max step <= 1.5 x the worst static step (+0.01) for Skew, FM, PM");
    }

    //==========================================================================
    // 11. per-grain
    //==========================================================================
    struct GrainOutD6
    {
        std::vector<float> l, r;
        int maxActive = 0;
    };

    template <class Fn>
    GrainOutD6 renderGrain (const warpr::SampleData& sd, int os, int totalBase, double rho, std::uint32_t seed, Fn&& fn)
    {
        warpr::RegionFold rg;
        rg.configure (sd, 0.0f, 1.0f, warpr::RegionFold::Mode::Forward);

        auto osc = std::make_unique<warpr::UnisonOsc>();
        osc->start (rg, false, 0, 0.0, seed);
        auto scratch = std::make_unique<warpr::GrainScratch>();

        constexpr int chunk = 32;
        const int total = (totalBase / chunk) * chunk;

        GrainOutD6 out;
        out.l.assign (ix (total) * ix (os), 0.0f);
        out.r.assign (ix (total) * ix (os), 0.0f);

        int ci = 0;
        for (int i = 0; i < total; i += chunk, ++ci)
        {
            warpr::GrainSpan gs;
            gs.active = true;
            gs.w0 = gs.w1 = 1.0f;
            gs.sampleRate = kFs;
            gs.hb = &warpr::HalfbandCoeffs::get();
            gs.scratch = scratch.get();
            fn (ci, gs);

            osc->render (sd, rg, 0, rho, rho, os, 1.0f, 1.0f, 1.0f, 1.0f,
                         out.l.data() + ix (i) * ix (os), out.r.data() + ix (i) * ix (os), chunk * os,
                         nullptr, nullptr, &gs);

            out.maxActive = std::max (out.maxActive, osc->grain.activeCount());
        }

        return out;
    }

    void testPerGrain (bool full)
    {
        std::printf ("\n[D6] per-grain phase warps: analytic output of the map modes and FM / PM, identities, all modes x schedulers\n");
        constexpr double amp = 0.8;
        // 256 frames per cycle: a power of two, so the head cycle clocks (increment 1/256) and the playheads accumulate EXACTLY, the
        // wraps land on the exact sample, and every mip level (128 .. 1 frames) has an integer cycle length
        constexpr int cycleFrames = 256;
        const auto cycle = makeCycle (cycleFrames, (float) amp);
        (void) warpr::GrainWindowTable::get();
        (void) warpr::HalfbandCoeffs::get();

        const auto hc = warpr::shaperpresets::generate (1);
        const auto curveSet = warpr::ShaperLutSet::build (warpr::ShaperCurve::identity(), hc, true);

        // ---- exact: a Loop grain (2 s, 0.5 ms ramps, flat top) over a one-cycle sine, rho 1, Lc 200: y = amp * (taps) * window
        {
            struct Case { const char* name; int mode; float a, A, B; int src; bool blep; };
            const Case cases[] = {
                { "Phase Distortion a 0.8",            44, 0.8f, 0.5f, 0.0f, 0, true },
                { "Phase Distortion a 0.5 B 0.1 (res)", 44, 0.5f, 0.5f, 0.1f, 0, true },
                { "Phase Stretch a 0.5 B 0.6",         45, 0.5f, 0.5f, 0.6f, 0, true },
                { "Phase Compression a 0.4 B 0.5",     46, 0.4f, 0.5f, 0.5f, 0, true },
                { "Phase Mirror a 0.5 B 0.3",          47, 0.5f, 0.5f, 0.3f, 0, true },
                { "Phase Skew a 0.6 A 0.3 B 0.4",      48, 0.6f, 0.3f, 0.4f, 0, true },
                { "Phase Quantization a 0.5",          49, 0.5f, 1.0f, 0.0f, 0, true },
                { "Phase Remapping a 0.8 A 0.7",       50, 0.8f, 0.7f, 0.0f, 0, true },
                { "Windowed Sync a 0.4 B 0.3",         51, 0.4f, 0.5f, 0.3f, 0, true },
                { "Hard Sync a 0.4 (BLEP)",            52, 0.4f, 0.5f, 0.0f, 0, true },
                { "Hard Sync a 0.4 (no BLEP)",         52, 0.4f, 0.5f, 0.0f, 0, false },
                { "Soft Sync a 0.5 B 0.2",             53, 0.5f, 0.5f, 0.2f, 0, true },
                // (speeds stay below ~8x: mip levels 0-3 of the 256-frame sample)
                { "FM a 0.1 A 0.5 (sine, ratio 1)",    54, 0.1f, 0.5f, 0.0f, 0, true },
                { "PM a 0.3 A 0.5 (sine, ratio 1)",    55, 0.3f, 0.5f, 0.0f, 0, true }
            };

            for (const auto& cs : cases)
            {
                const auto out = renderGrain (*cycle, 1, 3200, 1.0, 1u, [&cs, &curveSet] (int, warpr::GrainSpan& gs)
                {
                    gs.mode = 1;
                    gs.sizeMs = 2000.0f;
                    gs.shape = 0.0f;
                    gs.warp.active = true;
                    gs.warp.mode = cs.mode;
                    gs.warp.a0 = gs.warp.a1 = cs.a;
                    gs.warp.A0 = gs.warp.A1 = cs.A;
                    gs.warp.B0 = gs.warp.B1 = cs.B;
                    gs.warp.mix0 = gs.warp.mix1 = 1.0f;
                    gs.warp.Lc = (double) cycleFrames;
                    gs.warp.modSource = cs.src;
                    gs.warp.ratio0 = gs.warp.ratio1 = 1.0f;
                    gs.warp.curve = curveSet.get();
                    gs.warp.blep = cs.blep;
                });

                double maxErr = 0.0, peak = 0.0;
                const double dTheta = 1.0 / (double) cycleFrames;

                const bool bridged = warpr::warpModeInfo (cs.mode).seamJump;       // the SeamBridge reshapes the two samples after a wrap

                for (size_t n = 64; n < 3000; ++n)
                {
                    if (bridged && (n % (size_t) cycleFrames) < 2)
                        continue;

                    const int cyc = (int) (n / (size_t) cycleFrames);
                    const double th = (double) (n % (size_t) cycleFrames) / (double) cycleFrames;
                    auto sampleAt = [&] (double x) { return amp * std::sin (2.0 * kPi * x); };
                    double expect;

                    if (cs.mode == 54)
                    {
                        const double I = 4.0 * (double) cs.a * (0.25 + 1.5 * (double) cs.A);
                        expect = sampleAt (th - I * std::cos (2.0 * kPi * th));
                    }
                    else if (cs.mode == 55)
                    {
                        const double I = 2.0 * (double) cs.a * (0.25 + 1.5 * (double) cs.A);
                        expect = sampleAt (th + I * std::sin (2.0 * kPi * th));
                    }
                    else
                    {
                        pw::MapCtx cx;
                        cx.curve = curveSet.get();
                        cx.parity = cyc & 1;
                        cx.blep = cs.blep;
                        const auto po = pw::evalMap (cs.mode, (double) cs.a, (double) cs.A, (double) cs.B, th, dTheta, cx);
                        expect = (double) po.amp * ((double) po.g0 * sampleAt (th + po.d0) + (double) po.g1 * sampleAt (th + po.d1));
                    }

                    maxErr = std::max (maxErr, std::abs ((double) out.l[n] - expect));
                    peak = std::max (peak, std::abs (expect));
                }

                std::printf ("        %-38s max error %.2e (peak %.2f)\n", cs.name, maxErr, peak);
                char label[220];
                std::snprintf (label, sizeof (label), "per-grain '%s': output equals the analytic taps (error %.1e <= 5e-3)", cs.name, maxErr);
                // HAND-GUESSED tolerance (as in D7b): the interpolating sinc8 of a 256-frame sine at fractional positions and sped-up reads
                check (maxErr < 5.0e-3 && peak > 0.2, label);
            }
        }

        // ---- identities: a = 0, mix = 0, amplitude-kind D6 modes (AM / RM are the stage's job)
        {
            const auto sd = makeCycle (480, 0.5f);

            auto render = [&] (auto&& warpFn)
            {
                return renderGrain (*sd, 2, 9600, 1.0, 3u, [&] (int, warpr::GrainSpan& gs)
                {
                    gs.mode = 2;
                    gs.density = 6;
                    gs.sizeMs = 30.0f;
                    gs.variation = 0.3f;
                    gs.spread = 0.3f;
                    gs.position = 0.2f;
                    warpFn (gs.warp);
                });
            };

            const auto base = render ([] (warpr::GrainWarp&) {});
            bool zeroOk = true, mixOk = true, ampOk = true;

            for (int mode : { 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 58, 59 })
            {
                const auto z = render ([mode] (warpr::GrainWarp& w) { w.active = true; w.mode = mode; w.Lc = 480.0; });
                zeroOk = zeroOk && bitEqual (base.l, z.l) && bitEqual (base.r, z.r);
                const auto m0 = render ([mode] (warpr::GrainWarp& w) { w.active = true; w.mode = mode; w.a0 = w.a1 = 0.9f; w.mix0 = w.mix1 = 0.0f; w.Lc = 480.0; });
                mixOk = mixOk && bitEqual (base.l, m0.l) && bitEqual (base.r, m0.r);
            }

            for (int mode : { 56, 57 })
            {
                const auto am = render ([mode] (warpr::GrainWarp& w) { w.active = true; w.mode = mode; w.a0 = w.a1 = 0.9f; w.Lc = 480.0; });
                ampOk = ampOk && bitEqual (base.l, am.l) && bitEqual (base.r, am.r);
            }

            check (zeroOk, "per-grain: warp amount 0 is bit-identical to no warp for all D6 phase modes");
            check (mixOk, "per-grain: warp mix 0 (stage active, a 0.9) is bit-identical to no warp for all D6 phase modes");
            check (ampOk, "per-grain: AM / RM (amplitude kind) are not applied by the grains");

            const auto fm = render ([] (warpr::GrainWarp& w) { w.active = true; w.mode = 54; w.a0 = w.a1 = 0.6f; w.modSource = 4; w.Lc = 480.0; });
            const auto self = render ([] (warpr::GrainWarp& w) { w.active = true; w.mode = 58; w.a0 = w.a1 = 0.8f; w.Lc = 480.0; });
            std::printf ("        (Sample-source FM vs no warp: null %.1f dB | Self FM vs no warp: null %.1f dB)\n", nullDb (base.l, fm.l, 512), nullDb (base.l, self.l, 512));
            check (allFinite (fm.l) && allFinite (self.l) && nullDb (base.l, fm.l, 512) > -20.0 && nullDb (base.l, self.l, 512) > -20.0,
                   "per-grain Sample-source FM and Self FM really change the grain output and stay finite");
        }

        // ---- every D6 phase mode x scheduler x OS x amount x source: finite, bounded, pool respected
        {
            const auto sd = makeCycle (480, 0.6f);
            int bad = 0, combos = 0;
            double worstPeak = 0.0;

            for (int mode : { 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59 })
                for (int sched = 0; sched < 3; ++sched)
                    for (int os : (full ? std::vector<int> { 1, 2, 4, 8 } : std::vector<int> { 1, 2 }))
                        for (float a : { 0.5f, 1.0f })
                            for (int src : { 0, 4 })
                            {
                                const auto out = renderGrain (*sd, os, 9600, 1.0, 13u, [=] (int, warpr::GrainSpan& gs)
                                {
                                    gs.mode = sched;
                                    gs.density = 12;
                                    gs.sizeMs = 35.0f;
                                    gs.variation = 0.4f;
                                    gs.spread = 0.5f;
                                    gs.reverse = 0.3f;
                                    gs.position = 0.25f;
                                    gs.warp.active = true;
                                    gs.warp.mode = mode;
                                    gs.warp.a0 = gs.warp.a1 = a;
                                    gs.warp.A0 = gs.warp.A1 = 0.5f;
                                    gs.warp.B0 = gs.warp.B1 = 0.4f;
                                    gs.warp.mix0 = gs.warp.mix1 = 1.0f;
                                    gs.warp.Lc = 480.0;
                                    gs.warp.modSource = src;
                                    gs.warp.ratio0 = gs.warp.ratio1 = 1.5f;
                                });

                                ++combos;
                                const double pk = std::max (peakOf (out.l), peakOf (out.r));
                                worstPeak = std::max (worstPeak, pk);
                                if (! allFinite (out.l) || ! allFinite (out.r) || pk > 4.0 || out.maxActive > warpr::GrainEngine::poolSize)
                                {
                                    ++bad;
                                    DETAIL ("        bad: mode %d sched %d os %d a %.1f src %d peak %.2f\n", mode, sched, os, (double) a, src, pk);
                                }
                            }

            std::printf ("        (%d combinations, worst peak %.2f)\n", combos, worstPeak);
            check (bad == 0, "per-grain: all 16 D6 modes x Classic/Loop/Cloud x OS x amount x modulator source: finite, |y| <= 4, pool respected");
        }

        // ---- zero allocations with the per-grain D6 kernels and mode / source changes
        {
            const auto sd = makeCycle (480, 0.6f);
            warpr::RegionFold rg;
            rg.configure (*sd, 0.0f, 1.0f, warpr::RegionFold::Mode::Forward);
            auto osc = std::make_unique<warpr::UnisonOsc>();
            auto scratch = std::make_unique<warpr::GrainScratch>();
            osc->start (rg, false, 0, 480.0, 99u);
            juce::Random rnd (31);
            std::array<float, 64> bl {}, br {};
            std::size_t allocs = 0;

            {
                warpr::tests::ScopedAllocCount counter;

                for (int ci = 0; ci < 800; ++ci)
                {
                    warpr::GrainSpan gs;
                    gs.active = true;
                    gs.w0 = gs.w1 = 1.0f;
                    gs.sampleRate = kFs;
                    gs.hb = &warpr::HalfbandCoeffs::get();
                    gs.scratch = scratch.get();
                    gs.mode = rnd.nextInt (3);
                    gs.density = 1 + rnd.nextInt (20);
                    gs.sizeMs = 5.0f + 100.0f * rnd.nextFloat();
                    gs.variation = rnd.nextFloat();
                    gs.spread = rnd.nextFloat();
                    gs.warp.active = true;
                    gs.warp.mode = 44 + rnd.nextInt (16);
                    gs.warp.a0 = rnd.nextFloat();
                    gs.warp.a1 = rnd.nextFloat();
                    gs.warp.A0 = gs.warp.A1 = rnd.nextFloat();
                    gs.warp.B0 = gs.warp.B1 = rnd.nextFloat();
                    gs.warp.mix0 = gs.warp.mix1 = rnd.nextFloat();
                    gs.warp.Lc = 480.0;
                    gs.warp.modSource = rnd.nextInt (5);
                    gs.warp.ratio0 = gs.warp.ratio1 = 0.5f + 3.0f * rnd.nextFloat();
                    gs.warp.curve = curveSet.get();

                    osc->render (*sd, rg, 0, 1.0, 1.0, 2, 1.0f, 1.0f, 1.0f, 1.0f, bl.data(), br.data(), 64, nullptr, nullptr, &gs);
                }

                allocs = counter.count();
            }

            check (allocs == 0, "per-grain D6 kernels (random mode / source / ratio / curve edits, Classic / Loop / Cloud) perform zero allocations");
        }
    }

    //==========================================================================
    // 12. processor level
    //==========================================================================
    int activeVoiceOs (PluginProcessor& p)
    {
        for (int i = 0; i < warpr::WarprSynth::totalSlots; ++i)
        {
            const auto vi = p.getSynth().getVoiceInfo (i);
            if (vi.active && ! vi.ghost)
                return vi.os;
        }
        return 0;
    }

    void testProcessor()
    {
        std::printf ("\n[D6] processor level: all modes through the voice path, minOS, parameter plumbing, allocations, determinism\n");
        constexpr double fs = kFs;

        {
            PluginProcessor proc;
            setParam (proc, "ampSustain", 1.0f);
            setParam (proc, "ampAttack", 1.0f);
            setParam (proc, "warpAmount", 1.0f);

            juce::MidiBuffer midi;
            midi.addEvent (juce::MidiMessage::noteOn (1, 60, 0.9f), 0);

            int dcBad = 0, finiteBad = 0, peakBad = 0;
            double worstDc = 0.0;

            for (float B : { 0.0f, 1.0f })
            {
                setParam (proc, "warpParamB", B);

                for (int mode = 44; mode <= 59; ++mode)
                {
                    setParam (proc, "warpMode", (float) mode);
                    const auto out = warpr::tests::render (proc, midi, fs, 256, 1.0);
                    const int start = (int) (0.6 * fs);

                    for (int ch = 0; ch < 2; ++ch)
                    {
                        const auto s = warpr::tests::scan (out.getReadPointer (ch) + start, out.getNumSamples() - start);
                        worstDc = std::max (worstDc, std::abs (s.mean));
                        if (! s.allFinite) ++finiteBad;
                        if (s.peak > 1.0f) ++peakBad;
                        if (std::abs (s.mean) > 0.02)
                        {
                            ++dcBad;
                            DETAIL ("        DC %.4f: mode %d (%s) B=%.0f ch %d\n", s.mean, mode, modeName (mode), (double) B, ch);
                        }
                    }
                }
            }

            std::printf ("        (worst |DC| %.5f)\n", worstDc);
            check (finiteBad == 0 && peakBad == 0, "full voice path: finite and master output <= 1.0 for all 16 D6 modes (a = 1, B in {0, 1}, default Sample modulator)");
            check (dcBad == 0, "|DC| <= 0.02 after the voice + master DC blockers for all 16 D6 modes");
            proc.releaseResources();
        }

        // minOS enforcement for the new rows
        {
            auto latched = [] (float mode, float amount, float userOsIndex)
            {
                PluginProcessor p;
                setParam (p, "oversampling", userOsIndex);
                setParam (p, "warpMode", mode);
                setParam (p, "warpAmount", amount);
                p.setPlayConfigDetails (0, 2, fs, 256);
                p.prepareToPlay (fs, 256);
                juce::AudioBuffer<float> buf (2, 256);
                juce::MidiBuffer on;
                on.addEvent (juce::MidiMessage::noteOn (1, 60, 0.9f), 0);
                p.processBlock (buf, on);
                const int os = activeVoiceOs (p);
                p.releaseResources();
                return os;
            };

            const int fm = latched (54.0f, 0.5f, 0.0f), hs = latched (52.0f, 0.5f, 0.0f), am = latched (56.0f, 0.5f, 0.0f), off = latched (54.0f, 0.0f, 0.0f);
            std::printf ("        (user 1x: FM -> %dx, Hard Sync -> %dx, AM -> %dx, FM at amount 0 -> %dx)\n", fm, hs, am, off);
            check (fm == 4 && hs == 4 && am == 2 && off == 1, "minOS is enforced for the D6 rows (FM / Hard Sync 4x, AM 2x) only while the warp is audible");
        }

        // parameter plumbing: modOscSource and modOscRatio reach the oscillator
        {
            auto renderFm = [&] (float src, float ratio)
            {
                PluginProcessor p;
                setParam (p, "ampSustain", 1.0f);
                setParam (p, "ampAttack", 1.0f);
                setParam (p, "warpMode", 54.0f);
                setParam (p, "warpAmount", 0.6f);
                setParam (p, "modOscSource", src);
                setParam (p, "modOscRatio", ratio);
                const auto out = warpr::tests::render (p, warpr::tests::singleNote (60, 0.9f, fs, 0.6), fs, 256, 0.5);
                return std::vector<float> (out.getReadPointer (0), out.getReadPointer (0) + out.getNumSamples());
            };

            const auto s0r1 = renderFm (0.0f, 1.0f), s0r3 = renderFm (0.0f, 3.0f), s4r1 = renderFm (4.0f, 1.0f), s0r1b = renderFm (0.0f, 1.0f);
            std::printf ("        (FM: ratio 1 vs 3 null %.1f dB, Sine vs Sample null %.1f dB)\n", nullDb (s0r1, s0r3, 4800), nullDb (s0r1, s4r1, 4800));
            check (nullDb (s0r1, s0r3, 4800) > -30.0 && nullDb (s0r1, s4r1, 4800) > -30.0 && bitEqual (s0r1, s0r1b) && allFinite (s0r3) && allFinite (s4r1),
                   "modOscRatio and modOscSource change the FM sound (> -30 dB null) deterministically");
        }

        // zero allocations in processBlock: Classic and granular, FM / Self FM / Hard Sync / Cross Mod / AM, mode + source edits
        for (int granular = 0; granular < 2; ++granular)
        {
            PluginProcessor p;
            setParam (p, "unisonVoices", 3.0f);
            setParam (p, "oversampling", 1.0f);
            setParam (p, "warpAmount", 0.7f);
            if (granular == 1)
            {
                setParam (p, "playMode", 1.0f);
                setParam (p, "grainMode", 2.0f);
                setParam (p, "grainDensity", 10.0f);
                setParam (p, "grainSize", 40.0f);
            }

            p.setPlayConfigDetails (0, 2, fs, 256);
            p.prepareToPlay (fs, 256);
            juce::AudioBuffer<float> buf (2, 256);
            juce::MidiBuffer on;
            on.addEvent (juce::MidiMessage::noteOn (1, 57, 0.9f), 0);
            on.addEvent (juce::MidiMessage::noteOn (1, 64, 0.9f), 0);
            p.processBlock (buf, on);
            juce::MidiBuffer none;

            std::size_t allocations = 0;
            bool sounding = false;
            const int modes[] = { 54, 58, 52, 59, 56, 44, 50, 53 };

            for (int round = 0; round < 16; ++round)
            {
                setParam (p, "warpMode", (float) modes[round % 8]);
                setParam (p, "modOscSource", (float) (round % 5));
                setParam (p, "modOscRatio", 0.5f + (float) (round % 4));
                setParam (p, "warpParamB", (float) (round % 3) * 0.5f);

                warpr::tests::ScopedAllocCount counter;
                for (int i = 0; i < 6; ++i)
                {
                    p.processBlock (buf, none);
                    sounding = sounding || p.getSynth().getNumActiveVoices() > 0;
                }
                allocations += counter.count();
            }

            char label[200];
            std::snprintf (label, sizeof (label), "processBlock performs zero allocations with the D6 modes (%s playback, mode / source / ratio edits)", granular == 1 ? "granular Cloud" : "Classic");
            check (sounding && allocations == 0, label);
            p.releaseResources();
        }

        // determinism of a session with D6 edits, granular + Classic
        {
            auto run = [&]
            {
                PluginProcessor p;
                setParam (p, "unisonVoices", 2.0f);
                setParam (p, "warpAmount", 0.6f);
                setParam (p, "playMode", 1.0f);
                setParam (p, "grainMode", 2.0f);
                setParam (p, "grainSize", 50.0f);
                p.setPlayConfigDetails (0, 2, fs, 256);
                p.prepareToPlay (fs, 256);

                std::vector<float> out;
                juce::AudioBuffer<float> buf (2, 256);
                juce::MidiBuffer on, none;
                on.addEvent (juce::MidiMessage::noteOn (1, 55, 0.8f), 0);
                const int modes[] = { 54, 58, 52, 59, 55, 47 };

                for (int b = 0; b < 90; ++b)
                {
                    if (b % 15 == 0)
                        setParam (p, "warpMode", (float) modes[(b / 15) % 6]);
                    if (b == 45)
                        setParam (p, "playMode", 0.0f);

                    buf.clear();
                    p.processBlock (buf, b == 0 ? on : none);
                    out.insert (out.end(), buf.getReadPointer (0), buf.getReadPointer (0) + 256);
                }

                p.releaseResources();
                return out;
            };

            const auto a = run(), b = run();
            check (bitEqual (a, b) && allFinite (a) && peakOf (a) > 0.05, "two identical sessions (D6 mode edits, Cloud -> Classic switch) are bit-identical");
        }
    }
}

//==============================================================================
int runPhaseD6Tests (bool full)
{
    failures = 0;
    printedDetails = 0;

    std::printf ("\n=== Phase D6: phase & time, sync, FM family, mod oscillator, speed-aware mips ===\n");

    testModeTable();
    testModulator();
    testPhaseMaps();
    testHardSync();
    testAliasGateA3();
    testUniversal (full);
    testFeedbackStability (full);
    testMipCorrectness();
    testPhaseRemapping();
    testStereoDeterminismAllocations();
    testSwitchAndSweep();
    testPerGrain (full);
    testProcessor();

    return failures;
}

JUCE_END_IGNORE_WARNINGS_GCC_LIKE
