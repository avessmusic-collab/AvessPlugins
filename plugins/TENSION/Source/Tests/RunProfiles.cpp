/*
    TENSION_Profiles - Phase 3.5 offline metric harness (console target, like DROP_Profiles).

    Build:  cmake --build build --target TENSION_Profiles
    Run:    ./build/plugins/TENSION/TENSION_Profiles [--quick] [--comp] [--out <dir>]
              --quick   only the 4 and 8 bar ramps (default: 4 / 8 / 16 / 32 bars)
              --comp    enable the optional wet-bus level compensator for every run
              --out     base directory (default "logs/TENSION", relative to the working directory)
    Output: <base>/metrics_<YYYYmmdd_HHMMSS>/summary.txt   (readable report; also echoed to stdout)
            <base>/metrics_<...>/ramp_<mode>_<bars>bar_<pink|silence>[_comp].csv   (one row / 100 ms)
    Exit code 0 when every automatic criterion passes, 1 otherwise (the files are always written).

    What is rendered (all 48 kHz, host blocks of 480 samples = 10 ms, 120 BPM 4/4, sync on, MIX 100 %,
    output 0 dB, engine t = LINEAR ramp 0 -> 1 over 4 / 8 / 16 / 32 bars = 8 / 16 / 32 / 64 s):
      (a) -18 dBFS RMS stereo pink noise (mid-heavy: L/R share 70 % of the power, i.e. correlation 0.7,
          roughly what a mixed stereo source looks like; pure decorrelated pink would inflate the width stage)
      (b) silence (the noise/riser generator on its own)
    for all three rise modes.

    Per 100 ms window (4800 samples) the CSV has:
      t           ramp position at the window centre
      in_dB       input RMS (both channels), dBFS
      wet_dB      WET-BUS RMS from the engine telemetry (pre-MIX, pre-OutputSafety, i.e. what the wet
                  chain delivers; dry*(1-e) + proc*e + noise + delay + reverb), dBFS
      out_dB      engine output RMS (post safety knee), dBFS
      wet_rel_dB  wet_dB minus the long-run input RMS of the run (silence runs: minus the nominal -18 dBFS)
      centroid_Hz spectral centroid of the output mid ((L+R)/2), 4096-pt Hann FFT at the window start, 20 Hz..20 kHz
      side_mid_dB 10*log10( sum (L-R)^2 / sum (L+R)^2 ) of the output over the window (stereo width proxy)
      noise_tgt_dB mapping noise level target (dBFS, floored at -100)
    Loudness proxy = plain (unweighted) RMS in dBFS; no A-weighting (documented choice: the questions asked
    are relative - "does the wet bus grow by more than X dB / jump by more than Y dB" - and the pink
    reference has a fixed spectrum so weighting would only add a constant per spectrum).

    Automatic criteria (per run; "pink" runs unless noted):
      LOUD   wet-bus RMS at the end of the ramp (max of the 400 ms smoothed level for t >= 0.95) must be
             <= +6.0 dB over the input RMS                                           (every mode, every duration)
      STEP   no 100 ms step > 3 dB in the 400 ms smoothed wet level while audible (> -70 dBFS)   (pink + silence)
      MONO   smoothed (1 s) wet level never drops > 2 dB (Aggressive: 2.5 dB, tuning log 3.5.4) below its
             running maximum   (monotone intensity)
      DEAD   no stretch longer than 15 % of the ramp where NONE of {wet level, centroid, width, noise target}
             moves (>= 0.3 dB / 0.03 oct / 0.3 dB / 0.5 dB over a 3 % lag)
    Reverb section: impulse through ReverbEngine with the mapping targets of each mode at t = 0.5 / 0.8 / 1.0:
      RT60 (Schroeder backward integral, T20 x 3) and the tail "ringing index" versus a decaying-white-noise
      baseline (see ReverbEngine.h, PHASE 3.5 VERDICT) -> Freeverb GO / NO-GO.
*/
#include "DSP/TensionEngine.h"
#include "DSP/ReverbEngine.h"
#include "DSP/TensionMapping.h"

#include <juce_dsp/juce_dsp.h>
#include <juce_events/juce_events.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <string>
#include <vector>

