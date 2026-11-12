#pragma once
/*
  VizAnalyzer - audio-thread producer of WarprVizFrame (architecture C15 + "warprViz frame producers").

  Fed with the MASTER OUTPUT (after master gain / safety clip; recorded as ambiguity A10) once per
  processBlock.  Everything is per-sample one-pole / biquad work with fixed state: no allocation,
  no locks, no libm in the sample loop.  A frame is pushed into the SPSC WarprVizFifo at most once
  per block and at most ~30 times per second (architecture: once per ~33 ms; the editor timer runs at
  30 Hz and reads the newest frame).  push() is wait-free and drops the frame when the UI is behind.

  Frame fields (all 0..1 unless noted):
    low, high   mid signal band-passed (one-pole LP @ 200 Hz / 2nd-order Butterworth HP @ 4 kHz), rectified,
                follower (attack 5 ms, release 120 ms), normalised by a running maximum (8 s decay) with a
                floor of 0.02 (-34 dB) so quiet material does not get inflated; exactly 0 below 1e-3.
    rms         300 ms RMS follower of sqrt((L^2 + R^2) / 2); exactly 0 below -90 dBFS (organism is static).
    transient, tseq
                HP @ 2 kHz |x| fast envelope (1 ms) against a slow adaptive envelope (50 ms):
                hit when fast > 3 slow + 0.003, refractory 60 ms, re-armed once fast < 1.5 slow + 0.003.
                tseq++ per hit; `transient` = strength of the strongest hit in this frame period, else 0.
    tang        spectral-centroid surrogate: zero-crossing rate of the HP @ 500 Hz mid signal over 20 ms
                windows -> log map 200 Hz .. 12 kHz -> 0 .. 2 pi (0 when silent).
    dom         dominant-frequency surrogate: zero-crossing rate of the DC-blocked (HP @ 10 Hz) raw mid
                signal over 50 ms windows -> log map 40 Hz .. 8 kHz -> 0..1.  For tonal material this
                tracks the strongest partial; the UI turns it into the organism's cymatic lobe count.
    sharp       spectral sharpness: high / (low + high) of the normalised band envelopes (0 when silent).
    theta, r    Mid / Side energies over the frame period:  theta = atan2(rmsSide, rmsMid) in [0, pi/2],
                r = clamp(sqrt(meanM^2 + meanS^2) * sqrt(2), 0, 1)  (a full-scale mono sine -> 1).
    peakL/R     max |sample| since the previous PUSH (post master gain), the quantity the meters show.
    voices      sounding voices' playhead position / envelope level from the synth's wait-free snapshot.
    lfoPhase    all -1 until the LFO system exists (D8).

  Idle suppression: when a frame is entirely zero (no signal, no voices, no transient) and the previous
  pushed frame was also entirely zero, nothing is pushed - an idle, silent session emits nothing.
*/

#include "DSP/VizFifo.h"
#include "DSP/WarprSynth.h"

#include <algorithm>
#include <cmath>

namespace warpr
{
    class VizAnalyzer
    {
    public:
        static constexpr int pushHz = 30;

        void prepare (double sampleRate) noexcept
        {
            fs = sampleRate > 1000.0 ? sampleRate : 44100.0;

            auto onePole = [this] (double hz) { return static_cast<float> (1.0 - std::exp (-6.283185307179586 * hz / fs)); };
            auto timeConst = [this] (double seconds) { return static_cast<float> (1.0 - std::exp (-1.0 / (seconds * fs))); };

            aLow = onePole (200.0);
            a2k = onePole (2000.0);
            a500 = onePole (500.0);
            kAtt = timeConst (0.005);
            kRel = timeConst (0.120);
            kRms = timeConst (0.300);
            kFast = timeConst (0.001);
            kSlow = timeConst (0.050);
            maxDecay = static_cast<float> (std::exp (-1.0 / (8.0 * fs)));

            // 2nd-order Butterworth high-pass @ 4 kHz (RBJ cookbook, Q = 1/sqrt(2))
            const double w0 = 6.283185307179586 * 4000.0 / fs;
            const double cw = std::cos (w0), alpha = std::sin (w0) / (2.0 * 0.7071067811865476);
            const double a0 = 1.0 + alpha;
            hb0 = static_cast<float> (0.5 * (1.0 + cw) / a0);
            hb1 = static_cast<float> (-(1.0 + cw) / a0);
            hb2 = hb0;
            ha1 = static_cast<float> (-2.0 * cw / a0);
            ha2 = static_cast<float> ((1.0 - alpha) / a0);

            aDc = onePole (10.0);

            interval = std::max (1, static_cast<int> (std::lround (fs / static_cast<double> (pushHz))));
            refractorySamples = static_cast<int> (std::lround (0.060 * fs));
            zcrWindow = std::max (1, static_cast<int> (std::lround (0.020 * fs)));
            zcrDomWindow = std::max (1, static_cast<int> (std::lround (0.050 * fs)));

            reset();
        }

