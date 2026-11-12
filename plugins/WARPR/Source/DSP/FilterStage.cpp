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
    FilterCore::FilterCore()
    {
        hb.load();            // designed once (thread-safe static); construction is never on the audio thread
    }

    void FilterCore::prepare (double sampleRate)
    {
        hb.load();
        const auto length = static_cast<std::size_t> (filterdesign::combBufferLength (sampleRate));

        for (auto& l : line)
            if (l.size() != length)
                l.assign (length, 0.0);

        lineDirty = false;
        reset();
    }

    void FilterCore::reset() noexcept
    {
        for (auto& ch : ic1)
            ch.fill (0.0);
        for (auto& ch : ic2)
            ch.fill (0.0);

        fbState.fill (0.0);
        lpState.fill (0.0);
        writePos.fill (0);
        haveDelay = false;

        for (auto& s : nls)
            s.reset();
        for (auto& s : hbUp)
            s.reset();
        for (auto& s : hbDown)
            s.reset();

        if (lineDirty)
        {
            for (auto& l : line)
                std::fill (l.begin(), l.end(), 0.0);

            lineDirty = false;
        }
    }

    void FilterCore::setDesign (const FilterDesign& d) noexcept
    {
        design.topo = d.topo;
        design.numSections = d.numSections;
        design.gain = d.gain;
        design.fb = d.fb;
        design.outDry = d.outDry;
        design.outWet = d.outWet;
        design.comb = d.comb;
        design.nl = d.nl;

        for (int i = 0; i < d.numSections; ++i)
            design.sec[static_cast<std::size_t> (i)] = d.sec[static_cast<std::size_t> (i)];

        if (d.topo == FilterDesign::Topo::Comb)
        {
            delayStart = haveDelay ? delayEnd : d.comb.delay;
            delayEnd = d.comb.delay;
            haveDelay = true;
        }
        else
            haveDelay = false;
    }

    double FilterCore::runChain (std::size_t c, double x) noexcept
    {
        const int ns = design.numSections;

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

        return x;
    }

    void FilterCore::process (int ch, const double* in, double* out, int n) noexcept
    {
        const auto c = static_cast<std::size_t> (ch);

        if (design.topo == FilterDesign::Topo::Nonlinear)
        {
            processNonlinear (c, in, out, n);
            return;
        }

        // ---- comb: delay line + loop one-pole + (linear below 6) soft limiter
        if (design.topo == FilterDesign::Topo::Comb)
        {
            std::vector<double>& ln = line[c];
            const int len = static_cast<int> (ln.size());

            if (len < 16)                                          // never prepared: pass through
            {
                for (int i = 0; i < n; ++i)
                    out[i] = in[i];
                return;
            }

            double* buf = ln.data();
            const CombSpec& cs = design.comb;
            const double maxDelay = static_cast<double> (len - 6);
            const double invN = 1.0 / static_cast<double> (n);
            int w = writePos[c];
            double lp = lpState[c];
            lineDirty = true;

            for (int i = 0; i < n; ++i)
            {
                const double delay = std::min (std::max (delayStart + (delayEnd - delayStart) * (static_cast<double> (i + 1) * invN), 2.0), maxDelay);
                int base = 0;
                double lc[4];
                filterdesign::lagrange3 (delay, base, lc);

                // y[n - k] lives at (w - k) mod len;  base >= 2 so the youngest tap is y[n-1]
                int i0 = w - (base - 1), i1 = w - base, i2 = w - base - 1, i3 = w - base - 2;
                i0 += i0 < 0 ? len : 0;
                i1 += i1 < 0 ? len : 0;
                i2 += i2 < 0 ? len : 0;
                i3 += i3 < 0 ? len : 0;
                const double yd = lc[0] * buf[i0] + lc[1] * buf[i1] + lc[2] * buf[i2] + lc[3] * buf[i3];

                lp += cs.a * (yd - lp);

                const double mag = std::abs (lp);
                constexpr double T = filterdesign::kCombSoftLimit, R = 1.5;
                const double fbv = mag <= T ? lp : std::copysign (T + R * std::tanh ((mag - T) / R), lp);

                const double y = cs.c * in[i] + cs.sign * cs.g * fbv;
                buf[w] = y;
                w = w + 1 == len ? 0 : w + 1;
                out[i] = y;
            }

            writePos[c] = w;
            lpState[c] = lp;
            return;
        }

        // ---- parallel bank
        if (design.topo == FilterDesign::Topo::Parallel)
        {
            const int ns = design.numSections;

            for (int i = 0; i < n; ++i)
            {
                const double x = in[i];
                double sum = 0.0;

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
                        sum += sc.m0 * x + sc.m1 * v1 + sc.m2 * v2;
                    }
                    else if (sc.kind == FilterSection::Kind::OnePole)
                    {
                        const double v = (x - st1) * sc.a1;
                        const double lp = v + st1;
                        st1 = lp + v;
                        sum += sc.m0 * x + sc.m2 * lp;
                    }
                }

                out[i] = sum * design.gain;
            }

            return;
        }

        // ---- series chain with the phaser's feedback / dry sum
        if (! (juce::exactlyEqual (design.fb, 0.0) && juce::exactlyEqual (design.outDry, 0.0) && juce::exactlyEqual (design.outWet, 1.0)))
        {
            double fbPrev = fbState[c];

            for (int i = 0; i < n; ++i)
            {
                const double x = in[i];
                const double chain = runChain (c, x + design.fb * fbPrev);
                fbPrev = chain;
                out[i] = design.gain * (design.outDry * x + design.outWet * chain);
            }

            fbState[c] = fbPrev;
            return;
        }

        // ---- plain series chain (D5a types, serial all-pass)
        for (int i = 0; i < n; ++i)
            out[i] = runChain (c, in[i]) * design.gain;
    }

    //==========================================================================
    // D5c: nonlinear kernels.  Each runs in place on `n` samples at the CORE rate (2 fs behind the local half-band pair,
    // or fs for the S&H), per channel, in double.
    //==========================================================================
    void FilterCore::processNonlinear (std::size_t c, const double* in, double* out, int n) noexcept
    {
        constexpr int chunk = 64;                       // base samples per pass; 2 * chunk core samples on the stack
        double buf[2 * chunk];
        const bool os = design.nl.os2x;

        for (int done = 0; done < n; done += chunk)
        {
            const int m = std::min (chunk, n - done);

            if (os)
            {
                nonlin::up2 (hb, hbUp[c], in + done, buf, m);
                runKernel (c, buf, 2 * m);
                nonlin::down2 (hb, hbDown[c], buf, out + done, m);
            }
            else
            {
                for (int i = 0; i < m; ++i)
                    buf[i] = in[done + i];

                runKernel (c, buf, m);

                for (int i = 0; i < m; ++i)
                    out[done + i] = buf[i];
            }
        }
    }

    void FilterCore::runKernel (std::size_t c, double* buf, int n) noexcept
    {
        const NonlinearSpec& nl = design.nl;
        NlState& st = nls[c];

        switch (nl.kind)
        {
            //------------------------------------------------------------------ ladders (28 / 29 / 30)
            case NonlinearSpec::Kind::Ladder:
            {
                // y_i = G v_i + (1 - G) s_i with v_1 = sat (L u) / L, v_i = sat (L y_{i-1}) / L and u = comp x - k y4.
                // The saturators are replaced by their SECANT gains m_i = sat (b) / b (taken from the previous sample's
                // operating point) to solve the zero-delay loop in closed form:  y4 = A u + B,  u = comp x - k y4.  The
                // forward pass then uses the exact saturators and refreshes the secants, so state and outputs are exact
                // (bounded: every v_i is in [-1/L, 1/L]); only the loop solve lags by one sample.
                const double bigG = nl.bigG, omG = 1.0 - bigG, k = nl.k, comp = nl.comp, loop = nl.loopScale, invLoop = 1.0 / loop;
                const double g4 = bigG * bigG * bigG * bigG;
                const double tb = nl.tapBlend;
                const bool hp = nl.highpass;
                double s0 = st.s[0], s1 = st.s[1], s2 = st.s[2], s3 = st.s[3];
                double m0 = st.m[0], m1 = st.m[1], m2 = st.m[2], m3 = st.m[3];

                for (int i = 0; i < n; ++i)
                {
                    const double xin = comp * buf[i];
                    const double f1 = omG * s0, f2 = omG * s1, f3 = omG * s2, f4 = omG * s3;
                    const double a = g4 * m0 * m1 * m2 * m3;
                    const double b = f4 + bigG * m3 * (f3 + bigG * m2 * (f2 + bigG * m1 * f1));
                    const double y4p = (a * xin + b) / (1.0 + k * a);
                    const double u = xin - k * y4p;

                    double arg = loop * u;
                    const double y1 = bigG * (nonlin::sat (arg) * invLoop) + f1;
                    m0 = nonlin::secant (arg);

                    arg = loop * y1;
                    const double y2 = bigG * (nonlin::sat (arg) * invLoop) + f2;
                    m1 = nonlin::secant (arg);

                    arg = loop * y2;
                    const double y3 = bigG * (nonlin::sat (arg) * invLoop) + f3;
                    m2 = nonlin::secant (arg);

                    arg = loop * y3;
                    const double y4 = bigG * (nonlin::sat (arg) * invLoop) + f4;
                    m3 = nonlin::secant (arg);

                    s0 = 2.0 * y1 - s0;
                    s1 = 2.0 * y2 - s1;
                    s2 = 2.0 * y3 - s2;
                    s3 = 2.0 * y4 - s3;

                    if (hp)
                        buf[i] = (1.0 - tb) * (u - 2.0 * y1 + y2) + tb * (u - 4.0 * y1 + 6.0 * y2 - 4.0 * y3 + y4);
                    else
                        buf[i] = (1.0 - tb) * y2 + tb * y4;
                }

                st.s = { { s0, s1, s2, s3 } };
                st.m = { { m0, m1, m2, m3 } };
                break;
            }

            //------------------------------------------------------------------ Scream (31)
            case NonlinearSpec::Kind::Scream:
            {
                // TPT SVF band-pass (peak gain 1) fed by  clipAsym (inGain x) + fbAmount * clipAsym (y[n-1]).  The clipper
                // has slope <= 1 and the band-pass |H| <= 1, so the loop gain is <= fbAmount < 1 (small-gain theorem):
                // bounded for every input, Q and delay.  The band-pass has no DC gain, so the clip's DC offset never reaches
                // the output.
                const FilterSection& sc = design.sec[0];
                const double inGain = nl.inGain, fbAmount = nl.fbAmount;
                double zc1 = st.ic1, zc2 = st.ic2, fb = st.fb;

                for (int i = 0; i < n; ++i)
                {
                    const double u = nonlin::clipAsym (inGain * buf[i]) + fbAmount * fb;
                    const double v3 = u - zc2;
                    const double v1 = sc.a1 * zc1 + sc.a2 * v3;
                    const double v2 = zc2 + sc.a2 * zc1 + sc.a3 * v3;
                    zc1 = 2.0 * v1 - zc1;
                    zc2 = 2.0 * v2 - zc2;

                    const double y = sc.m1 * v1;
                    fb = nonlin::clipAsym (y);
                    buf[i] = y;
                }

                st.ic1 = zc1;
                st.ic2 = zc2;
                st.fb = fb;
                break;
            }

            //------------------------------------------------------------------ Sample & hold (32), base rate
            case NonlinearSpec::Kind::SampleHold:
            {
                const double ga = design.sec[0].a1, gb = design.sec[1].a1;
                const double step = nl.holdStep, interp = nl.interp;
                double s0 = st.s[0], s1 = st.s[1];
                double phase = st.phase, held = st.held, prev = st.prev;
                bool primed = st.primed;

                for (int i = 0; i < n; ++i)
                {
                    const double va = (buf[i] - s0) * ga;
                    const double la = va + s0;
                    s0 = la + va;
                    const double vb = (la - s1) * gb;
                    const double lb = vb + s1;
                    s1 = lb + vb;

                    phase += step;

                    if (phase >= 1.0)
                    {
                        phase -= std::floor (phase);
                        prev = primed ? held : lb;
                        held = lb;
                        primed = true;
                    }

                    const double ramp = prev + (held - prev) * phase;       // continuous at every strobe
                    buf[i] = held + interp * (ramp - held);
                }

                st.s[0] = s0;
                st.s[1] = s1;
                st.phase = phase;
                st.held = held;
                st.prev = prev;
                st.primed = primed;
                break;
            }

            //------------------------------------------------------------------ Ring mod (33)
            case NonlinearSpec::Kind::RingMod:
            {
                const double step = nl.carrierStep, morph = nl.carrierMorph, bigG = nl.smoothG;
                const double dc = 0.5 * (1.0 - morph), amp = 0.5 * (1.0 + morph);
                constexpr double twoPi = 6.283185307179586476925286766559;
                double s0 = st.s[0], s1 = st.s[1], phase = st.phase;

                for (int i = 0; i < n; ++i)
                {
                    phase += step;
                    phase -= std::floor (phase);

                    const double p = buf[i] * (dc + amp * std::sin (twoPi * phase));
                    const double va = (p - s0) * bigG;           // product LP: two TPT one-poles (G = 1: transparent)
                    const double la = va + s0;
                    s0 = la + va;
                    const double vb = (la - s1) * bigG;
                    const double lb = vb + s1;
                    s1 = lb + vb;
                    buf[i] = lb;
                }

                st.s[0] = s0;
                st.s[1] = s1;
                st.phase = phase;
                break;
            }

            case NonlinearSpec::Kind::None:
                break;
        }
    }

    //==========================================================================
    void FilterStage::prepare (double newSampleRate) noexcept
    {
        sampleRate = newSampleRate;
        fadeTotal = primeSamples + std::max (1, static_cast<int> (std::lround (0.010 * newSampleRate)));

        for (auto& c : cores)
            c.prepare (newSampleRate);                // comb delay lines: the only allocation of the filter, done here

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

            filterdesign::make (coreType[curIdx], fc, res, morph, sampleRate, scratchCurrent, driveDb);
            cores[curIdx].setDesign (scratchCurrent);

            if (fading)
            {
                filterdesign::make (coreType[oldIdx], fc, res, morph, sampleRate, scratchOutgoing, driveDb);
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
