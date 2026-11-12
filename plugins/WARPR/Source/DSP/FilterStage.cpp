#include "DSP/FilterStage.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <cmath>

namespace warpr
{
    namespace
    {
        constexpr double kLn2 = 0.69314718055994531;

        double clamp01 (double x) noexcept { return x < 0.0 ? 0.0 : (x > 1.0 ? 1.0 : x); }

        /** One-pole smoother step that lands exactly on the target (so "bypass when exactly 0" can be decided with ==). */
        void approach (double& value, double target, double k, double tolerance) noexcept
        {
            value += (target - value) * k;

            if (std::abs (target - value) < tolerance)
                value = target;
        }

        /** ln cosh(u), overflow-free:  |u| + ln(1 + e^{-2|u|}) - ln 2. */
        double lnCosh (double u) noexcept
        {
            const double a = std::abs (u);
            return a + std::log1p (std::exp (-2.0 * a)) - kLn2;
        }

        /** Drive stage for one sample: first-order ADAA of  f(x) = tanh(P x) / P  (F = ln cosh(P x) / P^2), blended in by
            `blend`.  The midpoint fallback covers |dx| < 1e-5 where the divided difference loses precision. */
        double drive (double x, double previous, double p, double blend) noexcept
        {
            const double dx = x - previous;
            const double sat = std::abs (dx) < 1.0e-5
                                 ? std::tanh (p * 0.5 * (x + previous)) / p
                                 : (lnCosh (p * x) - lnCosh (p * previous)) / (p * p * dx);
            return x + (sat - x) * blend;
        }
    }

    //==========================================================================
    void FilterCore::process (int ch, const double* in, double* out, int n) noexcept
    {
        const auto c = static_cast<std::size_t> (ch);
        const int ns = design.numSections;

        for (int i = 0; i < n; ++i)
        {
            double x = in[i];

            for (int s = 0; s < ns; ++s)
            {
                const FilterSection& sc = design.sec[static_cast<std::size_t> (s)];
                double& st1 = ic1[c][static_cast<std::size_t> (s)];

                if (sc.kind == FilterSection::Kind::Svf)
                {
                    double& st2 = ic2[c][static_cast<std::size_t> (s)];
                    const double v3 = x - st2;
                    const double v1 = sc.a1 * st1 + sc.a2 * v3;
                    const double v2 = st2 + sc.a2 * st1 + sc.a3 * v3;
                    st1 = 2.0 * v1 - st1;
                    st2 = 2.0 * v2 - st2;
                    x = sc.m0 * x + sc.m1 * v1 + sc.m2 * v2;
                }
                else if (sc.kind == FilterSection::Kind::OnePole)
                {
                    const double v = (x - st1) * sc.a1;
                    const double lp = v + st1;
                    st1 = lp + v;
                    x = sc.m0 * x + sc.m2 * lp;
                }
            }

            out[i] = x * design.gain;
        }
    }

    //==========================================================================
    void FilterStage::prepare (double newSampleRate) noexcept
    {
        sampleRate = newSampleRate;
        fadeTotal = primeSamples + std::max (1, static_cast<int> (std::lround (0.010 * newSampleRate)));
        reset();
    }

    void FilterStage::reset() noexcept
    {
        for (auto& c : cores)
            c.reset();

        adaaPrev.fill (0.0);
        fade.reset();
        coreType = { { -1, -1 } };
        current = 0;
        parked = true;
    }

    void FilterStage::adoptSilently (int type) noexcept
    {
        coreType[static_cast<std::size_t> (current)] = type;
        fade.reset();
        fade.request (type, 1);                       // first request adopts the index without a fade
    }

    void FilterStage::snap (const FilterTargets& t) noexcept
    {
        const int type = std::clamp (t.type, 0, kNumFilterTypes - 1);

        cutLogCur = std::log (filterdesign::clampCutoff (static_cast<double> (t.cutoffHz), sampleRate));
        resCur = clamp01 (static_cast<double> (t.res));
        driveCur = std::min (24.0, std::max (0.0, static_cast<double> (t.driveDb)));
        morphCur = clamp01 (static_cast<double> (t.morph));
        mixCur = clamp01 (static_cast<double> (t.mix));

        const bool wanted = t.enable && ! filterdesign::isOpenLowpass (type, static_cast<double> (t.cutoffHz),
                                                                      static_cast<double> (t.res),
                                                                      static_cast<double> (t.driveDb));
        enCur = wanted ? 1.0 : 0.0;

        for (auto& c : cores)
            c.reset();

        adaaPrev.fill (0.0);
        current = 0;
        coreType = { { type, type } };
        adoptSilently (type);
        parked = false;
    }

