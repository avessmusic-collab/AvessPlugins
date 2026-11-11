/*
    WARPR - phase D1 test block (sample engine + playback + ADSR + voices).

    Called from RunTests.cpp main():   failures += runPhaseD1Tests (full);
    Quick tier (default) keeps the whole block to a few minutes; `--full` widens the sweeps
    (every MIDI note, 10 simulated minutes of voice storm).

    House rules applied: setPlayConfigDetails BEFORE prepareToPlay (OfflineRender / Runner),
    zero-allocation proof through ScopedAllocCount, deterministic seeded randomness.
*/

#include "PluginProcessor.h"
#include "DSP/AmpEnvelope.h"
#include "DSP/HalfbandIIR.h"
#include "DSP/MasterStage.h"
#include "DSP/RegionFold.h"
#include "DSP/SampleData.h"
#include "DSP/UnisonOsc.h"
#include "Tests/AllocHook.h"
#include "Tests/OfflineRender.h"
#include "Tests/Scanners.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <limits>
#include <set>
#include <vector>

// Bit-exact float equality is the point of several assertions in this file (mono L == R,
// deterministic re-renders, cache-identity checks), so the warning is suppressed here only.
JUCE_BEGIN_IGNORE_WARNINGS_GCC_LIKE ("-Wfloat-equal")

int runPhaseD1Tests (bool full);

namespace
{
    int failures = 0;

    void check (bool cond, const char* name)
    {
        std::printf ("  %s  %s\n", cond ? "PASS" : "FAIL", name);
        if (! cond) ++failures;
    }

    constexpr double kPi = 3.14159265358979323846;

    //==========================================================================
    // helpers
    //==========================================================================
    void setParam (PluginProcessor& p, const char* id, float value)
    {
        if (auto* prm = p.apvts.getParameter (id))
        {
            prm->setValueNotifyingHost (prm->convertTo0to1 (value));
        }
        else
        {
            std::printf ("  FAIL  unknown parameter '%s'\n", id);
            ++failures;
        }
    }

    warpr::SampleData::Ptr makeSine (int frames, double cyclesPerFrame, float amp, double fileSr,
                                     double phaseCycles = 0.0, bool periodic = false, const char* name = "sine")
    {
        juce::AudioBuffer<float> b (1, frames);
        for (int i = 0; i < frames; ++i)
            b.setSample (0, i, amp * (float) std::sin (2.0 * kPi * (cyclesPerFrame * (double) i + phaseCycles)));
        return warpr::SampleData::create (b, fileSr, name, periodic, false);
    }

    /** Positions (in samples, fractional) of the rising zero crossings. */
    std::vector<double> risingCrossings (const float* x, int n)
    {
        std::vector<double> t;
        for (int i = 1; i < n; ++i)
            if (x[i - 1] < 0.0f && x[i] >= 0.0f)
                t.push_back ((double) (i - 1) + (double) (-x[i - 1]) / (double) (x[i] - x[i - 1]));
        return t;
    }

    double estimateHz (const float* x, int n, double fs)
    {
        const auto t = risingCrossings (x, n);
        if (t.size() < 4)
            return 0.0;
        return (double) (t.size() - 1) / (t.back() - t.front()) * fs;
    }

    double rms (const float* x, int n)
    {
        double s = 0.0;
        for (int i = 0; i < n; ++i)
            s += (double) x[i] * (double) x[i];
        return n > 0 ? std::sqrt (s / (double) n) : 0.0;
    }

    double maxStep (const float* x, int n)
    {
        double m = 0.0;
        for (int i = 1; i < n; ++i)
            m = std::max (m, (double) std::abs (x[i] - x[i - 1]));
        return m;
    }

    juce::MidiBuffer noteOnly (int note, float vel = 1.0f, int at = 0)
    {
        juce::MidiBuffer m;
        m.addEvent (juce::MidiMessage::noteOn (1, note, vel), at);
        return m;
    }

    /** Persistent block runner (state survives between calls, unlike OfflineRender::render). */
    struct Runner
    {
        Runner (PluginProcessor& p, double sampleRate, int blockSize)
            : proc (p), fs (sampleRate), block (blockSize), buf (2, blockSize)
        {
            proc.setPlayConfigDetails (0, 2, fs, block);      // BEFORE prepareToPlay
            proc.prepareToPlay (fs, block);
        }

        void process (juce::MidiBuffer& midi)
        {
            buf.clear();
            proc.processBlock (buf, midi);

            // stands in for the processor's 100 ms message-thread timer (no event loop in the console harness)
            if ((++blockCounter & 7) == 0)
                proc.drainRetired();

            for (int ch = 0; ch < 2; ++ch)
            {
                const auto s = warpr::tests::scan (buf, ch);
                finite = finite && s.allFinite;
                peak = std::max (peak, s.peak);
            }
        }

        void process() { juce::MidiBuffer none; process (none); }
        void processSeconds (double seconds) { for (int i = 0, n = (int) (seconds * fs / block); i < n; ++i) process(); }

        PluginProcessor& proc;
        double fs;
        int block;
        juce::AudioBuffer<float> buf;
        bool finite = true;
        float peak = 0.0f;
        unsigned blockCounter = 0;
    };

    //==========================================================================
    // 1. SampleData / MipBuilder
    //==========================================================================
    void testSampleData()
    {
        std::printf ("\n[D1] SampleData + mip pyramid\n");

        auto wave = warpr::SampleData::createDefaultWave();
        check (wave != nullptr && wave->isDefaultWave && wave->singleCycle && wave->numFrames == 2048
               && wave->numChannels == 1, "default wave: 2048 frames, mono, periodic");
        check (std::abs (wave->fileSampleRate - 2048.0 * juce::MidiMessage::getMidiNoteInHertz (60)) < 1.0e-6,
               "default wave: fileSampleRate = 2048 * midiToHz(60)");

        float peak = 0.0f;
        const float* d = wave->level (0).channel (0);
        for (int i = 0; i < 2048; ++i) peak = std::max (peak, std::abs (d[i]));
        check (std::abs (peak - 0.9f) < 1.0e-5f, "default wave: peak 0.9");

        bool guards = true;
        for (int k = 1; k <= warpr::SampleData::pad; ++k)
        {
            guards = guards && d[-k] == d[2048 - k] && d[2048 + k - 1] == d[k - 1];
        }
        check (guards, "default wave: guard frames are a bit-exact circular copy (periodic)");
        check (wave->numLevels == 9, "default wave: full 9-level pyramid");

        // non-finite scrub + channel clamp
        juce::AudioBuffer<float> dirty (3, 100);
        dirty.clear();
        dirty.setSample (0, 10, std::numeric_limits<float>::quiet_NaN());
        dirty.setSample (1, 20, std::numeric_limits<float>::infinity());
        auto scrubbed = warpr::SampleData::create (dirty, 48000.0, "dirty", false, false);
        bool clean = scrubbed != nullptr && scrubbed->numChannels == 2;
        for (int c = 0; clean && c < 2; ++c)
            clean = warpr::tests::scan (scrubbed->level (0).channel (c), 100).allFinite;
        check (clean, "NaN / inf scrubbed to 0 at load; > 2 channels keep the first two");
        check (warpr::SampleData::create (juce::AudioBuffer<float> (1, 0), 48000.0, "x", false, false) == nullptr,
               "empty input is rejected");

        // peaks
        auto s = makeSine (10000, 0.01, 0.5f, 48000.0);
        check (s->peaksMinMax.size() == 2048 * 2, "peaks: 2048 min/max pairs for long samples");
        bool peaksOk = true;
        for (size_t i = 0; i + 1 < s->peaksMinMax.size(); i += 2)
            peaksOk = peaksOk && s->peaksMinMax[i] <= s->peaksMinMax[i + 1] && std::abs (s->peaksMinMax[i]) <= 0.5001f;
        check (peaksOk, "peaks: min <= max and within the signal range");

        // mip band-limiting: a 15 kHz tone at 48 kHz must be gone from level 1 (>= ~95 dB down)
        auto tone = makeSine (16384, 15000.0 / 48000.0, 0.5f, 48000.0, 0.0, true);
        const double inRms = rms (tone->level (0).channel (0), 16384);
        const double l1Rms = rms (tone->level (1).channel (0), tone->level (1).numFrames);
        const double l1Db = 20.0 * std::log10 (std::max (1.0e-12, l1Rms / inRms));
        std::printf ("        (15 kHz tone in level 1: %.1f dB)\n", l1Db);
        check (l1Db < -90.0, "mip level 1 removes a 15 kHz tone (stopband)");

        // 5 kHz tone survives level 1 (passband) with level ~ unity
        auto pass = makeSine (16384, 5000.0 / 48000.0, 0.5f, 48000.0, 0.0, true);
        const double pIn = rms (pass->level (0).channel (0), 16384);
        const double pOut = rms (pass->level (1).channel (0), pass->level (1).numFrames);
        check (std::abs (20.0 * std::log10 (pOut / pIn)) < 0.1, "mip level 1 passes a 5 kHz tone within 0.1 dB");
    }