namespace
{
    constexpr double kFs        = 48000.0;
    constexpr int    kBlock     = 480;                       // 10 ms host block
    constexpr int    kWin       = 4800;                      // 100 ms metric window
    constexpr int    kBlocksPerWin = kWin / kBlock;
    constexpr double kRefDb     = -18.0;                     // pink reference RMS
    constexpr double kLoudLimitDb = 6.0;
    constexpr double kStepLimitDb = 3.0;
    constexpr double kMonoLimitDb = 2.0;
    // Aggressive only: a mid-build RMS dip of up to 2.5 dB is the intended "tension through
    // subtraction" character (measured 2.0 .. 2.3 dB; see the tuning log 3.5.4 in TensionMapping.h).
    constexpr double kMonoLimitAggressiveDb = 2.5;
    constexpr double kDeadLimitPct = 15.0;
    constexpr double kAudibleDb = -70.0;
    constexpr double kPi        = 3.14159265358979323846;

    const char* const kModeName[3] = { "SlowBurn", "Cinematic", "Aggressive" };

    // ---------------------------------------------------------------- report plumbing
    std::string g_report;

    void out (const char* fmt, ...)
    {
        char buf[2048];
        va_list ap;
        va_start (ap, fmt);
        std::vsnprintf (buf, sizeof buf, fmt, ap);
        va_end (ap);
        std::fputs (buf, stdout);
        g_report += buf;
    }

    double pdb (double meanSquare) { return 10.0 * std::log10 (meanSquare + 1.0e-20); }

    // ---------------------------------------------------------------- signals
    struct Xor32
    {
        std::uint32_t s;
        float white() noexcept   // [-1, 1)
        {
            s ^= s << 13; s ^= s >> 17; s ^= s << 5;
            return (float) (s & 0xFFFFFFu) / 8388608.0f - 1.0f;
        }
    };

    struct PinkGen   // Paul Kellett refined filter
    {
        Xor32 r;
        float b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0;

        float next() noexcept
        {
            const float w = r.white();
            b0 = 0.99886f * b0 + w * 0.0555179f;
            b1 = 0.99332f * b1 + w * 0.0750759f;
            b2 = 0.96900f * b2 + w * 0.1538520f;
            b3 = 0.86650f * b3 + w * 0.3104856f;
            b4 = 0.55000f * b4 + w * 0.5329522f;
            b5 = -0.7616f * b5 - w * 0.0168980f;
            const float p = b0 + b1 + b2 + b3 + b4 + b5 + b6 + w * 0.5362f;
            b6 = w * 0.115926f;
            return p * 0.11f;
        }
    };

    struct Stereo
    {
        std::vector<float> l, r;
    };

    // Mid-heavy stereo pink at exactly kRefDb RMS per channel (whole buffer).
    Stereo makePinkReference (int n)
    {
        PinkGen common { { 0x1234567u }, 0, 0, 0, 0, 0, 0, 0 };
        PinkGen indL   { { 0x2345678u }, 0, 0, 0, 0, 0, 0, 0 };
        PinkGen indR   { { 0x3456789u }, 0, 0, 0, 0, 0, 0, 0 };

        const float a = std::sqrt (0.7f), b = std::sqrt (0.3f);
        Stereo s;
        s.l.resize ((size_t) n);
        s.r.resize ((size_t) n);

        for (int i = 0; i < n; ++i)
        {
            const float c = common.next();
            s.l[(size_t) i] = a * c + b * indL.next();
            s.r[(size_t) i] = a * c + b * indR.next();
        }

        for (auto* ch : { &s.l, &s.r })
        {
            double sq = 0.0;
            for (float v : *ch) sq += (double) v * v;
            const double rms = std::sqrt (sq / (double) n);
            const float g = (float) (std::pow (10.0, kRefDb / 20.0) / std::max (rms, 1.0e-12));
            for (float& v : *ch) v *= g;
        }

        return s;
    }

    // ---------------------------------------------------------------- analysis helpers
    // Spectral centroid of (l + r) / 2 over 4096 samples (Hann), 20 Hz .. 20 kHz. 0 when silent.
    double centroidHz (const float* l, const float* r)
    {
        constexpr int N = 4096;
        static juce::dsp::FFT fft (12);
        static std::vector<float> data (2 * N);

        for (int i = 0; i < N; ++i)
        {
            const double w = 0.5 - 0.5 * std::cos (2.0 * kPi * (double) i / (double) N);
            data[(size_t) i] = (float) (0.5 * ((double) l[i] + (double) r[i]) * w);
        }
        std::fill (data.begin() + N, data.end(), 0.0f);

        fft.performFrequencyOnlyForwardTransform (data.data());

        double num = 0.0, den = 0.0;
        for (int k = 1; k <= N / 2; ++k)
        {
            const double f = (double) k * kFs / (double) N;
            if (f < 20.0 || f > 20000.0) continue;
            const double p = (double) data[(size_t) k] * (double) data[(size_t) k];
            num += f * p;
            den += p;
        }
        return den > 1.0e-18 ? num / den : 0.0;
    }