        void reset() noexcept
        {
            lowLp = lowEnv = lowMax = 0.0f;
            highEnv = highMax = 0.0f;
            hz1 = hz2 = 0.0f;
            msAcc = 0.0f;
            t2Lp = fastEnv = slowEnv = 0.0f;
            refractory = 0;
            armed = true;
            z500Lp = 0.0f;
            zPos = false;
            zc = zcrCount = zcrLatched = 0;
            dcLp = 0.0f;
            zDomPos = false;
            zDom = zDomCount = zDomLatched = 0;
            tseq = 0;
            frameTransient = 0.0f;
            peakL = peakR = 0.0f;
            eM = eS = 0.0;
            accCount = 0;
            countdown = interval;
            lastPushedIdle = false;
        }

        /** AUDIO THREAD.  `synth` may be null (no voices in the frame). */
        void process (const float* l, const float* r, int n, WarprVizFifo& fifo, const WarprSynth* synth) noexcept
        {
            for (int i = 0; i < n; ++i)
            {
                const float xl = l[i], xr = r[i];
                const float m = 0.5f * (xl + xr), s = 0.5f * (xl - xr);

                peakL = std::max (peakL, std::abs (xl));
                peakR = std::max (peakR, std::abs (xr));
                eM += static_cast<double> (m) * static_cast<double> (m);
                eS += static_cast<double> (s) * static_cast<double> (s);

                // low band
                lowLp += aLow * (m - lowLp);
                follow (lowEnv, std::abs (lowLp));
                lowMax = std::max (lowMax * maxDecay, lowEnv);

                // high band
                const float yh = hb0 * m + hz1;
                hz1 = hb1 * m - ha1 * yh + hz2;
                hz2 = hb2 * m - ha2 * yh;
                follow (highEnv, std::abs (yh));
                highMax = std::max (highMax * maxDecay, highEnv);

                // rms
                msAcc += kRms * (0.5f * (xl * xl + xr * xr) - msAcc);

                // transient detector
                t2Lp += a2k * (m - t2Lp);
                const float h2 = std::abs (m - t2Lp);
                fastEnv += kFast * (h2 - fastEnv);
                slowEnv += kSlow * (h2 - slowEnv);

                if (refractory > 0)
                    --refractory;

                if (! armed && fastEnv < 1.5f * slowEnv + 0.003f)
                    armed = true;

                if (armed && refractory == 0 && fastEnv > 3.0f * slowEnv + 0.003f)
                {
                    ++tseq;
                    refractory = refractorySamples;
                    armed = false;
                    const float strength = std::min (1.0f, std::max (0.05f, (fastEnv / (slowEnv + 0.003f)) * 0.05f));
                    frameTransient = std::max (frameTransient, strength);
                }

                // zero-crossing rate of the 500 Hz high-passed mid signal
                z500Lp += a500 * (m - z500Lp);
                const float h5 = m - z500Lp;

                if ((h5 > 1.0e-4f && ! zPos) || (h5 < -1.0e-4f && zPos))
                {
                    ++zc;
                    zPos = h5 > 0.0f;
                }

                if (++zcrCount >= zcrWindow)
                {
                    zcrLatched = zc;
                    zc = 0;
                    zcrCount = 0;
                }

                // zero-crossing rate of the DC-blocked raw mid signal (dominant frequency; the longer
                // 50 ms window keeps ~10 Hz resolution down to low fundamentals)
                dcLp += aDc * (m - dcLp);
                const float hd = m - dcLp;

                if ((hd > 1.0e-4f && ! zDomPos) || (hd < -1.0e-4f && zDomPos))
                {
                    ++zDom;
                    zDomPos = hd > 0.0f;
                }

                if (++zDomCount >= zcrDomWindow)
                {
                    zDomLatched = zDom;
                    zDom = 0;
                    zDomCount = 0;
                }
            }

            accCount += n;
            countdown -= n;

            if (countdown > 0)
                return;

            countdown += interval;
            if (countdown <= 0)
                countdown = interval;

            push (fifo, synth);
        }