    //==========================================================================
    // 2. SampleReader (interpolators)
    //==========================================================================
    void testReader()
    {
        std::printf ("\n[D1] SampleReader (sinc8 / Hermite4)\n");

        // integer positions are exact (row 0 is a delta)
        auto wave = warpr::SampleData::createDefaultWave();
        const auto rp = warpr::makeReadParams (*wave, 1.0, 0, false, 0.0, 2048.0);
        bool exact = true;
        for (int i = 0; i < 2048; ++i)
        {
            float l, r;
            warpr::readFrame (rp, (double) i, l, r);
            exact = exact && l == wave->level (0).channel (0)[i] && r == l;
        }
        check (exact, "sinc8 returns the stored sample bit-exactly at integer positions (mono -> L == R)");

        // a slow sine is interpolated accurately at fractional positions
        auto sine = makeSine (4000, 0.01, 0.5f, 48000.0);
        const auto sp = warpr::makeReadParams (*sine, 1.0, 0, false, 0.0, 4000.0);
        const auto hp = warpr::makeReadParams (*sine, 1.0, 1, false, 0.0, 4000.0);
        double worstSinc = 0.0, worstHermite = 0.0;
        for (int i = 0; i < 1000; ++i)
        {
            const double pos = 100.0 + 2.37 * i * 0.37;
            const double ref = 0.5 * std::sin (2.0 * kPi * 0.01 * pos);
            float l, r;
            warpr::readFrame (sp, pos, l, r);
            worstSinc = std::max (worstSinc, std::abs ((double) l - ref));
            warpr::readFrame (hp, pos, l, r);
            worstHermite = std::max (worstHermite, std::abs ((double) l - ref));
        }
        std::printf ("        (worst error: sinc8 %.2e, Hermite4 %.2e)\n", worstSinc, worstHermite);
        // An 8-tap Kaiser (beta 5) interpolating sinc measures ~1.9e-4 worst-case on this sweep; that
        // is its designed quality (the plan's audible gates are pitch +-1 cent and alias <= -80 dBc).
        check (worstSinc < 2.5e-4 && worstHermite < 1.0e-4, "sinc8 tracks a slow sine within 2.5e-4, Hermite4 within 1e-4");

        // loop-wrapped neighbourhood: reading across the seam of a periodic loop is continuous
        auto per = makeSine (5000, 0.005, 0.5f, 48000.0, 0.0, false);     // exactly 25 cycles in 5000 frames
        const auto wp = warpr::makeReadParams (*per, 1.0, 0, true, 0.0, 5000.0);
        double worstSeam = 0.0;
        for (double pos = 4990.0; pos < 5000.0; pos += 0.173)
        {
            float l, r;
            warpr::readFrame (wp, pos, l, r);
            worstSeam = std::max (worstSeam, std::abs ((double) l - 0.5 * std::sin (2.0 * kPi * 0.005 * pos)));
        }
        std::printf ("        (seam read error %.2e)\n", worstSeam);
        check (worstSeam < 1.0e-4, "loop-wrapped taps: no band-limiting error at the seam of a periodic loop");
    }

    //==========================================================================
    // 3. Loop seams (RegionFold + UnisonOsc)
    //==========================================================================
    struct OscRun
    {
        double maxDy = 0.0;
        bool ended = false;
        bool visitedBothDirections = false;
        double endTailPeak = 0.0;
        bool mono = true;
        double xfade = 0.0;
    };

    OscRun runOsc (const warpr::SampleData& sd, warpr::RegionFold::Mode mode, float s, float e,
                   double rho, int os, int totalOS, bool reverse, int skipOnset = 0)
    {
        warpr::RegionFold rg;
        rg.configure (sd, s, e, mode);

        warpr::UnisonOsc osc;
        osc.start (rg, reverse, 0);

        std::vector<float> L ((size_t) totalOS, 0.0f), R ((size_t) totalOS, 0.0f);
        OscRun run;
        run.xfade = rg.xfade;
        bool sawForward = false, sawBackward = false;
        int endedAt = -1;

        for (int i = 0; i < totalOS; i += 256)
        {
            const int n = std::min (256, totalOS - i);
            osc.render (sd, rg, 0, rho, rho, os, 1.0f, 1.0f, 1.0f, 1.0f, L.data() + i, R.data() + i, n);
            (osc.dir > 0 ? sawForward : sawBackward) = true;
            if (osc.ended && endedAt < 0)
                endedAt = i;
        }

        run.maxDy = maxStep (L.data() + skipOnset, totalOS - skipOnset);
        run.ended = osc.ended;
        run.visitedBothDirections = sawForward && sawBackward;
        run.mono = std::memcmp (L.data(), R.data(), (size_t) totalOS * sizeof (float)) == 0;

        if (endedAt >= 0)
            for (int i = endedAt + 256; i < totalOS; ++i)
                run.endTailPeak = std::max (run.endTailPeak, (double) std::abs (L[(size_t) i]));

        return run;
    }

    void testLoopSeams()
    {
        std::printf ("\n[D1] loop seams (|dy| bounds, all three loop modes)\n");
        using Mode = warpr::RegionFold::Mode;

        // periodic sine, 5 cycles in 5000 frames -> clean seam, no crossfade
        auto periodic = makeSine (5000, 0.001, 0.5f, 48000.0);
        const auto a = runOsc (*periodic, Mode::Forward, 0.0f, 1.0f, 1.0, 2, 40000, false);
        std::printf ("        (periodic forward: max|dy| %.5f, xfade %.0f)\n", a.maxDy, a.xfade);
        check (a.xfade == 0.0 && a.maxDy <= 0.01, "Forward Loop, periodic data: no fade needed, |dy| <= 0.01");
        check (a.maxDy <= 0.0035, "Forward Loop, periodic data: seam is as smooth as the signal itself");

        // non-periodic sine -> seam crossfade
        auto odd = makeSine (5000, 1.0 / 997.3, 0.5f, 48000.0);
        const auto b = runOsc (*odd, Mode::Forward, 0.1f, 0.9f, 1.0, 2, 40000, false);
        std::printf ("        (non-periodic forward: max|dy| %.5f, xfade %.0f frames)\n", b.maxDy, b.xfade);
        check (b.xfade > 0.0 && b.maxDy <= 0.01, "Forward Loop, non-periodic data: seam crossfade keeps |dy| <= 0.01");

        const auto br = runOsc (*odd, Mode::Forward, 0.1f, 0.9f, 1.0, 2, 40000, true);
        check (br.maxDy <= 0.01, "Forward Loop, reversed playback: |dy| <= 0.01");

        const auto c = runOsc (*odd, Mode::PingPong, 0.1f, 0.9f, 1.0, 2, 40000, false);
        std::printf ("        (ping-pong: max|dy| %.5f)\n", c.maxDy);
        check (c.maxDy <= 0.01 && c.visitedBothDirections, "Ping-Pong: reflects at both ends, |dy| <= 0.01");

        auto nonZeroEnd = makeSine (4000, 0.00084, 0.5f, 48000.0, 0.1);           // ends at a non-zero level
        // skipOnset 32: this sample STARTS at a non-zero level too, so the sinc taps ramp in over the
        // first 4 frames (the correct band-limited rendering of an intended onset discontinuity;
        // onsets are the amp envelope's job at voice level). Measure only after the taps are inside data.
        const auto d = runOsc (*nonZeroEnd, Mode::OneShot, 0.0f, 1.0f, 1.0, 2, 20000, false, 32);
        std::printf ("        (one-shot: max|dy| %.5f, ended %d)\n", d.maxDy, d.ended ? 1 : 0);
        check (d.ended && d.maxDy <= 0.01 && d.endTailPeak == 0.0, "One-Shot: fades into the end, then exact silence, |dy| <= 0.01");

        check (a.mono && b.mono && c.mono && d.mono, "mono sample: L and R are bit-identical (all loop modes)");

        // default single-cycle wave: periodic to the bit
        auto wave = warpr::SampleData::createDefaultWave();
        warpr::RegionFold rg;
        rg.configure (*wave, 0.0f, 1.0f, Mode::Forward);
        warpr::UnisonOsc osc;
        osc.start (rg, false, 0);
        std::vector<float> L (4096 * 2, 0.0f), R (4096 * 2, 0.0f);
        for (int i = 0; i < 4096 * 2; i += 512)
            osc.render (*wave, rg, 0, 1.0, 1.0, 1, 1.0f, 1.0f, 1.0f, 1.0f, L.data() + i, R.data() + i, 512);
        bool periodicBits = true, matchesData = true;
        for (int i = 0; i < 4096; ++i)
        {
            periodicBits = periodicBits && L[(size_t) i] == L[(size_t) i + 2048];
            matchesData = matchesData && L[(size_t) i] == wave->level (0).channel (0)[i % 2048];
        }
        check (rg.xfade == 0.0 && periodicBits && matchesData,
               "default wave: output over two periods is bit-exactly periodic and equals the stored cycle");

        // direction + start / end
        auto ramp = [] { juce::AudioBuffer<float> r (1, 8000); for (int i = 0; i < 8000; ++i) r.setSample (0, i, 0.8f * (float) i / 8000.0f); return warpr::SampleData::create (r, 48000.0, "ramp", false, false); }();
        warpr::RegionFold rr;
        rr.configure (*ramp, 0.5f, 1.0f, Mode::OneShot);
        warpr::UnisonOsc fwd, rev;
        fwd.start (rr, false, 0);
        rev.start (rr, true, 0);
        std::vector<float> fl (400, 0.0f), fr (400, 0.0f), rl (400, 0.0f), rrr (400, 0.0f);
        fwd.render (*ramp, rr, 0, 1.0, 1.0, 1, 1.0f, 1.0f, 1.0f, 1.0f, fl.data(), fr.data(), 400);
        rev.render (*ramp, rr, 0, 1.0, 1.0, 1, 1.0f, 1.0f, 1.0f, 1.0f, rl.data(), rrr.data(), 400);
        check (rr.lo == 4000.0 && rr.hi == 8000.0 && std::abs (fl[0] - 0.4f) < 1.0e-4f && fl[399] > fl[0],
               "sampleStart / sampleEnd: forward playback starts at the region start and rises");
        check (std::abs (rl[1] - 0.8f * 7999.0f / 8000.0f) < 2.0e-3f && rl[200] < rl[1],
               "direction = Reverse: starts at the region end and descends");
    }

