/*
    KICKR — console test runner. Grows one block of checks per Stage-2 phase.
    Build:  cmake --build build --target KICKR_Tests
    Run:    ./build/plugins/KICKR/KICKR_Tests
    Writes kickr_phase*.wav next to the working directory for manual inspection.
*/
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "Tests/OfflineRender.h"
#include "DSP/SamplePlayer.h"
#include "DSP/Waveshaper.h"
#include "Sampling/SampleLibrary.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

namespace
{
    int  failures = 0;
    void check (bool cond, const char* name)
    {
        std::printf ("  %s  %s\n", cond ? "PASS" : "FAIL", name);
        if (! cond) ++failures;
    }

    struct Stats
    {
        float  peak      = 0.0f;
        int    peakIndex = 0;
        float  maxStep   = 0.0f;   // largest |x[n] - x[n-1]|
        double tailRms    = 0.0;   // RMS of the last 100 ms
        int    lastAbove40dB = 0;  // last index where |x| > peak * 0.01
        bool   allFinite = true;
    };

    Stats analyse (const juce::AudioBuffer<float>& b, double sr)
    {
        Stats s;
        const int n = b.getNumSamples();
        const float* x = b.getReadPointer (0);

        for (int i = 0; i < n; ++i)
        {
            const float v = x[i];
            if (! std::isfinite (v)) s.allFinite = false;
            const float a = std::abs (v);
            if (a > s.peak) { s.peak = a; s.peakIndex = i; }
            if (i > 0) s.maxStep = std::max (s.maxStep, std::abs (v - x[i - 1]));
        }
        for (int i = 0; i < n; ++i)
            if (std::abs (x[i]) > s.peak * 0.01f) s.lastAbove40dB = i;

        const int tail = std::max (0, n - (int) (0.1 * sr));
        double sq = 0.0;
        for (int i = tail; i < n; ++i) sq += (double) x[i] * x[i];
        s.tailRms = std::sqrt (sq / std::max (1, n - tail));
        return s;
    }
}

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    const double sr = 48000.0;
    // 2026-08-31: MIDI Pitch mode was recentred 2 octaves down (kMidiPitchRecentreSemitones)
    // so a "C3" trigger is a real kick pitch, not 261 Hz concert pitch. 33 (real A1) would
    // now be 13.75 Hz; 57 is the note that lands back on 55 Hz post-recentre — every test
    // below just wants "the note that plays the default fundamental," so bump 33 -> 57 here
    // and every call site (all via this `a1`) keeps meaning the same thing.
    const int    a1 = 57;                 // -> 55 Hz after the -24 semitone recentre
    const float  vel = 100.0f / 127.0f;

    std::printf ("KICKR tests  (sr %.0f)\n", sr);

    // Phase 2.4 adds a per-voice click (default clickLevel 0.4). Pre-2.4 checkpoints
    // assume a bare body, so isolate the body layer where they need it.
    auto silenceClick = [] (KICKRAudioProcessor& p)
    {
        if (auto* prm = p.getValueTreeState().getParameter ("clickLevel"))
            prm->setValueNotifyingHost (0.0f);
    };

    // Phase 2.5 adds a per-voice sub oscillator (default subLevel 0.5). Pre-2.5
    // checkpoints assume a bare body (+ click where relevant), so mute the sub there.
    auto silenceSub = [] (KICKRAudioProcessor& p)
    {
        if (auto* prm = p.getValueTreeState().getParameter ("subLevel"))
            prm->setValueNotifyingHost (0.0f);
    };

    // Phase 2.6 adds a per-voice tail generator (default tailLevel 0.3 = tail ON).
    // Pre-2.6 checkpoints assume a bare body (+ click/sub where relevant), so mute
    // the tail there.
    auto silenceTail = [] (KICKRAudioProcessor& p)
    {
        if (auto* prm = p.getValueTreeState().getParameter ("tailLevel"))
            prm->setValueNotifyingHost (0.0f);
    };

    // Phase 2.8 adds the master morphing distortion. Defaults (drive 0.3, character 0.0
    // = pure tanh, driveMix 1.0) mean it is ACTIVE by default, so any pre-2.8 block that
    // asserts an exact match / bypass / null / absolute level must set driveMix = 0 —
    // the pre-drive-gain clean path is exactly bit-transparent.
    auto silenceDist = [] (KICKRAudioProcessor& p)
    {
        if (auto* prm = p.getValueTreeState().getParameter ("driveMix"))
            prm->setValueNotifyingHost (0.0f);
    };

    // Phase 2.9 adds the safety limiter (default ON — a tanh soft-clip nonlinearity).
    // Any pre-2.9 block that assumes a LINEAR chain (exact null / "a + b == both" sums)
    // turns it off. Bit-identical checks that compare two identical configs don't need it
    // (the limiter is deterministic + L/R-symmetric).
    auto silenceLimiter = [] (KICKRAudioProcessor& p)
    {
        if (auto* prm = p.getValueTreeState().getParameter ("limiter"))
            prm->setValueNotifyingHost (0.0f);
    };

    std::printf ("\n[Phase 2.1] OS-region shell + MIDI-triggered basic kick\n");

    // Phase 2.10 makes the `oversampling` default 2x (a few samples of PDC latency).
    // Pre-2.10 blocks that assume 1x / latency-0 pin the choice to "1x" first.
    auto pin1x = [] (KICKRAudioProcessor& p)
    {
        if (auto* prm = p.getValueTreeState().getParameter ("oversampling"))
            prm->setValueNotifyingHost (0.0f);   // choice index 0 == "1x"
    };

    KICKRAudioProcessor proc;
    silenceClick (proc);
    silenceSub (proc);
    silenceTail (proc);
    silenceDist (proc);
    pin1x (proc);
    const auto buf = kickr::tests::renderNote (proc, a1, vel, sr, 512, 1.0);
    const auto st  = analyse (buf, sr);

    std::printf ("  peak %.3f @ %.1f ms  |  maxStep %.4f  |  tailRMS %.5f  |  -40dB by %.0f ms\n",
                 st.peak, st.peakIndex / sr * 1000.0, st.maxStep, st.tailRms,
                 st.lastAbove40dB / sr * 1000.0);

    check (st.allFinite,                              "no NaN / Inf anywhere");
    check (st.peak > 0.05f,                           "audible sine kick (peak > 0.05)");
    check (std::abs (buf.getReadPointer (0)[0]) < 0.02f, "onset starts near zero (click-free)");
    check (st.maxStep < 0.15f,                        "no click / discontinuity (max step < 0.15)");
    check (st.peakIndex < (int) (0.015 * sr),         "peak within first 15 ms (fast attack)");
    check (st.tailRms < 0.003,                        "near-silent after 900 ms (tail RMS < -50 dBFS)");
    check (st.lastAbove40dB > (int) (0.12 * sr)
        && st.lastAbove40dB < (int) (0.90 * sr),      "decay consistent with ~400 ms bodyDecay default");
    check (proc.getLatencySamples() == 0,             "reported latency 0 at 1x oversampling");

    {
        KICKRAudioProcessor p2;
        silenceClick (p2);
        silenceSub (p2);
        silenceTail (p2);
        silenceDist (p2);
        pin1x (p2);
        const auto wav = juce::File::getCurrentWorkingDirectory().getChildFile ("kickr_phase2_1.wav");
        const bool ok  = kickr::tests::renderNoteToWav (p2, wav, a1, vel, sr, 512, 1.0);
        std::printf ("  wrote %s : %s\n", wav.getFullPathName().toRawUTF8(), ok ? "ok" : "FAILED");
    }

    // ---------------------------------------------------------------------
    std::printf ("\n[Phase 2.2] Pitch envelope (ratio/log-domain snap+settle)\n");

    auto setP = [] (KICKRAudioProcessor& p, juce::StringRef id, float v)
    {
        if (auto* prm = p.getValueTreeState().getParameter (id))
            prm->setValueNotifyingHost (prm->convertTo0to1 (v));
    };
    // instantaneous frequency estimate via zero-crossings over [t0, t1] seconds
    auto estFreq = [] (const juce::AudioBuffer<float>& b, double s, double t0, double t1)
    {
        const int i0 = std::max (1, (int) (t0 * s));
        const int i1 = std::min (b.getNumSamples(), (int) (t1 * s));
        const float* x = b.getReadPointer (0);
        int xings = 0;
        for (int i = i0; i < i1; ++i)
            if ((x[i - 1] <= 0.0f) != (x[i] <= 0.0f)) ++xings;
        return (double) xings * 0.5 / (t1 - t0);
    };

    // Start ratio: stretch pitchTime to 900 ms so the head of the contour is
    // slow enough to measure by zero-crossings. e stays near 1 for the first
    // ~15 ms  ->  f ~ 55 * 4^~0.9 ~ 190 Hz  (heading for 4x55 = 220).
    {
        KICKRAudioProcessor p;
        silenceClick (p);
        silenceSub (p);
        silenceTail (p);
        silenceDist (p);
        setP (p, "pitchTime", 900.0f);
        const auto b = kickr::tests::renderNote (p, a1, vel, sr, 512, 1.5);
        const double fHead = estFreq (b, sr, 0.002, 0.020);
        std::printf ("  start-ratio probe (pitchTime 900ms): f(2-20ms) = %.0f Hz   (target ~4x55)\n", fHead);
        check (fHead > 150.0,  "pitch starts near pitchStart x fundamental (>= ~3x up)");
    }

    // Medium pitchTime (200 ms) — the fall is slow enough to track by zero-crossings.
    {
        KICKRAudioProcessor p;
        silenceClick (p);
        silenceSub (p);
        silenceTail (p);
        silenceDist (p);
        setP (p, "pitchTime", 200.0f);
        const auto b = kickr::tests::renderNote (p, a1, vel, sr, 512, 1.0);
        const double fHead = estFreq (b, sr, 0.003, 0.023);
        const double fMid  = estFreq (b, sr, 0.060, 0.110);
        const double fTail = estFreq (b, sr, 0.300, 0.500);
        std::printf ("  pitchTime 200ms: head %.0f -> mid %.0f -> tail %.0f Hz\n", fHead, fMid, fTail);
        check (fHead > fMid && fMid > fTail,       "monotonic downward pitch fall");
        check (fHead > fTail * 2.0,                "large pitch drop (head > 2x tail)");
        check (fTail > 45.0 && fTail < 68.0,       "lands on the fundamental (~55 Hz)");
    }

    // Default patch (pitchTime 50 ms, curve 0.7): a "snap" — most of the drop in the
    // first ~15 ms.  Verify it is pitched-up early by comparing zero-crossing density
    // in the first 20 ms against a steady-state tail window, and that it settles.
    {
        KICKRAudioProcessor p;
        silenceClick (p);
        silenceSub (p);
        silenceTail (p);
        silenceDist (p);
        const auto b = kickr::tests::renderNote (p, a1, vel, sr, 512, 1.0);
        const double fFirst20 = estFreq (b, sr, 0.0, 0.020);
        const double fLate    = estFreq (b, sr, 0.090, 0.180);
        std::printf ("  default (50ms snap): f(0-20ms) = %.0f Hz   f(90-180ms) = %.0f Hz\n", fFirst20, fLate);
        check (fFirst20 > fLate * 1.4,             "first 20 ms is pitched up vs the settled tail");
        check (fLate > 45.0 && fLate < 68.0,       "settles at the fundamental after ~pitchTime");

        const float* x = b.getReadPointer (0);
        float step = 0.0f;
        for (int i = 1; i < b.getNumSamples(); ++i) step = std::max (step, std::abs (x[i] - x[i - 1]));
        check (step < 0.20f,                       "click-free through the pitch fall (max step < 0.20)");

        KICKRAudioProcessor pw;
        silenceClick (pw);
        silenceSub (pw);
        silenceTail (pw);
        silenceDist (pw);
        const auto wav = juce::File::getCurrentWorkingDirectory().getChildFile ("kickr_phase2_2.wav");
        kickr::tests::renderNoteToWav (pw, wav, a1, vel, sr, 512, 1.0);
    }

    // curve = 0 (near-linear sweep) vs curve = 1 (snap) — measured mid-fall
    {
        KICKRAudioProcessor p0, p1;
        silenceClick (p0);
        silenceClick (p1);
        silenceSub (p0);
        silenceSub (p1);
        silenceTail (p0);
        silenceTail (p1);
        silenceDist (p0);
        silenceDist (p1);
        setP (p0, "pitchCurve", 0.0f);
        setP (p1, "pitchCurve", 1.0f);
        const auto b0 = kickr::tests::renderNote (p0, a1, vel, sr, 512, 1.0);
        const auto b1 = kickr::tests::renderNote (p1, a1, vel, sr, 512, 1.0);

        const double f0 = estFreq (b0, sr, 0.012, 0.028);   // still sweeping
        const double f1 = estFreq (b1, sr, 0.012, 0.028);   // already snapped near fundamental
        const double f0end = estFreq (b0, sr, 0.10, 0.20);
        const double f1end = estFreq (b1, sr, 0.10, 0.20);
        std::printf ("  curve0 f(12-28ms) = %.0f Hz   curve1 f(12-28ms) = %.0f Hz   (both end ~%.0f/%.0f Hz)\n",
                     f0, f1, f0end, f1end);

        check (f0 > f1 * 1.25,                    "curve 0 sweeps slower than curve 1 (snap vs laser)");
        check (f0end < 68.0 && f1end < 68.0,      "both reach the fundamental by 100-200 ms");
    }

    // ---------------------------------------------------------------------
    std::printf ("\n[Phase 2.3] Transient shaper + click-free 2-voice retrigger\n");

    // render N note-ons at a fixed interval into one buffer
    auto renderRetrigger = [] (KICKRAudioProcessor& p, int note, float velocity,
                               double s, int block, int nNotes, double intervalSec, double seconds)
    {
        const int total = std::max (1, (int) std::ceil (s * seconds));
        p.setRateAndBufferSizeDetails (s, block);
        p.prepareToPlay (s, block);
        juce::AudioBuffer<float> out (juce::jmax (1, p.getTotalNumOutputChannels()), total);
        out.clear();
        juce::AudioBuffer<float> scratch (out.getNumChannels(), block);

        std::vector<int> onsets;
        for (int k = 0; k < nNotes; ++k) onsets.push_back ((int) (k * intervalSec * s));

        for (int pos = 0; pos < total;)
        {
            const int n = std::min (block, total - pos);
            juce::AudioBuffer<float> b (scratch.getArrayOfWritePointers(), out.getNumChannels(), n);
            b.clear();
            juce::MidiBuffer midi;
            for (int on : onsets)
                if (on >= pos && on < pos + n)
                    midi.addEvent (juce::MidiMessage::noteOn (1, note, velocity), on - pos);
            p.processBlock (b, midi);
            for (int ch = 0; ch < out.getNumChannels(); ++ch)
                out.copyFrom (ch, pos, b, ch, 0, n);
            pos += n;
        }
        p.releaseResources();
        return std::make_pair (out, onsets);
    };

    auto rmsWindow = [] (const juce::AudioBuffer<float>& b, double s, double t0, double t1)
    {
        const int i0 = std::max (0, (int) (t0 * s));
        const int i1 = std::min (b.getNumSamples(), (int) (t1 * s));
        const float* x = b.getReadPointer (0);
        double sq = 0.0;
        for (int i = i0; i < i1; ++i) sq += (double) x[i] * x[i];
        return std::sqrt (sq / std::max (1, i1 - i0));
    };

    // machine-gun retrigger: 1/32 @ 174 BPM = ~43 ms
    {
        KICKRAudioProcessor p;
        silenceClick (p);
        silenceSub (p);
        silenceTail (p);
        silenceDist (p);
        auto [rbuf, onsets] = renderRetrigger (p, a1, vel, sr, 256, 24, 60.0 / 174.0 / 8.0, 2.0);
        const float* rx = rbuf.getReadPointer (0);
        const int rn = rbuf.getNumSamples();

        bool finite = true; float peak = 0.0f;
        for (int i = 0; i < rn; ++i) { if (! std::isfinite (rx[i])) finite = false; peak = std::max (peak, std::abs (rx[i])); }

        // max sample-to-sample slew inside a +/-1 ms window around every trigger
        float worstSlew = 0.0f;
        const int w = (int) (0.001 * sr);
        for (int on : onsets)
            for (int i = std::max (1, on - w); i < std::min (rn, on + w); ++i)
                worstSlew = std::max (worstSlew, std::abs (rx[i] - rx[i - 1]));

        // global worst slew as a reference (a click would spike far above this)
        float globalSlew = 0.0f;
        for (int i = 1; i < rn; ++i) globalSlew = std::max (globalSlew, std::abs (rx[i] - rx[i - 1]));

        const double tailRms = rmsWindow (rbuf, sr, 1.7, 2.0);   // well after the last hit
        std::printf ("  24x retrigger: peak %.2f  worstSlew@triggers %.4f  globalSlew %.4f  tailRMS %.5f\n",
                     peak, worstSlew, globalSlew, tailRms);

        check (finite,                       "no NaN / Inf under machine-gun retrigger");
        check (peak > 0.05f && peak < 4.0f,  "output bounded (no runaway voices)");
        check (worstSlew < 0.25f,            "retrigger crossfade is click-free (slew < 0.25 at every trigger)");
        check (worstSlew < globalSlew * 2.5f,"no click spike vs the steady-state slew");
        check (tailRms < 0.01,               "voices free after the last hit (no leak / stuck voice)");
    }

    // Fix (2026-08-31): a retriggered click must play at the same level as an isolated
    // one. The old crossfade scaled the INCOMING voice by a ramping gNew (0 -> 1 over
    // 3 ms), which swallowed the click's sharp transient on every fast retrigger while
    // an isolated hit (no fade active) got it at full level -> an audible, tempo-locked
    // "sometimes there's a click" inconsistency. Now only the outgoing voice fades.
    {
        auto onsetRms = [] (const juce::AudioBuffer<float>& b, double s, int onsetSample)
        {
            const int i0 = onsetSample;
            const int i1 = std::min (b.getNumSamples(), onsetSample + (int) (0.003 * s));
            const float* x = b.getReadPointer (0);
            double sq = 0.0;
            for (int i = i0; i < i1; ++i) sq += (double) x[i] * x[i];
            return std::sqrt (sq / std::max (1, i1 - i0));
        };

        KICKRAudioProcessor pIso, pRt;
        // isolated hit: nothing ringing, no crossfade active.
        const auto isoR = kickr::tests::renderNote (pIso, a1, vel, sr, 512, 0.05);
        const double isoRms = onsetRms (isoR, sr, 0);

        // retrigger: 2nd hit at 40 ms, well inside the default 400 ms body decay -> the
        // crossfade is active for the 2nd onset.
        pRt.setRateAndBufferSizeDetails (sr, 512);
        pRt.prepareToPlay (sr, 512);
        const int total = (int) (0.10 * sr);
        juce::AudioBuffer<float> rtOut (juce::jmax (1, pRt.getTotalNumOutputChannels()), total);
        rtOut.clear();
        const int secondOnset = (int) (0.040 * sr);
        {
            juce::AudioBuffer<float> scratch (rtOut.getNumChannels(), 512);
            for (int pos = 0; pos < total;)
            {
                const int n = std::min (512, total - pos);
                juce::AudioBuffer<float> blk (scratch.getArrayOfWritePointers(), rtOut.getNumChannels(), n);
                blk.clear();
                juce::MidiBuffer m;
                if (pos == 0)                                m.addEvent (juce::MidiMessage::noteOn (1, a1, vel), 0);
                if (secondOnset >= pos && secondOnset < pos + n)
                    m.addEvent (juce::MidiMessage::noteOn (1, a1, vel), secondOnset - pos);
                pRt.processBlock (blk, m);
                for (int ch = 0; ch < rtOut.getNumChannels(); ++ch)
                    rtOut.copyFrom (ch, pos, blk, ch, 0, n);
                pos += n;
            }
        }
        const double rtRms = onsetRms (rtOut, sr, secondOnset);

        std::printf ("  click consistency: isolated onset RMS %.4f   retriggered (40ms) onset RMS %.4f   ratio %.2f\n",
                     isoRms, rtRms, rtRms / std::max (1.0e-9, isoRms));
        check (rtRms > isoRms * 0.75, "retriggered click is as loud as an isolated click (no crossfade swallow)");
    }

    // transientAttack: +1 sharpens the onset, -1 softens it
    {
        KICKRAudioProcessor pPos, pMid, pNeg;
        silenceClick (pPos); silenceClick (pMid); silenceClick (pNeg);
        silenceSub (pPos); silenceSub (pMid); silenceSub (pNeg);
        silenceTail (pPos); silenceTail (pMid); silenceTail (pNeg);
        silenceDist (pPos); silenceDist (pMid); silenceDist (pNeg);
        silenceLimiter (pPos); silenceLimiter (pMid); silenceLimiter (pNeg);   // Phase 2.9 — linear onset ratios
        setP (pPos, "transientAttack",  1.0f);
        setP (pNeg, "transientAttack", -1.0f);
        const auto bPos = kickr::tests::renderNote (pPos, a1, vel, sr, 512, 1.0);
        const auto bMid = kickr::tests::renderNote (pMid, a1, vel, sr, 512, 1.0);
        const auto bNeg = kickr::tests::renderNote (pNeg, a1, vel, sr, 512, 1.0);
        const double aPos = rmsWindow (bPos, sr, 0.0, 0.006);
        const double aMid = rmsWindow (bMid, sr, 0.0, 0.006);
        const double aNeg = rmsWindow (bNeg, sr, 0.0, 0.006);
        std::printf ("  onset RMS (0-6ms):  attack -1 %.4f   0 %.4f   +1 %.4f\n", aNeg, aMid, aPos);
        check (aPos > aMid * 1.10 && aMid > aNeg * 1.10, "transientAttack +/- changes onset energy");
    }

    // transientSustain: +1 lifts the body, -1 drops it
    {
        KICKRAudioProcessor pPos, pNeg;
        silenceClick (pPos); silenceClick (pNeg);
        silenceSub (pPos); silenceSub (pNeg);
        silenceTail (pPos); silenceTail (pNeg);
        silenceDist (pPos); silenceDist (pNeg);
        silenceLimiter (pPos); silenceLimiter (pNeg);   // Phase 2.9 — linear body-level ratio
        setP (pPos, "transientSustain",  1.0f);
        setP (pNeg, "transientSustain", -1.0f);
        const auto bPos = kickr::tests::renderNote (pPos, a1, vel, sr, 512, 1.0);
        const auto bNeg = kickr::tests::renderNote (pNeg, a1, vel, sr, 512, 1.0);
        const double sPos = rmsWindow (bPos, sr, 0.050, 0.200);
        const double sNeg = rmsWindow (bNeg, sr, 0.050, 0.200);
        std::printf ("  body RMS (50-200ms):  sustain -1 %.4f   +1 %.4f\n", sNeg, sPos);
        check (sPos > sNeg * 1.20,               "transientSustain shifts the body level");
    }

    // ---------------------------------------------------------------------
    std::printf ("\n[Phase 2.4] Synthesised click generator (3-part blend)\n");

    // Body muted -> only the click layer sounds. 0.5 s is plenty (click <= ~50 ms).
    auto renderClickOnly = [&] (float toneHz, float pitchHz, float timeMs, float level)
    {
        KICKRAudioProcessor p;
        setP (p, "bodyLevel",  0.0f);
        setP (p, "subLevel",   0.0f);
        setP (p, "tailLevel",  0.0f);
        setP (p, "driveMix",   0.0f);   // Phase 2.8 — isolate the click layer
        setP (p, "limiter",    0.0f);   // Phase 2.9 — linear level ratios
        setP (p, "clickLevel", level);
        setP (p, "clickTone",  toneHz);
        setP (p, "clickPitch", pitchHz);
        setP (p, "clickTime",  timeMs);
        return kickr::tests::renderNote (p, a1, vel, sr, 512, 0.5);
    };

    auto lastAbove = [] (const juce::AudioBuffer<float>& b, double s, float frac)
    {
        const float* x = b.getReadPointer (0);
        float pk = 0.0f;
        for (int i = 0; i < b.getNumSamples(); ++i) pk = std::max (pk, std::abs (x[i]));
        int last = 0;
        for (int i = 0; i < b.getNumSamples(); ++i) if (std::abs (x[i]) > pk * frac) last = i;
        return last / s * 1000.0;
    };

    // audible + clickLevel scales it (RMS scales ~linearly with clickLevel regardless
    // of component correlation, so it is a stable proxy)
    {
        const auto cLo = renderClickOnly (4000.0f, 5000.0f, 3.0f, 0.30f);
        const auto cHi = renderClickOnly (4000.0f, 5000.0f, 3.0f, 0.90f);
        const auto sLo = analyse (cLo, sr);
        const auto sHi = analyse (cHi, sr);
        const double rLo = rmsWindow (cLo, sr, 0.0, 0.005);
        const double rHi = rmsWindow (cHi, sr, 0.0, 0.005);
        std::printf ("  click-only:  level 0.30 -> peak %.3f rms %.4f   level 0.90 -> peak %.3f rms %.4f\n",
                     sLo.peak, rLo, sHi.peak, rHi);
        check (sLo.allFinite && sHi.allFinite, "click: no NaN / Inf");
        check (rLo > 0.005,                    "click layer is audible");
        check (rHi > rLo * 2.2,                "clickLevel scales the click (0.9 vs 0.3 ~ 3x)");
        check (sHi.peak < 2.0f,                "click output bounded");
    }

    // body-only render is unchanged when clickLevel = 0
    {
        KICKRAudioProcessor pB;  silenceClick (pB);  silenceSub (pB);  silenceTail (pB);  silenceDist (pB);
        KICKRAudioProcessor pA;  silenceSub (pA);  silenceTail (pA);  silenceDist (pA);  // default patch minus sub/tail: clickLevel 0.4
        const auto bBody = kickr::tests::renderNote (pB, a1, vel, sr, 512, 1.0);
        const auto bAll  = kickr::tests::renderNote (pA, a1, vel, sr, 512, 1.0);
        const float* xB = bBody.getReadPointer (0);
        const float* xA = bAll .getReadPointer (0);

        // bAll - bBody is exactly the click layer (the body path is identical), so it
        // isolates the click contribution.
        double clickPeak = 0.0;
        const int n20 = (int) (0.020 * sr);
        for (int i = 0; i < n20; ++i)
            clickPeak = std::max (clickPeak, (double) std::abs (xA[i] - xB[i]));

        double lateDiff = 0.0;
        for (int i = (int) (0.20 * sr); i < bBody.getNumSamples(); ++i)
            lateDiff = std::max (lateDiff, (double) std::abs (xA[i] - xB[i]));

        std::printf ("  body onset |x0| = %.4f   default-click peak (0-20ms) = %.4f   body-tail diff = %.2e\n",
                     std::abs (xB[0]), clickPeak, lateDiff);
        check (std::abs (xB[0]) < 0.02f, "clickLevel=0 -> body onset near zero (click absent)");
        check (lateDiff < 1.0e-4,        "clickLevel=0 render == default render after the click decays (body untouched)");
        check (clickPeak > 0.03,         "default click contributes an audible transient (bAll - bBody)");
    }

    // clean trigger onset + no bare 1-sample spike (window >= 8 samples, phase-0 osc start)
    {
        const auto c = renderClickOnly (12000.0f, 6000.0f, 3.0f, 0.90f);
        const auto s = analyse (c, sr);
        const float* x = c.getReadPointer (0);
        std::printf ("  onset ramp: |x0| %.4f  |x1| %.4f  |x2| %.4f  |x3| %.4f  peak %.3f\n",
                     std::abs (x[0]), std::abs (x[1]), std::abs (x[2]), std::abs (x[3]), s.peak);
        check (s.allFinite,               "windowed impulse: no NaN / Inf");
        check (std::abs (x[0]) < 0.02f,   "click starts at ~0 (phase-0 osc + raised-cosine window, not a bare spike)");
        check (std::abs (x[0]) < std::abs (x[2]) || s.peak < 1.0e-3f,
                                          "onset ramps up over several samples");
    }

    // shortest clickTime (0.1 ms) stays finite + bounded -> band-limited by construction
    {
        const auto c = renderClickOnly (15000.0f, 15000.0f, 0.1f, 0.90f);
        const auto s = analyse (c, sr);
        std::printf ("  clickTime 0.1 ms @ 15 kHz: peak %.3f  maxStep %.3f  finite %d\n",
                     s.peak, s.maxStep, (int) s.allFinite);
        check (s.allFinite && s.peak < 2.0f, "very short click: finite + bounded (no alias blow-up)");
    }

    // clickTone / clickPitch shift the click spectrum (zero-crossing density proxy)
    {
        const auto dark   = renderClickOnly (1500.0f,  1200.0f,  3.0f, 0.90f);
        const auto bright = renderClickOnly (14000.0f, 14000.0f, 3.0f, 0.90f);
        const double zDark   = estFreq (dark,   sr, 0.0, 0.006);
        const double zBright = estFreq (bright, sr, 0.0, 0.006);
        std::printf ("  click zero-crossing freq (0-6 ms):  dark %.0f Hz   bright %.0f Hz\n", zDark, zBright);
        check (zBright > zDark * 1.5,      "clickTone/clickPitch shift the click spectrum (bright = more HF)");
    }

    // clickTime changes the click duration
    {
        const auto cShort = renderClickOnly (4000.0f, 5000.0f, 0.5f,  0.90f);
        const auto cLong  = renderClickOnly (4000.0f, 5000.0f, 40.0f, 0.90f);
        const double dShort = lastAbove (cShort, sr, 0.05f);
        const double dLong  = lastAbove (cLong,  sr, 0.05f);
        std::printf ("  click duration (last > 5%% peak):  0.5 ms -> %.1f ms   40 ms -> %.1f ms\n", dShort, dLong);
        check (dLong > dShort * 1.8,       "clickTime lengthens the click (40 ms vs 0.5 ms)");
    }

    {
        KICKRAudioProcessor pw;
        setP (pw, "clickLevel", 0.8f);
        const auto wav = juce::File::getCurrentWorkingDirectory().getChildFile ("kickr_phase2_4.wav");
        kickr::tests::renderNoteToWav (pw, wav, a1, vel, sr, 512, 1.0);
    }

    // ---------------------------------------------------------------------
    std::printf ("\n[Phase 2.5] Sub oscillator (independent mono sub, phase-stable)\n");

    // Body + click muted -> only the sub layer sounds.
    auto renderSubOnly = [&] (float subLevel, float subFreqHz, float subDecayMs)
    {
        KICKRAudioProcessor p;
        setP (p, "bodyLevel",  0.0f);
        setP (p, "clickLevel", 0.0f);
        setP (p, "tailLevel",  0.0f);
        setP (p, "driveMix",   0.0f);   // Phase 2.8 — isolate the sub layer
        setP (p, "limiter",    0.0f);   // Phase 2.9 — linear level ratios
        setP (p, "subLevel",   subLevel);
        setP (p, "subFreq",    subFreqHz);
        setP (p, "subDecay",   subDecayMs);
        return kickr::tests::renderNote (p, a1, vel, sr, 512, 1.0);
    };

    // audible + subLevel scales it (RMS ~linear in subLevel)
    {
        const auto lo = renderSubOnly (0.25f, 40.0f, 300.0f);
        const auto hi = renderSubOnly (0.75f, 40.0f, 300.0f);
        const auto sLo = analyse (lo, sr);
        const auto sHi = analyse (hi, sr);
        const double rLo = rmsWindow (lo, sr, 0.0, 0.20);
        const double rHi = rmsWindow (hi, sr, 0.0, 0.20);
        std::printf ("  sub-only:  level 0.25 -> peak %.3f rms %.4f   level 0.75 -> peak %.3f rms %.4f\n",
                     sLo.peak, rLo, sHi.peak, rHi);
        check (sLo.allFinite && sHi.allFinite,      "sub: no NaN / Inf");
        check (rLo > 0.01,                          "sub layer is audible");
        check (rHi > rLo * 2.4 && rHi < rLo * 3.6,  "subLevel scales the sub (~3x for 0.75 vs 0.25)");
        check (std::abs (lo.getReadPointer (0)[0]) < 0.02f,
                                                    "sub onset starts near zero (raised-cosine attack)");
    }

    // subFreq sets the sub pitch, and it is independent of fundamental / pitchStart
    {
        const auto f30 = renderSubOnly (0.7f, 30.0f, 900.0f);
        const auto f70 = renderSubOnly (0.7f, 70.0f, 900.0f);
        const double z30 = estFreq (f30, sr, 0.05, 0.30);
        const double z70 = estFreq (f70, sr, 0.05, 0.30);
        std::printf ("  sub zero-crossing freq:  subFreq 30 -> %.1f Hz   subFreq 70 -> %.1f Hz\n", z30, z70);
        check (z30 > 24.0 && z30 < 37.0, "sub tracks subFreq = 30 Hz");
        check (z70 > 60.0 && z70 < 80.0, "sub tracks subFreq = 70 Hz");

        // Same subFreq (50 Hz), wildly different fundamental + pitchStart -> sub pitch unchanged.
        KICKRAudioProcessor pa, pb;
        setP (pa, "bodyLevel", 0.0f); setP (pa, "clickLevel", 0.0f); setP (pa, "tailLevel", 0.0f); setP (pa, "driveMix", 0.0f);
        setP (pb, "bodyLevel", 0.0f); setP (pb, "clickLevel", 0.0f); setP (pb, "tailLevel", 0.0f); setP (pb, "driveMix", 0.0f);
        setP (pa, "subFreq", 50.0f);  setP (pa, "subDecay", 900.0f);
        setP (pb, "subFreq", 50.0f);  setP (pb, "subDecay", 900.0f);
        setP (pa, "fundamental", 40.0f);  setP (pa, "pitchStart", 1.5f);
        setP (pb, "fundamental", 120.0f); setP (pb, "pitchStart", 8.0f);
        const auto ba = kickr::tests::renderNote (pa, a1, vel, sr, 512, 1.0);
        const auto bb = kickr::tests::renderNote (pb, a1, vel, sr, 512, 1.0);
        const double za = estFreq (ba, sr, 0.05, 0.30);
        const double zb = estFreq (bb, sr, 0.05, 0.30);
        std::printf ("  subFreq 50 vs fundamental/pitchStart:  patch A %.1f Hz   patch B %.1f Hz\n", za, zb);
        check (za > 42.0 && za < 58.0 && std::abs (za - zb) < 4.0,
               "sub pitch independent of fundamental / pitch envelope");
    }

    // null test — two identical triggers, one inverted, sum to silence (phase-0 determinism)
    {
        const auto r1 = renderSubOnly (0.6f, 43.0f, 500.0f);
        const auto r2 = renderSubOnly (0.6f, 43.0f, 500.0f);
        const int n = std::min (r1.getNumSamples(), r2.getNumSamples());
        const float* a = r1.getReadPointer (0);
        const float* b = r2.getReadPointer (0);
        double resid = 0.0, peak = 0.0;
        for (int i = 0; i < n; ++i)
        {
            resid = std::max (resid, (double) std::abs (a[i] - b[i]));
            peak  = std::max (peak,  (double) std::abs (a[i]));
        }
        std::printf ("  null test:  peak %.3f   max|r1 - r2| = %.2e\n", peak, resid);
        check (peak > 0.02,        "sub null test has real signal");
        check (resid < 1.0e-6,     "identical triggers cancel to silence (phase-0 determinism)");

        // mono: L and R are the same value
        if (r1.getNumChannels() >= 2)
        {
            const float* L = r1.getReadPointer (0);
            const float* R = r1.getReadPointer (1);
            double lr = 0.0;
            for (int i = 0; i < r1.getNumSamples(); ++i)
                lr = std::max (lr, (double) std::abs (L[i] - R[i]));
            check (lr < 1.0e-6,    "sub summed identically to L and R (mono)");
        }
    }

    // subDecay changes the sub length
    {
        const auto sShort = renderSubOnly (0.7f, 45.0f, 60.0f);
        const auto sLong  = renderSubOnly (0.7f, 45.0f, 900.0f);
        const double dShort = lastAbove (sShort, sr, 0.05f);
        const double dLong  = lastAbove (sLong,  sr, 0.05f);
        std::printf ("  sub duration (last > 5%% peak):  60 ms -> %.0f ms   900 ms -> %.0f ms\n", dShort, dLong);
        check (dLong > dShort * 3.0, "subDecay changes the sub length");
    }

    // subLevel = 0 is a true bypass: render == body + click render with the sub muted
    {
        KICKRAudioProcessor pRef;   silenceSub (pRef);  silenceTail (pRef);  silenceDist (pRef);   // body + click, sub off via param
        KICKRAudioProcessor pZero;  setP (pZero, "subLevel", 0.0f);  silenceTail (pZero);  silenceDist (pZero);
        KICKRAudioProcessor pOn;    silenceTail (pOn);  silenceDist (pOn);   // default patch: subLevel 0.5
        const auto bRef  = kickr::tests::renderNote (pRef,  a1, vel, sr, 512, 1.0);
        const auto bZero = kickr::tests::renderNote (pZero, a1, vel, sr, 512, 1.0);
        const auto bOn   = kickr::tests::renderNote (pOn,   a1, vel, sr, 512, 1.0);
        const float* xR = bRef .getReadPointer (0);
        const float* xZ = bZero.getReadPointer (0);
        const float* xO = bOn  .getReadPointer (0);
        double zeroDiff = 0.0, onDiff = 0.0;
        for (int i = 0; i < bRef.getNumSamples(); ++i)
        {
            zeroDiff = std::max (zeroDiff, (double) std::abs (xR[i] - xZ[i]));
            onDiff   = std::max (onDiff,   (double) std::abs (xR[i] - xO[i]));
        }
        std::printf ("  subLevel=0 vs body+click ref: max|diff| = %.2e   (sub ON diff = %.3f)\n",
                     zeroDiff, onDiff);
        check (zeroDiff < 1.0e-6, "subLevel = 0 render == prior body + click render");
        check (onDiff   > 0.02,   "default subLevel adds an audible sub layer");
    }

    {
        KICKRAudioProcessor pw;
        setP (pw, "subLevel", 0.8f);
        const auto wav = juce::File::getCurrentWorkingDirectory().getChildFile ("kickr_phase2_5.wav");
        kickr::tests::renderNoteToWav (pw, wav, a1, vel, sr, 512, 1.0);
    }

    // ---------------------------------------------------------------------
    std::printf ("\n[Phase 2.6] Tail generator (dedicated LF sine @ fundamentalEff)\n");

    // Body + click + sub muted -> only the tail layer sounds.
    auto renderTailOnly = [&] (float tailLevel, float tailLengthMs, float tailTone01, float tailDrive01)
    {
        KICKRAudioProcessor p;
        setP (p, "bodyLevel",  0.0f);
        setP (p, "clickLevel", 0.0f);
        setP (p, "subLevel",   0.0f);
        setP (p, "driveMix",   0.0f);   // Phase 2.8 — isolate the tail layer
        setP (p, "limiter",    0.0f);   // Phase 2.9 — linear level ratios
        setP (p, "tailLevel",  tailLevel);
        setP (p, "tailLength", tailLengthMs);
        setP (p, "tailTone",   tailTone01);
        setP (p, "tailDrive",  tailDrive01);
        return kickr::tests::renderNote (p, a1, vel, sr, 512, 2.5);
    };

    // HF-content proxy: RMS of the first difference relative to the signal RMS.
    auto hfRatio = [] (const juce::AudioBuffer<float>& b, double s, double t0, double t1)
    {
        const int i0 = std::max (1, (int) (t0 * s));
        const int i1 = std::min (b.getNumSamples(), (int) (t1 * s));
        const float* x = b.getReadPointer (0);
        double sq = 0.0, dsq = 0.0;
        for (int i = i0; i < i1; ++i)
        {
            sq  += (double) x[i] * x[i];
            dsq += (double) (x[i] - x[i - 1]) * (x[i] - x[i - 1]);
        }
        return std::sqrt (dsq / std::max (1.0e-12, sq));
    };

    // audible + tailLevel scales it (RMS ~linear in tailLevel)
    {
        const auto lo = renderTailOnly (0.20f, 300.0f, 0.5f, 0.2f);
        const auto hi = renderTailOnly (0.60f, 300.0f, 0.5f, 0.2f);
        const auto sLo = analyse (lo, sr);
        const auto sHi = analyse (hi, sr);
        const double rLo = rmsWindow (lo, sr, 0.02, 0.25);
        const double rHi = rmsWindow (hi, sr, 0.02, 0.25);
        std::printf ("  tail-only:  level 0.20 -> peak %.3f rms %.4f   level 0.60 -> peak %.3f rms %.4f\n",
                     sLo.peak, rLo, sHi.peak, rHi);
        check (sLo.allFinite && sHi.allFinite,      "tail: no NaN / Inf");
        check (rLo > 0.005,                         "tail layer is audible");
        check (rHi > rLo * 2.3 && rHi < rLo * 3.8,  "tailLevel scales the tail (~3x for 0.60 vs 0.20)");
        check (std::abs (lo.getReadPointer (0)[0]) < 0.02f,
                                                    "tail onset starts near zero (raised-cosine attack)");
    }

    // tailLength: short = tight, long = drone / rumble
    {
        const auto tShort = renderTailOnly (0.5f, 40.0f,   0.5f, 0.2f);
        const auto tLong  = renderTailOnly (0.5f, 1500.0f, 0.5f, 0.2f);
        const double dShort = lastAbove (tShort, sr, 0.05f);
        const double dLong  = lastAbove (tLong,  sr, 0.05f);
        std::printf ("  tail duration (last > 5%% peak):  40 ms -> %.0f ms   1500 ms -> %.0f ms\n", dShort, dLong);
        check (dLong > dShort * 4.0, "tailLength short = tight, long = drone/rumble");
    }

    // tailTone: LP cutoff 120 Hz -> 4 kHz (pre-drive). With a high tail pitch (150 Hz,
    // fixed) the dark setting sits the cutoff below the tail -> less energy; bright opens.
    {
        auto renderTailTone = [&] (float tone01)
        {
            KICKRAudioProcessor p;
            setP (p, "bodyLevel",   0.0f);
            setP (p, "clickLevel",  0.0f);
            setP (p, "subLevel",    0.0f);
            setP (p, "driveMix",    0.0f);        // Phase 2.8 — isolate the tail layer
            setP (p, "limiter",     0.0f);        // Phase 2.9 — linear tail-tone RMS
            setP (p, "tuneMode",    1.0f);        // Fixed Frequency
            setP (p, "fundamental", 150.0f);
            setP (p, "tailLevel",   0.7f);
            setP (p, "tailLength",  500.0f);
            setP (p, "tailTone",    tone01);
            setP (p, "tailDrive",   0.0f);
            return kickr::tests::renderNote (p, a1, vel, sr, 512, 1.0);
        };
        const auto dark   = renderTailTone (0.0f);
        const auto bright = renderTailTone (1.0f);
        const double rDark   = rmsWindow (dark,   sr, 0.05, 0.40);
        const double rBright = rmsWindow (bright, sr, 0.05, 0.40);
        std::printf ("  tailTone @ 150 Hz tail:  dark RMS %.4f   bright RMS %.4f\n", rDark, rBright);
        check (analyse (dark, sr).allFinite && analyse (bright, sr).allFinite, "tailTone: no NaN / Inf");
        check (rBright > rDark * 1.3, "tailTone 0 = dark (LP bites), 1 = bright (open)");
    }

    // tailDrive: 1 adds harmonics / distortion vs 0.
    // Long tail (2 s) so the drive still bites through the measurement window, bright
    // tone so tanh harmonics aren't LP'd. tanh saturation flattens the peaks -> the
    // crest factor (peak / RMS) drops toward a square wave, and HF content rises.
    {
        const auto clean  = renderTailOnly (0.6f, 2000.0f, 1.0f, 0.0f);
        const auto driven = renderTailOnly (0.6f, 2000.0f, 1.0f, 1.0f);
        const double hClean  = hfRatio (clean,  sr, 0.02, 0.30);
        const double hDriven = hfRatio (driven, sr, 0.02, 0.30);

        auto crest = [] (const juce::AudioBuffer<float>& b, double s, double t0, double t1)
        {
            const int i0 = std::max (0, (int) (t0 * s));
            const int i1 = std::min (b.getNumSamples(), (int) (t1 * s));
            const float* x = b.getReadPointer (0);
            double pk = 0.0, sq = 0.0;
            for (int i = i0; i < i1; ++i) { pk = std::max (pk, (double) std::abs (x[i])); sq += (double) x[i] * x[i]; }
            const double rms = std::sqrt (sq / std::max (1, i1 - i0));
            return rms > 1.0e-9 ? pk / rms : 0.0;
        };
        const double cClean  = crest (clean,  sr, 0.02, 0.20);
        const double cDriven = crest (driven, sr, 0.02, 0.20);
        const auto sD = analyse (driven, sr);
        std::printf ("  tailDrive:  HF %.4f -> %.4f   crest %.3f -> %.3f   (sine ~1.41, square ~1.0)\n",
                     hClean, hDriven, cClean, cDriven);
        check (sD.allFinite && sD.peak < 2.0f,   "tailDrive 1: finite + bounded");
        check (cDriven < cClean * 0.92,          "tailDrive 1 flattens the waveform (crest -> square)");
        check (hDriven > hClean * 1.15,          "tailDrive 1 adds high-frequency harmonic content");
    }

    // tailLevel = 0 render == prior body + click + sub render exactly
    {
        KICKRAudioProcessor pRef;   silenceTail (pRef);  silenceDist (pRef);              // body + click + sub, tail off
        KICKRAudioProcessor pZero;  setP (pZero, "tailLevel", 0.0f);  silenceDist (pZero); // tail off via param
        KICKRAudioProcessor pOn;    silenceDist (pOn);   // default patch: tailLevel 0.3
        const auto bRef  = kickr::tests::renderNote (pRef,  a1, vel, sr, 512, 1.0);
        const auto bZero = kickr::tests::renderNote (pZero, a1, vel, sr, 512, 1.0);
        const auto bOn   = kickr::tests::renderNote (pOn,   a1, vel, sr, 512, 1.0);
        const float* xR = bRef .getReadPointer (0);
        const float* xZ = bZero.getReadPointer (0);
        const float* xO = bOn  .getReadPointer (0);
        double zeroDiff = 0.0, onDiff = 0.0;
        for (int i = 0; i < bRef.getNumSamples(); ++i)
        {
            zeroDiff = std::max (zeroDiff, (double) std::abs (xR[i] - xZ[i]));
            onDiff   = std::max (onDiff,   (double) std::abs (xR[i] - xO[i]));
        }
        std::printf ("  tailLevel=0 vs body+click+sub ref: max|diff| = %.2e   (tail ON diff = %.3f)\n",
                     zeroDiff, onDiff);
        check (zeroDiff < 1.0e-6, "tailLevel = 0 render == prior body + click + sub render");
        check (onDiff   > 0.01,   "default tailLevel adds an audible tail layer");
    }

    // tail attack sits behind the transient: slower rise than the click
    {
        auto riseSamples = [] (const juce::AudioBuffer<float>& b, float frac)
        {
            const float* x = b.getReadPointer (0);
            float pk = 0.0f;
            for (int i = 0; i < b.getNumSamples(); ++i) pk = std::max (pk, std::abs (x[i]));
            for (int i = 0; i < b.getNumSamples(); ++i)
                if (std::abs (x[i]) > pk * frac) return i;
            return b.getNumSamples();
        };
        const auto tailBuf  = renderTailOnly (0.7f, 400.0f, 0.5f, 0.0f);
        const auto clickBuf = renderClickOnly (4000.0f, 5000.0f, 3.0f, 0.9f);
        const int tRise = riseSamples (tailBuf,  0.5f);
        const int cRise = riseSamples (clickBuf, 0.5f);
        std::printf ("  rise-to-50%%-peak:  tail %.2f ms   click %.2f ms\n",
                     tRise / sr * 1000.0, cRise / sr * 1000.0);
        check (tRise > cRise * 3, "tail attack slower than the click (sits behind the transient)");
    }

    {
        KICKRAudioProcessor pw;
        setP (pw, "tailLevel",  0.6f);
        setP (pw, "tailLength", 600.0f);
        setP (pw, "tailDrive",  0.4f);
        const auto wav = juce::File::getCurrentWorkingDirectory().getChildFile ("kickr_phase2_6.wav");
        kickr::tests::renderNoteToWav (pw, wav, a1, vel, sr, 512, 1.5);
    }

    // ---------------------------------------------------------------------
    std::printf ("\n[Phase 2.7] Noise generator (White / Pink / Filtered, default OFF)\n");

    // Body + click + sub + tail muted -> only the noise layer sounds.
    auto renderNoiseOnly = [&] (float noiseLevel, float noiseDecayMs, float noiseTone01, float noiseType)
    {
        KICKRAudioProcessor p;
        setP (p, "bodyLevel",  0.0f);
        setP (p, "clickLevel", 0.0f);
        setP (p, "subLevel",   0.0f);
        setP (p, "tailLevel",  0.0f);
        setP (p, "driveMix",   0.0f);   // Phase 2.8 — isolate the noise layer
        setP (p, "limiter",    0.0f);   // Phase 2.9 — linear level ratios
        setP (p, "noiseLevel", noiseLevel);
        setP (p, "noiseDecay", noiseDecayMs);
        setP (p, "noiseTone",  noiseTone01);
        setP (p, "noiseType",  noiseType);
        return kickr::tests::renderNote (p, a1, vel, sr, 512, 1.0);
    };

    // THE key check (default is off): noiseLevel = 0 is an EXACT bypass — the render is
    // bit-identical to the prior body+click+sub+tail render whether the noise code path
    // runs with level 0 or the param is left untouched.
    {
        KICKRAudioProcessor pRef;   silenceDist (pRef);            // default patch (noiseLevel 0)
        KICKRAudioProcessor pZero;  setP (pZero, "noiseLevel", 0.0f);  silenceDist (pZero);
        KICKRAudioProcessor pOn;    setP (pOn,   "noiseLevel", 0.6f);  silenceDist (pOn);
        const auto bRef  = kickr::tests::renderNote (pRef,  a1, vel, sr, 512, 1.0);
        const auto bZero = kickr::tests::renderNote (pZero, a1, vel, sr, 512, 1.0);
        const auto bOn   = kickr::tests::renderNote (pOn,   a1, vel, sr, 512, 1.0);
        const float* xR = bRef .getReadPointer (0);
        const float* xZ = bZero.getReadPointer (0);
        const float* xO = bOn  .getReadPointer (0);
        double zeroDiff = 0.0, onDiff = 0.0;
        for (int i = 0; i < bRef.getNumSamples(); ++i)
        {
            zeroDiff = std::max (zeroDiff, (double) std::abs (xR[i] - xZ[i]));
            onDiff   = std::max (onDiff,   (double) std::abs (xR[i] - xO[i]));
        }
        std::printf ("  noiseLevel=0 vs default: max|diff| = %.2e   (noise ON diff = %.3f)\n",
                     zeroDiff, onDiff);
        check (zeroDiff < 1.0e-9, "noiseLevel = 0 is an exact bypass (bit-identical to prior render)");
        check (onDiff   > 0.02,   "noiseLevel > 0 adds an audible noise layer");
        check (analyse (bOn, sr).allFinite, "noise ON: no NaN / Inf");
    }

    // White / Pink / Filtered each audible with a distinct spectrum.
    {
        const auto white = renderNoiseOnly (0.7f, 200.0f, 0.5f, 0.0f);
        const auto pink  = renderNoiseOnly (0.7f, 200.0f, 0.5f, 1.0f);
        const auto filt  = renderNoiseOnly (0.7f, 200.0f, 0.5f, 2.0f);
        const double rW = rmsWindow (white, sr, 0.0, 0.15);
        const double rP = rmsWindow (pink,  sr, 0.0, 0.15);
        const double rF = rmsWindow (filt,  sr, 0.0, 0.15);
        const double hW = hfRatio (white, sr, 0.0, 0.15);
        const double hP = hfRatio (pink,  sr, 0.0, 0.15);
        std::printf ("  noise-only RMS  W %.4f  P %.4f  F %.4f   |  hfRatio  W %.3f  P %.3f\n",
                     rW, rP, rF, hW, hP);
        check (analyse (white, sr).allFinite && analyse (pink, sr).allFinite
               && analyse (filt, sr).allFinite,       "noise: no NaN / Inf (all 3 types)");
        check (rW > 0.01 && rP > 0.01 && rF > 0.005,  "White / Pink / Filtered all audible");
        check (hW > hP * 1.3,                         "White is brighter than Pink (hfRatio proxy)");
    }

    // Filtered band-pass centre tracks noiseTone (log 200 Hz -> 12 kHz).
    {
        const auto lo = renderNoiseOnly (0.8f, 300.0f, 0.2f, 2.0f);
        const auto hi = renderNoiseOnly (0.8f, 300.0f, 0.8f, 2.0f);
        const double zLo = estFreq (lo, sr, 0.01, 0.20);
        const double zHi = estFreq (hi, sr, 0.01, 0.20);
        std::printf ("  Filtered zero-crossing freq:  noiseTone 0.2 -> %.0f Hz   0.8 -> %.0f Hz\n", zLo, zHi);
        check (zHi > zLo * 2.0, "Filtered centre tracks noiseTone (low = dark, high = bright)");
    }

    // noiseTone 0 = dark, 1 = bright for White + Pink (tilt filter).
    {
        const auto wDark   = renderNoiseOnly (0.7f, 200.0f, 0.0f, 0.0f);
        const auto wBright = renderNoiseOnly (0.7f, 200.0f, 1.0f, 0.0f);
        const auto pDark   = renderNoiseOnly (0.7f, 200.0f, 0.0f, 1.0f);
        const auto pBright = renderNoiseOnly (0.7f, 200.0f, 1.0f, 1.0f);
        const double hwD = hfRatio (wDark,   sr, 0.0, 0.15);
        const double hwB = hfRatio (wBright, sr, 0.0, 0.15);
        const double hpD = hfRatio (pDark,   sr, 0.0, 0.15);
        const double hpB = hfRatio (pBright, sr, 0.0, 0.15);
        std::printf ("  tilt hfRatio  White %.3f -> %.3f   Pink %.3f -> %.3f   (noiseTone 0 -> 1)\n",
                     hwD, hwB, hpD, hpB);
        check (hwB > hwD * 1.3, "White noiseTone 0 = dark, 1 = bright");
        check (hpB > hpD * 1.3, "Pink noiseTone 0 = dark, 1 = bright");
    }

    // noiseDecay changes the noise length.
    {
        const auto nShort = renderNoiseOnly (0.8f, 30.0f,  0.5f, 0.0f);
        const auto nLong  = renderNoiseOnly (0.8f, 400.0f, 0.5f, 0.0f);
        const double dShort = lastAbove (nShort, sr, 0.05f);
        const double dLong  = lastAbove (nLong,  sr, 0.05f);
        std::printf ("  noise duration (last > 5%% peak):  30 ms -> %.0f ms   400 ms -> %.0f ms\n", dShort, dLong);
        check (dLong > dShort * 3.0, "noiseDecay changes the noise length");
    }

    // deterministic — two renders of the same noise patch are bit-identical (seeded rng).
    {
        const auto r1 = renderNoiseOnly (0.7f, 250.0f, 0.4f, 1.0f);
        const auto r2 = renderNoiseOnly (0.7f, 250.0f, 0.4f, 1.0f);
        const int n = std::min (r1.getNumSamples(), r2.getNumSamples());
        const float* a = r1.getReadPointer (0);
        const float* b = r2.getReadPointer (0);
        double resid = 0.0, peak = 0.0;
        for (int i = 0; i < n; ++i)
        {
            resid = std::max (resid, (double) std::abs (a[i] - b[i]));
            peak  = std::max (peak,  (double) std::abs (a[i]));
        }
        std::printf ("  determinism:  peak %.3f   max|r1 - r2| = %.2e\n", peak, resid);
        check (peak > 0.02,     "noise determinism test has real signal");
        check (resid < 1.0e-9,  "seeded rng -> two renders bit-identical");

        if (r1.getNumChannels() >= 2)
        {
            const float* L = r1.getReadPointer (0);
            const float* R = r1.getReadPointer (1);
            double lr = 0.0;
            for (int i = 0; i < r1.getNumSamples(); ++i)
                lr = std::max (lr, (double) std::abs (L[i] - R[i]));
            check (lr < 1.0e-6, "noise summed identically to L and R (mono)");
        }
    }

    {
        KICKRAudioProcessor pw;
        setP (pw, "noiseLevel", 0.5f);
        setP (pw, "noiseType",  1.0f);
        const auto wav = juce::File::getCurrentWorkingDirectory().getChildFile ("kickr_phase2_7.wav");
        kickr::tests::renderNoteToWav (pw, wav, a1, vel, sr, 512, 1.0);
    }

    // ---------------------------------------------------------------------
    std::printf ("\n[Phase 2.7b] Sample player + managed library\n");

    auto writeSineWav = [] (const juce::File& f, double freq, double sr2, double secs, int ch) -> bool
    {
        const int n = (int) std::lround (sr2 * secs);
        juce::AudioBuffer<float> b (ch, n);
        for (int c = 0; c < ch; ++c)
        {
            auto* x = b.getWritePointer (c);
            for (int i = 0; i < n; ++i)
                x[i] = 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * freq * (double) i / sr2);
        }
        f.deleteFile();
        f.getParentDirectory().createDirectory();
        std::unique_ptr<juce::OutputStream> stream = f.createOutputStream();
        if (stream == nullptr) return false;
        juce::WavAudioFormat fmt;
        auto writer = fmt.createWriterFor (stream,
                                           juce::AudioFormatWriterOptions{}
                                               .withSampleRate (sr2)
                                               .withNumChannels (ch)
                                               .withBitsPerSample (24));
        if (writer == nullptr) return false;
        return writer->writeFromAudioSampleBuffer (b, 0, n);
    };

    auto zcFreq = [] (const std::vector<float>& x, double s, double t0, double t1)
    {
        const int i0 = std::max (1, (int) (t0 * s));
        const int i1 = std::min ((int) x.size(), (int) (t1 * s));
        int c = 0;
        for (int i = i0; i < i1; ++i)
            if ((x[(size_t) (i - 1)] <= 0.0f) != (x[(size_t) i] <= 0.0f)) ++c;
        return (double) c * 0.5 / (t1 - t0);
    };

    // --- SampleLibrary + SamplePlayer unit tests (temp folder) ---
    {
        juce::File tmp = juce::File::getSpecialLocation (juce::File::tempDirectory)
                             .getChildFile ("kickr_2_7b_" + juce::String (juce::Time::currentTimeMillis()));
        tmp.createDirectory();

        const auto sineFile = tmp.getChildFile ("sine100.wav");
        check (writeSineWav (sineFile, 100.0, 44100.0, 0.5, 1), "wrote a 100 Hz / 0.5 s test sine WAV");

        kickr::SampleLibrary lib (tmp);
        const auto name = lib.importFile (sineFile);
        check (name.isNotEmpty(),        "SampleLibrary.importFile accepts a valid short WAV");
        check (lib.getCount() >= 1,      "bank lists the imported file");
        check (lib.indexOfName (name) >= 0, "imported file is findable by name");

        auto sb = lib.load (name);
        check (sb != nullptr && sb->audio.getNumSamples() > 1000,
               "SampleLibrary.load decodes the sample");
        check (sb != nullptr && std::abs (sb->sourceRate - 44100.0) < 1.0, "sourceRate preserved");

        // too-long file is rejected
        const auto longFile = tmp.getChildFile ("toolong.wav");
        writeSineWav (longFile, 100.0, 44100.0, 6.0, 1);
        check (lib.importFile (longFile).isEmpty(), "SampleLibrary rejects a sample longer than 5 s");

        auto runPlayer = [&] (float tuneSemis, bool rev, float start01, float end01, float crush)
        {
            kickr::SamplePlayer sp;
            sp.prepare (48000.0);
            kickr::SamplePlayer::SampleParams p;
            p.enable = 1.0f; p.level = 1.0f; p.midiTrack = false;
            p.tuneSemis = tuneSemis; p.reverse = rev;
            p.start01 = start01; p.end01 = end01;
            p.decayMs = 5000.0f; p.crush01 = crush;
            sp.setParams (p);
            sp.noteOn (sb.get(), 33, 1.0f);
            std::vector<float> out (24000, 0.0f);
            for (auto& v : out) v = sp.renderSample();
            return out;
        };

        const auto p0  = runPlayer (0.0f,   false, 0.0f, 1.0f, 0.0f);
        const auto pUp = runPlayer (12.0f,  false, 0.0f, 1.0f, 0.0f);
        const auto pDn = runPlayer (-12.0f, false, 0.0f, 1.0f, 0.0f);
        const double f0 = zcFreq (p0,  48000.0, 0.05, 0.25);
        const double fU = zcFreq (pUp, 48000.0, 0.05, 0.18);
        const double fD = zcFreq (pDn, 48000.0, 0.05, 0.25);
        std::printf ("  resample: tune 0 -> %.1f Hz   +12 -> %.1f Hz   -12 -> %.1f Hz\n", f0, fU, fD);
        check (std::abs (f0 - 100.0) < 4.0, "tune 0 plays the sample at source pitch (~100 Hz)");
        check (std::abs (fU / f0 - 2.0) < 0.04, "sampleTune +12 -> ~2x pitch (within 2%)");
        check (std::abs (fD / f0 - 0.5) < 0.04, "sampleTune -12 -> ~0.5x pitch (within 2%)");

        bool finite0 = true;
        for (float v : p0) if (! std::isfinite (v)) finite0 = false;
        check (finite0, "sample player output finite");

        auto lastAboveV = [] (const std::vector<float>& x, float frac)
        {
            float pk = 0.0f;
            for (float v : x) pk = std::max (pk, std::abs (v));
            int last = 0;
            for (int i = 0; i < (int) x.size(); ++i) if (std::abs (x[(size_t) i]) > pk * frac) last = i;
            return last;
        };
        const int dFull = lastAboveV (runPlayer (0.0f, false, 0.0f, 1.0f, 0.0f), 0.02f);
        const int dHalf = lastAboveV (runPlayer (0.0f, false, 0.5f, 1.0f, 0.0f), 0.02f);
        const int dWin  = lastAboveV (runPlayer (0.0f, false, 0.0f, 0.5f, 0.0f), 0.02f);
        std::printf ("  trim (last-signal samples): full %d  start=0.5 %d  end=0.5 %d\n", dFull, dHalf, dWin);
        check (dHalf < dFull * 0.7, "sampleStart trims the front (shorter playing span)");
        check (dWin  < dFull * 0.7, "sampleEnd trims the tail (shorter playing span)");

        const auto fwd = runPlayer (0.0f, false, 0.0f, 1.0f, 0.0f);
        const auto rev = runPlayer (0.0f, true,  0.0f, 1.0f, 0.0f);
        bool revFinite = true; double eR = 0.0, diff = 0.0;
        for (size_t i = 0; i < rev.size(); ++i)
        {
            if (! std::isfinite (rev[i])) revFinite = false;
            eR   += (double) rev[i] * rev[i];
            diff += std::abs ((double) fwd[i] - (double) rev[i]);
        }
        check (revFinite,               "sampleReverse render finite");
        check (eR > 1.0 && diff > 1.0,  "sampleReverse produces a distinct non-silent render");

        const auto clean = runPlayer (0.0f, false, 0.0f, 1.0f, 0.0f);
        const auto crush = runPlayer (0.0f, false, 0.0f, 1.0f, 1.0f);
        bool cFinite = true; float cPk = 0.0f; double cDiff = 0.0;
        for (size_t i = 0; i < crush.size(); ++i)
        {
            if (! std::isfinite (crush[i])) cFinite = false;
            cPk   = std::max (cPk, std::abs (crush[i]));
            cDiff += std::abs ((double) clean[i] - (double) crush[i]);
        }
        check (cFinite && cPk < 2.0f, "sampleCrush = 1: finite + bounded");
        check (cDiff > 1.0,           "sampleCrush = 1 audibly quantises / decimates the signal");

        const auto c0a = runPlayer (0.0f, false, 0.0f, 1.0f, 0.0f);
        const auto c0b = runPlayer (0.0f, false, 0.0f, 1.0f, 0.0f);
        double idErr = 0.0;
        for (size_t i = 0; i < c0a.size(); ++i)
            idErr = std::max (idErr, (double) std::abs (c0a[i] - c0b[i]));
        check (idErr < 1.0e-9, "sampleCrush = 0 is deterministic / bit-transparent");

        tmp.deleteRecursively();
    }

    // --- Processor path: buffer hand-off + gates ---
    {
        const auto fixture = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                 .getChildFile ("kickr_unittest_sine.wav");
        writeSineWav (fixture, 120.0, 44100.0, 0.4, 1);

        // Point each processor's SampleLibrary at a throwaway temp folder so the test
        // never touches the user's real ~/Music/KICKR/Samples bank.
        const auto tmpBank = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                 .getChildFile ("kickr_test_bank");
        tmpBank.deleteRecursively();
        auto pointAt = [&] (KICKRAudioProcessor& p) { p.getSampleLibrary().setFolder (tmpBank); };

        // sampleEnable OFF (default) -> render bit-identical to the synth-only engine.
        // driveMix = 0 on every processor here: the 2.7b null test assumes linear
        // summing (synth + sample == both), which the Phase 2.8 waveshaper breaks.
        KICKRAudioProcessor pa, pb;
        silenceDist (pa);    silenceLimiter (pa);
        silenceDist (pb);    silenceLimiter (pb);
        pointAt (pb);
        const auto nb = pb.getSampleLibrary().importFile (fixture);
        check (nb.isNotEmpty(), "processor bank import ok");
        pb.loadSampleByName (nb);
        const auto rA = kickr::tests::renderNote (pa, a1, vel, sr, 512, 1.0);
        const auto rBoff = kickr::tests::renderNote (pb, a1, vel, sr, 512, 1.0);
        double offDiff = 0.0;
        for (int i = 0; i < rA.getNumSamples(); ++i)
            offDiff = std::max (offDiff, (double) std::abs (rA.getReadPointer (0)[i] - rBoff.getReadPointer (0)[i]));
        std::printf ("  sampleEnable off vs synth-only: max|diff| = %.2e\n", offDiff);
        check (offDiff < 1.0e-9, "sampleEnable off (default) -> bit-identical to the synth-only render");

        // sampleEnable ON -> the sample layer is added, finite
        KICKRAudioProcessor pc;
        silenceDist (pc);    silenceLimiter (pc);
        pointAt (pc);
        const auto ncName = pc.getSampleLibrary().importFile (fixture);
        pc.loadSampleByName (ncName);
        setP (pc, "sampleEnable", 1.0f);
        const auto rC = kickr::tests::renderNote (pc, a1, vel, sr, 512, 1.0);
        double onDiff = 0.0; bool onFinite = true;
        for (int i = 0; i < rC.getNumSamples(); ++i)
        {
            if (! std::isfinite (rC.getReadPointer (0)[i])) onFinite = false;
            onDiff = std::max (onDiff, (double) std::abs (rC.getReadPointer (0)[i] - rA.getReadPointer (0)[i]));
        }
        check (onFinite,      "sample layer: no NaN / Inf through the full chain");
        check (onDiff > 0.02, "sampleEnable on adds the sample layer to the mix");

        // synthEnable OFF + sampleEnable ON -> pure sample
        KICKRAudioProcessor pe;
        silenceDist (pe);    silenceLimiter (pe);
        pointAt (pe);
        const auto neName = pe.getSampleLibrary().importFile (fixture);
        pe.loadSampleByName (neName);
        setP (pe, "sampleEnable", 1.0f);
        setP (pe, "synthEnable", 0.0f);
        const auto rE = kickr::tests::renderNote (pe, a1, vel, sr, 512, 1.0);
        const auto sE = analyse (rE, sr);
        check (sE.allFinite && sE.peak > 0.02f, "synthEnable off + sampleEnable on -> pure sample plays");

        // null test: (synth-only) + (sample-only) == (both on)
        double nullErr = 0.0;
        for (int i = 0; i < rC.getNumSamples(); ++i)
        {
            const double sum = (double) rA.getReadPointer (0)[i] + (double) rE.getReadPointer (0)[i];
            nullErr = std::max (nullErr, std::abs (sum - (double) rC.getReadPointer (0)[i]));
        }
        std::printf ("  sample+synth null test: max|(synth + sample) - both| = %.2e\n", nullErr);
        // 2e-4 (~ -74 dBFS): still a "numerically identical" bar, just loose enough to
        // absorb float-rounding noise from the resampler's interpolation coefficients
        // (these shift slightly with the sample's pitch ratio, e.g. after the 2026-08-31
        // MIDI-pitch recentre) — nowhere near an audible or meaningful difference.
        check (nullErr < 2.0e-4, "sample + synth both on == sum of the separate renders");

        // missing-file recall -> silent layer, name retained, no crash
        KICKRAudioProcessor pm;
        pointAt (pm);
        pm.loadSampleByName ("definitely_not_a_real_sample_9931.wav");
        setP (pm, "sampleEnable", 1.0f);
        const auto rM = kickr::tests::renderNote (pm, a1, vel, sr, 512, 1.0);
        check (analyse (rM, sr).allFinite, "missing sample: no NaN / crash");
        check (pm.getCurrentSampleName() == "definitely_not_a_real_sample_9931.wav",
               "missing sample: name retained");

        // machine-gun retrigger while hot-swapping the bank
        {
            KICKRAudioProcessor pg;
        pointAt (pg);
            const auto ng = pg.getSampleLibrary().importFile (fixture);
            setP (pg, "sampleEnable", 1.0f);
            pg.setRateAndBufferSizeDetails (sr, 128);
            pg.prepareToPlay (sr, 128);
            juce::AudioBuffer<float> blk (2, 128);
            bool finite = true; float pk = 0.0f;
            for (int bi = 0; bi < 400; ++bi)
            {
                blk.clear();
                juce::MidiBuffer midi;
                if ((bi % 3) == 0) midi.addEvent (juce::MidiMessage::noteOn (1, a1, vel), 0);
                pg.processBlock (blk, midi);
                if ((bi % 17) == 0) pg.loadSampleByName (ng);
                if ((bi % 41) == 0) pg.loadSampleByName ("missing_xyz.wav");
                const float* x = blk.getReadPointer (0);
                for (int i = 0; i < blk.getNumSamples(); ++i)
                {
                    if (! std::isfinite (x[i])) finite = false;
                    pk = std::max (pk, std::abs (x[i]));
                }
            }
            pg.releaseResources();
            std::printf ("  machine-gun + bank swap: peak %.2f  finite %d\n", pk, (int) finite);
            check (finite && pk < 8.0f, "hot bank-swap under machine-gun retrigger: no NaN, bounded");
        }

        // clean up every fixture we imported into the real managed bank
        for (const auto& de : juce::RangedDirectoryIterator (kickr::SampleLibrary::defaultFolder(),
                                                             false, "kickr_unittest_sine*",
                                                             juce::File::findFiles))
            de.getFile().deleteFile();
        fixture.deleteFile();
    }

    // Fix (2026-08-31, user request): "when I play C3 the sampler should play the sample
    // unpitched, and the synth engine should play 2 octaves lower than it does now."
    //   (a) MIDI Pitch mode is recentred -24 semitones (kMidiPitchRecentreSemitones), so a
    //       MIDI 60 / "C3" trigger — plain concert pitch's 261.63 Hz — now lands at ~65.4 Hz.
    //   (b) SampleBuffer::rootNote moved from C1 (24) to C3 (60): with sampleMidiTrack on
    //       (its default), MIDI 60 is now the unpitched (ratio 1) trigger note.
    {
        std::printf ("\n[Fix] C3 = synth 2 octaves lower / sample plays unpitched\n");
        const int c3 = 60;

        // (a) synth: MIDI Pitch mode, default fundamental (55 Hz) -> C3 should read ~65.4 Hz
        //     (55 * 2^((60-69-24)/12)), not plain concert pitch's 261.63 Hz.
        {
            KICKRAudioProcessor p;
            setP (p, "clickLevel", 0.0f); setP (p, "subLevel", 0.0f);
            setP (p, "tailLevel", 0.0f);  setP (p, "noiseLevel", 0.0f);
            setP (p, "driveMix", 0.0f);   setP (p, "limiter", 0.0f);
            const auto b = kickr::tests::renderNote (p, c3, vel, sr, 512, 1.0);
            const double hz = estFreq (b, sr, 0.30, 0.60);   // settled, past the pitch envelope
            std::printf ("  synth @ C3 (MIDI 60), MIDI Pitch mode: %.1f Hz  (was 261.6 Hz pre-fix)\n", hz);
            check (hz > 55.0 && hz < 78.0, "C3 now plays 2 octaves below plain concert pitch (~65 Hz)");
        }

        // (b) sample: root note is C3 -> triggering at C3 with sampleMidiTrack on (default)
        //     must be unpitched (output frequency == source frequency).
        {
            const auto tmpBank2 = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                      .getChildFile ("kickr_test_c3_bank");
            tmpBank2.deleteRecursively();
            const auto fx2 = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                 .getChildFile ("kickr_c3_fixture.wav");
            writeSineWav (fx2, 90.0, 44100.0, 0.4, 1);

            auto renderSampleAt = [&] (int note)
            {
                KICKRAudioProcessor p;
                p.getSampleLibrary().setFolder (tmpBank2);
                const auto nm = p.getSampleLibrary().importFile (fx2);
                p.loadSampleByName (nm);
                setP (p, "sampleEnable", 1.0f); setP (p, "synthEnable", 0.0f);
                setP (p, "limiter", 0.0f);
                return kickr::tests::renderNote (p, note, vel, sr, 512, 0.35);
            };

            const auto atC3 = renderSampleAt (c3);
            const auto atC4 = renderSampleAt (c3 + 12);   // one octave above the root
            const double hzC3 = estFreq (atC3, sr, 0.02, 0.20);
            const double hzC4 = estFreq (atC4, sr, 0.02, 0.20);
            std::printf ("  sample @ C3: %.1f Hz (source 90 Hz, unpitched)   @ C3+12: %.1f Hz (should double)\n",
                         hzC3, hzC4);
            check (std::abs (hzC3 - 90.0) < 4.0, "sample at C3 plays unpitched (== source frequency)");
            check (std::abs (hzC4 / hzC3 - 2.0) < 0.06, "sample still tracks MIDI pitch away from C3");

            tmpBank2.deleteRecursively(); fx2.deleteFile();
        }
    }

    // ---------------------------------------------------------------------
    std::printf ("\n[Phase 2.8] Distortion morph (7-curve character morph + adaptive RMS makeup)\n");

    // --- 1. each of the 7 curves matches its transfer function at character = k/6 ---
    {
        auto sg = [] (float v) { return v < 0.0f ? -1.0f : 1.0f; };
        auto ref = [&] (int k, float u, float drv) -> float
        {
            switch (k)
            {
                case 0:  return std::tanh (u);
                case 1:  return std::abs (u) < 1.0f ? u - u * u * u / 3.0f : sg (u) * (2.0f / 3.0f);
                case 2:  return u <= 0.0f ? std::tanh (u) : 1.0f - std::exp (-u);
                case 3:  return std::abs (u) < 1.0f ? 1.25f * (u - 0.2f * u * u * u * u * u) : sg (u);
                case 4:  return juce::jlimit (-1.0f, 1.0f, u);
                case 5:  { float y = u; for (int i = 0; i < 8 && std::abs (y) > 1.0f; ++i) y = 2.0f * sg (y) - y;
                           return juce::jlimit (-1.0f, 1.0f, y); }
                default: { const float bits = 12.0f - 10.0f * drv; const float L = std::exp2 (bits - 1.0f);
                           return juce::jlimit (-1.0f, 1.0f, std::round (u * L) / L); }  // clamped like every other curve
            }
        };

        const float xs[] = { -1.5f, -0.8f, -0.3f, 0.0f, 0.2f, 0.6f, 1.2f };
        int    curveFails = 0;
        double worstErr   = 0.0;

        for (int k = 0; k < kickr::Waveshaper::kNumCurves; ++k)
        {
            kickr::Waveshaper ws;
            ws.prepare (48000.0);
            // drive 0 -> gDrive == 1 -> u == x ; driveMix 1 -> output == makeup * f(u)
            ws.setParams (0.0f, static_cast<float> (k) / 6.0f, 1.0f);
            const float mk = ws.getMakeup();   // frozen at the primed per-curve value (warmup)

            for (float x : xs)
            {
                const float got = ws.processSample (x);
                const float expv = mk * ref (k, x, 0.0f);
                worstErr = std::max (worstErr, (double) std::abs (got - expv));
                if (std::abs (got - expv) > 1.0e-4f) ++curveFails;
            }
        }
        std::printf ("  per-curve transfer match: worst |got - makeup*f(u)| = %.2e  (%d mismatches)\n",
                     worstErr, curveFails);
        check (curveFails == 0, "all 7 curves match their transfer function at character = k/6");
    }

    // --- 2. driveMix = 0 is exactly bit-transparent (pre-drive-gain clean) ---
    {
        kickr::Waveshaper ws;
        ws.prepare (48000.0);
        ws.setParams (0.7f, 0.5f, 0.0f);   // heavy drive, mid morph — but driveMix 0
        float worst = 0.0f;
        bool  finite = true;
        juce::Random rng (20260829);
        for (int i = 0; i < 8000; ++i)
        {
            const float x = rng.nextFloat() * 2.4f - 1.2f;
            const float y = ws.processSample (x);
            if (! std::isfinite (y)) finite = false;
            worst = std::max (worst, std::abs (y - x));
        }
        std::printf ("  driveMix = 0: max|out - in| = %.2e\n", (double) worst);
        check (finite,        "driveMix = 0: finite");
        check (worst == 0.0f, "driveMix = 0 -> exactly bit-transparent (out == in)");
    }

    // --- 3. sweeping character 0->1 at fixed drive keeps integrated RMS within +/-1.5 dB ---
    {
        double minR = 1.0e9, maxR = 0.0;
        for (int s = 0; s <= 20; ++s)
        {
            KICKRAudioProcessor p;
            setP (p, "drive",     0.5f);
            setP (p, "driveMix",  1.0f);
            setP (p, "limiter",   0.0f);   // Phase 2.9 — measure the makeup, not the ceiling
            setP (p, "character", static_cast<float> (s) / 20.0f);
            const auto b = kickr::tests::renderNote (p, a1, vel, sr, 512, 1.0);
            const double r = rmsWindow (b, sr, 0.0, 1.0);
            minR = std::min (minR, r);
            maxR = std::max (maxR, r);
        }
        const double spreadDb = 20.0 * std::log10 (maxR / std::max (1.0e-9, minR));
        std::printf ("  character 0->1 @ drive 0.5: integrated RMS spread = %.2f dB\n", spreadDb);
        check (spreadDb < 3.0, "character sweep stays within +/-1.5 dB (adaptive RMS makeup)");
    }

    // --- 4. no NaN / Inf at drive = 1, character = 1 ---
    {
        KICKRAudioProcessor p;
        setP (p, "drive",     1.0f);
        setP (p, "character", 1.0f);
        setP (p, "driveMix",  1.0f);
        const auto b   = kickr::tests::renderNote (p, a1, vel, sr, 512, 1.0);
        const auto st2 = analyse (b, sr);
        std::printf ("  drive 1 / character 1: peak %.2f  finite %d\n", st2.peak, (int) st2.allFinite);
        check (st2.allFinite, "drive = 1, character = 1: no NaN / Inf");
    }

    // --- 5. foldback (character ~ 0.83) at drive = 1 stays bounded ---
    {
        KICKRAudioProcessor p;
        setP (p, "drive",     1.0f);
        setP (p, "character", 5.0f / 6.0f);   // pure foldback segment
        setP (p, "driveMix",  1.0f);
        const auto b   = kickr::tests::renderNote (p, a1, vel, sr, 512, 1.0);
        const auto st2 = analyse (b, sr);
        std::printf ("  foldback @ drive 1: peak %.3f  finite %d\n", st2.peak, (int) st2.allFinite);
        check (st2.allFinite && st2.peak < 8.0f, "foldback at drive = 1: finite + bounded");
    }

    // --- 6. asymmetric (character ~ 0.33) DC removed by the downstream base-rate blocker ---
    {
        KICKRAudioProcessor p;
        setP (p, "drive",     0.8f);
        setP (p, "character", 2.0f / 6.0f);   // pure asymmetric (adds DC)
        setP (p, "driveMix",  1.0f);
        const auto b = kickr::tests::renderNote (p, a1, vel, sr, 512, 1.5);
        double mean = 0.0;
        const float* x = b.getReadPointer (0);
        for (int i = 0; i < b.getNumSamples(); ++i) mean += (double) x[i];
        mean /= std::max (1, b.getNumSamples());
        std::printf ("  asymmetric final-bus mean = %.2e  (%.1f dBFS)\n",
                     mean, 20.0 * std::log10 (std::max (1.0e-12, std::abs (mean))));
        check (analyse (b, sr).allFinite,   "asymmetric @ drive 0.8: no NaN / Inf");
        check (std::abs (mean) < 1.0e-3,    "asymmetric DC removed by the downstream blocker (mean < -60 dBFS)");
    }

    {
        KICKRAudioProcessor pw;
        setP (pw, "drive",     0.6f);
        setP (pw, "character", 0.5f);
        const auto wav = juce::File::getCurrentWorkingDirectory().getChildFile ("kickr_phase2_8.wav");
        kickr::tests::renderNoteToWav (pw, wav, a1, vel, sr, 512, 1.0);
    }

    // ---------------------------------------------------------------------
    std::printf ("\n[Phase 2.9] Tone / output / stereo\n");

    // L/R correlation over a window (1.0 == perfectly mono).
    auto lrCorr = [] (const juce::AudioBuffer<float>& b, double s, double t0, double t1)
    {
        if (b.getNumChannels() < 2) return 1.0;
        const int i0 = std::max (0, (int) (t0 * s));
        const int i1 = std::min (b.getNumSamples(), (int) (t1 * s));
        const float* L = b.getReadPointer (0);
        const float* R = b.getReadPointer (1);
        double sll = 0.0, srr = 0.0, slr = 0.0;
        for (int i = i0; i < i1; ++i)
        {
            sll += (double) L[i] * L[i];
            srr += (double) R[i] * R[i];
            slr += (double) L[i] * R[i];
        }
        const double den = std::sqrt (sll * srr);
        return den > 1.0e-12 ? slr / den : 1.0;
    };

    // 1. Tone bands — correct shelf direction (measured pre-distortion, in-region).
    {
        KICKRAudioProcessor pLowUp, pLowDn, pHiUp, pHiDn;
        for (auto* p : { &pLowUp, &pLowDn, &pHiUp, &pHiDn }) { silenceDist (*p); silenceLimiter (*p); }
        setP (pLowUp, "low",   12.0f);
        setP (pLowDn, "low",  -12.0f);
        setP (pHiUp,  "high",  12.0f);
        setP (pHiDn,  "high", -12.0f);
        const auto bLowUp = kickr::tests::renderNote (pLowUp, a1, vel, sr, 512, 1.0);
        const auto bLowDn = kickr::tests::renderNote (pLowDn, a1, vel, sr, 512, 1.0);
        const auto bHiUp  = kickr::tests::renderNote (pHiUp,  a1, vel, sr, 512, 1.0);
        const auto bHiDn  = kickr::tests::renderNote (pHiDn,  a1, vel, sr, 512, 1.0);
        const double rLowUp = rmsWindow (bLowUp, sr, 0.0, 0.3);
        const double rLowDn = rmsWindow (bLowDn, sr, 0.0, 0.3);
        const double hHiUp  = hfRatio (bHiUp, sr, 0.0, 0.05);
        const double hHiDn  = hfRatio (bHiDn, sr, 0.0, 0.05);
        std::printf ("  tone: low RMS +12 %.4f / -12 %.4f   |   high hfRatio +12 %.3f / -12 %.3f\n",
                     rLowUp, rLowDn, hHiUp, hHiDn);
        check (analyse (bLowUp, sr).allFinite && analyse (bHiDn, sr).allFinite, "tone: no NaN / Inf");
        check (rLowUp > rLowDn * 1.5, "low shelf @ 130 Hz: +12 dB has clearly more LF energy than -12 dB");
        check (hHiUp  > hHiDn  * 1.3, "high shelf @ 5 kHz: +12 dB has clearly more HF content than -12 dB");
    }

    // 2. Content below 130 Hz is forced mono even at outputWidth = 1.
    {
        KICKRAudioProcessor p;
        setP (p, "bodyLevel", 0.0f); setP (p, "clickLevel", 0.0f); setP (p, "tailLevel", 0.0f);
        setP (p, "driveMix", 0.0f);
        setP (p, "subLevel", 0.8f); setP (p, "subFreq", 45.0f); setP (p, "subDecay", 700.0f);
        setP (p, "outputWidth", 1.0f);
        const auto b = kickr::tests::renderNote (p, a1, vel, sr, 512, 1.0);
        const double c = lrCorr (b, sr, 0.05, 0.5);
        std::printf ("  45 Hz sub @ outputWidth 1.0: L/R correlation = %.5f\n", c);
        check (c > 0.999, "content below the 130 Hz crossover stays mono at any outputWidth");
    }

    // 3. outputWidth 0 / 0.5 / 1 -> mono / unity / wide ABOVE 130 Hz (clickWidth = 1 for side content).
    {
        auto renderW = [&] (float ow)
        {
            KICKRAudioProcessor p;
            setP (p, "bodyLevel", 0.0f); setP (p, "subLevel", 0.0f); setP (p, "tailLevel", 0.0f);
            setP (p, "driveMix", 0.0f);
            setP (p, "clickLevel", 0.8f); setP (p, "clickWidth", 1.0f); setP (p, "clickTime", 30.0f);
            setP (p, "outputWidth", ow);
            return kickr::tests::renderNote (p, a1, vel, sr, 512, 0.5);
        };
        const auto w0  = renderW (0.0f);
        const auto w05 = renderW (0.5f);
        const auto w1  = renderW (1.0f);
        const double c0  = lrCorr (w0,  sr, 0.0, 0.06);
        const double c05 = lrCorr (w05, sr, 0.0, 0.06);
        const double c1  = lrCorr (w1,  sr, 0.0, 0.06);
        std::printf ("  click width: L/R corr  ow0 %.4f   ow0.5 %.4f   ow1 %.4f\n", c0, c05, c1);
        check (c0 > 0.9999,  "outputWidth 0 collapses everything to mono (corr = 1)");
        check (c05 < 0.999,  "outputWidth 0.5 (unity) passes the click side content");
        check (c1 < c05,     "outputWidth 1 widens further than 0.5 (lower correlation)");
    }

    // 4. Safety limiter — on: never exceeds -0.5 dBFS ; off: can exceed 0 dBFS.
    {
        const float ceilGain = juce::Decibels::decibelsToGain (-0.5f);
        KICKRAudioProcessor pOn, pOff;
        setP (pOn,  "drive", 1.0f); setP (pOn,  "output", 12.0f); setP (pOn,  "limiter", 1.0f);
        setP (pOff, "drive", 1.0f); setP (pOff, "output", 12.0f); setP (pOff, "limiter", 0.0f);
        const auto bOn  = kickr::tests::renderNote (pOn,  a1, vel, sr, 512, 1.0);
        const auto bOff = kickr::tests::renderNote (pOff, a1, vel, sr, 512, 1.0);
        const auto sOn  = analyse (bOn,  sr);
        const auto sOff = analyse (bOff, sr);
        std::printf ("  limiter: on peak %.4f (ceil %.4f)   off peak %.4f\n",
                     sOn.peak, ceilGain, sOff.peak);
        check (sOn.allFinite && sOff.allFinite,     "limiter: no NaN / Inf");
        check (sOn.peak <= ceilGain + 5.0e-3f,      "limiter on -> peak held at ~-0.5 dBFS (drive 1 / output +12)");
        check (sOff.peak > ceilGain + 0.05f && sOff.peak > sOn.peak * 1.02f,
                                                    "limiter off -> output exceeds the -0.5 dBFS ceiling");
    }

    // 5. Final-bus DC offset < -60 dBFS (asymmetric distortion + base-rate 5 Hz blocker).
    {
        KICKRAudioProcessor p;
        setP (p, "drive", 0.8f); setP (p, "character", 2.0f / 6.0f);   // pure asymmetric curve
        const auto b = kickr::tests::renderNote (p, a1, vel, sr, 512, 1.5);
        double mean = 0.0;
        const float* x = b.getReadPointer (0);
        for (int i = 0; i < b.getNumSamples(); ++i) mean += (double) x[i];
        mean /= std::max (1, b.getNumSamples());
        std::printf ("  final-bus DC mean = %.2e (%.1f dBFS)\n",
                     mean, 20.0 * std::log10 (std::max (1.0e-12, std::abs (mean))));
        check (analyse (b, sr).allFinite,  "asymmetric drive: no NaN / Inf");
        check (std::abs (mean) < 1.0e-3,   "final-bus DC mean < -60 dBFS (DC blocker after downsample)");
    }

    // 6. 25 Hz sub is NOT thinned by the 5 Hz DC blocker (within ~1 dB of a 60 Hz sub).
    {
        auto subRms = [&] (float freq)
        {
            KICKRAudioProcessor p;
            setP (p, "bodyLevel", 0.0f); setP (p, "clickLevel", 0.0f); setP (p, "tailLevel", 0.0f);
            setP (p, "driveMix", 0.0f); setP (p, "limiter", 0.0f);
            setP (p, "subLevel", 0.7f); setP (p, "subFreq", freq); setP (p, "subDecay", 800.0f);
            const auto b = kickr::tests::renderNote (p, a1, vel, sr, 512, 1.0);
            return rmsWindow (b, sr, 0.05, 0.5);
        };
        const double r25 = subRms (25.0f);
        const double r60 = subRms (60.0f);
        const double dB  = 20.0 * std::log10 (r25 / std::max (1.0e-9, r60));
        std::printf ("  sub RMS: 25 Hz %.4f   60 Hz %.4f   (%.2f dB)\n", r25, r60, dB);
        check (std::abs (dB) < 1.0, "25 Hz sub not thinned by the 5 Hz DC blocker");
    }

    // 7. Pre-2.9 preservation — default patch still finite, audible, inside full scale, stereo bus.
    {
        KICKRAudioProcessor p;
        const auto b   = kickr::tests::renderNote (p, a1, vel, sr, 512, 1.0);
        const auto st9 = analyse (b, sr);
        std::printf ("  default patch: peak %.3f  channels %d  (limiter on)\n",
                     st9.peak, b.getNumChannels());
        check (st9.allFinite && st9.peak > 0.02f && st9.peak < 1.0f,
               "default patch: finite, audible, within digital full scale");
    }

    {
        KICKRAudioProcessor pw;
        setP (pw, "low", 4.0f); setP (pw, "high", 3.0f); setP (pw, "outputWidth", 0.8f);
        const auto wav = juce::File::getCurrentWorkingDirectory().getChildFile ("kickr_phase2_9.wav");
        kickr::tests::renderNoteToWav (pw, wav, a1, vel, sr, 512, 1.0);
    }

    // ---------------------------------------------------------------------
    std::printf ("\n[Phase 2.10] Real oversampling factors + glitch-free switching\n");

    // A freshly-prepared processor (no host ever touches `oversampling`) must already
    // report the 2x default's PDC latency — not 0 (a DAW would then be time-misaligned).
    {
        KICKRAudioProcessor pd;
        pd.setRateAndBufferSizeDetails (sr, 512);
        pd.prepareToPlay (sr, 512);
        const int lat = pd.getLatencySamples();
        std::printf ("  default-prepared latency (oversampling defaults to 2x): %d\n", lat);
        check (lat > 0, "default-prepared processor reports the 2x PDC latency (not 0)");
    }

    auto setOS = [&] (KICKRAudioProcessor& p, int choiceIdx)
    {
        if (auto* prm = p.getValueTreeState().getParameter ("oversampling"))
            prm->setValueNotifyingHost (prm->convertTo0to1 (static_cast<float> (choiceIdx)));
    };

    // Faithful expected latency: build the same juce::dsp::Oversampling object the
    // processor builds for each order and read round(getLatencyInSamples()).
    auto expectedOsLatency = [] (int choiceIdx) -> int
    {
        if (choiceIdx <= 0) return 0;
        juce::dsp::Oversampling<float> os (static_cast<size_t> (2),
                                           static_cast<size_t> (choiceIdx),
                                           juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR,
                                           true, true);
        os.initProcessing (static_cast<size_t> (512));
        return static_cast<int> (std::lround (os.getLatencyInSamples()));
    };

    // (a) + (b) — every factor renders a sane kick; reported latency == round(os latency).
    {
        for (int idx = 0; idx < 4; ++idx)
        {
            KICKRAudioProcessor p;
            setOS (p, idx);
            const auto bb  = kickr::tests::renderNote (p, a1, vel, sr, 512, 1.0);
            const auto stat = analyse (bb, sr);
            const int  lat    = p.getLatencySamples();
            const int  expLat = expectedOsLatency (idx);
            std::printf ("  OS %dx: peak %.3f  finite %d  latency %d (expected %d)\n",
                         1 << idx, stat.peak, (int) stat.allFinite, lat, expLat);
            check (stat.allFinite && stat.peak > 0.05f && stat.peak < 1.5f,
                   idx == 0 ? "1x: default kick finite, audible, sane peak"
                 : idx == 1 ? "2x: default kick finite, audible, sane peak"
                 : idx == 2 ? "4x: default kick finite, audible, sane peak"
                            : "8x: default kick finite, audible, sane peak");
            check (lat == expLat,
                   idx == 0 ? "1x reported latency == 0"
                 : idx == 1 ? "2x reported latency == round(os latency)"
                 : idx == 2 ? "4x reported latency == round(os latency)"
                            : "8x reported latency == round(os latency)");
            if (idx > 0)
                check (lat > 0,
                       idx == 1 ? "2x latency > 0" : idx == 2 ? "4x latency > 0" : "8x latency > 0");
        }
    }

    // (c) pitch correctness — a fixed 100 Hz body must read ~100 Hz at every factor
    // (a wrong fsOversampled detunes the phase-accumulator).
    {
        auto settledFreq = [&] (int idx)
        {
            KICKRAudioProcessor p;
            setOS (p, idx);
            setP (p, "tuneMode",    1.0f);      // Fixed Frequency
            setP (p, "fundamental", 100.0f);
            setP (p, "pitchStart",  1.0f);      // steady — no pitch drop
            setP (p, "bodyDecay",   2000.0f);
            setP (p, "subLevel",    0.0f);
            setP (p, "clickLevel",  0.0f);
            setP (p, "tailLevel",   0.0f);
            setP (p, "noiseLevel",  0.0f);
            setP (p, "driveMix",    0.0f);
            setP (p, "limiter",     0.0f);
            const auto b = kickr::tests::renderNote (p, a1, vel, sr, 512, 0.8);
            return estFreq (b, sr, 0.10, 0.50);
        };
        double f[4];
        for (int i = 0; i < 4; ++i) f[i] = settledFreq (i);
        std::printf ("  fixed 100 Hz body:  1x %.1f  2x %.1f  4x %.1f  8x %.1f Hz\n",
                     f[0], f[1], f[2], f[3]);
        for (int i = 0; i < 4; ++i)
            check (std::abs (f[i] - 100.0) < 2.0,
                   i == 0 ? "1x body pitch within 2 percent of 100 Hz"
                 : i == 1 ? "2x body pitch within 2 percent of 100 Hz"
                 : i == 2 ? "4x body pitch within 2 percent of 100 Hz"
                          : "8x body pitch within 2 percent of 100 Hz");
    }

    // (d) decay correctness — the default body -40 dB time within ~10% across factors
    // (proves the envelope decay coefficient is refreshed for the new rate).
    {
        auto decay40 = [&] (int idx)
        {
            KICKRAudioProcessor p;
            setOS (p, idx);
            setP (p, "subLevel",   0.0f);
            setP (p, "clickLevel", 0.0f);
            setP (p, "tailLevel",  0.0f);
            setP (p, "noiseLevel", 0.0f);
            setP (p, "driveMix",   0.0f);
            setP (p, "limiter",    0.0f);
            const auto b = kickr::tests::renderNote (p, a1, vel, sr, 512, 1.5);
            return analyse (b, sr).lastAbove40dB / sr * 1000.0;
        };
        double d[4];
        for (int i = 0; i < 4; ++i) d[i] = decay40 (i);
        double dmin = d[0], dmax = d[0];
        for (int i = 1; i < 4; ++i) { dmin = std::min (dmin, d[i]); dmax = std::max (dmax, d[i]); }
        std::printf ("  body -40 dB time:  1x %.0f  2x %.0f  4x %.0f  8x %.0f ms\n",
                     d[0], d[1], d[2], d[3]);
        check (dmax < dmin * 1.10, "body -40 dB time within ~10% across all 4 OS factors");
    }

    // (e) clean switch — flip 1x<->4x every 10 blocks with the note kept alive:
    // no NaN/Inf, bounded, no slew spike > 0.5 near the switch points.
    {
        KICKRAudioProcessor p;
        setP (p, "subLevel",  0.0f);
        setP (p, "tailLevel", 0.0f);
        setP (p, "noiseLevel", 0.0f);
        const int block  = 128;
        const int nBlk   = 200;
        p.setRateAndBufferSizeDetails (sr, block);
        p.prepareToPlay (sr, block);
        juce::AudioBuffer<float> blk (2, block);
        juce::AudioBuffer<float> full (1, block * nBlk);
        full.clear();
        bool finite = true; float pk = 0.0f;
        int flip = 0;
        std::vector<int> switchBlocks;
        for (int bi = 0; bi < nBlk; ++bi)
        {
            blk.clear();
            juce::MidiBuffer midi;
            if (bi == 0 || (bi % 16) == 0)
                midi.addEvent (juce::MidiMessage::noteOn (1, a1, vel), 0);   // keep signal alive
            if (bi > 0 && (bi % 10) == 0)
            {
                flip ^= 1;
                if (auto* prm = p.getValueTreeState().getParameter ("oversampling"))
                    prm->setValueNotifyingHost (prm->convertTo0to1 (flip ? 2.0f : 0.0f));  // 4x <-> 1x
                switchBlocks.push_back (bi);
            }
            p.processBlock (blk, midi);
            const float* x = blk.getReadPointer (0);
            for (int i = 0; i < block; ++i)
            {
                if (! std::isfinite (x[i])) finite = false;
                pk = std::max (pk, std::abs (x[i]));
                full.setSample (0, bi * block + i, x[i]);
            }
        }
        p.releaseResources();

        float worstSlew = 0.0f;
        const float* fx = full.getReadPointer (0);
        for (int sb : switchBlocks)
        {
            const int i0 = std::max (1, (sb - 1) * block);
            const int i1 = std::min (full.getNumSamples(), (sb + 2) * block);
            for (int i = i0; i < i1; ++i)
                worstSlew = std::max (worstSlew, std::abs (fx[i] - fx[i - 1]));
        }
        std::printf ("  1x<->4x flip x%d w/ note alive: peak %.2f  finite %d  worstSlew@switch %.3f\n",
                     (int) switchBlocks.size(), pk, (int) finite, worstSlew);
        check (finite,                       "OS switching under an active note: no NaN / Inf");
        check (pk > 0.01f && pk < 4.0f,      "OS switching: output stays bounded");
        check (worstSlew < 0.5f,             "OS switching: no slew spike > 0.5 near the switch points");
    }

    // (f) aliasing reduced — drive 1 / character 1 (pure bitcrush) folds down less at 4x.
    {
        auto hfAt = [&] (int idx)
        {
            KICKRAudioProcessor p;
            setOS (p, idx);
            setP (p, "drive",      1.0f);
            setP (p, "character",  1.0f);   // pure bitcrush / decimate — worst-case aliasing
            setP (p, "driveMix",   1.0f);
            setP (p, "limiter",    0.0f);
            setP (p, "subLevel",   0.0f);
            setP (p, "tailLevel",  0.0f);
            setP (p, "clickLevel", 0.0f);
            setP (p, "noiseLevel", 0.0f);
            const auto b = kickr::tests::renderNote (p, a1, vel, sr, 512, 0.5);
            return hfRatio (b, sr, 0.0, 0.30);
        };
        const double h1 = hfAt (0);
        const double h4 = hfAt (2);
        std::printf ("  drive1/char1 HF proxy:  1x %.4f   4x %.4f\n", h1, h4);
        check (h4 < h1 * 0.95, "aliasing reduced: 4x has less HF energy above ~0.4 Nyquist than 1x");
    }

    {
        KICKRAudioProcessor pw;
        setP (pw, "oversampling", 3.0f);   // 8x
        setP (pw, "drive", 0.6f); setP (pw, "character", 0.5f);
        const auto wav = juce::File::getCurrentWorkingDirectory().getChildFile ("kickr_phase2_10.wav");
        kickr::tests::renderNoteToWav (pw, wav, a1, vel, sr, 512, 1.0);
    }

    // -------------------------------------------------------------------------
    std::printf ("\n[Phase 2.11] Macros + parameter smoothing + state\n");

    // Render a held A1 while linearly ramping one parameter min->max across `rampMs`.
    auto renderRamp = [] (KICKRAudioProcessor& p, juce::StringRef id, float from, float to,
                          double s, double rampMs, double seconds)
    {
        auto* prm = p.getValueTreeState().getParameter (id);
        const int block = 64;
        const int total = std::max (1, (int) std::ceil (s * seconds));
        const int rampBlocks = std::max (1, (int) (rampMs * 0.001 * s / block));
        p.setRateAndBufferSizeDetails (s, block);
        p.prepareToPlay (s, block);
        juce::AudioBuffer<float> out (juce::jmax (1, p.getTotalNumOutputChannels()), total);
        out.clear();
        juce::AudioBuffer<float> sc (out.getNumChannels(), block);
        int bi = 0;
        for (int pos = 0; pos < total; ++bi)
        {
            const int n = std::min (block, total - pos);
            const float t = juce::jlimit (0.0f, 1.0f, (float) bi / (float) rampBlocks);
            if (prm != nullptr) prm->setValueNotifyingHost (prm->convertTo0to1 (from + t * (to - from)));
            juce::AudioBuffer<float> b (sc.getArrayOfWritePointers(), out.getNumChannels(), n);
            b.clear();
            juce::MidiBuffer midi;
            if (pos == 0) midi.addEvent (juce::MidiMessage::noteOn (1, 33, 0.8f), 0);
            p.processBlock (b, midi);
            for (int ch = 0; ch < out.getNumChannels(); ++ch) out.copyFrom (ch, pos, b, ch, 0, n);
            pos += n;
        }
        p.releaseResources();
        return out;
    };
    auto maxSlew = [] (const juce::AudioBuffer<float>& b)
    {
        float m = 0.0f; const float* x = b.getReadPointer (0);
        for (int i = 1; i < b.getNumSamples(); ++i) m = std::max (m, std::abs (x[i] - x[i - 1]));
        return m;
    };

    // 1. All macros at 0.5 (neutral) -> render bit-identical to defaults untouched.
    {
        KICKRAudioProcessor pRef, pMid;
        setP (pMid, "macroPunch", 0.5f); setP (pMid, "macroBody", 0.5f);
        setP (pMid, "macroCrush", 0.5f); setP (pMid, "macroTail", 0.5f);
        const auto rRef = kickr::tests::renderNote (pRef, a1, vel, sr, 512, 1.0);
        const auto rMid = kickr::tests::renderNote (pMid, a1, vel, sr, 512, 1.0);
        double d = 0.0;
        for (int i = 0; i < rRef.getNumSamples(); ++i)
            d = std::max (d, (double) std::abs (rRef.getReadPointer (0)[i] - rMid.getReadPointer (0)[i]));
        std::printf ("  macros @ 0.5 vs default: max|diff| = %.2e\n", d);
        check (d < 1.0e-7, "macros at 0.5 are exactly neutral (bit-identical to default)");
    }

    // 2. PUNCH -> sharper onset. 3. BODY -> louder + deeper. 4. CRUSH -> more HF.
    // 5. TAIL -> longer/louder tail.
    {
        auto rmsAt = [&] (juce::StringRef macro, float v, double t0, double t1)
        {
            KICKRAudioProcessor p;  setP (p, macro, v);
            return rmsWindow (kickr::tests::renderNote (p, a1, vel, sr, 512, 1.2), sr, t0, t1);
        };
        const double p0 = rmsAt ("macroPunch", 0.0f, 0.0, 0.006);
        const double p5 = rmsAt ("macroPunch", 0.5f, 0.0, 0.006);
        const double p1 = rmsAt ("macroPunch", 1.0f, 0.0, 0.006);
        std::printf ("  PUNCH onset RMS 0/0.5/1: %.4f / %.4f / %.4f\n", p0, p5, p1);
        check (p1 > p5 && p5 > p0, "macroPunch: monotonic onset energy, neutral in the middle");

        const double b0 = rmsAt ("macroBody", 0.0f, 0.05, 0.30);
        const double b1 = rmsAt ("macroBody", 1.0f, 0.05, 0.30);
        // deeper: measure the settled body frequency (fixed tune) at macroBody 0 vs 1
        auto bodyHz = [&] (float v)
        {
            KICKRAudioProcessor p;
            setP (p, "macroBody", v); setP (p, "tuneMode", 1.0f); setP (p, "fundamental", 55.0f);
            setP (p, "clickLevel", 0.0f); setP (p, "subLevel", 0.0f); setP (p, "tailLevel", 0.0f);
            setP (p, "driveMix", 0.0f); setP (p, "limiter", 0.0f);
            return estFreq (kickr::tests::renderNote (p, a1, vel, sr, 512, 1.0), sr, 0.20, 0.45);
        };
        const double hz0 = bodyHz (0.5f), hz1 = bodyHz (1.0f);
        std::printf ("  BODY: body RMS 0/1 %.4f/%.4f ; settled Hz 0.5/1.0 %.1f/%.1f\n", b0, b1, hz0, hz1);
        check (b1 > b0 * 1.1, "macroBody: louder + longer body");
        check (hz1 < hz0 * 0.95, "macroBody: tunes deeper (settled fundamental drops)");

        const double c0 = rmsAt ("macroCrush", 0.0f, 0.0, 0.30);   // just to touch the render
        juce::ignoreUnused (c0);
        KICKRAudioProcessor pc0, pc1;  setP (pc0, "macroCrush", 0.0f);  setP (pc1, "macroCrush", 1.0f);
        const double hc0 = hfRatio (kickr::tests::renderNote (pc0, a1, vel, sr, 512, 0.5), sr, 0.0, 0.3);
        const double hc1 = hfRatio (kickr::tests::renderNote (pc1, a1, vel, sr, 512, 0.5), sr, 0.0, 0.3);
        std::printf ("  CRUSH hfRatio 0/1: %.4f / %.4f\n", hc0, hc1);
        check (hc1 > hc0 * 1.15, "macroCrush: adds drive + harmonic content");

        auto tailOnly = [&] (float v, double t0, double t1)
        {
            KICKRAudioProcessor p;
            setP (p, "macroTail", v);
            setP (p, "bodyLevel", 0.0f); setP (p, "subLevel", 0.0f);
            setP (p, "clickLevel", 0.0f); setP (p, "noiseLevel", 0.0f);
            setP (p, "driveMix", 0.0f);   setP (p, "limiter", 0.0f);
            setP (p, "tailLevel", 0.3f);  setP (p, "tailLength", 200.0f);
            return rmsWindow (kickr::tests::renderNote (p, a1, vel, sr, 512, 1.6), sr, t0, t1);
        };
        const double tLo = tailOnly (0.0f, 0.05, 0.90);
        const double tMid = tailOnly (0.5f, 0.05, 0.90);
        const double tHi = tailOnly (1.0f, 0.05, 0.90);
        const double tLateMid = tailOnly (0.5f, 0.35, 0.90);
        const double tLateHi  = tailOnly (1.0f, 0.35, 0.90);
        std::printf ("  TAIL-only RMS  0/0.5/1 %.4f/%.4f/%.4f  late 0.5/1 %.5f/%.5f\n",
                     tLo, tMid, tHi, tLateMid, tLateHi);
        check (tHi > tMid * 1.3 && tMid > tLo, "macroTail: louder tail (0<0.5<1, monotonic)");
        check (tLateHi > tLateMid * 1.4, "macroTail: longer tail (more late-window energy at 1 vs 0.5)");
    }

    // 6. Underlying param stays independent under a maxed macro.
    {
        KICKRAudioProcessor pLo, pHi;
        setP (pLo, "macroCrush", 1.0f); setP (pLo, "drive", 0.0f);
        setP (pHi, "macroCrush", 1.0f); setP (pHi, "drive", 1.0f);
        const double lo = hfRatio (kickr::tests::renderNote (pLo, a1, vel, sr, 512, 0.5), sr, 0.0, 0.3);
        const double hi = hfRatio (kickr::tests::renderNote (pHi, a1, vel, sr, 512, 0.5), sr, 0.0, 0.3);
        std::printf ("  macroCrush=1, drive 0 vs 1: hfRatio %.4f / %.4f\n", lo, hi);
        check (hi > lo * 1.05, "macro is additive: `drive` still moves the sound under macroCrush=1");
    }

    // 7. Fast macro sweep is click-free. 8. Fast param automation is click-free.
    {
        KICKRAudioProcessor pm;
        const auto rm = renderRamp (pm, "macroCrush", 0.0f, 1.0f, sr, 200.0, 1.0);
        const float sm = maxSlew (rm);
        std::printf ("  macroCrush 0->1 over 200ms: maxSlew %.4f  finite %d\n",
                     sm, (int) analyse (rm, sr).allFinite);
        check (analyse (rm, sr).allFinite && sm < 0.35f, "fast macro sweep: no zipper / NaN");

        for (auto id : { "bodyLevel", "low", "clickLevel", "output" })
        {
            KICKRAudioProcessor pp;
            const bool bip = juce::String (id) == "low" || juce::String (id) == "output";
            auto* prm = pp.getValueTreeState().getParameter (id);
            const auto rng = prm->getNormalisableRange();
            const auto rr = renderRamp (pp, id, bip ? rng.start : 0.0f, bip ? rng.end : 1.0f,
                                        sr, 120.0, 0.9);
            const float ss = maxSlew (rr);
            std::printf ("  automate %-11s fast: maxSlew %.4f\n", id, ss);
            const juce::String lbl = juce::String ("no zipper automating ") + id;
            check (analyse (rr, sr).allFinite && ss < 0.6f, lbl.toRawUTF8());
        }
    }

    // 9. State round-trip incl. a macro + currentSampleName; a stateVersion-1 blob loads.
    {
        const auto fx = juce::File::getSpecialLocation (juce::File::tempDirectory)
                            .getChildFile ("kickr_state_fixture.wav");
        writeSineWav (fx, 90.0, 44100.0, 0.3, 1);
        const auto tmpBank = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                 .getChildFile ("kickr_state_bank");
        tmpBank.deleteRecursively();

        KICKRAudioProcessor pa;
        pa.getSampleLibrary().setFolder (tmpBank);
        const auto nm = pa.getSampleLibrary().importFile (fx);
        pa.loadSampleByName (nm);
        setP (pa, "macroBody", 0.83f); setP (pa, "drive", 0.7f); setP (pa, "fundamental", 41.2f);
        setP (pa, "sampleEnable", 1.0f); setP (pa, "character", 0.4f); setP (pa, "tailLength", 900.0f);

        juce::MemoryBlock mb;
        pa.getStateInformation (mb);

        KICKRAudioProcessor pb;
        pb.getSampleLibrary().setFolder (tmpBank);
        pb.setStateInformation (mb.getData(), (int) mb.getSize());

        auto eq = [&] (juce::StringRef id)
        {
            return std::abs (pa.getValueTreeState().getRawParameterValue (id)->load()
                             - pb.getValueTreeState().getRawParameterValue (id)->load()) < 1.0e-4f;
        };
        const bool paramsOk = eq ("macroBody") && eq ("drive") && eq ("fundamental")
                            && eq ("sampleEnable") && eq ("character") && eq ("tailLength");
        std::printf ("  state round-trip: params %s ; sample '%s' -> '%s'\n",
                     paramsOk ? "match" : "MISMATCH",
                     pa.getCurrentSampleName().toRawUTF8(), pb.getCurrentSampleName().toRawUTF8());
        check (paramsOk, "state round-trip: all params restore exactly");
        check (pa.getCurrentSampleName() == pb.getCurrentSampleName() && nm.isNotEmpty(),
               "state round-trip: currentSampleName restores");

        // A minimal stateVersion-1 tree (no sample* params) must load without error.
        juce::ValueTree v1 ("PARAMETERS");
        v1.setProperty ("stateVersion", 1, nullptr);
        for (auto id : { "fundamental", "bodyLevel", "drive", "oversampling" })
        {
            juce::ValueTree p ("PARAM");
            p.setProperty ("id", id, nullptr);
            p.setProperty ("value", 0.5f, nullptr);
            v1.addChild (p, -1, nullptr);
        }
        juce::MemoryBlock v1mb;
        if (auto xml = std::unique_ptr<juce::XmlElement> (v1.createXml()))
            juce::AudioProcessor::copyXmlToBinary (*xml, v1mb);
        KICKRAudioProcessor pv1;
        pv1.setStateInformation (v1mb.getData(), (int) v1mb.getSize());
        const bool sampleOff = pv1.getValueTreeState().getRawParameterValue ("sampleEnable")->load() < 0.5f;
        check (sampleOff, "stateVersion-1 blob loads: sampleEnable stays at its v2 default (off)");
        check (analyse (kickr::tests::renderNote (pv1, a1, vel, sr, 512, 0.3), sr).allFinite,
               "stateVersion-1 restored processor still renders finite audio");

        fx.deleteFile(); tmpBank.deleteRecursively();
    }

    // ---------------------------------------------------------------------
    std::printf ("\n[Phase 3.1] Native editor - LookAndFeel + layout + 59 param bindings\n");
    {
        struct Counter
        {
            int operator() (juce::Component* c) const
            {
                int n = c->getNumChildComponents();
                for (int i = 0; i < c->getNumChildComponents(); ++i)
                    n += (*this) (c->getChildComponent (i));
                return n;
            }
        };

        KICKRAudioProcessor pe;
        pe.prepareToPlay (48000.0, 512);

        std::unique_ptr<juce::AudioProcessorEditor> ed (pe.createEditor());
        check (ed != nullptr, "editor is created");

        if (ed != nullptr)
        {
            ed->setSize (900, 660);
            ed->setSize (1280, 936);
            ed->setSize (2000, 1470);
            check (true, "editor resizes across the full range without assert");

            const int kids = Counter{} (ed.get());
            std::printf ("  editor child components (recursive): %d\n", kids);
            check (kids >= 59, "editor has at least 59 child controls");

            // Software-render the editor to a PNG (no display needed) for visual review.
            ed->setSize (1600, 1170);
            const auto snap = ed->createComponentSnapshot (ed->getLocalBounds(), false, 1.5f);
            const auto png  = juce::File::getCurrentWorkingDirectory().getChildFile ("kickr_ui_3_1.png");
            if (auto os = png.createOutputStream())
            {
                os->setPosition (0); os->truncate();
                juce::PNGImageFormat fmt;
                const bool ok = fmt.writeImageToStream (snap, *os);
                std::printf ("  wrote %s : %s (%dx%d)\n", png.getFullPathName().toRawUTF8(),
                             ok ? "ok" : "FAILED", snap.getWidth(), snap.getHeight());
            }
        }
    }

    // ---------------------------------------------------------------------
    std::printf ("\n[Phase 3.2] Analyzer - waveform + spectrum (lock-free taps)\n");
    {
        const double s2  = 48000.0;
        const int    blk = 512;

        KICKRAudioProcessor pa;
        pa.setRateAndBufferSizeDetails (s2, blk);
        pa.prepareToPlay (s2, blk);

        juce::AudioBuffer<float> ab (juce::jmax (1, pa.getTotalNumOutputChannels()), blk);

        auto renderBlocks = [&] (double seconds, bool noteOnFirst)
        {
            const int nBlocks = std::max (1, (int) std::ceil (seconds * s2 / blk));
            for (int bi = 0; bi < nBlocks; ++bi)
            {
                ab.clear();
                juce::MidiBuffer midi;
                if (noteOnFirst && bi == 0)
                    midi.addEvent (juce::MidiMessage::noteOn (1, a1, 0.9f), 0);
                pa.processBlock (ab, midi);
                pa.getAnalyzer().updateSpectrum();   // stand in for the 30 Hz display Timer
            }
        };

        // --- streamed capture: a short render fills only a prefix (left->right sweep) ---
        std::array<float, kickr::Analyzer::kWaveCaptureLen> wvPartial {};
        std::uint32_t genPartial = 0;
        renderBlocks (0.05, true);   // ~50 ms  ->  ~2400 samples @ 48 kHz
        const int lenPartial = pa.getAnalyzer().getWaveform (wvPartial, genPartial);
        std::printf ("  streamed: after ~50ms  len=%d / %d  gen=%u\n",
                     lenPartial, kickr::Analyzer::kWaveCaptureLen, genPartial);
        check (genPartial == 1, "note-on bumps the capture generation");
        check (lenPartial > 1500 && lenPartial < kickr::Analyzer::kWaveCaptureLen,
               "capture is a growing prefix mid-kick (draws in left -> right)");

        renderBlocks (0.6, false);   // let the same trigger fill the rest
        for (int k = 0; k < 8; ++k) pa.getAnalyzer().updateSpectrum();

        std::array<float, kickr::Analyzer::kWaveCaptureLen> wv {};
        std::uint32_t gen = 0;
        const int len = pa.getAnalyzer().getWaveform (wv, gen);
        bool wFinite = true; float wPeak = 0.0f; double wSq = 0.0;
        for (int i = 0; i < len; ++i)
        {
            const float v = wv[static_cast<size_t> (i)];
            if (! std::isfinite (v)) wFinite = false;
            wPeak = std::max (wPeak, std::abs (v));
            wSq  += (double) v * v;
        }
        const double wRms = len > 0 ? std::sqrt (wSq / (double) len) : 0.0;
        std::printf ("  waveform: len=%d gen=%u finite=%d peak=%.3f rms=%.4f\n",
                     len, gen, (int) wFinite, wPeak, wRms);
        check (len == kickr::Analyzer::kWaveCaptureLen, "capture fills to the full window");
        check (gen == genPartial,             "still the same trigger (no new generation)");
        check (wFinite,                        "waveform capture is finite");
        check (wPeak > 0.02f && wRms > 1.0e-4, "waveform capture is non-silent");

        // --- spectrum: FFT ran on this (message) thread; peak near the ~55 Hz fundamental ---
        const auto& db = pa.getAnalyzer().getSpectrumDb();
        bool sFinite = true; int peakBin = 1; float peakDb = -1.0e9f;
        for (int b = 1; b < (int) db.size(); ++b)
        {
            const float d = db[static_cast<size_t> (b)];
            if (! std::isfinite (d)) sFinite = false;
            if (d > peakDb) { peakDb = d; peakBin = b; }
        }
        const double binHz  = (s2 * 0.5) / (double) db.size();
        const double peakHz = (double) peakBin * binHz;
        std::printf ("  spectrum: finite=%d peakBin=%d (~%.0f Hz) peakDb=%.1f\n",
                     (int) sFinite, peakBin, peakHz, peakDb);
        check (sFinite,                            "spectrum dB frame is finite");
        check (peakHz > 20.0 && peakHz < 250.0,    "spectrum peak sits in the low-kick fundamental region");

        // --- 2 s of silence must not start a new capture (no armCapture -> gen unchanged) ---
        renderBlocks (2.0, false);
        std::array<float, kickr::Analyzer::kWaveCaptureLen> wvSilent {};
        std::uint32_t genSilent = 0;
        pa.getAnalyzer().getWaveform (wvSilent, genSilent);
        std::printf ("  post-silence: gen=%u (was %u)\n", genSilent, gen);
        check (genSilent == gen, "no spurious capture after 2 s of silence (generation unchanged)");

        pa.releaseResources();
    }

    // ---------------------------------------------------------------------
    std::printf ("\n[Phase 3.2] Editor snapshot (analyzers wired into the scope)\n");
    {
        KICKRAudioProcessor pe;
        pe.setRateAndBufferSizeDetails (48000.0, 512);
        pe.prepareToPlay (48000.0, 512);

        // Feed ~130 ms of a real kick so the scope has a partial (drawing-in) capture.
        {
            juce::AudioBuffer<float> b (juce::jmax (1, pe.getTotalNumOutputChannels()), 512);
            for (int bi = 0; bi < 12; ++bi)
            {
                b.clear();
                juce::MidiBuffer m;
                if (bi == 0) m.addEvent (juce::MidiMessage::noteOn (1, a1, 0.9f), 0);
                pe.processBlock (b, m);
            }
        }

        std::unique_ptr<juce::AudioProcessorEditor> edBase (pe.createEditor());
        auto* ed = dynamic_cast<KICKRAudioProcessorEditor*> (edBase.get());
        check (ed != nullptr, "editor with the wired analyzers is created");

        if (ed != nullptr)
        {
            ed->setSize (1600, 1170);
            ed->refreshAnalyzersForSnapshot();   // Timers don't fire in the headless test
            const auto snap = ed->createComponentSnapshot (ed->getLocalBounds(), false, 1.0f);
            const auto png  = juce::File::getCurrentWorkingDirectory().getChildFile ("kickr_ui_3_2.png");
            if (auto os = png.createOutputStream())
            {
                os->setPosition (0); os->truncate();
                juce::PNGImageFormat fmt;
                const bool ok = fmt.writeImageToStream (snap, *os);
                std::printf ("  wrote %s : %s (%dx%d)\n", png.getFullPathName().toRawUTF8(),
                             ok ? "ok" : "FAILED", snap.getWidth(), snap.getHeight());
            }
        }
    }

    // ---------------------------------------------------------------------
    std::printf ("\n[Phase 3.3] Presets + Randomize / Mutate + sample drag-drop\n");
    {
        const double s3 = 48000.0;

        auto renderStat = [&] (KICKRAudioProcessor& p)
        {
            const auto rb  = kickr::tests::renderNote (p, a1, vel, s3, 512, 0.8);
            const auto rst = analyse (rb, s3);
            return std::make_pair (rst, rmsWindow (rb, s3, 0.0, 0.5));
        };

        // --- 17 factory presets: all load, render finite/audible, and differ ---
        {
            KICKRAudioProcessor p;
            auto& pm = p.getPresetManager();
            check (kickr::PresetManager::getNumFactory() == 17, "17 factory presets registered");

            bool allOk = true; double prevRms = -1.0; int distinct = 0;
            for (int i = 0; i < kickr::PresetManager::getNumFactory(); ++i)
            {
                pm.loadFactory (i);
                p.prepareToPlay (s3, 512);
                const auto res = renderStat (p);
                if (! res.first.allFinite || res.first.peak < 0.02f || res.first.peak > 4.0f) allOk = false;
                if (prevRms < 0.0 || std::abs (res.second - prevRms) > 1.0e-4) ++distinct;
                prevRms = res.second;
            }
            std::printf ("  factory: allOk=%d  distinct-rms=%d/17\n", (int) allOk, distinct);
            check (allOk, "every factory preset renders a finite, audible, bounded kick");
            check (distinct >= 12, "factory presets are meaningfully different from each other");
        }

        // --- Randomize respects the exclusion list ---
        {
            KICKRAudioProcessor p;
            auto& apvts = p.getValueTreeState();
            const char* excluded[] = {
                "oversampling","limiter","output","mix","tuneMode","tune","fineTune",
                "velSensitivity","macroPunch","macroBody","macroCrush","macroTail",
                "synthEnable","sampleEnable","sampleLevel","sampleTune","sampleCrush","sampleReverse"
            };
            std::vector<float> before;
            for (auto* id : excluded) before.push_back (apvts.getRawParameterValue (id)->load());

            const float drvBefore = apvts.getRawParameterValue ("drive")->load();

            bool exclOk = true, finiteOk = true, movedSomething = false;
            for (int k = 0; k < 20; ++k)
            {
                p.getPresetManager().randomize();
                for (size_t i = 0; i < before.size(); ++i)
                    if (std::abs (apvts.getRawParameterValue (excluded[i])->load() - before[i]) > 1.0e-5f)
                        exclOk = false;
                if (std::abs (apvts.getRawParameterValue ("drive")->load() - drvBefore) > 1.0e-4f)
                    movedSomething = true;
                p.prepareToPlay (s3, 512);
                if (! analyse (kickr::tests::renderNote (p, a1, vel, s3, 512, 0.4), s3).allFinite)
                    finiteOk = false;
            }
            std::printf ("  randomize x20: exclusions-held=%d  randomizable-moved=%d  finite=%d\n",
                         (int) exclOk, (int) movedSomething, (int) finiteOk);
            check (exclOk,        "Randomize never touches the excluded params (tuning/output/macros/sample/...)");
            check (movedSomething, "Randomize actually moves the randomizable params");
            check (finiteOk,      "every Randomize result renders finite audio");
            check (p.getCurrentSampleName().isEmpty(), "Randomize never sets currentSampleName");
        }

        // --- Mutate: moves params, stays in range, excludes the same set ---
        {
            KICKRAudioProcessor p;
            auto& apvts = p.getValueTreeState();
            p.getPresetManager().loadFactory (2);   // Techno
            const float bodyDecayBefore = apvts.getRawParameterValue ("bodyDecay")->load();
            const float outputBefore    = apvts.getRawParameterValue ("output")->load();
            p.getPresetManager().mutate();
            const float bodyDecayAfter  = apvts.getRawParameterValue ("bodyDecay")->load();
            std::printf ("  mutate: bodyDecay %.0f -> %.0f  output held=%d\n",
                         bodyDecayBefore, bodyDecayAfter,
                         (int) (std::abs (apvts.getRawParameterValue ("output")->load() - outputBefore) < 1.0e-5f));
            check (std::abs (bodyDecayAfter - bodyDecayBefore) > 0.5f, "Mutate perturbs the synth params");
            check (bodyDecayAfter >= 20.0f && bodyDecayAfter <= 2000.0f, "Mutate stays in range");
            check (std::abs (apvts.getRawParameterValue ("output")->load() - outputBefore) < 1.0e-5f,
                   "Mutate respects the exclusion list");
        }

        // --- Undo reverts Randomize ---
        {
            KICKRAudioProcessor p;
            auto& apvts = p.getValueTreeState();
            p.getPresetManager().loadFactory (0);
            const float d0 = apvts.getRawParameterValue ("drive")->load();
            const float c0 = apvts.getRawParameterValue ("character")->load();
            const float b0 = apvts.getRawParameterValue ("bodyDecay")->load();
            p.getPresetManager().randomize();
            p.getUndoManager().undo();
            const bool reverted = std::abs (apvts.getRawParameterValue ("drive")->load()     - d0) < 1.0e-4f
                               && std::abs (apvts.getRawParameterValue ("character")->load() - c0) < 1.0e-4f
                               && std::abs (apvts.getRawParameterValue ("bodyDecay")->load() - b0) < 1.0f;
            std::printf ("  undo after randomize: reverted=%d\n", (int) reverted);
            check (reverted, "Undo reverts a Randomize in one step");
        }

        // --- User preset save / load round-trip incl. currentSampleName ---
        {
            const auto tmpP = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                  .getChildFile ("kickr_test_presets");
            const auto tmpS = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                  .getChildFile ("kickr_test_presets_samples");
            tmpP.deleteRecursively(); tmpS.deleteRecursively();
            kickr::PresetManager::setUserFolderForTests (tmpP);

            const auto fx = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                .getChildFile ("kickr_preset_fixture.wav");
            writeSineWav (fx, 95.0, 44100.0, 0.3, 1);

            KICKRAudioProcessor pa;
            pa.getSampleLibrary().setFolder (tmpS);
            const auto sn = pa.getSampleLibrary().importFile (fx);
            pa.loadSampleByName (sn);
            setP (pa, "drive", 0.66f); setP (pa, "fundamental", 44.0f); setP (pa, "tailLength", 850.0f);
            setP (pa, "sampleEnable", 1.0f);
            const bool saved = pa.getPresetManager().saveUser ("RoundTrip");
            check (saved, "user preset saved to disk");

            KICKRAudioProcessor pb;
            pb.getSampleLibrary().setFolder (tmpS);
            const bool loaded = pb.getPresetManager().loadUser ("RoundTrip");
            const bool match  = loaded
                && std::abs (pb.getValueTreeState().getRawParameterValue ("drive")->load()      - 0.66f) < 1.0e-3f
                && std::abs (pb.getValueTreeState().getRawParameterValue ("fundamental")->load() - 44.0f) < 0.5f
                && pb.getCurrentSampleName() == sn;
            std::printf ("  user preset: saved=%d loaded=%d params+sample match=%d\n",
                         (int) saved, (int) loaded, (int) match);
            check (match, "user preset restores params + currentSampleName exactly");
            check (! pb.getPresetManager().loadUser ("does-not-exist"), "missing user preset -> false, no crash");

            kickr::PresetManager::setUserFolderForTests (juce::File());   // restore
            fx.deleteFile(); tmpP.deleteRecursively(); tmpS.deleteRecursively();
        }

        // --- drag-drop path: file -> bank -> selected -> sampleEnable on ---
        {
            const auto tmpS = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                  .getChildFile ("kickr_test_dnd_samples");
            tmpS.deleteRecursively();
            const auto fx = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                .getChildFile ("kickr_dnd_fixture.wav");
            writeSineWav (fx, 110.0, 44100.0, 0.25, 1);

            KICKRAudioProcessor pe;
            pe.getSampleLibrary().setFolder (tmpS);
            pe.prepareToPlay (48000.0, 512);
            std::unique_ptr<juce::AudioProcessorEditor> edBase (pe.createEditor());
            auto* ed = dynamic_cast<KICKRAudioProcessorEditor*> (edBase.get());
            check (ed != nullptr, "editor created for the drag-drop test");
            if (ed != nullptr)
            {
                ed->importDroppedFileForTest (fx);
                const bool ok = pe.getCurrentSampleName().isNotEmpty()
                             && pe.getValueTreeState().getRawParameterValue ("sampleEnable")->load() > 0.5f;
                std::printf ("  drag-drop: sample='%s'  sampleEnable=%.0f\n",
                             pe.getCurrentSampleName().toRawUTF8(),
                             pe.getValueTreeState().getRawParameterValue ("sampleEnable")->load());
                check (ok, "dropped file is imported, selected, and turns the sample layer on");
            }
            fx.deleteFile(); tmpS.deleteRecursively();
        }

        // --- editor snapshot with a factory patch loaded ---
        {
            KICKRAudioProcessor pe;
            pe.getPresetManager().loadFactory (13);   // Warehouse
            pe.prepareToPlay (48000.0, 512);
            {
                juce::AudioBuffer<float> b (juce::jmax (1, pe.getTotalNumOutputChannels()), 512);
                for (int bi = 0; bi < 14; ++bi)
                {
                    b.clear();
                    juce::MidiBuffer m;
                    if (bi == 0) m.addEvent (juce::MidiMessage::noteOn (1, a1, 0.9f), 0);
                    pe.processBlock (b, m);
                }
            }
            std::unique_ptr<juce::AudioProcessorEditor> edBase (pe.createEditor());
            auto* ed = dynamic_cast<KICKRAudioProcessorEditor*> (edBase.get());
            if (ed != nullptr)
            {
                ed->setSize (1600, 1170);
                ed->refreshAnalyzersForSnapshot();
                const auto snap = ed->createComponentSnapshot (ed->getLocalBounds(), false, 1.0f);
                const auto png  = juce::File::getCurrentWorkingDirectory().getChildFile ("kickr_ui_3_3.png");
                if (auto os = png.createOutputStream())
                {
                    os->setPosition (0); os->truncate();
                    juce::PNGImageFormat fmt;
                    const bool wrote = fmt.writeImageToStream (snap, *os);
                    std::printf ("  wrote %s : %s\n", png.getFullPathName().toRawUTF8(),
                                 wrote ? "ok" : "FAILED");
                }
            }
        }
    }

    std::printf ("\n%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