    //==========================================================================
    void FilterStage::process (float* left, float* right, int n, const FilterTargets& t) noexcept
    {
        if (n <= 0)
            return;

        const int type = std::clamp (t.type, 0, kNumFilterTypes - 1);
        const double nD = static_cast<double> (n);
        const double k15 = 1.0 - std::exp (-nD / (0.015 * sampleRate));
        const double k10 = 1.0 - std::exp (-nD / (0.010 * sampleRate));
        const double enStep = nD / (0.005 * sampleRate);

        // ---- smoothers: values at the start of the chunk, then advance to the end of the chunk
        const double cutLog0 = cutLogCur, res0 = resCur, drive0 = driveCur, morph0 = morphCur;
        const double mix0 = mixCur * enCur;

        approach (cutLogCur, std::log (filterdesign::clampCutoff (static_cast<double> (t.cutoffHz), sampleRate)), k15, 1.0e-9);
        approach (resCur, clamp01 (static_cast<double> (t.res)), k10, 1.0e-9);
        approach (driveCur, std::min (24.0, std::max (0.0, static_cast<double> (t.driveDb))), k10, 1.0e-9);
        approach (morphCur, clamp01 (static_cast<double> (t.morph)), k10, 1.0e-9);
        approach (mixCur, clamp01 (static_cast<double> (t.mix)), k10, 1.0e-9);

        const bool wanted = t.enable && ! filterdesign::isOpenLowpass (type, static_cast<double> (t.cutoffHz),
                                                                      static_cast<double> (t.res),
                                                                      static_cast<double> (t.driveDb));
        enCur = wanted ? std::min (1.0, enCur + enStep) : std::max (0.0, enCur - enStep);
        const double mix1 = mixCur * enCur;

        // ---- bypass: untouched samples, parked state
        if (juce::exactlyEqual (mix0, 0.0) && juce::exactlyEqual (mix1, 0.0))
        {
            if (! parked)
            {
                for (auto& c : cores)
                    c.reset();

                adaaPrev.fill (0.0);
                parked = true;
            }

            adoptSilently (type);
            return;
        }

        parked = false;

        // ---- type switch: the running core becomes the outgoing one, the other core starts fresh
        const bool wasEmpty = fade.current() < 0;

        if (fade.request (type, fadeTotal))
        {
            current ^= 1;
            cores[static_cast<std::size_t> (current)].reset();
            coreType[static_cast<std::size_t> (current)] = type;
        }
        else if (wasEmpty)
        {
            cores[static_cast<std::size_t> (current)].reset();
            coreType[static_cast<std::size_t> (current)] = type;
        }

        const std::size_t curIdx = static_cast<std::size_t> (current);
        const std::size_t oldIdx = curIdx ^ 1u;
        const double fadeTotalD = static_cast<double> (fadeTotal);
        const double rampD = static_cast<double> (fadeTotal - primeSamples);

        // ---- sub-blocks
        bool bad = false;
        int done = 0;

        while (done < n)
        {
            const int m = std::min (subBlock, n - done);
            const double tMid = (static_cast<double> (done) + 0.5 * static_cast<double> (m)) / nD;
            const double res = res0 + (resCur - res0) * tMid;
            const double morph = morph0 + (morphCur - morph0) * tMid;
            const double driveDb = drive0 + (driveCur - drive0) * tMid;
            const double fc = std::exp (cutLog0 + (cutLogCur - cutLog0) * tMid);

            // per-sample fade weight (incoming core) and wet/dry mix; shared by both channels
            const bool fading = fade.fading();
            double wIn[subBlock] = {};
            double mixW[subBlock] = {};

            for (int i = 0; i < m; ++i)
            {
                if (fading && fade.fading())
                {
                    const double elapsed = static_cast<double> (fade.advance()) * fadeTotalD;
                    wIn[i] = std::min (1.0, std::max (0.0, (elapsed - static_cast<double> (primeSamples)) / rampD));
                }
                else
                    wIn[i] = 1.0;

                mixW[i] = mix0 + (mix1 - mix0) * (static_cast<double> (done + i + 1) / nD);
            }

            filterdesign::make (coreType[curIdx], fc, res, morph, sampleRate, scratchCurrent);
            cores[curIdx].setDesign (scratchCurrent);

            if (fading)
            {
                filterdesign::make (coreType[oldIdx], fc, res, morph, sampleRate, scratchOutgoing);
                cores[oldIdx].setDesign (scratchOutgoing);
            }

            const bool driveOn = ! (juce::exactlyEqual (drive0, 0.0) && juce::exactlyEqual (driveCur, 0.0));
            const double p = std::pow (10.0, driveDb / 20.0);
            const double blend = std::min (1.0, driveDb);

            for (int ch = 0; ch < 2; ++ch)
            {
                float* data = (ch == 0 ? left : right) + done;
                double dry[subBlock] = {}, in[subBlock] = {}, wetCur[subBlock] = {}, wetOld[subBlock] = {};

                for (int i = 0; i < m; ++i)
                    dry[i] = static_cast<double> (data[i]);

                if (driveOn)
                {
                    double prev = adaaPrev[static_cast<std::size_t> (ch)];

                    for (int i = 0; i < m; ++i)
                    {
                        in[i] = drive (dry[i], prev, p, blend);
                        prev = dry[i];
                    }

                    adaaPrev[static_cast<std::size_t> (ch)] = prev;
                }
                else
                {
                    for (int i = 0; i < m; ++i)
                        in[i] = dry[i];

                    adaaPrev[static_cast<std::size_t> (ch)] = dry[m - 1];
                }

                cores[curIdx].process (ch, in, wetCur, m);

                if (fading)
                    cores[oldIdx].process (ch, in, wetOld, m);

                for (int i = 0; i < m; ++i)
                {
                    const double wet = fading ? wetOld[i] + (wetCur[i] - wetOld[i]) * wIn[i] : wetCur[i];
                    const double y = mixW[i] >= 1.0 ? wet : dry[i] + (wet - dry[i]) * mixW[i];

                    if (std::abs (y) < 1.0e6)
                        data[i] = static_cast<float> (y);
                    else
                    {
                        data[i] = static_cast<float> (dry[i]);        // also catches NaN (the comparison is false)
                        bad = true;
                    }
                }
            }

            if (bad)
            {
                for (auto& c : cores)
                    c.reset();

                adaaPrev.fill (0.0);
                bad = false;
            }

            done += m;
        }
    }
}