    //==========================================================================
    // 4. Envelope
    //==========================================================================
    void testEnvelope()
    {
        std::printf ("\n[D1] AmpEnvelope (class level)\n");
        constexpr double fs = 48000.0;
        constexpr int total = 48000 * 3;
        std::vector<float> lv ((size_t) total);

        // attack 100 ms / decay 200 ms / sustain 0 (pluck)
        warpr::AmpEnvelope env;
        env.prepare (fs);
        env.setParams (200.0f, 0.0f, 300.0f);
        env.snapSustain();
        env.noteOn (100.0f);
        env.process (lv.data(), total);

        int tPeak = -1, t60 = -1, tIdle = -1;
        for (int i = 0; i < total; ++i)
        {
            if (tPeak < 0 && lv[(size_t) i] >= 0.999999f) tPeak = i;
            if (tPeak >= 0 && t60 < 0 && lv[(size_t) i] <= 0.001f) t60 = i;
            if (tPeak >= 0 && tIdle < 0 && lv[(size_t) i] == 0.0f) tIdle = i;
        }
        const double attackErr = std::abs ((double) (tPeak + 1) / (0.1 * fs) - 1.0);
        const double decayErr = std::abs ((double) (t60 - tPeak) / (0.2 * fs) - 1.0);
        const double idleErr = std::abs ((double) (tIdle - tPeak) / (0.3 * fs) - 1.0);
        std::printf ("        (attack err %.3f%%, decay -60 dB err %.3f%%, free at -90 dB err %.3f%%)\n",
                     attackErr * 100.0, decayErr * 100.0, idleErr * 100.0);
        check (attackErr < 0.02, "attack time within 2%");
        check (decayErr < 0.02, "decay -60 dB time within 2%");
        check (idleErr < 0.02 && tIdle > 0 && ! env.isActive(), "voice is freed at -90 dB (1.5 x the -60 dB time)");

        // release from a sustain of 0.8
        env.reset();
        env.setParams (100.0f, 0.8f, 300.0f);
        env.snapSustain();
        env.noteOn (1.0f);
        env.process (lv.data(), 48000);
        const bool atSustain = std::abs (lv[47999] - 0.8f) < 1.0e-4f;
        env.noteOff();
        env.process (lv.data(), total);
        int r60 = -1, rIdle = -1;
        for (int i = 0; i < total; ++i)
        {
            if (r60 < 0 && lv[(size_t) i] <= 0.8e-3f) r60 = i;
            if (rIdle < 0 && lv[(size_t) i] == 0.0f) rIdle = i;
        }
        const double relErr = std::abs ((double) r60 / (0.3 * fs) - 1.0);
        const double expectIdle = 0.3 * fs * std::log (0.8 / 3.1622777e-5) / std::log (1000.0);
        check (atSustain && relErr < 0.02, "release -60 dB time within 2% (from the sustain level)");
        check (rIdle > 0 && std::abs ((double) rIdle / expectIdle - 1.0) < 0.02, "release ends (voice freed) at -90 dB");

        // early release during the attack: no snap
        env.reset();
        env.setParams (100.0f, 0.5f, 300.0f);
        env.snapSustain();
        env.noteOn (100.0f);
        env.process (lv.data(), 1500);
        const float before = lv[1499];
        env.noteOff();
        env.process (lv.data(), 4000);
        double worst = std::abs ((double) lv[0] - (double) before);
        for (int i = 1; i < 4000; ++i)
            worst = std::max (worst, (double) std::abs (lv[(size_t) i] - lv[(size_t) i - 1]));
        check (before > 0.2f && worst < 4.0e-4, "note-off during the attack releases from the current level (no snap)");

        // retrigger from the current level
        env.reset();
        env.setParams (100.0f, 0.5f, 300.0f);
        env.snapSustain();
        env.noteOn (100.0f);
        env.process (lv.data(), 2400);                     // level ~0.5
        const float mid = lv[2399];
        env.noteOn (100.0f);
        env.process (lv.data(), 4800);
        bool monotone = lv[0] > mid;
        for (int i = 1; i < 4800 && monotone; ++i)
            monotone = lv[(size_t) i] >= lv[(size_t) i - 1];
        check (monotone && mid > 0.4f && mid < 0.6f && std::abs (lv[4799] - 1.0f) < 1.0e-3f,
               "retrigger attacks from the current level (monotone, no drop to zero)");
    }

    //==========================================================================
    // 5. Decimator
    //==========================================================================
    double decimatorGainDb (const warpr::HalfbandCoeffs& c, int factor, double fOverFs)
    {
        constexpr int settle = 2048, measure = 8192;
        const int outTotal = settle + measure;
        const int inTotal = outTotal * factor;

        std::vector<float> l ((size_t) inTotal), r ((size_t) inTotal);
        for (int i = 0; i < inTotal; ++i)
            l[(size_t) i] = r[(size_t) i] = (float) std::sin (2.0 * kPi * fOverFs * (double) i / (double) factor + 0.3);

        warpr::OsDecimator dec;
        dec.reset();
        dec.process (c, l.data(), r.data(), outTotal, factor);

        const double out = rms (l.data() + settle, measure);
        return 20.0 * std::log10 (std::max (1.0e-12, out / (1.0 / std::sqrt (2.0))));
    }

    void testDecimator()
    {
        std::printf ("\n[D1] HalfbandIIR decimator (per stage and cascaded)\n");
        const auto& c = warpr::HalfbandCoeffs::get();
        std::printf ("        (%d all-pass sections: %d direct + %d delayed)\n", c.numStages, c.directStages, c.numStages - c.directStages);
        check (c.numStages >= 4 && c.numStages <= warpr::HalfbandCoeffs::maxStages && c.directStages > 0, "coefficients designed");

        bool stable = true;
        for (int i = 0; i < c.numStages; ++i)
            stable = stable && std::abs (c.alpha[(size_t) i]) < 1.0f;
        check (stable, "all-pass coefficients inside the unit circle (stable)");

        for (int factor : { 2, 4, 8 })
        {
            double worstPass = 0.0, worstStop = -300.0;

            for (double f = 0.01; f <= 0.4001; f += 0.01)
                worstPass = std::max (worstPass, std::abs (decimatorGainDb (c, factor, f)));

            const double top = 0.5 * (double) factor;
            for (double f = 0.6; f <= top + 1.0e-9; f += 0.0371)
                worstStop = std::max (worstStop, decimatorGainDb (c, factor, std::min (f, top)));
            for (double f = 1.0; f <= top + 1.0e-9; f += 1.0)
                worstStop = std::max (worstStop, decimatorGainDb (c, factor, f));

            std::printf ("        (%dx: passband ripple %.4f dB, worst stopband %.1f dB)\n", factor, worstPass, worstStop);

            char name[96];
            std::snprintf (name, sizeof (name), "%dx %s: passband <= 0.1 dB to 0.4 fs, stopband <= -90 dB above 0.6 fs",
                           factor, factor == 2 ? "(single stage)" : "(cascade)");
            check (worstPass <= 0.1 && worstStop <= -90.0, name);
        }

        // no instability: long impulse + noise run stays bounded and decays
        warpr::OsDecimator dec;
        std::vector<float> l (8 * 512), r (8 * 512);
        juce::Random rng (5);
        bool bounded = true;
        for (int blockIndex = 0; blockIndex < 400; ++blockIndex)
        {
            for (size_t i = 0; i < l.size(); ++i)
                l[i] = r[i] = blockIndex < 100 ? rng.nextFloat() * 2.0f - 1.0f : 0.0f;
            dec.process (c, l.data(), r.data(), 512, 8);
            for (int i = 0; i < 512; ++i)
                bounded = bounded && std::abs (l[(size_t) i]) < 4.0f;
        }
        check (bounded && std::abs (l[511]) < 1.0e-6f, "stable: bounded under noise and decays to silence");
    }