    // Trailing power average of a dB series.
    std::vector<double> trailingPowerAvgDb (const std::vector<double>& db, int span)
    {
        std::vector<double> o (db.size());
        for (size_t i = 0; i < db.size(); ++i)
        {
            const size_t from = (i + 1 > (size_t) span) ? i + 1 - (size_t) span : 0;
            double s = 0.0;
            for (size_t k = from; k <= i; ++k) s += std::pow (10.0, db[k] / 10.0);
            o[i] = pdb (s / (double) (i - from + 1));
        }
        return o;
    }

    std::vector<double> trailingAvg (const std::vector<double>& x, int span)
    {
        std::vector<double> o (x.size());
        for (size_t i = 0; i < x.size(); ++i)
        {
            const size_t from = (i + 1 > (size_t) span) ? i + 1 - (size_t) span : 0;
            double s = 0.0;
            for (size_t k = from; k <= i; ++k) s += x[k];
            o[i] = s / (double) (i - from + 1);
        }
        return o;
    }

    // ---------------------------------------------------------------- ramp rendering
    struct Win
    {
        double t = 0, inDb = 0, wetDb = 0, outDb = 0, cen = 0, sideMidDb = 0, noiseTgtDb = -100;
    };

    struct Run
    {
        int  mode = 1, bars = 8;
        bool pink = true, comp = false;
        double inRefDb = kRefDb;          // long-run input RMS (dBFS)
        std::vector<Win> w;
    };

    Run renderRamp (int mode, int bars, bool pink, bool comp, const Stereo& ref)
    {
        Run run;
        run.mode = mode; run.bars = bars; run.pink = pink; run.comp = comp;

        const int N    = bars * 2 * (int) kFs;               // 4 beats at 120 BPM = 2 s per bar
        const int nWin = N / kWin;

        if (pink)
        {
            double sq = 0.0;
            for (int i = 0; i < N; ++i)
                sq += 0.5 * ((double) ref.l[(size_t) i] * ref.l[(size_t) i] + (double) ref.r[(size_t) i] * ref.r[(size_t) i]);
            run.inRefDb = pdb (sq / (double) N);
        }

        TensionEngine eng;
        eng.prepare (kFs, kBlock, 2);
        eng.setLevelCompensatorEnabled (comp);

        juce::AudioBuffer<float> buf (2, kBlock);
        std::vector<float> oL ((size_t) kWin), oR ((size_t) kWin), iL ((size_t) kWin), iR ((size_t) kWin);

        run.w.reserve ((size_t) nWin);

        for (int wi = 0; wi < nWin; ++wi)
        {
            double wetSq = 0.0, lastNoiseDb = -100.0;

            for (int b = 0; b < kBlocksPerWin; ++b)
            {
                const int pos = wi * kWin + b * kBlock;
                const double t = std::min (1.0, (double) pos / (double) (N - kBlock));

                for (int i = 0; i < kBlock; ++i)
                {
                    const float xl = pink ? ref.l[(size_t) (pos + i)] : 0.0f;
                    const float xr = pink ? ref.r[(size_t) (pos + i)] : 0.0f;
                    buf.setSample (0, i, xl);
                    buf.setSample (1, i, xr);
                    iL[(size_t) (b * kBlock + i)] = xl;
                    iR[(size_t) (b * kBlock + i)] = xr;
                }

                EngineParams p;
                p.tension = (float) t; p.mix = 1.0f; p.outputDb = 0.0f; p.sync = true; p.riseMode = mode;
                eng.process (buf, p, HostTransport {});

                for (int i = 0; i < kBlock; ++i)
                {
                    oL[(size_t) (b * kBlock + i)] = buf.getSample (0, i);
                    oR[(size_t) (b * kBlock + i)] = buf.getSample (1, i);
                }

                const double wr = (double) eng.getTelemetry().wetRms.load();
                wetSq += wr * wr;
                lastNoiseDb = 20.0 * std::log10 (std::max ((double) eng.getLastTargets().noiseGain, 1.0e-5));
            }

            double inSq = 0.0, outSq = 0.0, sSq = 0.0, mSq = 0.0;
            for (int i = 0; i < kWin; ++i)
            {
                inSq  += 0.5 * ((double) iL[(size_t) i] * iL[(size_t) i] + (double) iR[(size_t) i] * iR[(size_t) i]);
                outSq += 0.5 * ((double) oL[(size_t) i] * oL[(size_t) i] + (double) oR[(size_t) i] * oR[(size_t) i]);
                const double s = (double) oL[(size_t) i] - (double) oR[(size_t) i];
                const double m = (double) oL[(size_t) i] + (double) oR[(size_t) i];
                sSq += s * s;
                mSq += m * m;
            }

            Win w;
            w.t          = ((double) wi * kWin + 0.5 * kWin) / (double) N;
            w.inDb       = pdb (inSq / kWin);
            w.wetDb      = pdb (wetSq / kBlocksPerWin);
            w.outDb      = pdb (outSq / kWin);
            w.cen        = centroidHz (oL.data(), oR.data());
            w.sideMidDb  = (sSq + mSq) > 1.0e-18 ? 10.0 * std::log10 ((sSq + 1.0e-20) / (mSq + 1.0e-20)) : 0.0;
            w.noiseTgtDb = lastNoiseDb;
            run.w.push_back (w);
        }

        return run;
    }