        uint32_t getTransientCount() const noexcept { return tseq; }

    private:
        void follow (float& env, float x) const noexcept
        {
            env += (x > env ? kAtt : kRel) * (x - env);
        }

        void push (WarprVizFifo& fifo, const WarprSynth* synth) noexcept
        {
            WarprVizFrame f;

            auto norm = [] (float env, float runMax)
            {
                const float v = std::min (1.0f, env / std::max (runMax, 0.02f));
                return v < 1.0e-3f ? 0.0f : v;
            };

            f.low = norm (lowEnv, lowMax);
            f.high = norm (highEnv, highMax);

            const float rmsNow = std::sqrt (std::max (0.0f, msAcc));
            f.rms = rmsNow < 3.1623e-5f ? 0.0f : std::min (1.0f, rmsNow);

            const double cnt = static_cast<double> (std::max (1, accCount));
            const double em = eM / cnt, es = eS / cnt;

            f.theta = (em + es) > 1.0e-12 ? static_cast<float> (std::atan2 (std::sqrt (es), std::sqrt (em))) : 0.0f;
            f.r = static_cast<float> (std::min (1.0, std::sqrt (em + es) * 1.4142135623730951));

            if (f.rms > 0.0f && zcrLatched > 0)
            {
                const double hz = static_cast<double> (zcrLatched) * 0.5 / 0.020;
                const double t = std::log (std::max (200.0, std::min (12000.0, hz)) / 200.0) / std::log (60.0);
                f.tang = static_cast<float> (6.283185307179586 * t);
            }

            if (f.rms > 0.0f && zDomLatched > 0)
            {
                const double hz = static_cast<double> (zDomLatched) * 0.5 / 0.050;
                f.dom = static_cast<float> (std::log (std::max (40.0, std::min (8000.0, hz)) / 40.0) / std::log (200.0));
            }

            if (f.low + f.high > 0.0f)
                f.sharp = f.high / (f.low + f.high);

            f.tseq = tseq;
            f.transient = frameTransient;
            f.peakL = peakL;
            f.peakR = peakR;

            if (synth != nullptr)
            {
                float pos[WarprVizFrame::maxVoices], lvl[WarprVizFrame::maxVoices];
                const int nv = synth->readPlayheads (pos, lvl, WarprVizFrame::maxVoices);

                for (int i = 0; i < nv; ++i)
                    f.voices[static_cast<size_t> (i)] = { pos[i], lvl[i] };

                f.numVoices = nv;
            }

            // every compared quantity is non-negative, so <= 0 means "exactly zero"
            const bool idle = f.rms <= 0.0f && f.peakL <= 0.0f && f.peakR <= 0.0f && f.numVoices == 0
                              && f.low <= 0.0f && f.high <= 0.0f && f.transient <= 0.0f;

            if (! (idle && lastPushedIdle))
                fifo.push (f);

            lastPushedIdle = idle;

            peakL = peakR = 0.0f;
            eM = eS = 0.0;
            accCount = 0;
            frameTransient = 0.0f;
        }

        double fs = 44100.0;
        float aLow = 0.0f, a2k = 0.0f, a500 = 0.0f;
        float kAtt = 0.0f, kRel = 0.0f, kRms = 0.0f, kFast = 0.0f, kSlow = 0.0f, maxDecay = 1.0f;
        float hb0 = 0.0f, hb1 = 0.0f, hb2 = 0.0f, ha1 = 0.0f, ha2 = 0.0f;
        int interval = 1470, refractorySamples = 2880, zcrWindow = 960;

        float lowLp = 0.0f, lowEnv = 0.0f, lowMax = 0.0f;
        float highEnv = 0.0f, highMax = 0.0f, hz1 = 0.0f, hz2 = 0.0f;
        float msAcc = 0.0f;
        float t2Lp = 0.0f, fastEnv = 0.0f, slowEnv = 0.0f;
        int refractory = 0;
        bool armed = true;
        float z500Lp = 0.0f;
        bool zPos = false;
        int zc = 0, zcrCount = 0, zcrLatched = 0;
        float aDc = 0.0f, dcLp = 0.0f;
        bool zDomPos = false;
        int zDom = 0, zDomCount = 0, zDomLatched = 0, zcrDomWindow = 2205;
        uint32_t tseq = 0;
        float frameTransient = 0.0f;
        float peakL = 0.0f, peakR = 0.0f;
        double eM = 0.0, eS = 0.0;
        int accCount = 0;
        int countdown = 1470;
        bool lastPushedIdle = false;
    };
}