    //==========================================================================
    // 6. Pitch accuracy
    //==========================================================================
    double pitchErrorCents (PluginProcessor& proc, double fileSr, double hostFs, int note)
    {
        const double factor = std::pow (2.0, (double) (note - 60) / 12.0);
        double c = std::min (0.1, 1000.0 / (factor * fileSr));
        constexpr int frames = 65536;
        const int cycles = std::max (1, (int) std::lround (c * frames));
        c = (double) cycles / (double) frames;
        const double expectedHz = c * fileSr * factor;

        // exactly `cycles` periods over the whole sample -> periodic (circular mips, seamless loop)
        proc.getSampleStore().publish (makeSine (frames, c, 0.5f, fileSr, 0.0, true));
        const auto out = warpr::tests::render (proc, noteOnly (note), hostFs, 256, 0.7);

        const int start = (int) (0.2 * hostFs);
        const double measured = estimateHz (out.getReadPointer (0) + start, out.getNumSamples() - start, hostFs);
        return measured > 0.0 ? 1200.0 * std::log2 (measured / expectedHz) : 1.0e9;
    }

    void testPitch (bool full)
    {
        std::printf ("\n[D1] pitch accuracy (MIDI range, file SR vs host SR)\n");

        PluginProcessor proc;
        setParam (proc, "ampSustain", 1.0f);
        setParam (proc, "ampAttack", 1.0f);

        double worst = 0.0;
        int count = 0, bad = 0;
        auto run = [&] (double fileSr, double hostFs, int note)
        {
            const double e = std::abs (pitchErrorCents (proc, fileSr, hostFs, note));
            worst = std::max (worst, e);
            ++count;
            if (! (e <= 1.0))
            {
                ++bad;
                std::printf ("        pitch error %.3f cents (file %.0f, host %.0f, note %d)\n", e, fileSr, hostFs, note);
            }
        };

        if (full)
            for (int n = 0; n < 128; ++n) run (48000.0, 48000.0, n);
        else
            for (int n : { 0, 12, 24, 36, 48, 60, 61, 72, 84, 96, 108, 120, 127 }) run (48000.0, 48000.0, n);

        for (double fileSr : { 22050.0, 44100.0, 48000.0, 96000.0, 192000.0 })
            for (double hostFs : { 44100.0, 48000.0, 96000.0, 192000.0 })
            {
                if (full)
                    for (int n : { 0, 36, 60, 84, 127 }) run (fileSr, hostFs, n);
                else
                    for (int n : { 60, 100 }) run (fileSr, hostFs, n);
            }

        // every oversampling factor
        for (int osIndex = 0; osIndex < 4; ++osIndex)
        {
            setParam (proc, "oversampling", (float) osIndex);
            run (48000.0, 48000.0, 60);
            run (48000.0, 48000.0, 84);
        }
        setParam (proc, "oversampling", 1.0f);

        // draft interpolator
        proc.getSynth().setInterpolationQuality (1);
        run (48000.0, 48000.0, 60);
        run (48000.0, 48000.0, 84);
        proc.getSynth().setInterpolationQuality (0);

        std::printf ("        (%d cases, worst error %.4f cents)\n", count, worst);
        check (bad == 0, "pitch within +-1 cent over the MIDI range and file / host sample rates");
    }

    //==========================================================================
    // 7. Mip alias
    //==========================================================================
    void testMipAlias()
    {
        std::printf ("\n[D1] mip alias: 15 kHz sample played +1 / +3 octaves\n");

        PluginProcessor proc;
        setParam (proc, "ampSustain", 1.0f);
        setParam (proc, "ampAttack", 1.0f);

        constexpr double fs = 48000.0;
        proc.getSampleStore().publish (makeSine (16384, 15000.0 / fs, 0.5f, fs, 0.0, true));
        const double reference = 0.5 / std::sqrt (2.0);

        auto level = [&] (int note)
        {
            const auto out = warpr::tests::render (proc, noteOnly (note), fs, 256, 0.6);
            const int start = (int) (0.35 * fs);
            return rms (out.getReadPointer (0) + start, out.getNumSamples() - start);
        };

        const double control = level (60);
        const double plus1 = level (72);
        const double plus3 = level (96);
        const double db1 = 20.0 * std::log10 (std::max (1.0e-12, plus1 / reference));
        const double db3 = 20.0 * std::log10 (std::max (1.0e-12, plus3 / reference));
        std::printf ("        (control %.3f of %.3f, +1 oct %.1f dBc, +3 oct %.1f dBc)\n", control, reference, db1, db3);

        check (control > 0.5 * reference, "control: the 15 kHz tone is audible when played at its root");
        check (db1 <= -80.0, "+1 octave: alias <= -80 dBc");
        check (db3 <= -80.0, "+3 octaves: alias <= -80 dBc");
    }

    //==========================================================================
    // 8. Voice behaviour through the processor
    //==========================================================================
    void testEnvelopeAudio()
    {
        std::printf ("\n[D1] ADSR through the processor (timing, click-free retrigger / early release)\n");
        constexpr double fs = 48000.0;

        PluginProcessor proc;
        proc.getSampleStore().publish (makeSine (48000, 1000.0 / fs, 0.5f, fs, 0.0, true));    // 1 kHz, 1 s loop
        setParam (proc, "ampAttack", 100.0f);
        setParam (proc, "ampDecay", 100.0f);
        setParam (proc, "ampSustain", 1.0f);
        setParam (proc, "ampRelease", 300.0f);

        juce::MidiBuffer midi = noteOnly (60, 1.0f, 0);
        midi.addEvent (juce::MidiMessage::noteOff (1, 60), (int) (1.0 * fs));
        const auto out = warpr::tests::render (proc, midi, fs, 256, 2.2);
        const float* x = out.getReadPointer (0);

        auto windowPeak = [&] (int centre)
        {
            float p = 0.0f;
            for (int i = centre - 24; i < centre + 24; ++i)
                p = std::max (p, std::abs (x[i]));
            return p;
        };

        int t90 = -1;
        for (int i = 100; i < (int) (0.5 * fs); i += 8)
            if (windowPeak (i) >= 0.9f * 0.5f) { t90 = i; break; }
        const double attackErr = std::abs ((double) t90 / (0.9 * 0.1 * fs) - 1.0);

        // release: -40 dB (a third of the way down the -60 dB time constant ratio)
        int r40 = -1;
        for (int i = (int) fs + 100; i < (int) (2.1 * fs); i += 8)
            if (windowPeak (i) <= 0.5f * 0.01f) { r40 = i - (int) fs; break; }
        const double relErr = std::abs ((double) r40 / (0.3 * fs * (40.0 / 60.0)) - 1.0);

        std::printf ("        (audio attack-90%% err %.2f%%, release -40 dB err %.2f%%)\n", attackErr * 100.0, relErr * 100.0);
        check (t90 > 0 && attackErr < 0.03, "audio: attack reaches 90% at 0.9 x A (within 3%)");
        check (r40 > 0 && relErr < 0.03, "audio: release reaches -40 dB at 2/3 of R (within 3%)");
        check (proc.getNumActiveVoices() == 0, "voice is freed after the release (no stuck note)");

        // early release + mono retrigger click bounds, 100 Hz sine (slow, so |dy| is meaningful)
        PluginProcessor p2;
        p2.getSampleStore().publish (makeSine (48000, 100.0 / fs, 0.5f, fs, 0.0, true));
        setParam (p2, "ampAttack", 100.0f);
        setParam (p2, "ampSustain", 1.0f);
        setParam (p2, "ampRelease", 200.0f);

        juce::MidiBuffer early = noteOnly (60, 1.0f, 0);
        early.addEvent (juce::MidiMessage::noteOff (1, 60), (int) (0.03 * fs));
        const auto o2 = warpr::tests::render (p2, early, fs, 256, 0.6);
        const double natural = 2.0 * kPi * 100.0 / fs * 0.5;
        check (maxStep (o2.getReadPointer (0), o2.getNumSamples()) < 1.15 * natural,
               "early note-off during the attack is click-free");

        PluginProcessor p3;
        p3.getSampleStore().publish (makeSine (48000, 100.0 / fs, 0.5f, fs, 0.0, true));
        setParam (p3, "voiceMode", 1.0f);
        setParam (p3, "ampAttack", 20.0f);
        setParam (p3, "ampSustain", 1.0f);
        juce::MidiBuffer retrig = noteOnly (60, 1.0f, 0);
        retrig.addEvent (juce::MidiMessage::noteOn (1, 62, 1.0f), (int) (0.2 * fs));
        retrig.addEvent (juce::MidiMessage::noteOn (1, 64, 1.0f), (int) (0.3 * fs));
        const auto o3 = warpr::tests::render (p3, retrig, fs, 256, 0.6);
        check (maxStep (o3.getReadPointer (0), o3.getNumSamples()) < 1.35 * natural,
               "Mono retrigger (envelope re-attack from the current level) is click-free");
    }