    // ---------------------------------------------------------------- run statistics
    struct Stats
    {
        double wetEndRel = 0, maxStep = 0, stepAtT = 0, monoViol = 0, deadPct = 0, deadAtT = 0;
        double cen0 = 0, cen50 = 0, cen100 = 0, wid0 = 0, wid50 = 0, wid100 = 0, out50 = -200, wet50 = -200;
        bool loudOk = true, stepOk = true, monoOk = true, deadOk = true;
    };

    double atFrac (const std::vector<double>& v, double f)
    {
        if (v.empty()) return 0.0;
        const size_t i = (size_t) std::lround (f * (double) (v.size() - 1));
        return v[std::min (i, v.size() - 1)];
    }

    Stats analyse (const Run& run)
    {
        Stats st;
        const size_t n = run.w.size();
        if (n < 20) return st;

        std::vector<double> wet (n), cenLog (n), wid (n), nz (n), outDb (n);
        for (size_t i = 0; i < n; ++i)
        {
            wet[i]    = run.w[i].wetDb;
            cenLog[i] = std::log2 (std::max (run.w[i].cen, 20.0));
            wid[i]    = run.w[i].sideMidDb;
            nz[i]     = run.w[i].noiseTgtDb;
            outDb[i]  = run.w[i].outDb;
        }

        const auto sm4 = trailingPowerAvgDb (wet, 4);        // 400 ms
        const auto sm10 = trailingPowerAvgDb (wet, 10);      // 1 s

        // LOUD: end of ramp relative to the input
        double endMax = -300.0;
        for (size_t i = 0; i < n; ++i)
            if (run.w[i].t >= 0.95) endMax = std::max (endMax, sm4[i]);
        st.wetEndRel = endMax - run.inRefDb;
        st.loudOk = ! run.pink || st.wetEndRel <= kLoudLimitDb;

        // STEP: 100 ms steps of the 400 ms smoothed level while audible
        for (size_t i = 5; i < n; ++i)
            if (sm4[i - 1] > kAudibleDb)
            {
                const double d = std::abs (sm4[i] - sm4[i - 1]);
                if (d > st.maxStep) { st.maxStep = d; st.stepAtT = run.w[i].t; }
            }
        st.stepOk = st.maxStep <= kStepLimitDb;

        // MONO: smoothed level never falls far below its running maximum
        double runMax = -300.0;
        for (size_t i = 10; i < n; ++i)
        {
            runMax = std::max (runMax, sm10[i]);
            st.monoViol = std::max (st.monoViol, runMax - sm10[i]);
        }
        st.monoOk = st.monoViol <= (run.mode == 2 ? kMonoLimitAggressiveDb : kMonoLimitDb);

        // DEAD: longest stretch where nothing moves
        const auto cenS = trailingAvg (cenLog, 5);
        const auto widS = trailingAvg (wid, 5);
        const size_t lag = (size_t) std::max (5L, std::lround (0.03 * (double) n));
        size_t bestLen = 0, bestStart = 0, curLen = 0, curStart = 0;

        for (size_t i = 0; i + lag < n; ++i)
        {
            const bool moved = std::abs (sm4[i + lag] - sm4[i]) >= 0.3
                            || std::abs (cenS[i + lag] - cenS[i]) >= 0.03
                            || std::abs (widS[i + lag] - widS[i]) >= 0.3
                            || std::abs (nz[i + lag] - nz[i]) >= 0.5;
            if (! moved)
            {
                if (curLen == 0) curStart = i;
                ++curLen;
                if (curLen > bestLen) { bestLen = curLen; bestStart = curStart; }
            }
            else
            {
                curLen = 0;
            }
        }
        st.deadPct = bestLen > 0 ? 100.0 * (double) (bestLen + lag) / (double) n : 0.0;
        st.deadAtT = run.w[bestStart].t;
        st.deadOk = ! run.pink || st.deadPct <= kDeadLimitPct;

        st.cen0 = atFrac (std::vector<double> (cenS), 0.02);
        st.cen50 = atFrac (cenS, 0.5);
        st.cen100 = atFrac (cenS, 1.0);
        st.cen0 = std::pow (2.0, st.cen0); st.cen50 = std::pow (2.0, st.cen50); st.cen100 = std::pow (2.0, st.cen100);
        st.wid0 = atFrac (widS, 0.02); st.wid50 = atFrac (widS, 0.5); st.wid100 = atFrac (widS, 1.0);
        st.out50 = atFrac (outDb, 0.5);
        st.wet50 = atFrac (sm4, 0.5);
        return st;
    }