    void testVoiceModes()
    {
        std::printf ("\n[D1] voice modes: Poly / Mono / Legato, polyphony, pedal, all-notes-off\n");
        constexpr double fs = 48000.0;

        auto nonGhostNotes = [] (PluginProcessor& p)
        {
            std::vector<int> notes;
            for (int s = 0; s < warpr::WarprSynth::totalSlots; ++s)
            {
                const auto info = p.getSynth().getVoiceInfo (s);
                if (info.active && ! info.ghost)
                    notes.push_back (info.note);
            }
            std::sort (notes.begin(), notes.end());
            return notes;
        };

        // ---- Poly with polyphony 3
        {
            PluginProcessor proc;
            setParam (proc, "polyphony", 3.0f);
            setParam (proc, "ampSustain", 1.0f);
            Runner r (proc, fs, 256);

            for (int n : { 60, 62, 64, 65, 67 })
            {
                auto m = noteOnly (n);
                r.process (m);
                r.process();
            }
            check (proc.getSynth().getNumNonGhostVoices() == 3, "Poly: polyphony 3 caps the sounding voices");
            check (nonGhostNotes (proc) == std::vector<int> { 64, 65, 67 }, "Poly: the oldest voices were stolen");
            check (proc.getNumActiveVoices() == 3, "Poly: stolen voices (ghosts) are gone after their 3 ms fade");

            setParam (proc, "polyphony", 1.0f);                    // live reduction
            r.process();
            r.process();
            check (proc.getNumActiveVoices() == 1, "Poly: live polyphony reduction steals down to 1");

            juce::MidiBuffer same = noteOnly (67);                  // same note again -> one voice, old one fades
            r.process (same);
            r.process();
            check (proc.getNumActiveVoices() == 1, "Poly: re-triggering the same note keeps a single voice");

            r.finite = true;
            r.processSeconds (0.1);
            check (r.finite, "Poly: output stays finite through stealing");
        }

        // ---- Mono: note stack, last-note priority, return to the held note
        {
            PluginProcessor proc;
            setParam (proc, "voiceMode", 1.0f);
            setParam (proc, "ampSustain", 1.0f);
            Runner r (proc, fs, 256);

            auto m1 = noteOnly (60);   r.process (m1);   r.process();
            auto m2 = noteOnly (64);   r.process (m2);   r.process();
            check (nonGhostNotes (proc) == std::vector<int> { 64 } && proc.getSynth().getNumNonGhostVoices() == 1,
                   "Mono: one voice, last-note priority");

            juce::MidiBuffer off64; off64.addEvent (juce::MidiMessage::noteOff (1, 64), 0);
            r.process (off64);   r.process();
            check (nonGhostNotes (proc) == std::vector<int> { 60 }, "Mono: releasing the top note returns to the held note");

            juce::MidiBuffer off60; off60.addEvent (juce::MidiMessage::noteOff (1, 60), 0);
            r.process (off60);
            r.processSeconds (1.5);
            check (proc.getNumActiveVoices() == 0, "Mono: releasing the last key releases the voice");
        }

        // ---- Legato
        {
            PluginProcessor proc;
            setParam (proc, "voiceMode", 2.0f);
            setParam (proc, "ampSustain", 1.0f);
            Runner r (proc, fs, 256);

            auto m1 = noteOnly (60);   r.process (m1);   r.process();
            const auto before = proc.getSynth().getVoiceInfo (0);
            auto m2 = noteOnly (67);   r.process (m2);   r.process();
            bool oneVoice = proc.getSynth().getNumActiveVoices() == 1;
            check (oneVoice && nonGhostNotes (proc) == std::vector<int> { 67 } && before.active,
                   "Legato: overlapping notes reuse the sounding voice (no new voice)");
        }

        // ---- Sustain pedal (Poly) and CC123
        {
            PluginProcessor proc;
            setParam (proc, "ampSustain", 1.0f);
            setParam (proc, "ampRelease", 100.0f);
            Runner r (proc, fs, 256);

            juce::MidiBuffer a;
            a.addEvent (juce::MidiMessage::noteOn (1, 60, 1.0f), 0);
            a.addEvent (juce::MidiMessage::controllerEvent (1, 64, 127), 10);
            a.addEvent (juce::MidiMessage::noteOff (1, 60), 20);
            r.process (a);
            r.processSeconds (0.6);
            check (proc.getNumActiveVoices() == 1, "pedal down: the released note keeps sounding");

            juce::MidiBuffer up;
            up.addEvent (juce::MidiMessage::controllerEvent (1, 64, 0), 0);
            r.process (up);
            r.processSeconds (1.0);
            check (proc.getNumActiveVoices() == 0, "pedal up: the held note releases and the voice is freed");

            juce::MidiBuffer chord;
            for (int n : { 48, 52, 55, 60 }) chord.addEvent (juce::MidiMessage::noteOn (1, n, 0.9f), 0);
            chord.addEvent (juce::MidiMessage::allNotesOff (1), 200);
            r.process (chord);
            r.process();
            r.processSeconds (1.0);
            check (proc.getNumActiveVoices() == 0, "CC123 all-notes-off releases every voice");
        }
    }

    //==========================================================================
    // 9. Glide, unison, stereo
    //==========================================================================
    void testGlide()
    {
        std::printf ("\n[D1] glide time\n");
        constexpr double fs = 48000.0;
        constexpr double glideMs = 400.0;

        PluginProcessor proc;
        proc.getSampleStore().publish (makeSine (65536, 683.0 / 65536.0, 0.5f, fs, 0.0, true));    // ~500 Hz at the root
        setParam (proc, "voiceMode", 2.0f);                    // Legato: glide on overlapping notes
        setParam (proc, "glideTime", (float) glideMs);
        setParam (proc, "ampSustain", 1.0f);
        setParam (proc, "ampAttack", 1.0f);

        juce::MidiBuffer midi = noteOnly (60, 1.0f, 0);
        midi.addEvent (juce::MidiMessage::noteOn (1, 72, 1.0f), (int) (0.6 * fs));
        const auto out = warpr::tests::render (proc, midi, fs, 256, 1.5);

        const auto t = risingCrossings (out.getReadPointer (0), out.getNumSamples());
        std::vector<double> stamps, freqs;
        double f0 = 0.0;
        int nBase = 0;

        for (size_t i = 0; i + 1 < t.size(); ++i)
        {
            const double f = fs / (t[i + 1] - t[i]);
            const double mid = 0.5 * (t[i] + t[i + 1]) / fs;
            if (mid > 0.2 && mid < 0.55) { f0 += f; ++nBase; }
            stamps.push_back (mid);
            freqs.push_back (f);
        }
        f0 /= std::max (1, nBase);

        auto crossing = [&] (double targetSemis)
        {
            for (size_t i = 1; i < stamps.size(); ++i)
            {
                if (stamps[i] < 0.6) continue;
                const double s0 = 12.0 * std::log2 (freqs[i - 1] / f0), s1 = 12.0 * std::log2 (freqs[i] / f0);
                if (s0 < targetSemis && s1 >= targetSemis)
                    return stamps[i - 1] + (stamps[i] - stamps[i - 1]) * (targetSemis - s0) / (s1 - s0);
            }
            return -1.0;
        };

        const double t10 = crossing (1.2), t90 = crossing (10.8);
        const double measuredMs = (t90 - t10) * 1000.0;
        const double expectedMs = 0.8 * glideMs;
        std::printf ("        (10%%..90%% took %.2f ms, expected %.2f ms)\n", measuredMs, expectedMs);
        check (t10 > 0.0 && t90 > 0.0 && std::abs (measuredMs / expectedMs - 1.0) < 0.02, "glide time within 2%");
        const double endSemis = 12.0 * std::log2 (freqs.back() / f0);
        check (std::abs (endSemis - 12.0) < 0.02, "glide ends exactly on the target pitch");
    }

    void testUnisonStereo()
    {
        std::printf ("\n[D1] unison (detune / spread / gain) and stereo symmetry\n");
        constexpr double fs = 48000.0;

        // pan law is exactly mirror-symmetric
        bool mirror = true, bounded = true;
        for (float p = -1.0f; p <= 1.0001f; p += 0.05f)
        {
            float gl, gr, gl2, gr2;
            warpr::balanceGains (p, gl, gr);
            warpr::balanceGains (-p, gl2, gr2);
            mirror = mirror && gl == gr2 && gr == gl2;
            bounded = bounded && gl <= 1.0f && gr <= 1.0f && gl >= 0.0f && gr >= 0.0f;
        }
        check (mirror && bounded, "balance law: gl(p) == gr(-p) bit-exactly, gains in [0, 1]");

        PluginProcessor proc;
        proc.getSampleStore().publish (makeSine (65536, 6830.0 / 65536.0 / 10.0, 0.5f, fs, 0.0, true));    // ~500 Hz at the root
        setParam (proc, "ampSustain", 1.0f);
        setParam (proc, "ampAttack", 1.0f);

        auto baseline = warpr::tests::render (proc, noteOnly (60), fs, 256, 1.0);
        const double f0 = estimateHz (baseline.getReadPointer (0) + 9600, baseline.getNumSamples() - 9600, fs);

        // two oscillators hard-panned: L = -detune, R = +detune
        setParam (proc, "unisonVoices", 2.0f);
        setParam (proc, "unisonDetune", 20.0f);
        setParam (proc, "unisonSpread", 1.0f);
        const auto out = warpr::tests::render (proc, noteOnly (60), fs, 256, 4.0);
        const int start = 9600;
        const double fl = estimateHz (out.getReadPointer (0) + start, out.getNumSamples() - start, fs);
        const double fr = estimateHz (out.getReadPointer (1) + start, out.getNumSamples() - start, fs);
        const double centsL = 1200.0 * std::log2 (fl / f0), centsR = 1200.0 * std::log2 (fr / f0);
        std::printf ("        (unison cents: L %.3f, R %.3f)\n", centsL, centsR);
        check (std::abs (centsL + 20.0) <= 0.5 && std::abs (centsR - 20.0) <= 0.5, "unison detune accuracy within +-0.5 cent");

        const double rl = rms (out.getReadPointer (0) + start, out.getNumSamples() - start);
        const double rr = rms (out.getReadPointer (1) + start, out.getNumSamples() - start);
        const double asym = std::abs (20.0 * std::log10 (rl / rr));
        std::printf ("        (L/R level difference %.4f dB, per-oscillator level %.3f)\n", asym, rl);
        check (asym <= 0.1, "L / R balance symmetric within 0.1 dB");
        check (std::abs (rl - 0.5 / std::sqrt (2.0) / std::sqrt (2.0)) < 0.02, "unison gain normalisation 1/sqrt(N)");

        // mono sample: L == R exactly, with and without unison (centre pan)
        setParam (proc, "unisonVoices", 1.0f);
        setParam (proc, "unisonSpread", 0.5f);
        auto one = warpr::tests::render (proc, noteOnly (60), fs, 256, 0.5);
        check (std::memcmp (one.getReadPointer (0), one.getReadPointer (1), sizeof (float) * (size_t) one.getNumSamples()) == 0,
               "mono sample, 1 oscillator: L == R exactly");

        setParam (proc, "unisonVoices", 4.0f);
        setParam (proc, "unisonSpread", 0.0f);
        auto four = warpr::tests::render (proc, noteOnly (60), fs, 256, 0.5);
        check (std::memcmp (four.getReadPointer (0), four.getReadPointer (1), sizeof (float) * (size_t) four.getNumSamples()) == 0
               && warpr::tests::scan (four, 0).peak > 0.05f,
               "mono sample, 4 oscillators at spread 0: L == R exactly");

        // stereo sample is NOT forced to mono
        juce::AudioBuffer<float> st (2, 20000);
        for (int i = 0; i < 20000; ++i)
        {
            st.setSample (0, i, 0.4f * (float) std::sin (2.0 * kPi * 0.01 * i));
            st.setSample (1, i, 0.4f * (float) std::sin (2.0 * kPi * 0.0125 * i));
        }
        proc.getSampleStore().publish (warpr::SampleData::create (st, fs, "stereo", false, false));
        setParam (proc, "unisonVoices", 1.0f);
        auto stereo = warpr::tests::render (proc, noteOnly (60), fs, 256, 0.3);
        check (std::memcmp (stereo.getReadPointer (0), stereo.getReadPointer (1), sizeof (float) * (size_t) stereo.getNumSamples()) != 0,
               "stereo sample keeps independent left and right channels");
    }

    //==========================================================================
    // 10. Robustness: storm, sample hand-off, governor, tiny samples, allocations, determinism
    //==========================================================================
    void testStorm (bool full)
    {
        std::printf ("\n[D1] voice storm (random notes + pedal + bend + all-notes-off)\n");
        constexpr double fs = 48000.0;
        constexpr int block = 256;

        PluginProcessor proc;
        setParam (proc, "polyphony", 8.0f);
        setParam (proc, "unisonVoices", 2.0f);
        setParam (proc, "ampAttack", 2.0f);
        setParam (proc, "ampDecay", 120.0f);
        setParam (proc, "ampSustain", 0.6f);
        setParam (proc, "ampRelease", 100.0f);
        setParam (proc, "glideTime", 30.0f);

        Runner r (proc, fs, block);
        juce::Random rng (20261112);
        std::set<int> held;
        const int totalBlocks = (int) ((full ? 600.0 : 10.0) * fs / block);

        for (int b = 0; b < totalBlocks; ++b)
        {
            juce::MidiBuffer midi;
            const int events = rng.nextInt (4);

            for (int e = 0; e < events; ++e)
            {
                const int at = rng.nextInt (block);
                switch (rng.nextInt (9))
                {
                    case 0: case 1: case 2:
                    {
                        const int n = 24 + rng.nextInt (73);
                        midi.addEvent (juce::MidiMessage::noteOn (1, n, 0.2f + 0.8f * rng.nextFloat()), at);
                        held.insert (n);
                        break;
                    }
                    case 3: case 4:
                        if (! held.empty())
                        {
                            auto it = held.begin();
                            std::advance (it, rng.nextInt ((int) held.size()));
                            midi.addEvent (juce::MidiMessage::noteOff (1, *it), at);
                            held.erase (it);
                        }
                        break;
                    case 5: midi.addEvent (juce::MidiMessage::controllerEvent (1, 64, rng.nextBool() ? 127 : 0), at); break;
                    case 6: midi.addEvent (juce::MidiMessage::pitchWheel (1, rng.nextInt (16384)), at); break;
                    case 7: if (rng.nextInt (20) == 0) { midi.addEvent (juce::MidiMessage::allNotesOff (1), at); held.clear(); } break;
                    default: break;
                }
            }

            if (b % 300 == 299)
            {
                setParam (proc, "voiceMode", (float) rng.nextInt (3));
                setParam (proc, "polyphony", (float) (1 + rng.nextInt (12)));
                setParam (proc, "unisonVoices", (float) (1 + rng.nextInt (4)));
                setParam (proc, "loopMode", (float) rng.nextInt (3));
                setParam (proc, "bendRange", (float) rng.nextInt (3));
            }

            r.process (midi);
        }

        const bool stormFinite = r.finite;
        const float stormPeak = r.peak;

        juce::MidiBuffer cleanup;
        for (int n : held) cleanup.addEvent (juce::MidiMessage::noteOff (1, n), 0);
        cleanup.addEvent (juce::MidiMessage::controllerEvent (1, 64, 0), 1);
        cleanup.addEvent (juce::MidiMessage::allNotesOff (1), 2);
        r.process (cleanup);
        setParam (proc, "loopMode", 0.0f);
        r.processSeconds (4.0);

        std::printf ("        (%d blocks, peak %.3f, active voices at the end %d)\n", totalBlocks, (double) stormPeak, proc.getNumActiveVoices());
        check (stormFinite && r.finite, "storm: no NaN / inf");
        check (stormPeak <= 1.0f, "storm: master output never exceeds 1.0");
        check (proc.getNumActiveVoices() == 0, "storm: no stuck notes - every voice is idle after release");
        check (proc.getSynth().getGuardKills() == 0 && proc.getSynth().getLeakedRefs() == 0 && proc.getSynth().getBacklogCount() == 0,
               "storm: no sanity-guard kills, no leaked sample references");
    }