    void writeCsv (const std::string& dir, const Run& run)
    {
        char name[160];
        std::snprintf (name, sizeof name, "ramp_%s_%dbar_%s%s.csv", kModeName[run.mode], run.bars, run.pink ? "pink" : "silence", run.comp ? "_comp" : "");
        std::ofstream f (dir + "/" + name);
        f << "t,in_dB,wet_dB,out_dB,wet_rel_dB,centroid_Hz,side_mid_dB,noise_tgt_dB\n";

        char row[256];
        for (const auto& w : run.w)
        {
            std::snprintf (row, sizeof row, "%.4f,%.2f,%.2f,%.2f,%.2f,%.0f,%.2f,%.2f\n",
                           w.t, w.inDb, w.wetDb, w.outDb, w.wetDb - run.inRefDb, w.cen, w.sideMidDb, w.noiseTgtDb);
            f << row;
        }
    }

    // ---------------------------------------------------------------- reverb tail analysis
    // Ringing index: std-dev (dB) of the 16-bin-smoothed spectrum after dividing out a ~1/3 octave
    // envelope, 200 Hz .. 5 kHz. 32768-pt Hann FFT.
    double ringingIndex (const std::vector<float>& seg)
    {
        constexpr int N = 32768;
        static juce::dsp::FFT fft (15);
        std::vector<float> data (2 * (size_t) N, 0.0f);

        for (int i = 0; i < N; ++i)
        {
            const double w = 0.5 - 0.5 * std::cos (2.0 * kPi * (double) i / (double) N);
            data[(size_t) i] = (float) ((double) seg[(size_t) i] * w);
        }

        fft.performFrequencyOnlyForwardTransform (data.data());

        const int half = N / 2;
        std::vector<double> pre ((size_t) half + 2, 0.0);          // prefix sums of the power
        for (int k = 0; k <= half; ++k)
            pre[(size_t) k + 1] = pre[(size_t) k] + (double) data[(size_t) k] * (double) data[(size_t) k];

        const double binHz = kFs / (double) N;
        const int k0 = (int) std::lround (200.0 / binHz), k1 = (int) std::lround (5000.0 / binHz);

        std::vector<double> ratio;
        ratio.reserve ((size_t) (k1 - k0 + 1));
        for (int k = k0; k <= k1; ++k)
        {
            const int h = std::max (24, (int) std::lround (0.1 * k));
            const int a = std::max (1, k - h), b = std::min (half, k + h);
            const double env = (pre[(size_t) b + 1] - pre[(size_t) a]) / (double) (b - a + 1);
            const double p = (double) data[(size_t) k] * (double) data[(size_t) k];
            ratio.push_back (p / std::max (env, 1.0e-30));
        }

        std::vector<double> gdb;
        for (size_t j = 0; j + 16 <= ratio.size(); j += 16)
        {
            double s = 0.0;
            for (size_t q = 0; q < 16; ++q) s += ratio[j + q];
            gdb.push_back (10.0 * std::log10 (s / 16.0 + 1.0e-20));
        }

        double mean = 0.0;
        for (double v : gdb) mean += v;
        mean /= (double) gdb.size();
        double var = 0.0;
        for (double v : gdb) var += (v - mean) * (v - mean);
        return std::sqrt (var / (double) gdb.size());
    }