    void testSampleHandoff()
    {
        std::printf ("\n[D1] sample swap with sounding voices (reference hand-off, no audio-thread free)\n");
        constexpr double fs = 48000.0;

        PluginProcessor proc;
        setParam (proc, "ampSustain", 1.0f);
        setParam (proc, "ampRelease", 50.0f);

        auto a = makeSine (20000, 0.01, 0.5f, fs);
        auto b = makeSine (30000, 0.02, 0.4f, fs);
        proc.getSampleStore().publish (a);

        Runner r (proc, fs, 256);
        auto on = noteOnly (60);
        r.process (on);
        r.process();

        proc.getSampleStore().publish (b);                  // the new sample arrives while the note sounds
        auto on2 = noteOnly (64);
        r.process (on2);                                     // acquired at this block start; new note uses `b`
        r.processSeconds (0.1);
        proc.drainRetired();

        // `a`: this test's reference + the first voice's own reference (the swap's reference was retired and freed)
        check (a->getReferenceCount() == 2, "old sample stays alive for the voice still playing it (exactly one voice reference)");
        check (r.finite, "output stays finite across the swap");

        juce::MidiBuffer off;
        off.addEvent (juce::MidiMessage::noteOff (1, 60), 0);
        off.addEvent (juce::MidiMessage::noteOff (1, 64), 0);
        r.process (off);
        r.processSeconds (0.6);
        check (proc.getNumActiveVoices() == 0, "voices ended after note-off");
        check (a->getReferenceCount() >= 1 && a->getReferenceCount() <= 2, "the finished voice handed its reference to the retire list (not freed on the audio thread)");

        proc.drainRetired();
        check (a->getReferenceCount() == 1, "after the message-thread drain only the test holds the old sample");
        check (proc.getSampleStore().latest() == b, "the store's latest sample is the new one");
    }

    void testGovernor()
    {
        std::printf ("\n[D1] oscillator-budget governor\n");
        constexpr double fs = 48000.0;

        auto unitsOfNonGhost = [] (PluginProcessor& p) { return p.getSynth().getOscillatorUnitsInUse(); };

        {   // budget 32 units, 2x, unison 8 -> two full voices fit
            PluginProcessor proc;
            setParam (proc, "unisonVoices", 8.0f);
            setParam (proc, "oversampling", 1.0f);
            setParam (proc, "ampSustain", 1.0f);
            proc.getSynth().setOscillatorBudget (32);
            Runner r (proc, fs, 256);

            for (int n : { 60, 62, 64 })
            {
                auto m = noteOnly (n);
                r.process (m);
                r.process();
            }

            bool sounding62 = false, sounding64 = false, gone60 = true, unisonIntact = true;
            for (int s = 0; s < warpr::WarprSynth::totalSlots; ++s)
            {
                const auto info = proc.getSynth().getVoiceInfo (s);
                if (! info.active || info.ghost) continue;
                sounding62 = sounding62 || info.note == 62;
                sounding64 = sounding64 || info.note == 64;
                gone60 = gone60 && info.note != 60;
                unisonIntact = unisonIntact && info.unison == 8;
            }
            check (unitsOfNonGhost (proc) <= 32, "units in use never exceed the budget");
            check (sounding62 && sounding64 && gone60, "a note that does not fit steals the oldest voice");
            check (unisonIntact, "sounding voices keep their unison count");
        }

        {   // budget 8 units at 4x: the first note reduces its OWN unison to 2
            PluginProcessor proc;
            setParam (proc, "unisonVoices", 8.0f);
            setParam (proc, "oversampling", 2.0f);
            setParam (proc, "ampSustain", 1.0f);
            proc.getSynth().setOscillatorBudget (8);
            Runner r (proc, fs, 256);

            auto m = noteOnly (60);
            r.process (m);
            r.process();
            int unison = 0;
            for (int s = 0; s < warpr::WarprSynth::totalSlots; ++s)
            {
                const auto info = proc.getSynth().getVoiceInfo (s);
                if (info.active && ! info.ghost) unison = info.unison;
            }
            check (unison == 2, "a new note trims its own unison to fit the budget");
        }

        {   // governor disabled
            PluginProcessor proc;
            setParam (proc, "unisonVoices", 8.0f);
            setParam (proc, "oversampling", 3.0f);
            setParam (proc, "ampSustain", 1.0f);
            proc.getSynth().setOscillatorBudget (0);
            Runner r (proc, fs, 256);
            for (int n : { 60, 62, 64, 65 })
            {
                auto m = noteOnly (n);
                r.process (m);
            }
            r.process();
            check (proc.getSynth().getNumNonGhostVoices() == 4 && unitsOfNonGhost (proc) == 4 * 64, "budget 0 disables the governor");
        }
    }

    void testTinySamples()
    {
        std::printf ("\n[D1] tiny samples\n");
        constexpr double fs = 48000.0;
        PluginProcessor proc;
        setParam (proc, "ampSustain", 1.0f);

        for (int frames : { 1, 8, 63, 300 })
        {
            juce::AudioBuffer<float> b (1, frames);
            for (int i = 0; i < frames; ++i)
                b.setSample (0, i, frames == 1 ? 0.5f : 0.5f * (float) std::sin (2.0 * kPi * (double) i / (double) frames));
            proc.getSampleStore().publish (warpr::SampleData::create (b, fs, "tiny", false, false));

            for (int mode = 0; mode < 3; ++mode)
            {
                setParam (proc, "loopMode", (float) mode);
                const auto out = warpr::tests::render (proc, noteOnly (72), fs, 256, 0.25);
                const auto s = warpr::tests::scan (out, 0);
                char name[96];
                std::snprintf (name, sizeof (name), "%d-frame sample, loop mode %d: finite and bounded", frames, mode);
                check (s.allFinite && s.peak <= 1.0f, name);
            }
        }
    }

    juce::MidiBuffer randomMidi (juce::Random& rng, double fs, double seconds)
    {
        juce::MidiBuffer m;
        const int total = (int) (fs * seconds);
        for (int i = 0; i < 40; ++i)
        {
            const int at = rng.nextInt (total - 1);
            const int n = 30 + rng.nextInt (60);
            m.addEvent (juce::MidiMessage::noteOn (1, n, 0.3f + 0.7f * rng.nextFloat()), at);
            m.addEvent (juce::MidiMessage::noteOff (1, n), std::min (total - 1, at + 2000 + rng.nextInt (30000)));
        }
        m.addEvent (juce::MidiMessage::pitchWheel (1, 12000), total / 3);
        m.addEvent (juce::MidiMessage::controllerEvent (1, 64, 127), total / 2);
        return m;
    }

    void testAllocationsAndDeterminism()
    {
        std::printf ("\n[D1] zero allocations in processBlock + determinism\n");
        constexpr double fs = 48000.0;
        constexpr int block = 256;

        // ---- allocations: every voice mode, heavy unison / OS, a sample swap and steals inside the counted region
        for (int mode = 0; mode < 3; ++mode)
        {
            PluginProcessor proc;
            setParam (proc, "voiceMode", (float) mode);
            setParam (proc, "unisonVoices", 8.0f);
            setParam (proc, "oversampling", 3.0f);
            setParam (proc, "polyphony", 4.0f);
            setParam (proc, "glideTime", 50.0f);
            setParam (proc, "ampRelease", 60.0f);

            Runner r (proc, fs, block);
            juce::Random rng (99 + mode);

            std::vector<juce::MidiBuffer> blocks;
            for (int b = 0; b < 300; ++b)
            {
                juce::MidiBuffer m;
                if (b % 3 == 0) m.addEvent (juce::MidiMessage::noteOn (1, 40 + rng.nextInt (40), 0.8f), rng.nextInt (block));
                if (b % 5 == 0) m.addEvent (juce::MidiMessage::noteOff (1, 40 + rng.nextInt (40)), rng.nextInt (block));
                if (b % 17 == 0) m.addEvent (juce::MidiMessage::pitchWheel (1, rng.nextInt (16384)), 0);
                if (b % 41 == 0) m.addEvent (juce::MidiMessage::controllerEvent (1, 64, b % 2 == 0 ? 127 : 0), 0);
                blocks.push_back (m);
            }

            r.process (blocks[0]);                             // warm-up
            proc.getSampleStore().publish (makeSine (30000, 0.01, 0.4f, fs));     // swap lands inside the counted region

            std::size_t allocations = 1;
            {
                warpr::tests::ScopedAllocCount counter;
                for (size_t b = 1; b < blocks.size(); ++b)
                {
                    r.buf.clear();
                    proc.processBlock (r.buf, blocks[b]);
                }
                allocations = counter.count();
            }

            char name[96];
            std::snprintf (name, sizeof (name), "voice mode %d, unison 8, OS 8x, steals + sample swap: zero allocations", mode);
            check (allocations == 0, name);
        }

        // ---- determinism
        juce::Random rng (4242);
        const auto midi = randomMidi (rng, fs, 3.0);

        auto configure = [] (PluginProcessor& p)
        {
            setParam (p, "unisonVoices", 3.0f);
            setParam (p, "polyphony", 6.0f);
            setParam (p, "oversampling", 2.0f);
            setParam (p, "ampRelease", 150.0f);
        };

        PluginProcessor a, b;
        configure (a);
        configure (b);
        const auto ra = warpr::tests::render (a, midi, fs, 256, 3.0);
        const auto rb = warpr::tests::render (b, midi, fs, 256, 3.0);
        const auto ra2 = warpr::tests::render (a, midi, fs, 256, 3.0);
        const size_t bytes = sizeof (float) * (size_t) ra.getNumSamples();

        const bool same = std::memcmp (ra.getReadPointer (0), rb.getReadPointer (0), bytes) == 0
                       && std::memcmp (ra.getReadPointer (1), rb.getReadPointer (1), bytes) == 0;
        const bool again = std::memcmp (ra.getReadPointer (0), ra2.getReadPointer (0), bytes) == 0
                        && std::memcmp (ra.getReadPointer (1), ra2.getReadPointer (1), bytes) == 0;
        check (warpr::tests::scan (ra, 0).peak > 0.05f, "determinism fixture is audible");
        check (same, "two instances render bit-identical output");
        check (again, "re-rendering on the same instance is bit-identical (prepareToPlay resets all state)");

        // block-size independence of note timing: sample-accurate starts
        auto early = warpr::tests::render (a, noteOnly (60, 1.0f, 1234), fs, 64, 0.2);
        const float* e = early.getReadPointer (0);
        int firstNonZero = -1;
        for (int i = 0; i < early.getNumSamples(); ++i)
            if (e[i] != 0.0f) { firstNonZero = i; break; }
        check (firstNonZero >= 1234 && firstNonZero <= 1236, "note-on lands within 1 sample of its MIDI timestamp");
    }

    void testMasterStage()
    {
        std::printf ("\n[D1] MasterStage (DC block, gain, safety knee)\n");
        constexpr double fs = 48000.0;
        warpr::MasterStage m;
        m.prepare (fs);

        std::vector<float> l (48000), r (48000);
        for (int i = 0; i < 48000; ++i)
            l[(size_t) i] = r[(size_t) i] = 0.5f * (float) std::sin (2.0 * kPi * 440.0 * i / fs) + 0.2f;      // DC 0.2
        m.process (l.data(), r.data(), 48000, 0.0f);
        const auto s = warpr::tests::scan (l.data() + 24000, 24000);
        check (std::abs (s.mean) < 0.01, "DC is removed (one-pole high-pass)");

        // below the knee the path is the DC blocker only (no clip); well above it the output stays < 1
        m.reset();
        m.prepare (fs);
        for (int i = 0; i < 48000; ++i)
            l[(size_t) i] = r[(size_t) i] = 6.0f * (float) std::sin (2.0 * kPi * 440.0 * i / fs);
        m.process (l.data(), r.data(), 48000, 12.0f);
        const auto big = warpr::tests::scan (l.data(), 48000);
        check (big.allFinite && big.peak <= 1.0f && big.peak > 0.95f, "safety knee: +12 dB into 6.0 FS stays <= 1.0 and saturates");

        m.reset();
        for (int i = 0; i < 1000; ++i)
        {
            l[(size_t) i] = (i % 7 == 0) ? std::numeric_limits<float>::quiet_NaN() : 0.3f;
            r[(size_t) i] = (i % 11 == 0) ? std::numeric_limits<float>::infinity() : 0.3f;
        }
        m.process (l.data(), r.data(), 1000, 0.0f);
        check (warpr::tests::scan (l.data(), 1000).allFinite && warpr::tests::scan (r.data(), 1000).allFinite,
               "NaN / inf at the master input never reach the output");

        double prev = 0.0;
        bool bypass = true;
        for (double x = -0.89; x <= 0.89; x += 0.0123)
        {
            const double y = warpr::MasterStage::softClipAdaa (x, prev);
            bypass = bypass && (y == x);
        }
        check (bypass, "inside the knee the safety stage is a bit-exact bypass");
    }

    void testSampleLoading()
    {
        std::printf ("\n[D1] file loading (loader thread, display, error handling)\n");
        constexpr double fs = 48000.0;

        juce::AudioBuffer<float> st (2, 48000);
        for (int i = 0; i < 48000; ++i)
        {
            st.setSample (0, i, 0.5f * (float) std::sin (2.0 * kPi * 220.0 * i / fs));
            st.setSample (1, i, 0.25f * (float) std::sin (2.0 * kPi * 330.0 * i / fs));
        }

        const auto file = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("warpr_d1_loader_test.wav");
        check (warpr::tests::writeWav (st, file, fs), "test WAV written (24-bit stereo)");

        PluginProcessor proc;
        auto& be = proc.getBackendImpl();

        const auto before = be.getSampleDisplay();
        check (before.name == "Default Wave" && before.numChannels == 1 && before.numSamples == 2048,
               "before any load the display shows the built-in default wave");

        const auto revision = be.revision (WarprUiBackend::Revision::sample);
        be.loadSampleFile (file);

        bool loaded = false;
        for (int i = 0; i < 500 && ! loaded; ++i)
        {
            loaded = be.revision (WarprUiBackend::Revision::sample) != revision;
            if (! loaded)
                juce::Thread::sleep (10);
        }
        check (loaded, "loadSampleFile finishes on the loader thread and bumps Revision::sample");

        const auto disp = be.getSampleDisplay();
        bool peaksOk = disp.peaksMinMax.size() == 2048 * 2;
        for (size_t i = 0; i + 1 < disp.peaksMinMax.size(); i += 2)
            peaksOk = peaksOk && disp.peaksMinMax[i] <= disp.peaksMinMax[i + 1] && std::abs (disp.peaksMinMax[i]) <= 1.0f;
        check (disp.name == "warpr_d1_loader_test" && disp.numChannels == 2 && disp.numSamples == 48000
               && std::abs (disp.sampleRate - fs) < 1.0e-9 && peaksOk,
               "display: name, 2 channels, 48000 frames, 48 kHz, 2048 peak pairs");

        const auto out = warpr::tests::render (proc, noteOnly (60), fs, 256, 0.5);
        const auto ls = warpr::tests::scan (out, 0), rs = warpr::tests::scan (out, 1);
        check (ls.allFinite && ls.peak > 0.05f && rs.peak > 0.02f && ls.peak != rs.peak, "the loaded stereo sample plays through the voices");

        // failure: the previous sample is kept, the revision does not move
        const auto revAfter = be.revision (WarprUiBackend::Revision::sample);
        be.loadSampleFile (juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("warpr_no_such_file.wav"));
        juce::Thread::sleep (300);
        check (be.revision (WarprUiBackend::Revision::sample) == revAfter && be.getSampleDisplay().numSamples == 48000,
               "a missing file keeps the previous sample (no revision bump)");

        juce::String error;
        auto missing = warpr::SampleStore::decodeFile (file.getSiblingFile ("warpr_no_such_file.wav"), proc.getSampleStore().formats(), error);
        check (missing == nullptr && error.isNotEmpty(), "decode failure reports an error message");

        file.deleteFile();
    }
}

//==============================================================================
int runPhaseD1Tests (bool full)
{
    failures = 0;
    std::printf ("\n=== Phase D1: sample engine + playback + ADSR + voices (%s) ===\n", full ? "full" : "quick");

    testSampleData();
    testReader();
    testLoopSeams();
    testEnvelope();
    testDecimator();
    testMasterStage();
    testPitch (full);
    testMipAlias();
    testEnvelopeAudio();
    testVoiceModes();
    testGlide();
    testUnisonStereo();
    testSampleHandoff();
    testGovernor();
    testTinySamples();
    testAllocationsAndDeterminism();
    testSampleLoading();
    testStorm (full);

    return failures;
}

JUCE_END_IGNORE_WARNINGS_GCC_LIKE