    struct ReverbPoint
    {
        double rt60 = -1.0, ring = 0.0, base = 0.0;
        bool   tailOk = true;
    };

    ReverbPoint analyseReverb (const TensionDSP::TensionTargets& tg)
    {
        ReverbPoint rp;

        ReverbEngine rv;
        rv.prepare (kFs);

        ReverbEngine::Targets t;
        t.sendGain = 1.0f; t.returnGain = 1.0f;
        t.roomSize = tg.reverbRoom; t.damping = tg.reverbDamping; t.width = tg.reverbWidth;
        t.preDelayMs = tg.reverbPreDelayMs; t.returnLpfHz = tg.reverbReturnLpfHz; t.sendHpfHz = tg.reverbSendHpfHz;

        const int n = 14 * (int) kFs;                       // multiple of 32
        std::vector<float> inL ((size_t) n, 0.0f), inR ((size_t) n, 0.0f), oL ((size_t) n), oR ((size_t) n);
        inL[0] = inR[0] = 1.0f;

        for (int pos = 0; pos < n; pos += 32)
        {
            rv.setTargets (t, pos == 0, 32);
            rv.process (inL.data() + pos, inR.data() + pos, oL.data() + pos, oR.data() + pos, 32);
        }

        // Schroeder backward integration -> T20 x 3
        std::vector<double> bi ((size_t) n + 1, 0.0);
        for (int i = n - 1; i >= 0; --i)
            bi[(size_t) i] = bi[(size_t) i + 1] + (double) oL[(size_t) i] * oL[(size_t) i] + (double) oR[(size_t) i] * oR[(size_t) i];

        int i5 = -1, i25 = -1;
        for (int i = 0; i < n; ++i)
        {
            const double d = 10.0 * std::log10 (bi[(size_t) i] / std::max (bi[0], 1.0e-30) + 1.0e-30);
            if (i5 < 0 && d <= -5.0) i5 = i;
            if (i25 < 0 && d <= -25.0) { i25 = i; break; }
        }
        if (i5 >= 0 && i25 > i5)
            rp.rt60 = 3.0 * (double) (i25 - i5) / kFs;

        // Tail segment 0.3 s .. 0.98 s
        const int start = (int) (0.3 * kFs), N = 32768;
        std::vector<float> seg (oL.begin() + start, oL.begin() + start + N);
        double e = 0.0;
        for (float v : seg) e += (double) v * v;
        rp.tailOk = e > 1.0e-14;

        if (rp.tailOk)
        {
            rp.ring = ringingIndex (seg);

            // baseline: white noise with the same exponential decay
            const double decaySec = rp.rt60 > 0.0 ? rp.rt60 : 5.0;
            Xor32 rng { 0x9E3779B9u };
            std::vector<float> base ((size_t) N);
            for (int i = 0; i < N; ++i)
                base[(size_t) i] = rng.white() * (float) std::exp (-6.907755 * (double) i / (decaySec * kFs));
            rp.base = ringingIndex (base);
        }
        return rp;
    }
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    bool quick = false, comp = false;
    std::string baseDir = "logs/TENSION";

    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        if (a == "--quick") quick = true;
        else if (a == "--comp") comp = true;
        else if (a == "--out" && i + 1 < argc) baseDir = argv[++i];
    }

    std::time_t now = std::time (nullptr);
    std::tm tmv {};
#ifdef _WIN32
    localtime_s (&tmv, &now);
#else
    localtime_r (&now, &tmv);
#endif
    char ts[32];
    std::strftime (ts, sizeof ts, "%Y%m%d_%H%M%S", &tmv);

    const std::string dir = baseDir + "/metrics_" + ts;
    juce::File::getCurrentWorkingDirectory().getChildFile (juce::String (dir)).createDirectory();

    std::vector<int> barList = quick ? std::vector<int> { 4, 8 } : std::vector<int> { 4, 8, 16, 32 };
    const int maxBars = barList.back();

    out ("TENSION metric harness (Phase 3.5)   %s\n", ts);
    out ("48 kHz, 120 BPM 4/4, sync on, MIX 100%%, output 0 dB, linear t ramp over %d..%d bars, compensator %s\n",
         barList.front(), maxBars, comp ? "ON" : "OFF");
    out ("pink reference: stereo, correlation 0.7, %.0f dBFS RMS per channel; silence runs: noise generator alone\n", kRefDb);
    out ("loudness = plain RMS (dBFS) of the WET BUS (engine telemetry, pre-MIX / pre-safety); rel = wet - input RMS\n");
    out ("limits: LOUD <= +%.1f dB at t=1 | STEP <= %.1f dB/100ms (400 ms smoothed) | MONO drop <= %.1f dB (1 s) | DEAD <= %.0f%% of ramp\n\n",
         kLoudLimitDb, kStepLimitDb, kMonoLimitDb, kDeadLimitPct);

    const Stereo ref = makePinkReference (maxBars * 2 * (int) kFs);

    std::vector<Run> runs;
    std::vector<Stats> stats;
    int failures = 0;

    // ------------------------------------------------------------------ one line per run
    out ("== RUN SUMMARY ==\n");
    out ("%-10s %4s %-7s | %7s %7s %6s %6s | %9s %9s %9s | %6s %6s %6s | %s\n",
         "mode", "bars", "input", "end_rel", "maxStep", "@t", "monoDr", "cen0", "cen50", "cen100", "wid0", "wid50", "wid100", "dead% (@t)  verdict");

    for (int mode = 0; mode < 3; ++mode)
        for (int bars : barList)
            for (int pk = 1; pk >= 0; --pk)
            {
                runs.push_back (renderRamp (mode, bars, pk == 1, comp, ref));
                const Run& r = runs.back();
                stats.push_back (analyse (r));
                const Stats& s = stats.back();
                writeCsv (dir, r);

                std::string bad;
                if (! s.loudOk) bad += " LOUD";
                if (! s.stepOk) bad += " STEP";
                if (! s.monoOk) bad += " MONO";
                if (! s.deadOk) bad += " DEAD";
                if (! bad.empty()) ++failures;

                out ("%-10s %4d %-7s | %+7.1f %7.2f %6.2f %6.2f | %9.0f %9.0f %9.0f | %6.1f %6.1f %6.1f | %5.1f (%.2f)  %s%s\n",
                     kModeName[mode], bars, pk == 1 ? "pink" : "silence", s.wetEndRel, s.maxStep, s.stepAtT, s.monoViol,
                     s.cen0, s.cen50, s.cen100, s.wid0, s.wid50, s.wid100, s.deadPct, s.deadAtT,
                     bad.empty() ? "ok" : "FAIL:", bad.c_str());
            }

    out ("(end_rel for silence runs is the absolute-vs-(-18) figure and is not a criterion; DEAD / MONO / LOUD are judged on pink runs)\n");

    // ------------------------------------------------------------------ detail tables, 8-bar ramp
    out ("\n== 8-BAR DETAIL (t = ramp position) ==\n");
    for (int mode = 0; mode < 3; ++mode)
    {
        const Run* pinkRun = nullptr;
        const Run* silRun  = nullptr;
        for (const auto& r : runs)
            if (r.mode == mode && r.bars == 8 && ! r.comp)
                (r.pink ? pinkRun : silRun) = &r;

        if (pinkRun == nullptr || silRun == nullptr) continue;

        out ("%s\n", kModeName[mode]);
        out ("   t   | pink: wet_rel dB  centroid Hz  side/mid dB  noiseTgt dBFS | silence: out dBFS  centroid Hz  side/mid dB\n");
        for (int k = 0; k <= 10; ++k)
        {
            const size_t ip = std::min (pinkRun->w.size() - 1, (size_t) std::lround ((double) k / 10.0 * (double) (pinkRun->w.size() - 1)));
            const size_t is = std::min (silRun->w.size() - 1, (size_t) std::lround ((double) k / 10.0 * (double) (silRun->w.size() - 1)));
            const Win& a = pinkRun->w[ip];
            const Win& b = silRun->w[is];
            out (" %4.2f  |       %+7.1f   %9.0f   %9.1f   %9.1f   |        %8.1f   %9.0f   %9.1f\n",
                 (double) k / 10.0, a.wetDb - pinkRun->inRefDb, a.cen, a.sideMidDb, a.noiseTgtDb, b.outDb, b.cen, b.sideMidDb);
        }
    }

    // Noise participation (the known Cinematic back-loading issue)
    out ("\n== NOISE PARTICIPATION (silence input, noise generator alone, 8-bar ramp, output dBFS at t) ==\n");
    out ("%-10s %8s %8s %8s %8s %8s\n", "mode", "t=0.2", "t=0.3", "t=0.5", "t=0.75", "t=1.0");
    for (int mode = 0; mode < 3; ++mode)
        for (const auto& r : runs)
            if (r.mode == mode && r.bars == 8 && ! r.pink && ! r.comp)
            {
                auto at = [&] (double f) { return r.w[std::min (r.w.size() - 1, (size_t) std::lround (f * (double) (r.w.size() - 1)))].outDb; };
                out ("%-10s %8.1f %8.1f %8.1f %8.1f %8.1f\n", kModeName[mode], at (0.2), at (0.3), at (0.5), at (0.75), at (1.0));
            }

    // ------------------------------------------------------------------ compensator comparison
    if (! comp)
    {
        out ("\n== LEVEL COMPENSATOR (8-bar pink, ON) vs raw ==\n");
        out ("%-10s %12s %12s %10s %10s\n", "mode", "end_rel raw", "end_rel ON", "maxStep ON", "monoDr ON");
        for (int mode = 0; mode < 3; ++mode)
        {
            const Run on = renderRamp (mode, 8, true, true, ref);
            const Stats so = analyse (on);
            writeCsv (dir, on);

            double raw = 0.0;
            for (size_t i = 0; i < runs.size(); ++i)
                if (runs[i].mode == mode && runs[i].bars == 8 && runs[i].pink && ! runs[i].comp)
                    raw = stats[i].wetEndRel;

            out ("%-10s %+12.1f %+12.1f %10.2f %10.2f\n", kModeName[mode], raw, so.wetEndRel, so.maxStep, so.monoViol);
        }
    }

    // ------------------------------------------------------------------ reverb
    out ("\n== REVERB (impulse through ReverbEngine with the real mapping targets) ==\n");
    out ("%-10s %5s | %9s | %10s %10s %8s\n", "mode", "t", "RT60 s", "ring idx", "baseline", "excess");

    bool reverbGo = true;
    for (int mode = 0; mode < 3; ++mode)
        for (float t : { 0.5f, 0.8f, 1.0f })
        {
            const auto tg = TensionDSP::evaluateTensionMapping (t, TensionDSP::riseModeFromIndex (mode));
            const auto rp = analyseReverb (tg);
            const double excess = rp.ring - rp.base;

            if (rp.rt60 > 0.0)
                out ("%-10s %5.2f | %9.2f | ", kModeName[mode], (double) t, rp.rt60);
            else
                out ("%-10s %5.2f | %9s | ", kModeName[mode], (double) t, ">14 / n.a.");

            if (rp.tailOk)
                out ("%10.2f %10.2f %+8.2f%s\n", rp.ring, rp.base, excess, (t >= 0.8f && excess > 1.5) ? "  <-- ringing" : "");
            else
                out ("%10s %10s %8s\n", "(tail empty)", "-", "-");

            if (rp.tailOk && t >= 0.8f && excess > 1.5)
                reverbGo = false;
        }

    out ("Freeverb verdict: %s (NO-GO trigger: ringing-index excess > 1.5 dB at any t >= 0.8; heuristic - confirm by listening to the 100%% tail)\n",
         reverbGo ? "GO - keep juce::dsp::Reverb for v1" : "NO-GO - schedule the FDN behind ReverbEngine");

    // ------------------------------------------------------------------ result
    out ("\n%s  (%d failing run%s)  files: %s\n", failures == 0 ? "ALL CRITERIA PASS" : "CRITERIA FAILED", failures, failures == 1 ? "" : "s", dir.c_str());

    {
        std::ofstream f (dir + "/summary.txt");
        f << g_report;
    }

    return failures == 0 ? 0 : 1;
}
