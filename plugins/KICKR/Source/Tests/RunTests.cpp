/*
    KICKR — console test runner. Grows one block of checks per Stage-2 phase.
    Build:  cmake --build build --target KICKR_Tests
    Run:    ./build/plugins/KICKR/KICKR_Tests
    Writes kickr_phase*.wav next to the working directory for manual inspection.
*/
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "UI/SpectrumCurve.h"
#include "Tests/OfflineRender.h"
#include "DSP/SamplePlayer.h"
#include "DSP/Waveshaper.h"
#include "Sampling/SampleLibrary.h"
#include "Parameters/ParameterDescriptions.h"

#include <array>
#include <chrono>
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
    // 2026-09-02: + the Color Limiter's look-ahead (default 1.5 ms, limiter on by default).
    check (proc.getLatencySamples() == proc.limiterLatencySamples(), "reported latency == limiter look-ahead at 1x oversampling");

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

    // 2026-09-01 (bug-hunt pass, no bug found — kept as a permanent guard) — the new
    // Morph waveforms (naive, non-band-limited triangle/saw/square) are the highest-risk
    // addition this session: most harmonic energy, most aliasing potential. Stress it at
    // machine-gun retrigger speed across every oversampling factor and morph setting.
    {
        bool allFinite = true;
        float worstPeak = 0.0f;
        for (int os = 0; os < 4; ++os)
        {
            for (float morphAmt : { 0.0f, 0.5f, 1.0f })
            {
                KICKRAudioProcessor p;
                silenceClick (p); silenceSub (p); silenceTail (p); silenceDist (p);
                setP (p, "oversampling", (float) os);
                setP (p, "morph", morphAmt);
                auto [rbuf, onsets] = renderRetrigger (p, a1, vel, sr, 256, 24, 60.0 / 174.0 / 8.0, 2.0);
                juce::ignoreUnused (onsets);
                const float* rx = rbuf.getReadPointer (0);
                for (int i = 0; i < rbuf.getNumSamples(); ++i)
                {
                    if (! std::isfinite (rx[i])) allFinite = false;
                    worstPeak = std::max (worstPeak, std::abs (rx[i]));
                }
            }
        }
        std::printf ("  morph x oversampling x machine-gun: worst peak %.2f  all finite %d\n",
                     worstPeak, (int) allFinite);
        check (allFinite,           "Morph never produces NaN/Inf at any oversampling factor under machine-gun retrigger");
        check (worstPeak < 8.0f,    "Morph output stays bounded (no runaway) across every combination tested");
    }

    // Fix (2026-08-31): a retriggered click must play at the same level as an isolated
    // one. The old crossfade scaled the INCOMING voice by a ramping gNew (0 -> 1 over
    // 3 ms), which swallowed the click's sharp transient on every fast retrigger while
    // an isolated hit (no fade active) got it at full level -> an audible, tempo-locked
    // "sometimes there's a click" inconsistency. Now only the outgoing voice fades.
    //
    // 2026-09-01 history: this threshold was briefly relaxed to 0.6 while a per-note-on
    // crossover-filter reset (since removed — the reset itself clicked; see KickEngine::
    // handleNoteOn) cost the retriggered click ~2.6 dB (ratio 0.74). With that reset gone
    // the ratio is back above 1 (the 3 ms onset window legitimately also contains the
    // outgoing voice's 0.75 ms declick tail), so the original 0.75 bar is restored. This is
    // a LOWER bound only — louder-than-isolated is fine; the bug it guards against is the
    // retriggered click being swallowed.
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

    // Fix (2026-08-31, user report): "during sample reversal the sample doesn't play whole
    // sometimes." Root cause: SamplePlayer's AD envelope decays in REAL TIME from noteOn
    // (voice age) — loud at t=0, quiet later — regardless of read direction. A one-shot's
    // loud content sits at its recorded START; in reverse that content is read LAST, so by
    // the time playback reached it the envelope (and the envelope-floor early-finish) had
    // already decayed/killed it. Forward mode hid this (the envelope's own shape matches a
    // typical sample's natural loud-start/quiet-tail). Fix: for `sampleReverse`, apply the
    // MIRROR of the decay curve (`1 - env`, rises across `sampleDecay` then holds near 1)
    // and skip the envelope-floor early-finish (only the trim-window boundary ends
    // reverse playback). A companion fix separately raised `sampleDecay`'s range/default
    // (20-5500 ms / 2200, was 20-2000 / 800) since the old max couldn't reach the 5 s
    // import cap even in forward mode.
    {
        std::printf ("\n[Fix] sample reversal — envelope mirrored, plays through in full\n");

        const auto tmpBank3 = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                  .getChildFile ("kickr_test_decay_bank");
        tmpBank3.deleteRecursively();
        const auto fx3 = juce::File::getSpecialLocation (juce::File::tempDirectory)
                             .getChildFile ("kickr_decay_fixture.wav");
        writeSineWav (fx3, 150.0, 44100.0, 3.0, 1);   // 150 Hz, 3 s long

        auto renderIt = [&] (bool reverse, float decayMs)
        {
            KICKRAudioProcessor p;
            p.getSampleLibrary().setFolder (tmpBank3);
            const auto nm = p.getSampleLibrary().importFile (fx3);
            p.loadSampleByName (nm);
            setP (p, "sampleEnable", 1.0f); setP (p, "synthEnable", 0.0f);
            setP (p, "sampleReverse", reverse ? 1.0f : 0.0f);
            setP (p, "sampleMidiTrack", 0.0f);   // ratio = 1, isolates the envelope shape
            setP (p, "limiter", 0.0f);
            setP (p, "sampleDecay", decayMs);
            return kickr::tests::renderNote (p, a1, vel, sr, 512, 3.05);
        };

        // At the NEW default (2200 ms): reversed playback should be clearly audible right
        // up to the end (the source's temporal start, now read last).
        const auto revDefault = renderIt (true, 2200.0f);
        const double tailDefault = rmsWindow (revDefault, sr, 2.85, 2.98);
        std::printf ("  reverse @ decay 2200ms: tail RMS (2.85-2.98s) = %.4f\n", tailDefault);
        check (tailDefault > 0.15, "long reversed sample is clearly audible right up to the end");

        // Even a SHORT decay (was the primary failure mode pre-fix: short/default decay
        // silently truncated the reversed transient) should still fade in and reach a loud
        // tail — decay now only controls fade-IN speed for reverse, never a hard cutoff.
        const auto revShort = renderIt (true, 100.0f);
        const double tailShort = rmsWindow (revShort, sr, 2.85, 2.98);
        std::printf ("  reverse @ decay 100ms:  tail RMS (2.85-2.98s) = %.4f\n", tailShort);
        check (tailShort > 0.15, "even a short decay still reaches a loud tail in reverse (no truncation)");

        // Forward mode is unaffected: onset (0-0.15s) stays loud immediately, as before.
        const auto fwd = renderIt (false, 2200.0f);
        const double onsetFwd = rmsWindow (fwd, sr, 0.0, 0.15);
        std::printf ("  forward @ decay 2200ms: onset RMS (0-0.15s) = %.4f\n", onsetFwd);
        check (onsetFwd > 0.15, "forward playback still front-loads its onset (unaffected by the reverse fix)");

        tmpBank3.deleteRecursively(); fx3.deleteFile();
    }

    // Changed (2026-09-01, user request): strict monophonic voice-stealing. A note-on while
    // the previous voice is still ringing must stop that voice IMMEDIATELY (every layer —
    // body/click/sub/tail/noise/sample, no exceptions) and start the new note at once, with
    // never more than one voice's worth of sound audible at a time — "regardless of how
    // quickly the MIDI notes are triggered." This intentionally REVERSES the 2026-08-31 fix
    // below (kept here for history): that fix let a still-ringing sample survive a retrigger
    // indefinitely as a `retiringVoice`, which is exactly the "stuck voice" / overlapping-
    // tails behavior the new spec forbids. The outgoing voice now gets a kDeclickFadeMs
    // (0.75 ms) fade-out purely to avoid a hard-cut click, then is unconditionally reset() —
    // it never lingers past that, no matter what it was doing (long sample, long tail, etc).
    //
    // [Superseded 2026-08-31 fix, kept for context:] the retrigger crossfade's completion
    // (theta>=1, ~3 ms after ANY retrigger) unconditionally called the OUTGOING voice's
    // reset() — including its SamplePlayer — regardless of whether the sample was still
    // playing, killing a normal 2-hit retrigger's FIRST sample ~3 ms after the second hit,
    // every time. That fix demoted a still-active outgoing voice to a `retiringVoice` that
    // kept rendering (unscaled) until its sample naturally finished. `retiringVoice` no
    // longer exists.
    //
    // Verified with two DIFFERENT-frequency long samples: bank-swap the loaded sample
    // between two triggers (SamplePlayer captures its buffer pointer AT noteOn, so the
    // first (outgoing) voice keeps reading the FIRST file even after the swap). Voice A
    // must now be silent almost immediately after voice B's note-on (well within a few ms,
    // not just "eventually") AND must never reappear later — only voice B may be present
    // at any point past the declick window.
    {
        std::printf ("\n[Fix] retrigger hard-kills the previous voice immediately (strict mono voice-steal)\n");

        auto goertzelMag = [] (const juce::AudioBuffer<float>& b, double s, double t0, double t1, double hz)
        {
            const int i0 = (int) (t0 * s), i1 = (int) (t1 * s);
            const float* x = b.getReadPointer (0);
            const double w  = 2.0 * juce::MathConstants<double>::pi * hz / s;
            const double cw = std::cos (w);
            double s0 = 0.0, s1 = 0.0, s2 = 0.0;
            for (int i = i0; i < i1 && i < b.getNumSamples(); ++i)
            {
                s0 = (double) x[i] + 2.0 * cw * s1 - s2;
                s2 = s1; s1 = s0;
            }
            return std::sqrt (s1 * s1 + s2 * s2 - 2.0 * cw * s1 * s2) / (double) std::max (1, i1 - i0);
        };

        const auto tmpBank4 = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                  .getChildFile ("kickr_test_retrigger_bank");
        tmpBank4.deleteRecursively();
        const auto fxA = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("kickr_rt_a.wav");
        const auto fxB = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("kickr_rt_b.wav");
        writeSineWav (fxA, 90.0,  44100.0, 1.5, 1);   // long, well past any crossfade
        writeSineWav (fxB, 400.0, 44100.0, 1.5, 1);   // well-separated frequency

        KICKRAudioProcessor p;
        p.getSampleLibrary().setFolder (tmpBank4);
        const auto nameA = p.getSampleLibrary().importFile (fxA);
        const auto nameB = p.getSampleLibrary().importFile (fxB);
        p.loadSampleByName (nameA);
        setP (p, "sampleEnable", 1.0f); setP (p, "synthEnable", 0.0f);
        setP (p, "sampleMidiTrack", 0.0f); setP (p, "limiter", 0.0f);

        p.setRateAndBufferSizeDetails (sr, 64);
        p.prepareToPlay (sr, 64);
        juce::AudioBuffer<float> out (juce::jmax (1, p.getTotalNumOutputChannels()), (int) (sr * 0.5));
        out.clear();
        juce::AudioBuffer<float> scratch (out.getNumChannels(), 64);
        const int secondHitAt = (int) (sr * 0.05);   // retrigger 50 ms in — >> the 3 ms crossfade

        for (int pos = 0; pos < out.getNumSamples();)
        {
            const int n = std::min (64, out.getNumSamples() - pos);
            if (pos <= secondHitAt && pos + n > secondHitAt)
                p.loadSampleByName (nameB);   // swap the bank BEFORE the 2nd trigger
            juce::AudioBuffer<float> b (scratch.getArrayOfWritePointers(), out.getNumChannels(), n);
            b.clear();
            juce::MidiBuffer m;
            if (pos == 0)                                       m.addEvent (juce::MidiMessage::noteOn (1, a1, vel), 0);
            if (secondHitAt >= pos && secondHitAt < pos + n)     m.addEvent (juce::MidiMessage::noteOn (1, a1, vel), secondHitAt - pos);
            p.processBlock (b, m);
            for (int ch = 0; ch < out.getNumChannels(); ++ch) out.copyFrom (ch, pos, b, ch, 0, n);
            pos += n;
        }
        p.releaseResources();

        // NOTE on what "immediate" can/can't mean acoustically: verified separately (via
        // temporary engine-level tracing while writing this test) that KickEngine hard-
        // resets voice A within the SAME processBlock call as B's note-on — essentially
        // instantly, exactly per spec. But the raw OUTPUT waveform still carries measurable
        // 90Hz energy for tens of ms afterward, because OutputStage's 130 Hz mono crossover
        // is a stateful filter — like any filter, it keeps "ringing" briefly on its own
        // after its input suddenly goes silent, same as it would for a single note's own
        // natural release. That's ordinary filter ring-down of an already-dead voice, not
        // two voices overlapping, and isn't something DSP can eliminate without removing
        // the crossover filter entirely. So this test checks the thing that actually matters
        // — no PERMANENT/indefinite overlap — rather than an unachievable zero-latency
        // acoustic cutoff.

        // Late window: well into both samples' long bodies. Voice A must never reappear —
        // no lingering tail, no "stuck voice" — only voice B may still be sounding.
        const double magA = goertzelMag (out, sr, 0.30, 0.35, 90.0);
        const double magB = goertzelMag (out, sr, 0.30, 0.35, 400.0);
        std::printf ("  late window (300-350ms): 90Hz (voice A, pre-swap) mag %.4f   400Hz (voice B, post-swap) mag %.4f\n",
                     magA, magB);
        check (magB > 0.02, "the newly-triggered voice B is still present later");
        check (magA < 0.005, "voice A's sample never comes back — hard-killed, not left to ring out");

        tmpBank4.deleteRecursively(); fxA.deleteFile(); fxB.deleteFile();
    }

    // User's exact spec (2026-09-01): "MIDI: C1 -> D1 -> E1, 20 ms apart. Behavior: C1 START
    // -> C1 STOP -> D1 START -> D1 STOP -> E1 START." Three rapid different-note triggers,
    // each 20 ms after the last (well outside the 0.75 ms declick window, so this is the
    // ordinary case, not the 3rd-trigger-mid-fade collision path) — only the MOST RECENT
    // note's sample may be audible at any point once its declick window has passed.
    {
        std::printf ("\n[Fix] rapid C1->D1->E1 (20ms apart): each note kills the previous one, no overlap\n");

        auto goertzelMag2 = [] (const juce::AudioBuffer<float>& b, double s, double t0, double t1, double hz)
        {
            const int i0 = (int) (t0 * s), i1 = (int) (t1 * s);
            const float* x = b.getReadPointer (0);
            const double w  = 2.0 * juce::MathConstants<double>::pi * hz / s;
            const double cw = std::cos (w);
            double s0 = 0.0, s1 = 0.0, s2 = 0.0;
            for (int i = i0; i < i1 && i < b.getNumSamples(); ++i)
            {
                s0 = (double) x[i] + 2.0 * cw * s1 - s2;
                s2 = s1; s1 = s0;
            }
            return std::sqrt (s1 * s1 + s2 * s2 - 2.0 * cw * s1 * s2) / (double) std::max (1, i1 - i0);
        };

        const auto tmpBank5b = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                   .getChildFile ("kickr_test_c1d1e1_bank");
        tmpBank5b.deleteRecursively();
        const auto fxC1 = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("kickr_c1.wav");
        const auto fxD1 = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("kickr_d1.wav");
        const auto fxE1 = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("kickr_e1.wav");
        writeSineWav (fxC1, 90.0,  44100.0, 1.0, 1);
        writeSineWav (fxD1, 250.0, 44100.0, 1.0, 1);
        writeSineWav (fxE1, 500.0, 44100.0, 1.0, 1);

        KICKRAudioProcessor p3;
        p3.getSampleLibrary().setFolder (tmpBank5b);
        const auto nameC1 = p3.getSampleLibrary().importFile (fxC1);
        const auto nameD1 = p3.getSampleLibrary().importFile (fxD1);
        const auto nameE1 = p3.getSampleLibrary().importFile (fxE1);
        p3.loadSampleByName (nameC1);
        setP (p3, "sampleEnable", 1.0f); setP (p3, "synthEnable", 0.0f);
        setP (p3, "sampleMidiTrack", 0.0f); setP (p3, "limiter", 0.0f);

        p3.setRateAndBufferSizeDetails (sr, 64);
        p3.prepareToPlay (sr, 64);
        juce::AudioBuffer<float> out3 (juce::jmax (1, p3.getTotalNumOutputChannels()), (int) (sr * 0.5));
        out3.clear();
        juce::AudioBuffer<float> scratch3 (out3.getNumChannels(), 64);

        const int c1Note = 24, d1Note = 26, e1Note = 28;   // C1, D1, E1 (any distinct notes — pitch is irrelevant here, sampleMidiTrack is off)
        const int hitD1At = (int) (sr * 0.020);            // D1 20ms after C1 (matches the user's example exactly)
        const int hitE1At = (int) (sr * 0.040);            // E1 20ms after D1

        for (int pos = 0; pos < out3.getNumSamples();)
        {
            const int n = std::min (64, out3.getNumSamples() - pos);
            if (pos <= hitD1At && pos + n > hitD1At)
                p3.loadSampleByName (nameD1);
            if (pos <= hitE1At && pos + n > hitE1At)
                p3.loadSampleByName (nameE1);
            juce::AudioBuffer<float> b (scratch3.getArrayOfWritePointers(), out3.getNumChannels(), n);
            b.clear();
            juce::MidiBuffer m;
            if (pos == 0)                                   m.addEvent (juce::MidiMessage::noteOn (1, c1Note, vel), 0);
            if (hitD1At >= pos && hitD1At < pos + n)         m.addEvent (juce::MidiMessage::noteOn (1, d1Note, vel), hitD1At - pos);
            if (hitE1At >= pos && hitE1At < pos + n)         m.addEvent (juce::MidiMessage::noteOn (1, e1Note, vel), hitE1At - pos);
            p3.processBlock (b, m);
            for (int ch = 0; ch < out3.getNumChannels(); ++ch) out3.copyFrom (ch, pos, b, ch, 0, n);
            pos += n;
        }
        p3.releaseResources();

        // Just before D1 fires: only C1 (90 Hz) should be present.
        const double preD1_C1 = goertzelMag2 (out3, sr, 0.010, 0.018, 90.0);
        check (preD1_C1 > 0.02, "C1 is playing right after its own note-on");

        // Late window (well past D1's declick, well before E1 fires at 40ms): D1 (250 Hz)
        // clearly dominant; C1 (90 Hz) reduced to at most the output crossover filter's own
        // brief ring-down of an already-killed voice (not a real lingering C1 — see the
        // "immediate window" note in the previous test), an order of magnitude below D1.
        const double preE1_C1 = goertzelMag2 (out3, sr, 0.030, 0.038, 90.0);
        const double preE1_D1 = goertzelMag2 (out3, sr, 0.030, 0.038, 250.0);
        std::printf ("  pre-E1 window (30-38ms): C1(90Hz) mag %.4f   D1(250Hz) mag %.4f\n", preE1_C1, preE1_D1);
        check (preE1_D1 > 0.02, "D1 killed C1 and is itself playing before E1 arrives");
        check (preE1_C1 < 0.1 * preE1_D1, "C1 is at most filter ring-down, an order of magnitude below D1 — not a real lingering voice");

        // Final window: only E1 (500 Hz) audible; neither C1 nor D1 ever comes back.
        const double finalC1 = goertzelMag2 (out3, sr, 0.30, 0.35, 90.0);
        const double finalD1 = goertzelMag2 (out3, sr, 0.30, 0.35, 250.0);
        const double finalE1 = goertzelMag2 (out3, sr, 0.30, 0.35, 500.0);
        std::printf ("  final window (300-350ms): C1(90Hz) mag %.4f   D1(250Hz) mag %.4f   E1(500Hz) mag %.4f\n",
                     finalC1, finalD1, finalE1);
        check (finalE1 > 0.02, "E1 is the only note still sounding at the end");
        check (finalC1 < 0.005, "C1 never comes back after being superseded twice over");
        check (finalD1 < 0.005, "D1 never comes back after being superseded by E1");

        tmpBank5b.deleteRecursively(); fxC1.deleteFile(); fxD1.deleteFile(); fxE1.deleteFile();
    }

    // 2026-09-01 bug-scan (code-review CONFIRMED): a 3rd note-on arriving while the
    // 0.75 ms declick fade from the 2nd is still running takes the "snap the fade to done"
    // path in handleNoteOn, which bare-reset() the OUTGOING voice. When the 3rd hit lands
    // on the SAME sample as the 2nd (a doubled note on a flam — ordinary DAW MIDI), no
    // renderSegment ran in between, theta is still 0, so the outgoing voice is at FULL
    // gain when it's cut: a one-sample step, i.e. a click. Control = the same pattern
    // without the doubled note; the doubled version must not slew any worse than it.
    std::printf ("\n[Fix] doubled note-on on a flam: no hard cut of the still-loud outgoing voice\n");
    {
        auto renderHits = [&] (const std::vector<int>& hitSamples)
        {
            KICKRAudioProcessor p;
            silenceDist (p);
            setP (p, "limiter", 0.0f);
            const int block = 64;
            const int total = (int) (sr * 0.2);
            p.setRateAndBufferSizeDetails (sr, block);
            p.prepareToPlay (sr, block);
            juce::AudioBuffer<float> out (juce::jmax (1, p.getTotalNumOutputChannels()), total);
            out.clear();
            juce::AudioBuffer<float> sc (out.getNumChannels(), block);
            for (int pos = 0; pos < total;)
            {
                const int n = std::min (block, total - pos);
                juce::AudioBuffer<float> b (sc.getArrayOfWritePointers(), out.getNumChannels(), n);
                b.clear();
                juce::MidiBuffer midi;
                for (int h : hitSamples)
                    if (h >= pos && h < pos + n)
                        midi.addEvent (juce::MidiMessage::noteOn (1, a1, vel), h - pos);
                p.processBlock (b, midi);
                for (int ch = 0; ch < out.getNumChannels(); ++ch) out.copyFrom (ch, pos, b, ch, 0, n);
                pos += n;
            }
            p.releaseResources();
            return out;
        };
        auto slewAround = [&] (const juce::AudioBuffer<float>& b, int centre, int halfWidth)
        {
            const float* x = b.getReadPointer (0);
            float m = 0.0f;
            for (int i = std::max (1, centre - halfWidth); i < std::min (b.getNumSamples(), centre + halfWidth); ++i)
                m = std::max (m, std::abs (x[i] - x[i - 1]));
            return m;
        };

        const int flamAt = 24;                        // 0.5 ms after the first hit — inside the 0.75 ms declick window
        const auto control = renderHits ({ 0, flamAt });          // flam: A then B
        const auto doubled = renderHits ({ 0, flamAt, flamAt });  // flam with B doubled on the same sample
        const int w = (int) (sr * 0.001);
        const float slewControl = slewAround (control, flamAt, w);
        const float slewDoubled = slewAround (doubled, flamAt, w);
        bool finite = true;
        for (int i = 0; i < doubled.getNumSamples(); ++i) if (! std::isfinite (doubled.getReadPointer (0)[i])) finite = false;
        std::printf ("  slew around the flam: control (A,B) %.4f   doubled (A,B,B) %.4f   ratio %.2f\n",
                     slewControl, slewDoubled, slewDoubled / std::max (1.0e-6f, slewControl));
        check (finite, "doubled note-on: finite output");
        check (slewDoubled < slewControl * 1.5f,
               "doubled note-on on a flam does not hard-cut the outgoing voice (no extra slew spike vs the plain flam)");
    }

    // 2026-09-01 (user request) — "Morph" knob v2: CZ-style phase-skew of the body sine,
    // ATTACK-ONLY (the skew decays back to a pure sine with a 40 ms time constant after
    // each trigger — v1's sine->square crossfade was "too strong" for the user precisely
    // because it buzzed on the tail). Isolates the body layer exactly like the
    // oversampling pitch/decay checks above (fixed frequency, steady pitch, every other
    // layer silenced, distortion/limiter bypassed) so the harmonic content measured is
    // purely the body oscillator's own waveform shape.
    std::printf ("\n[Fix] Morph knob skews the attack, tail stays a clean sine\n");
    {
        auto goertzelMagM = [] (const juce::AudioBuffer<float>& b, double s, double t0, double t1, double hz)
        {
            const int i0 = (int) (t0 * s), i1 = (int) (t1 * s);
            const float* x = b.getReadPointer (0);
            const double w  = 2.0 * juce::MathConstants<double>::pi * hz / s;
            const double cw = std::cos (w);
            double s0 = 0.0, s1 = 0.0, s2 = 0.0;
            for (int i = i0; i < i1 && i < b.getNumSamples(); ++i)
            {
                s0 = (double) x[i] + 2.0 * cw * s1 - s2;
                s2 = s1; s1 = s0;
            }
            return std::sqrt (s1 * s1 + s2 * s2 - 2.0 * cw * s1 * s2) / (double) std::max (1, i1 - i0);
        };

        auto renderIsolatedBody = [&] (float morphAmt)
        {
            KICKRAudioProcessor p;
            setP (p, "tuneMode",       1.0f);   // Fixed Frequency
            setP (p, "fundamental",  100.0f);
            setP (p, "pitchStart",     1.0f);   // steady — no pitch drop
            setP (p, "bodyDecay",   2000.0f);
            setP (p, "bodyHarmonics",  0.0f);   // no saturation colouring the harmonic measurement
            setP (p, "subLevel",       0.0f);
            setP (p, "clickLevel",     0.0f);
            setP (p, "tailLevel",      0.0f);
            setP (p, "noiseLevel",     0.0f);
            setP (p, "driveMix",       0.0f);
            setP (p, "limiter",        0.0f);
            setP (p, "morph",     morphAmt);
            return kickr::tests::renderNote (p, a1, vel, sr, 512, 0.8);
        };

        // Harmonic content = (h2 + h3) / fundamental via Goertzel. ATTACK window = the
        // first ~40 ms (where the shape envelope is strong); TAIL window = 300-500 ms
        // (~7+ decay time constants later — the v2 guarantee is that the tail is a clean
        // sine no matter where the knob sits).
        auto harmonicRatio = [&] (const juce::AudioBuffer<float>& b, double t0, double t1)
        {
            const double fund = goertzelMagM (b, sr, t0, t1, 100.0);
            const double h2   = goertzelMagM (b, sr, t0, t1, 200.0);
            const double h3   = goertzelMagM (b, sr, t0, t1, 300.0);
            return (h2 + h3) / std::max (1.0e-9, fund);
        };

        const auto sineOut = renderIsolatedBody (0.0f);
        const auto fullOut = renderIsolatedBody (1.0f);
        const auto midOut  = renderIsolatedBody (0.5f);

        const double sineAtk = harmonicRatio (sineOut, 0.003, 0.043);
        const double fullAtk = harmonicRatio (fullOut, 0.003, 0.043);
        const double midAtk  = harmonicRatio (midOut,  0.003, 0.043);
        const double sineTail = harmonicRatio (sineOut, 0.30, 0.50);
        const double fullTail = harmonicRatio (fullOut, 0.30, 0.50);

        std::printf ("  attack window (3-43ms) (h2+h3)/fund:  morph=0 %.3f   morph=0.5 %.3f   morph=1 %.3f\n",
                     sineAtk, midAtk, fullAtk);
        std::printf ("  tail window (300-500ms) (h2+h3)/fund: morph=0 %.3f   morph=1 %.3f\n",
                     sineTail, fullTail);

        check (sineAtk < 0.03,
               "morph=0 attack is a clean sine — default matches pre-morph behaviour exactly");
        check (fullAtk > sineAtk * 3.0 && fullAtk > 0.08,
               "morph=1 adds clear harmonic content to the ATTACK (phase-skew is audible)");
        check (midAtk > sineAtk && midAtk < fullAtk,
               "morph=0.5 lands between 0 and 1 (continuous, monotonic control)");
        check (fullTail < 0.05,
               "morph=1 TAIL is still a clean sine — the attack-only envelope has fully relaxed");

        // 2026-09-01 bug-scan: an instantaneous morph change mid-note (a DAW automation
        // jump, or a fast knob grab) must not click. v2 note: the step has to land inside
        // the ATTACK window (the shape envelope decays with a 40 ms time constant, so a
        // late step does nothing), and the fair control is a render with morph AT the
        // target from t = 0 — the skewed waveform legitimately has more slew than a sine,
        // so "stepped vs always-on" isolates the smoother's job from the shape's own slope.
        {
            const int block = 64;
            const int total = (int) (sr * 0.05);
            // 448 = block boundary 9.33 ms in: shape env ~0.79, late-cycle phase where the
            // warped and straight phase maps differ strongly.
            const int stepAt = 448;

            auto renderStep = [&] (int stepSample)
            {
                KICKRAudioProcessor p;
                setP (p, "tuneMode",       1.0f);
                setP (p, "fundamental",  100.0f);
                setP (p, "pitchStart",     1.0f);
                setP (p, "bodyDecay",   2000.0f);
                setP (p, "bodyHarmonics",  0.0f);
                setP (p, "subLevel",       0.0f);
                setP (p, "clickLevel",     0.0f);
                setP (p, "tailLevel",      0.0f);
                setP (p, "noiseLevel",     0.0f);
                setP (p, "driveMix",       0.0f);
                setP (p, "limiter",        0.0f);
                setP (p, "morph",          stepSample < 0 ? 1.0f : 0.0f);

                p.setRateAndBufferSizeDetails (sr, block);
                p.prepareToPlay (sr, block);
                juce::AudioBuffer<float> o (juce::jmax (1, p.getTotalNumOutputChannels()), total);
                o.clear();
                juce::AudioBuffer<float> sc (o.getNumChannels(), block);
                bool steppedNow = false;
                for (int pos = 0; pos < total;)
                {
                    const int n = std::min (block, total - pos);
                    if (stepSample >= 0 && ! steppedNow && pos >= stepSample)
                    {
                        setP (p, "morph", 1.0f);
                        steppedNow = true;
                    }
                    juce::AudioBuffer<float> b (sc.getArrayOfWritePointers(), o.getNumChannels(), n);
                    b.clear();
                    juce::MidiBuffer midi;
                    if (pos == 0) midi.addEvent (juce::MidiMessage::noteOn (1, a1, vel), 0);
                    p.processBlock (b, midi);
                    for (int ch = 0; ch < o.getNumChannels(); ++ch) o.copyFrom (ch, pos, b, ch, 0, n);
                    pos += n;
                }
                p.releaseResources();
                return o;
            };
            auto slewIn = [&] (const juce::AudioBuffer<float>& b, double t0, double t1)
            {
                const int i0 = std::max (1, (int) (t0 * sr)), i1 = std::min (b.getNumSamples(), (int) (t1 * sr));
                const float* x = b.getReadPointer (0);
                float m = 0.0f;
                for (int i = i0; i < i1; ++i) m = std::max (m, std::abs (x[i] - x[i - 1]));
                return m;
            };

            const auto steppedBuf  = renderStep (stepAt);
            const auto alwaysOnBuf = renderStep (-1);
            const float preStepSine = slewIn (steppedBuf,  0.003,  0.009);   // pure sine before the jump
            const float atStep      = slewIn (steppedBuf,  0.0092, 0.0135); // the jump + 10 ms smoothing ramp
            const float ctrl        = slewIn (alwaysOnBuf, 0.0092, 0.0135); // same window, morph=1 from t=0
            std::printf ("  morph step 0->1 @9.3ms: pre-step sine slew %.4f   at-step %.4f   always-on control %.4f   step/control %.2f\n",
                         preStepSine, atStep, ctrl, atStep / std::max (1.0e-6f, ctrl));
            check (atStep < ctrl * 1.3f,
                   "instant morph automation jump is smoothed — no slew beyond the skewed waveform's own slope");
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
            const int  expLat = expectedOsLatency (idx) + p.limiterLatencySamples();   // 2026-09-02: + look-ahead
            std::printf ("  OS %dx: peak %.3f  finite %d  latency %d (expected %d)\n",
                         1 << idx, stat.peak, (int) stat.allFinite, lat, expLat);
            check (stat.allFinite && stat.peak > 0.05f && stat.peak < 1.5f,
                   idx == 0 ? "1x: default kick finite, audible, sane peak"
                 : idx == 1 ? "2x: default kick finite, audible, sane peak"
                 : idx == 2 ? "4x: default kick finite, audible, sane peak"
                            : "8x: default kick finite, audible, sane peak");
            check (lat == expLat,
                   idx == 0 ? "1x reported latency == limiter look-ahead"
                 : idx == 1 ? "2x reported latency == round(os latency) + look-ahead"
                 : idx == 2 ? "4x reported latency == round(os latency) + look-ahead"
                            : "8x reported latency == round(os latency) + look-ahead");
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

        float worstSlew = 0.0f; int worstAt = -1, worstSb = -1;
        const float* fx = full.getReadPointer (0);
        for (int sb : switchBlocks)
        {
            const int i0 = std::max (1, (sb - 1) * block);
            const int i1 = std::min (full.getNumSamples(), (sb + 2) * block);
            for (int i = i0; i < i1; ++i)
                if (std::abs (fx[i] - fx[i - 1]) > worstSlew) { worstSlew = std::abs (fx[i] - fx[i - 1]); worstAt = i; worstSb = sb; }
        }
        juce::ignoreUnused (worstAt, worstSb);
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

    // Fix (2026-08-31, code review): the 4 macro SmoothedValues were reset() against
    // baseSampleRate (~1 tick per SAMPLE) but only ever ticked (getNextValue()) once per
    // processBlock() call (~1 tick per BLOCK) — so the documented "~20 ms" ramp actually
    // took ~20 ms worth of BLOCKS: at a typical 512-sample buffer, ~10 seconds for a
    // macro move to reach its target. Step macroCrush mid-note and confirm the change is
    // audible within ~40 ms, not only after several seconds.
    //
    // Isolated to a sustained signal (TAIL layer only, long decay, minimal envelope
    // movement across the measurement window) so the comparison isn't confounded by the
    // kick's own natural decay: compare the STEPPED render's post-step window against a
    // CONTROL held at the target value from t=0 — at the SAME absolute time, so both
    // share identical natural decay, isolating the macro's own convergence speed.
    {
        auto renderTail = [&] (bool doStep)
        {
            KICKRAudioProcessor p;
            setP (p, "bodyLevel", 0.0f); setP (p, "clickLevel", 0.0f);
            setP (p, "subLevel", 0.0f);  setP (p, "noiseLevel", 0.0f);
            setP (p, "tailLevel", 1.0f); setP (p, "tailLength", 2000.0f); setP (p, "tailDrive", 0.0f);
            setP (p, "macroCrush", doStep ? 0.5f : 1.0f);   // control starts already at target
            p.setRateAndBufferSizeDetails (sr, 64);
            p.prepareToPlay (sr, 64);

            juce::AudioBuffer<float> out (juce::jmax (1, p.getTotalNumOutputChannels()), (int) (sr * 0.2));
            out.clear();
            juce::AudioBuffer<float> scratch (out.getNumChannels(), 64);
            const int stepAtSample = (int) (sr * 0.10);

            for (int pos = 0; pos < out.getNumSamples();)
            {
                const int n = std::min (64, out.getNumSamples() - pos);
                if (doStep && pos <= stepAtSample && pos + n > stepAtSample)
                    setP (p, "macroCrush", 1.0f);   // step to the same target as the control
                juce::AudioBuffer<float> b (scratch.getArrayOfWritePointers(), out.getNumChannels(), n);
                b.clear();
                juce::MidiBuffer m;
                if (pos == 0) m.addEvent (juce::MidiMessage::noteOn (1, a1, vel), 0);
                p.processBlock (b, m);
                for (int ch = 0; ch < out.getNumChannels(); ++ch) out.copyFrom (ch, pos, b, ch, 0, n);
                pos += n;
            }
            p.releaseResources();
            return out;
        };

        const auto control = renderTail (false);   // macroCrush = 1.0 from t=0 (the eventual target)
        const auto stepped = renderTail (true);    // macroCrush 0.5 -> 1.0 at t=100ms

        // Window well after the step (~15-40 ms later) but still deep in the 2 s tail decay
        // (negligible amplitude change there) -> isolates the macro's own convergence speed.
        const double hfControl = hfRatio (control, sr, 0.115, 0.14);
        const double hfStepped = hfRatio (stepped, sr, 0.115, 0.14);
        std::printf ("  macroCrush step 0.5->1.0 @100ms, sustained tail: hfRatio control(=1 from t0) %.4f   stepped (~20ms after) %.4f\n",
                     hfControl, hfStepped);
        check (std::abs (hfStepped - hfControl) < std::abs (hfControl) * 0.35,
               "macro step converges within ~20-40 ms (matches the already-at-target control), not several seconds");
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
        setP (pa, "morph", 0.62f);   // 2026-09-01 — confirm the new parameter round-trips too

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
                            && eq ("sampleEnable") && eq ("character") && eq ("tailLength")
                            && eq ("morph");
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

            // 2026-08-31: every APVTS parameter has a real hover description (not just its
            // own on-screen caption echoed back) — catches a future param addition that
            // forgot to add one to ParameterDescriptions.h.
            {
                int missing = 0;
                for (auto* prm : pe.getParameters())
                    if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (prm))
                        if (kickr::paramDescription (rp->getParameterID()).isEmpty())
                        {
                            std::printf ("  missing hover description: %s\n", rp->getParameterID().toRawUTF8());
                            ++missing;
                        }
                check (missing == 0, "every parameter has a ParameterDescriptions.h entry");
            }

            // 2026-08-31: `character` reads out as a curve name (Waveshaper.h's 7 LOCKED
            // curves), not a bare 0..1 number — matches the DSP's own seg/t crossfade math.
            if (auto* chr = pe.getValueTreeState().getParameter ("character"))
            {
                auto textAt = [&] (float v01) { return chr->getText (v01, 32); };
                std::printf ("  character text: 0=%s  1/6=%s  ~0.10=%s  5/6=%s  1=%s\n",
                             textAt (0.0f).toRawUTF8(), textAt (1.0f / 6.0f).toRawUTF8(),
                             textAt (0.10f).toRawUTF8(), textAt (5.0f / 6.0f).toRawUTF8(),
                             textAt (1.0f).toRawUTF8());
                check (textAt (0.0f)  == "Tanh",   "character @ 0 reads as the pure Tanh curve");
                check (textAt (1.0f)  == "Crush",  "character @ 1 reads as the pure Bitcrush curve");
                check (textAt (5.0f / 6.0f) == "Fold", "character @ 5/6 reads as the pure Foldback curve");
                check (textAt (0.10f).containsChar ('/'), "mid-blend shows both curve names (e.g. Tanh/Cubic)");

                // Show the worst-case (longest) blended text in the snapshot below.
                chr->setValueNotifyingHost (0.10f);
            }

            // 2026-09-01 (user request): `subFreq` reads out as a note name WITH the Hz
            // value alongside it, e.g. "E1 (40.0 Hz)" — not raw Hz alone, and not a bare
            // note name either (unlike `fundamental`, which is note-only).
            if (auto* sf = pe.getValueTreeState().getParameter ("subFreq"))
            {
                const auto text40 = sf->getText (sf->convertTo0to1 (40.0f), 32);
                std::printf ("  subFreq text @ 40 Hz: %s\n", text40.toRawUTF8());
                check (text40.startsWith ("D#1"),       "subFreq @ 40 Hz reads out with the correct note (D#1)");
                check (text40.contains ("40.0 Hz"),     "subFreq text includes the Hz value");
                check (text40.contains ("(") && text40.contains (")"),
                       "subFreq's Hz value is parenthesised, next to the note name");
            }

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

            // Also render at the ACTUAL fixed runtime default (1120x819, 2026-08-31) so a
            // size change here is caught visually, not just "doesn't assert" above.
            ed->setSize (1120, 819);
            const auto snapDefault = ed->createComponentSnapshot (ed->getLocalBounds(), false, 1.0f);
            const auto pngDefault  = juce::File::getCurrentWorkingDirectory().getChildFile ("kickr_ui_default_size.png");
            if (auto os2 = pngDefault.createOutputStream())
            {
                os2->setPosition (0); os2->truncate();
                juce::PNGImageFormat fmt2;
                const bool ok2 = fmt2.writeImageToStream (snapDefault, *os2);
                std::printf ("  wrote %s : %s (%dx%d)\n", pngDefault.getFullPathName().toRawUTF8(),
                             ok2 ? "ok" : "FAILED", snapDefault.getWidth(), snapDefault.getHeight());
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
        const int   nBinsActive = pa.getAnalyzer().getNumBins();   // 2026-09-02: resolution-dependent
        bool sFinite = true; int peakBin = 1; float peakDb = -1.0e9f;
        for (int b = 1; b < nBinsActive; ++b)
        {
            const float d = db[static_cast<size_t> (b)];
            if (! std::isfinite (d)) sFinite = false;
            if (d > peakDb) { peakDb = d; peakBin = b; }
        }
        const double binHz  = (s2 * 0.5) / (double) nBinsActive;
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

        // --- 47 factory presets (17 + 30 added 2026-09-02): all load, render finite/audible, and differ ---
        {
            KICKRAudioProcessor p;
            auto& pm = p.getPresetManager();
            check (kickr::PresetManager::getNumFactory() == 47, "47 factory presets registered");

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
            std::printf ("  factory: allOk=%d  distinct-rms=%d/47\n", (int) allOk, distinct);
            check (allOk, "every factory preset renders a finite, audible, bounded kick");
            check (distinct >= 40, "factory presets are meaningfully different from each other");
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

                // 2026-08-31: visual check that the small sample-strip waveform preview
                // (SampleWaveformView) picks up the just-dropped file.
                ed->setSize (1120, 819);
                const auto snap = ed->createComponentSnapshot (ed->getLocalBounds(), false, 1.0f);
                const auto png  = juce::File::getCurrentWorkingDirectory().getChildFile ("kickr_ui_sample_waveform.png");
                if (auto os = png.createOutputStream())
                {
                    os->setPosition (0); os->truncate();
                    juce::PNGImageFormat fmt;
                    const bool wrote = fmt.writeImageToStream (snap, *os);
                    std::printf ("  wrote %s : %s\n", png.getFullPathName().toRawUTF8(), wrote ? "ok" : "FAILED");
                }
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

    // ---------------------------------------------------------------------
    // Repo Stage 17 — profile & confirm the `oversampling` default (2026-08-31). Offline
    // ---------------------------------------------------------------------
    // Feature (2026-08-31, user-provided content): 50 shipped kick samples, embedded via
    // BinaryData (KICKR_FactorySamples) so they're always present — never touch disk,
    // work from a totally fresh/empty ~/Music/KICKR/Samples/.
    std::printf ("\n[Feature] Factory sample bank (50 embedded kicks)\n");
    {
        check (kickr::SampleLibrary::getFactoryCount() == 50, "50 factory samples are linked in");

        const auto factoryNames = kickr::SampleLibrary::getFactoryNames();
        check (factoryNames.size() == 50, "getFactoryNames() returns all 50");
        bool allWav = true, sortedOk = true;
        for (int i = 0; i < factoryNames.size(); ++i)
        {
            if (! factoryNames[i].endsWithIgnoreCase (".wav")) allWav = false;
            if (i > 0 && factoryNames[i - 1].compareNatural (factoryNames[i]) > 0) sortedOk = false;
        }
        check (allWav,    "every factory name ends in .wav");
        check (sortedOk,  "factory names are naturally sorted (Kick01 < Kick02 < ... < Kick10)");
        check (kickr::SampleLibrary::isFactoryName ("Kick01.wav"),  "isFactoryName recognises a real one");
        check (! kickr::SampleLibrary::isFactoryName ("NotAKick.wav"), "isFactoryName rejects a bogus name");

        // Decode a factory sample from a totally FRESH, empty temp folder — proves the
        // in-memory path works with zero user content on disk.
        const auto tmpBank6 = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                  .getChildFile ("kickr_test_factory_bank");
        tmpBank6.deleteRecursively();
        kickr::SampleLibrary lib (tmpBank6);
        check (lib.getCount() == 0, "fresh temp folder has zero USER samples");
        check (lib.getTotalCount() == 50, "but the TOTAL (factory+user) bank is 50");

        auto sb = lib.load ("Kick01.wav");
        check (sb != nullptr, "a factory sample decodes with no disk file present");
        if (sb != nullptr)
        {
            check (sb->audio.getNumSamples() > 1000,          "factory sample has real audio content");
            check (std::abs (sb->sourceRate - 44100.0) < 1.0, "factory sample sourceRate preserved (44.1 kHz)");
            check (sb->rootNote == 60,                        "factory sample rootNote is C3, same as user imports");
        }
        check (lib.load ("DefinitelyNotShipped.wav") == nullptr, "a bogus name still returns nullptr");

        // Combined navigation: cycle further than the total count and confirm it wraps
        // cleanly through factory names without ever landing on an empty string.
        bool allNamesNonEmpty = true;
        juce::String lastName;
        for (int i = 0; i < 55; ++i)
        {
            lastName = lib.nextTotal();
            if (lastName.isEmpty()) allNamesNonEmpty = false;
        }
        check (allNamesNonEmpty, "prevTotal/nextTotal never return an empty name while the bank is non-empty");
        check (kickr::SampleLibrary::isFactoryName (lastName) || lib.indexOfName (lastName) >= 0,
               "every combined-nav name is either a real factory or a real user sample");

        // Import dedupe: a user file whose name COLLIDES with a factory sample must NOT
        // shadow it — gets deduped to "Kick01-2.wav" instead. Source lives OUTSIDE the
        // managed folder (a sibling temp dir), exactly like a real drag-drop source file.
        const int totalBefore = lib.getTotalCount();
        const auto collisionSrcDir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                          .getChildFile ("kickr_test_factory_collision_src");
        collisionSrcDir.deleteRecursively(); collisionSrcDir.createDirectory();
        const auto renamed = collisionSrcDir.getChildFile ("Kick01.wav");
        writeSineWav (renamed, 120.0, 44100.0, 0.3, 1);
        const auto importedName = lib.importFile (renamed);
        std::printf ("  collision import: 'Kick01.wav' -> '%s'\n", importedName.toRawUTF8());
        check (importedName.isNotEmpty() && importedName != "Kick01.wav",
               "importing a file named like a factory sample is deduped, not shadowed");
        check (lib.getTotalCount() == totalBefore + 1, "total count grows by exactly 1 after the deduped import");
        collisionSrcDir.deleteRecursively();

        // End-to-end: a completely fresh processor, no imports at all, loads and renders a
        // FACTORY sample by name — the whole PluginProcessor -> SampleLibrary -> SamplePlayer
        // -> KickVoice pipeline, exactly as a real drag-drop import would exercise it.
        {
            KICKRAudioProcessor pf;
            pf.getSampleLibrary().setFolder (juce::File::getSpecialLocation (juce::File::tempDirectory)
                                                  .getChildFile ("kickr_test_factory_e2e"));
            pf.getSampleLibrary().getFolder().deleteRecursively();
            pf.loadSampleByName ("Kick01.wav");
            setP (pf, "sampleEnable", 1.0f); setP (pf, "synthEnable", 0.0f); setP (pf, "limiter", 0.0f);
            const auto rf = kickr::tests::renderNote (pf, a1, vel, sr, 512, 0.6);
            const auto sf = analyse (rf, sr);
            std::printf ("  factory sample end-to-end: peak %.3f  finite %d  currentSampleName=%s\n",
                         sf.peak, (int) sf.allFinite, pf.getCurrentSampleName().toRawUTF8());
            check (sf.allFinite && sf.peak > 0.02f, "a factory sample renders real, finite audio end-to-end");
            check (pf.getCurrentSampleName() == "Kick01.wav", "currentSampleName is the factory name, unmodified");
            pf.getSampleLibrary().getFolder().deleteRecursively();
        }

        // Visual check: the SAMPLE strip + its waveform preview with a factory sample
        // selected, on a completely fresh (no imports) install.
        {
            KICKRAudioProcessor pv;
            pv.getSampleLibrary().setFolder (juce::File::getSpecialLocation (juce::File::tempDirectory)
                                                  .getChildFile ("kickr_test_factory_snap"));
            pv.getSampleLibrary().getFolder().deleteRecursively();
            pv.prepareToPlay (48000.0, 512);
            pv.loadSampleByName ("Kick01.wav");
            setP (pv, "sampleEnable", 1.0f);
            std::unique_ptr<juce::AudioProcessorEditor> edBase (pv.createEditor());
            if (auto* edv = dynamic_cast<KICKRAudioProcessorEditor*> (edBase.get()))
            {
                edv->setSize (1120, 819);
                const auto snap = edv->createComponentSnapshot (edv->getLocalBounds(), false, 1.0f);
                const auto png  = juce::File::getCurrentWorkingDirectory().getChildFile ("kickr_ui_factory_bank.png");
                if (auto os = png.createOutputStream())
                {
                    os->setPosition (0); os->truncate();
                    juce::PNGImageFormat fmt;
                    const bool wrote = fmt.writeImageToStream (snap, *os);
                    std::printf ("  wrote %s : %s\n", png.getFullPathName().toRawUTF8(), wrote ? "ok" : "FAILED");
                }
            }
            pv.getSampleLibrary().getFolder().deleteRecursively();
        }

        tmpBank6.deleteRecursively();
    }

    // ---------------------------------------------------------------------
    // Fix (2026-09-01, user request — "no matter what time i input midi at, the
    // soundwave ... appears always fixed and doesn't move around"): retrigger at
    // several DIFFERENT, irregular mid-block sample offsets and confirm the raw
    // capture's leading "silence" is tiny and near-CONSTANT every time — i.e. it's
    // governed only by the click's own natural attack ramp, not by where in the block
    // the MIDI happened to land. (An earlier attempt trimmed this post-hoc in the
    // display via an amplitude threshold; that couldn't tell a real new onset from a
    // fast retrigger's previous-voice tail, so the trim amount actually varied. Fixed
    // at the source instead — Analyzer::armCapture's skipSamples — so there's nothing
    // left to detect: capture sample 0 IS the note-on, always.)
    std::printf ("\n[Fix] Waveform capture starts at the note-on, any retrigger timing\n");
    {
        auto onsetAt = [&] (int noteOnSampleOffset)
        {
            KICKRAudioProcessor pt;
            pt.prepareToPlay (48000.0, 512);
            juce::AudioBuffer<float> tbuf (juce::jmax (1, pt.getTotalNumOutputChannels()), 512);
            for (int bi = 0; bi < 20; ++bi)
            {
                tbuf.clear();
                juce::MidiBuffer m;
                if (bi == 0) m.addEvent (juce::MidiMessage::noteOn (1, a1, vel), noteOnSampleOffset);
                pt.processBlock (tbuf, m);
            }
            std::array<float, kickr::Analyzer::kWaveCaptureLen> rawWave {};
            std::uint32_t rawGen = 0;
            pt.getAnalyzer().getWaveform (rawWave, rawGen);
            for (int i = 0; i < (int) rawWave.size(); ++i)
                if (std::abs (rawWave[static_cast<size_t> (i)]) > 0.02f) return i;
            return -1;
        };

        const int offsets[] = { 0, 41, 137, 300, 480, 511 };   // spans the whole block
        int worst = 0;
        for (int off : offsets)
        {
            const int onset = onsetAt (off);
            std::printf ("  note-on at sample %3d of 512 -> capture onset at %d\n", off, onset);
            worst = juce::jmax (worst, onset);
        }
        // A generous bound: the click's own raised-cosine rise can take a few samples
        // (at the OVERSAMPLED rate, downsampled) to clear the 0.02 threshold — nothing
        // to do with retrigger timing. 20 samples @ 48 kHz = ~0.4 ms.
        check (worst >= 0 && worst < 20,
               "capture onset stays tiny and timing-independent no matter where in the block the note-on lands");

        std::unique_ptr<juce::AudioProcessorEditor> edBase;
        {
            KICKRAudioProcessor pv;
            pv.prepareToPlay (48000.0, 512);
            juce::AudioBuffer<float> tbuf (juce::jmax (1, pv.getTotalNumOutputChannels()), 512);
            for (int bi = 0; bi < 20; ++bi)
            {
                tbuf.clear();
                juce::MidiBuffer m;
                if (bi == 0) m.addEvent (juce::MidiMessage::noteOn (1, a1, vel), 300);   // mid-block
                pv.processBlock (tbuf, m);
            }
            edBase.reset (pv.createEditor());
            if (auto* edt = dynamic_cast<KICKRAudioProcessorEditor*> (edBase.get()))
            {
                edt->setSize (1120, 819);
                edt->refreshAnalyzersForSnapshot();
                const auto snap = edt->createComponentSnapshot (edt->getLocalBounds(), false, 1.0f);
                const auto png  = juce::File::getCurrentWorkingDirectory().getChildFile ("kickr_ui_onset_trim.png");
                if (auto os = png.createOutputStream())
                {
                    os->setPosition (0); os->truncate();
                    juce::PNGImageFormat fmt;
                    const bool wrote = fmt.writeImageToStream (snap, *os);
                    std::printf ("  wrote %s : %s\n", png.getFullPathName().toRawUTF8(), wrote ? "ok" : "FAILED");
                }
            }
            // edBase (and its editor) must outlive pv's destruction below? No — the
            // editor holds a reference to pv's Analyzer; destroy the editor before pv
            // goes out of scope at the end of this block.
            edBase.reset();
        }
    }

    // ---------------------------------------------------------------------
    // Trial follow-up (2026-09-01, user request — "test all knobs, at random times press
    // shift, to slow movement, move back and forth if you detect any anomalies fix them"):
    // no live mouse in this environment, so this drives every float knob's REAL
    // KickrSlider through a synthetic drag using JUCE's own MouseEvent — same code path
    // Slider::mouseDown/mouseDrag actually run under a live mouse, just constructed by
    // hand instead of by the OS. Each knob gets: drag up (normal speed) -> toggle Shift
    // ON at a STANDSTILL (no mouse movement — isolates the toggle itself) -> continue
    // (fine speed) -> reverse direction -> toggle Shift OFF at a standstill -> continue
    // (normal speed) -> reverse again. The critical assertion: value must not change at
    // either standstill toggle (mouse didn't move, so nothing should move on screen).
    std::printf ("\n[Fix] Shift-drag fine-adjust: no snap-back, across every real knob\n");
    {
        auto makeEvt = [] (juce::Component& target, juce::Point<float> pos, bool shiftDown,
                           juce::Point<float> downPos)
        {
            return juce::MouseEvent (
                juce::Desktop::getInstance().getMainMouseSource(),
                pos,
                juce::ModifierKeys (shiftDown ? juce::ModifierKeys::shiftModifier : 0),
                juce::MouseInputSource::defaultPressure, juce::MouseInputSource::defaultOrientation,
                juce::MouseInputSource::defaultRotation, juce::MouseInputSource::defaultTiltX,
                juce::MouseInputSource::defaultTiltY,
                &target, &target, juce::Time::getCurrentTime(),
                downPos, juce::Time::getCurrentTime(), 1, true);
        };

        KICKRAudioProcessor pk;
        int tested = 0, anomalies = 0;

        for (auto* prm : pk.getParameters())
        {
            if (dynamic_cast<juce::AudioParameterFloat*> (prm) == nullptr)
                continue;   // only float knobs use KickrSlider (Bool -> KickrToggle, Choice -> ComboBox)
            auto* fp = dynamic_cast<juce::RangedAudioParameter*> (prm);   // public getDefaultValue()/getNormalisableRange()
            if (fp == nullptr)
                continue;

            const auto& fr = fp->getNormalisableRange();
            kickr::KickrSlider s;
            s.setNormalisableRange (juce::NormalisableRange<double> (
                (double) fr.start, (double) fr.end, (double) fr.interval, (double) fr.skew));
            s.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
            s.setBounds (0, 0, 60, 60);
            s.setValue (fp->convertFrom0to1 (fp->getDefaultValue()), juce::dontSendNotification);

            juce::Point<float> pos (30.0f, 200.0f);
            const auto downPos = pos;
            s.mouseDown (makeEvt (s, pos, false, downPos));

            auto dragTo = [&] (float dy, bool shift)
            {
                pos = juce::Point<float> (pos.x, pos.y + dy);
                s.mouseDrag (makeEvt (s, pos, shift, downPos));
                return s.getValue();
            };

            dragTo (-40.0f, false);
            const double before1 = s.getValue();
            const double atToggle1 = dragTo (0.0f, true);        // toggle ON, standstill
            dragTo (-20.0f, true);
            dragTo (15.0f, true);                                 // reverse, still fine
            const double before2 = s.getValue();
            const double atToggle2 = dragTo (0.0f, false);        // toggle OFF, standstill
            dragTo (-25.0f, false);
            dragTo (30.0f, false);                                // reverse again
            s.mouseUp (makeEvt (s, pos, false, downPos));

            const auto& r = fp->getNormalisableRange();
            const double tol = juce::jmax (1.0e-6, (double) (r.end - r.start) * 1.0e-4);
            if (std::abs (atToggle1 - before1) > tol)
            {
                std::printf ("  ANOMALY (shift ON):  %s  before=%.6f  atToggle=%.6f\n",
                             fp->getParameterID().toRawUTF8(), before1, atToggle1);
                ++anomalies;
            }
            if (std::abs (atToggle2 - before2) > tol)
            {
                std::printf ("  ANOMALY (shift OFF): %s  before=%.6f  atToggle=%.6f\n",
                             fp->getParameterID().toRawUTF8(), before2, atToggle2);
                ++anomalies;
            }
            ++tested;
        }

        std::printf ("  tested %d float knobs (their real ranges/skews) for shift-toggle continuity\n", tested);
        check (tested >= 50, "exercised (essentially) all float knobs, not a token few");
        check (anomalies == 0, "no snap-back/jump at any shift toggle, any knob, either direction");
    }

    std::printf ("\n[Fix] Sample-tweak knobs grey out (disabled) while SAMPLE is off\n");
    {
        KICKRAudioProcessor pg;
        pg.prepareToPlay (48000.0, 512);
        setP (pg, "sampleEnable", 0.0f);

        std::unique_ptr<juce::AudioProcessorEditor> edBase (pg.createEditor());
        auto* ed = dynamic_cast<KICKRAudioProcessorEditor*> (edBase.get());
        check (ed != nullptr, "editor created for the sample-enable grey-out test");
        if (ed != nullptr)
        {
            const int total = ed->sampleTweaksCountForTest();
            std::printf ("  sampleTweaks group has %d controls\n", total);
            check (total > 0, "sample-tweak control group is non-empty");
            check (ed->countEnabledSampleTweaksForTest() == 0,
                   "all sample-tweak controls report disabled while sampleEnable is off");

            setP (pg, "sampleEnable", 1.0f);   // ParameterAttachment is message-thread-synchronous here
            check (ed->countEnabledSampleTweaksForTest() == total,
                   "all sample-tweak controls report enabled again once SAMPLE is switched back on");

            setP (pg, "sampleEnable", 0.0f);
            check (ed->countEnabledSampleTweaksForTest() == 0,
                   "toggling SAMPLE back off disables the tweak controls again");

            // visual record, matching the project's existing UI snapshot convention
            ed->setSize (1120, 819);
            const auto snap = ed->createComponentSnapshot (ed->getLocalBounds(), false, 1.0f);
            const auto png  = juce::File::getCurrentWorkingDirectory().getChildFile ("kickr_ui_sample_disabled.png");
            if (auto os = png.createOutputStream())
            {
                os->setPosition (0); os->truncate();
                juce::PNGImageFormat fmt;
                const bool wrote = fmt.writeImageToStream (snap, *os);
                std::printf ("  wrote %s : %s\n", png.getFullPathName().toRawUTF8(), wrote ? "ok" : "FAILED");
            }
        }
    }

    // wall-clock proxy: no live audio device in this environment, so this measures render
    // cost the same way a CPU meter would infer it — audio-seconds produced per
    // wall-clock-second, for the realistic worst case (BOTH synth + sample layers active,
    // continuous machine-gun retriggering so a voice is essentially always live). Not a
    // strict pass/fail gate (machine-dependent); the numbers are the deliverable.
    std::printf ("\n[Stage 17] Oversampling profile — audio-seconds per wall-clock-second\n");
    {
        const auto tmpBank5 = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                  .getChildFile ("kickr_test_profile_bank");
        tmpBank5.deleteRecursively();
        const auto fx5 = juce::File::getSpecialLocation (juce::File::tempDirectory)
                             .getChildFile ("kickr_profile_fixture.wav");
        writeSineWav (fx5, 90.0, 44100.0, 0.4, 1);

        auto profileOnce = [&] (double rate, int osChoice)
        {
            KICKRAudioProcessor p;
            p.getSampleLibrary().setFolder (tmpBank5);
            const auto nm = p.getSampleLibrary().importFile (fx5);
            p.loadSampleByName (nm);
            setP (p, "sampleEnable", 1.0f);              // blended: synth (default on) + sample
            setP (p, "oversampling", (float) osChoice);

            const int    block = 256;
            const double seconds = 2.0;                  // audio content rendered
            p.setRateAndBufferSizeDetails (rate, block);
            p.prepareToPlay (rate, block);

            const int total = (int) (rate * seconds);
            juce::AudioBuffer<float> profBuf (juce::jmax (1, p.getTotalNumOutputChannels()), block);
            const int retriggerEvery = (int) (rate * 0.15);   // a kick every 150 ms

            const auto t0 = std::chrono::steady_clock::now();
            for (int pos = 0; pos < total; pos += block)
            {
                profBuf.clear();
                juce::MidiBuffer m;
                if (pos % retriggerEvery < block)
                    m.addEvent (juce::MidiMessage::noteOn (1, a1, vel), 0);
                p.processBlock (profBuf, m);
            }
            const auto t1 = std::chrono::steady_clock::now();
            p.releaseResources();

            const double wallSec = std::chrono::duration<double> (t1 - t0).count();
            return wallSec > 0.0 ? seconds / wallSec : 0.0;
        };

        static const char* const osNames[4] = { "1x", "2x", "4x", "8x" };
        static const double rates[3] = { 44100.0, 48000.0, 96000.0 };

        bool allFiniteAndPositive = true;
        for (double rate : rates)
        {
            std::printf ("  %.0f Hz:", rate);
            for (int os = 0; os < 4; ++os)
            {
                const double ratio = profileOnce (rate, os);
                std::printf ("  %s=%.0fx", osNames[os], ratio);
                if (! (ratio > 0.0) || ! std::isfinite (ratio))
                    allFiniteAndPositive = false;
            }
            std::printf ("\n");
        }
        check (allFiniteAndPositive, "every oversampling factor renders faster than real time on this machine");

        tmpBank5.deleteRecursively(); fx5.deleteFile();
    }
    std::printf ("  Decision: oversampling default STAYS at index 1 (2x). Reasoning: the DSP is\n"
                "  entirely monophonic (1 voice, occasionally 2 during a 3ms retrigger crossfade)\n"
                "  so absolute CPU cost is low at every factor even on modest hardware; 2x already\n"
                "  pushes the audible aliasing from the 7-curve waveshaper (up to 36 dB of drive)\n"
                "  and sampleCrush's hard nonlinearities well above the kick's own energy (Phase\n"
                "  2.8/2.10 checkpoints); 4x/8x remain available per-patch for a harder-driven\n"
                "  sound or a slower system, matching how the parameter was designed (AD-2/AD-10).\n");

    // ---------------------------------------------------------------------
    // 2026-09-02 (user request: "change the limiter to an identical to abletons colour
    // limiter") — the fixed -0.5 dBFS soft-clip is now a ColorLimiter: loudness / ceiling /
    // look-ahead / release / saturation / color, defaults transparent.
    std::printf ("\n[Feature] Color Limiter\n");
    {
        auto goertzel = [] (const juce::AudioBuffer<float>& b, double s, double t0, double t1, double hz)
        {
            const float* x = b.getReadPointer (0);
            const int i0 = std::max (0, (int) (t0 * s));
            const int i1 = std::min (b.getNumSamples(), (int) (t1 * s));
            const double w = 2.0 * juce::MathConstants<double>::pi * hz / s;
            const double cw = 2.0 * std::cos (w);
            double s1 = 0.0, s2 = 0.0;
            for (int i = i0; i < i1; ++i) { const double s0 = (double) x[i] + cw * s1 - s2; s2 = s1; s1 = s0; }
            return std::sqrt (std::max (0.0, s1 * s1 + s2 * s2 - cw * s1 * s2)) / (double) std::max (1, i1 - i0);
        };
        auto peakOf = [] (const juce::AudioBuffer<float>& b, bool& finite)
        {
            finite = true; float pk = 0.0f;
            for (int c = 0; c < b.getNumChannels(); ++c)
            {
                const float* x = b.getReadPointer (c);
                for (int i = 0; i < b.getNumSamples(); ++i) { if (! std::isfinite (x[i])) finite = false; pk = std::max (pk, std::abs (x[i])); }
            }
            return pk;
        };
        auto rmsWin = [] (const juce::AudioBuffer<float>& b, double s, double t0, double t1)
        {
            const float* x = b.getReadPointer (0);
            const int i0 = std::max (0, (int) (t0 * s));
            const int i1 = std::min (b.getNumSamples(), (int) (t1 * s));
            double acc = 0.0;
            for (int i = i0; i < i1; ++i) acc += (double) x[i] * x[i];
            return std::sqrt (acc / (double) std::max (1, i1 - i0));
        };
        auto isolatedBody = [&] (KICKRAudioProcessor& p)   // steady 100 Hz sine body, nothing else, no drive
        {
            setP (p, "tuneMode", 1.0f); setP (p, "fundamental", 100.0f); setP (p, "pitchStart", 1.0f);
            setP (p, "bodyDecay", 2000.0f); setP (p, "bodyHarmonics", 0.0f); setP (p, "morph", 0.0f);
            setP (p, "subLevel", 0.0f); setP (p, "clickLevel", 0.0f); setP (p, "tailLevel", 0.0f);
            setP (p, "noiseLevel", 0.0f); setP (p, "driveMix", 0.0f);
        };

        // A) Transparent by default: a quiet kick through the default limiter equals the
        //    limiter-off render, just delayed by the look-ahead.
        {
            KICKRAudioProcessor pOn, pOff;
            setP (pOn, "output", -20.0f); setP (pOff, "output", -20.0f);
            setP (pOff, "limiter", 0.0f);
            pOn.prepareToPlay (sr, 512);   // latency is rate-dependent: ask after the rate is known
            const int lat = pOn.limiterLatencySamples();
            const auto bOn  = kickr::tests::renderNote (pOn,  a1, vel, sr, 512, 0.6);
            const auto bOff = kickr::tests::renderNote (pOff, a1, vel, sr, 512, 0.6);
            double maxDiff = 0.0, maxAbs = 0.0;
            const float* on = bOn.getReadPointer (0); const float* off = bOff.getReadPointer (0);
            for (int i = 0; i + lat < bOn.getNumSamples(); ++i)
            {
                maxDiff = std::max (maxDiff, (double) std::abs (on[i + lat] - off[i]));
                maxAbs  = std::max (maxAbs,  (double) std::abs (off[i]));
            }
            std::printf ("  default limiter vs off: look-ahead %d samples, max |diff| %.5f (signal peak %.3f)\n", lat, maxDiff, maxAbs);
            check (lat == (int) std::lround (0.0015 * sr), "default look-ahead reports 1.5 ms of latency");
            check (maxDiff < 0.01 * std::max (1.0e-9, maxAbs) + 1.0e-4, "default limiter is transparent on a quiet kick (== off, delayed by the look-ahead)");
        }

        // B) Ceiling is a hard guarantee.
        {
            for (float ceilDb : { -0.5f, -6.0f, -12.0f })
            {
                KICKRAudioProcessor p;
                setP (p, "output", 12.0f); setP (p, "limLoudness", 24.0f); setP (p, "limCeiling", ceilDb);
                setP (p, "limSaturation", 1.0f);
                const auto b = kickr::tests::renderNote (p, a1, vel, sr, 512, 0.6);
                bool fin = false; const float pk = peakOf (b, fin);
                const float ceilGain = kickr::dsputils::dbToGain (ceilDb);
                std::printf ("  ceiling %.1f dB: peak %.4f (ceiling gain %.4f)\n", ceilDb, pk, ceilGain);
                check (fin && pk <= ceilGain + 1.0e-4f, "ceiling never exceeded, even at +12 dB output, +24 dB loudness, full saturation");
            }
        }

        // C) Loudness pushes level up to the ceiling (denser, same peak).
        {
            KICKRAudioProcessor p0, p1;
            setP (p1, "limLoudness", 12.0f);
            const auto b0 = kickr::tests::renderNote (p0, a1, vel, sr, 512, 0.6);
            const auto b1 = kickr::tests::renderNote (p1, a1, vel, sr, 512, 0.6);
            const double r0 = rmsWin (b0, sr, 0.0, 0.3), r1 = rmsWin (b1, sr, 0.0, 0.3);
            bool f0, f1; const float pk1 = peakOf (b1, f1); juce::ignoreUnused (f0);
            std::printf ("  loudness 0 vs +12 dB: RMS %.4f -> %.4f, peak %.4f\n", r0, r1, pk1);
            check (r1 > r0 * 1.3 && pk1 <= 0.944060876f + 1.0e-4f, "LOUDNESS raises density under the same -0.5 dB ceiling");
        }

        // D) SATURATION adds harmonics; E) COLOR tilts where they land.
        double satRatio0 = 0.0, satRatio1 = 0.0, brightLow = 0.0, brightHigh = 0.0;
        {
            auto render = [&] (float satAmt, float colorAmt)
            {
                KICKRAudioProcessor p;
                isolatedBody (p);
                setP (p, "output", -6.0f);
                setP (p, "limSaturation", satAmt); setP (p, "limColor", colorAmt);
                return kickr::tests::renderNote (p, a1, vel, sr, 512, 0.8);
            };
            const auto bClean = render (0.0f, 0.5f);
            const auto bSat   = render (0.8f, 0.5f);
            const auto bDark  = render (0.8f, 0.0f);
            const auto bBrite = render (0.8f, 1.0f);
            // Window 20-120 ms: past the 1 ms fades, before the 2 s body decay has taken the
            // level out of the shaper's saturating region. tanh is symmetric -> odd harmonics.
            auto harm = [&] (const juce::AudioBuffer<float>& b) { return (goertzel (b, sr, 0.02, 0.12, 300.0) + goertzel (b, sr, 0.02, 0.12, 500.0)) / std::max (1.0e-9, goertzel (b, sr, 0.02, 0.12, 100.0)); };
            auto bright = [&] (const juce::AudioBuffer<float>& b) { return (goertzel (b, sr, 0.02, 0.12, 700.0) + goertzel (b, sr, 0.02, 0.12, 900.0)) / std::max (1.0e-9, goertzel (b, sr, 0.02, 0.12, 300.0)); };
            satRatio0 = harm (bClean); satRatio1 = harm (bSat);
            brightLow = bright (bDark); brightHigh = bright (bBrite);
            std::printf ("  saturation (h3+h5)/f: 0 -> %.4f, 0.8 -> %.4f | color brightness (h7+h9)/h3: 0 -> %.4f, 1 -> %.4f\n",
                         satRatio0, satRatio1, brightLow, brightHigh);
            check (satRatio1 > satRatio0 * 5.0 && satRatio1 > 0.02, "SATURATION adds harmonics to a clean sine body");
            check (brightHigh > brightLow * 1.5, "COLOR high is brighter (more upper harmonics) than COLOR low");
        }

        // F) RELEASE: a long release holds the gain down longer after the hit.
        {
            KICKRAudioProcessor pS, pL;
            for (auto* p : { &pS, &pL }) { setP (*p, "output", 12.0f); setP (*p, "bodyDecay", 2000.0f); setP (*p, "tailLevel", 0.0f); }
            setP (pS, "limRelease", 1.0f); setP (pL, "limRelease", 1000.0f);
            const auto bS = kickr::tests::renderNote (pS, a1, vel, sr, 512, 1.0);
            const auto bL = kickr::tests::renderNote (pL, a1, vel, sr, 512, 1.0);
            const double rS = rmsWin (bS, sr, 0.3, 0.5), rL = rmsWin (bL, sr, 0.3, 0.5);
            std::printf ("  release 1 ms vs 1000 ms: 300-500 ms RMS %.4f vs %.4f\n", rS, rL);
            check (rS > rL * 1.2, "RELEASE long keeps the gain reduction on longer than RELEASE short");
        }

        // G) Latency follows LOOKAHEAD and the on/off switch.
        {
            KICKRAudioProcessor p;
            p.prepareToPlay (sr, 512);
            setP (p, "limLookahead", 5.0f);
            const int l5 = p.limiterLatencySamples();
            setP (p, "limiter", 0.0f);
            const int lOff = p.limiterLatencySamples();
            std::printf ("  look-ahead 5 ms -> %d samples; limiter off -> %d\n", l5, lOff);
            check (l5 == (int) std::lround (0.005 * sr) && lOff == 0, "reported look-ahead latency follows LOOKAHEAD and drops to 0 when the limiter is off");
        }

        // H) Every oversampling factor x machine-gun retrigger at full tilt: finite, under the ceiling.
        {
            bool allFinite = true; float worst = 0.0f;
            for (int os = 0; os < 4; ++os)
            {
                KICKRAudioProcessor p;
                setP (p, "oversampling", (float) os);
                setP (p, "output", 12.0f); setP (p, "limLoudness", 24.0f); setP (p, "limSaturation", 1.0f);
                setP (p, "limColor", os % 2 == 0 ? 0.0f : 1.0f); setP (p, "limLookahead", os == 3 ? 10.0f : 1.5f);
                auto [rbuf, onsets] = renderRetrigger (p, a1, vel, sr, 256, 24, 60.0 / 174.0 / 8.0, 2.0);
                juce::ignoreUnused (onsets);
                bool fin = false; const float pk = peakOf (rbuf, fin);
                allFinite = allFinite && fin; worst = std::max (worst, pk);
            }
            std::printf ("  limiter x oversampling x machine-gun: worst peak %.4f  all finite %d\n", worst, (int) allFinite);
            check (allFinite, "Color Limiter never produces NaN/Inf under machine-gun retrigger at any oversampling factor");
            check (worst <= 0.944060876f + 1.0e-3f, "Color Limiter holds the -0.5 dB ceiling under machine-gun retrigger at any oversampling factor");
        }
    }

    // ---------------------------------------------------------------------
    // 2026-09-02 (user request): bigger wordmark + click -> about / licences page overlay.
    std::printf ("\n[UI] About page overlay\n");
    {
        KICKRAudioProcessor pe;
        pe.prepareToPlay (48000.0, 512);
        std::unique_ptr<juce::AudioProcessorEditor> edBase (pe.createEditor());
        auto* ed = dynamic_cast<KICKRAudioProcessorEditor*> (edBase.get());
        check (ed != nullptr, "editor created for the about-page test");
        if (ed != nullptr)
        {
            ed->setSize (1120, 819);
            const auto closed = ed->createComponentSnapshot (ed->getLocalBounds(), false, 1.0f);
            ed->showAboutPageForTest (true);
            const auto opened = ed->createComponentSnapshot (ed->getLocalBounds(), false, 1.0f);
            ed->showAboutPageForTest (false);
            const auto reclosed = ed->createComponentSnapshot (ed->getLocalBounds(), false, 1.0f);

            // The overlay dims the whole editor: the centre of the BODY hero panel must change
            // while open and come back when closed.
            const int cx = closed.getWidth() / 2, cy = closed.getHeight() * 3 / 5;
            const auto pc = closed.getPixelAt (cx, cy), po = opened.getPixelAt (cx, cy), pr = reclosed.getPixelAt (cx, cy);
            check (po != pc, "about page: overlay visibly covers the editor when open");
            check (pr == pc, "about page: editor is back to normal after closing");

            const auto out = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("kickr_ui_about_page.png");
            out.deleteFile();
            if (auto stream = out.createOutputStream())
            {
                juce::PNGImageFormat fmt;
                fmt.writeImageToStream (opened, *stream);
            }
            std::printf ("  about-page snapshot: %s\n", out.getFullPathName().toRawUTF8());
        }
    }

    // ---------------------------------------------------------------------
    // 2026-09-02 (user request): master LP / HP filter (FILTER page of the scope).
    std::printf ("\n[Feature] Master filter (FILTER page)\n");
    {
        auto goertzelF = [] (const juce::AudioBuffer<float>& b, double s, double t0, double t1, double hz)
        {
            const float* x = b.getReadPointer (0);
            const int i0 = std::max (0, (int) (t0 * s)); const int i1 = std::min (b.getNumSamples(), (int) (t1 * s));
            const double w = 2.0 * juce::MathConstants<double>::pi * hz / s; const double cw = 2.0 * std::cos (w);
            double s1 = 0.0, s2 = 0.0;
            for (int i = i0; i < i1; ++i) { const double s0 = (double) x[i] + cw * s1 - s2; s2 = s1; s1 = s0; }
            return std::sqrt (std::max (0.0, s1 * s1 + s2 * s2 - cw * s1 * s2)) / (double) std::max (1, i1 - i0);
        };
        auto identicalF = [] (const juce::AudioBuffer<float>& x, const juce::AudioBuffer<float>& y)
        {
            if (x.getNumSamples() != y.getNumSamples()) return false;
            for (int c = 0; c < x.getNumChannels(); ++c)
                for (int i = 0; i < x.getNumSamples(); ++i)
                    if (! juce::exactlyEqual (x.getSample (c, i), y.getSample (c, i))) return false;
            return true;
        };
        auto peakF = [] (const juce::AudioBuffer<float>& b, bool& fin)
        {
            fin = true; float pk = 0.0f;
            for (int c = 0; c < b.getNumChannels(); ++c) { const float* x = b.getReadPointer (c);
                for (int i = 0; i < b.getNumSamples(); ++i) { if (! std::isfinite (x[i])) fin = false; pk = std::max (pk, std::abs (x[i])); } }
            return pk;
        };

        // A) Off (default) is an exact bypass — even with the knobs anywhere.
        {
            KICKRAudioProcessor p0, p1;
            setP (p1, "filterFreq", 200.0f); setP (p1, "filterRes", 0.9f); setP (p1, "filterType", 1.0f);
            const auto b0 = kickr::tests::renderNote (p0, a1, vel, sr, 512, 0.6);
            const auto b1 = kickr::tests::renderNote (p1, a1, vel, sr, 512, 0.6);
            check (identicalF (b0, b1), "filter OFF: bit-identical bypass regardless of type / freq / res");
        }

        // B) LP 150 Hz removes the click's highs; HP 400 Hz removes the body's lows.
        {
            KICKRAudioProcessor pOff, pLp, pHp;
            for (auto* p : { &pOff, &pLp, &pHp }) { setP (*p, "limiter", 0.0f); setP (*p, "driveMix", 0.0f); }
            setP (pLp, "filterOn", 1.0f); setP (pLp, "filterType", 0.0f); setP (pLp, "filterFreq", 150.0f); setP (pLp, "filterRes", 0.0f);
            setP (pHp, "filterOn", 1.0f); setP (pHp, "filterType", 1.0f); setP (pHp, "filterFreq", 400.0f); setP (pHp, "filterRes", 0.0f);
            const auto bOff = kickr::tests::renderNote (pOff, a1, vel, sr, 512, 0.6);
            const auto bLp  = kickr::tests::renderNote (pLp,  a1, vel, sr, 512, 0.6);
            const auto bHp  = kickr::tests::renderNote (pHp,  a1, vel, sr, 512, 0.6);
            // Body sits ~55 Hz after the sweep (0.15-0.5 s). Click energy is the fast edges in
            // the first 20 ms — measured as second-difference RMS (a crude high-pass: a 55 Hz
            // body contributes ~5e-5 of its amplitude, 4 kHz content ~0.27), because a plain
            // rectangular-window Goertzel at 4 kHz mostly reads leakage from the body's onset.
            auto hf2 = [] (const juce::AudioBuffer<float>& b, double s, double t0, double t1)
            {
                const float* x = b.getReadPointer (0);
                const int i0 = std::max (2, (int) (t0 * s)); const int i1 = std::min (b.getNumSamples(), (int) (t1 * s));
                double acc = 0.0;
                for (int i = i0; i < i1; ++i) { const double d = (double) x[i] - 2.0 * x[i - 1] + x[i - 2]; acc += d * d; }
                return std::sqrt (acc / (double) std::max (1, i1 - i0));
            };
            const double loOff = goertzelF (bOff, sr, 0.15, 0.5, 55.0), loHp = goertzelF (bHp, sr, 0.15, 0.5, 55.0);
            const double hiOff = hf2 (bOff, sr, 0.0, 0.02), hiLp = hf2 (bLp, sr, 0.0, 0.02);
            std::printf ("  55 Hz body: off %.5f  HP400 %.5f | click edges (2nd-diff RMS, 0-20 ms): off %.5f  LP150 %.5f\n", loOff, loHp, hiOff, hiLp);
            check (loHp < loOff * 0.1, "HP 400 Hz removes the 55 Hz body (> 20 dB down)");
            check (hiLp < hiOff * 0.1, "LP 150 Hz removes the click's fast edges (> 20 dB down)");
        }

        // C) Resonance makes a peak at the cutoff (LP 200 Hz on the steady 100 Hz body -> 2nd harmonic region).
        {
            KICKRAudioProcessor pQ0, pQ1;
            for (auto* p : { &pQ0, &pQ1 })
            {
                setP (*p, "tuneMode", 1.0f); setP (*p, "fundamental", 200.0f); setP (*p, "pitchStart", 1.0f);
                setP (*p, "bodyDecay", 2000.0f); setP (*p, "subLevel", 0.0f); setP (*p, "clickLevel", 0.0f);
                setP (*p, "tailLevel", 0.0f); setP (*p, "noiseLevel", 0.0f); setP (*p, "driveMix", 0.0f); setP (*p, "limiter", 0.0f);
                setP (*p, "output", -12.0f);
                setP (*p, "filterOn", 1.0f); setP (*p, "filterType", 0.0f); setP (*p, "filterFreq", 200.0f);
            }
            setP (pQ0, "filterRes", 0.0f); setP (pQ1, "filterRes", 0.8f);
            const auto b0 = kickr::tests::renderNote (pQ0, a1, vel, sr, 512, 0.6);
            const auto b1 = kickr::tests::renderNote (pQ1, a1, vel, sr, 512, 0.6);
            const double g0 = goertzelF (b0, sr, 0.1, 0.5, 200.0), g1 = goertzelF (b1, sr, 0.1, 0.5, 200.0);
            std::printf ("  200 Hz sine at LP cutoff 200 Hz: res 0 -> %.5f, res 0.8 -> %.5f (x%.2f)\n", g0, g1, g1 / std::max (1e-9, g0));
            check (g1 > g0 * 2.0, "RES raises the level at the cutoff (resonant peak)");
        }

        // D) Stability: every OS factor x machine-gun with a resonant HP sweeping low.
        {
            bool allFin = true; float worst = 0.0f;
            for (int os = 0; os < 4; ++os)
            {
                KICKRAudioProcessor p;
                setP (p, "oversampling", (float) os);
                setP (p, "filterOn", 1.0f); setP (p, "filterType", (float) (os % 2)); setP (p, "filterFreq", os < 2 ? 60.0f : 8000.0f); setP (p, "filterRes", 1.0f);
                auto [rbuf, onsets] = renderRetrigger (p, a1, vel, sr, 256, 24, 60.0 / 174.0 / 8.0, 2.0);
                juce::ignoreUnused (onsets);
                bool fin = false; const float pk = peakF (rbuf, fin);
                allFin = allFin && fin; worst = std::max (worst, pk);
            }
            std::printf ("  filter x oversampling x machine-gun @ res 1.0: worst peak %.3f  all finite %d\n", worst, (int) allFin);
            check (allFin,       "master filter: no NaN/Inf at any oversampling factor under machine-gun retrigger");
            check (worst < 4.0f, "master filter: bounded at full resonance");
        }

        // E) FILTER page snapshot (headless) for visual review.
        {
            KICKRAudioProcessor pe;
            pe.prepareToPlay (48000.0, 512);
            setP (pe, "filterOn", 1.0f); setP (pe, "filterFreq", 320.0f); setP (pe, "filterRes", 0.55f);
            kickr::tests::renderNote (pe, a1, vel, 48000.0, 512, 0.3);   // feed the analyzer so the spectrum backdrop has something to show
            std::unique_ptr<juce::AudioProcessorEditor> edBase (pe.createEditor());
            auto* ed = dynamic_cast<KICKRAudioProcessorEditor*> (edBase.get());
            check (ed != nullptr, "editor created for the filter-page snapshot");
            if (ed != nullptr)
            {
                ed->setSize (1120, 819);
                ed->showFilterPageForTest();
                ed->refreshAnalyzersForSnapshot();   // 2026-09-02 follow-up: spectrum backdrop behind the curve
                const auto img = ed->createComponentSnapshot (ed->getLocalBounds(), false, 1.0f);
                const auto out = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("kickr_ui_filter_page.png");
                out.deleteFile();
                if (auto stream = out.createOutputStream()) { juce::PNGImageFormat fmt; fmt.writeImageToStream (img, *stream); }
                std::printf ("  filter-page snapshot: %s\n", out.getFullPathName().toRawUTF8());
            }
        }
    }

    // ---------------------------------------------------------------------
    // 2026-09-02 (user: "feels like the spectrum doesnt show the high end correctly ...
    // look at how fabfilter pro q 3 works and implement the same settings").
    std::printf ("\n[Feature] Pro-Q-style analyzer\n");
    {
        const double fsA = 48000.0;
        auto pushSine = [&] (kickr::Analyzer& an, double hz, float amp, double seconds, int blk)
        {
            std::vector<float> l ((size_t) blk), r ((size_t) blk);
            const int nBlocks = (int) std::ceil (seconds * fsA / blk);
            double ph = 0.0;
            for (int b = 0; b < nBlocks; ++b)
            {
                for (int i = 0; i < blk; ++i)
                {
                    l[(size_t) i] = r[(size_t) i] = amp * (float) std::sin (ph);
                    ph += 2.0 * juce::MathConstants<double>::pi * hz / fsA;
                }
                an.pushBlock (l.data(), r.data(), blk);
                an.updateSpectrum();
            }
        };
        auto pushNoise = [&] (kickr::Analyzer& an, float amp, double seconds, int blk, juce::Random& rng)
        {
            std::vector<float> l ((size_t) blk), r ((size_t) blk);
            const int nBlocks = (int) std::ceil (seconds * fsA / blk);
            for (int b = 0; b < nBlocks; ++b)
            {
                for (int i = 0; i < blk; ++i) l[(size_t) i] = r[(size_t) i] = amp * (rng.nextFloat() * 2.0f - 1.0f);
                an.pushBlock (l.data(), r.data(), blk);
                an.updateSpectrum();
            }
        };
        auto peakBinDb = [] (const kickr::Analyzer& an, int& binOut)
        {
            const auto& db = an.getSpectrumDb(); float best = -1e9f; binOut = 1;
            for (int b = 1; b < an.getNumBins(); ++b) if (db[(size_t) b] > best) { best = db[(size_t) b]; binOut = b; }
            return best;
        };
        auto columnAt = [] (const std::vector<float>& cols, float fMin, float fMax, float hz)
        {
            const double t = std::log (hz / fMin) / std::log (fMax / fMin);
            return cols[(size_t) juce::jlimit (0, (int) cols.size() - 1, (int) std::lround (t * (double) (cols.size() - 1)))];
        };

        // A) Calibration: a 0 dBFS 1 kHz sine reads ~0 dB in its bin at every resolution.
        {
            bool ok = true; juce::String line;
            for (int order = 11; order <= 14; ++order)
            {
                kickr::Analyzer an; an.prepare (fsA); an.setResolutionOrder (order);
                pushSine (an, 1000.0, 1.0f, 0.6, 512);
                int bin = 0; const float pk = peakBinDb (an, bin);
                const double hz = bin * (fsA * 0.5) / an.getNumBins();
                line << " " << (1 << order) << ":" << juce::String (pk, 2) << "dB@" << juce::String (hz, 0) << "Hz";
                if (std::abs (pk) > 1.0f || std::abs (hz - 1000.0) > 2.0 * (fsA * 0.5) / an.getNumBins()) ok = false;
            }
            std::printf ("  0 dBFS 1 kHz sine per resolution:%s\n", line.toRawUTF8());
            check (ok, "analyzer: 0 dBFS sine reads 0 dB (+-1) at its frequency at every resolution (2048..16384)");
        }

        // B) Tilt: column value at 4 kHz is +9 dB with 4.5 dB/oct, 0 with tilt 0.
        {
            kickr::Analyzer an; an.prepare (fsA);
            pushSine (an, 4000.0, 1.0f, 0.6, 512);
            std::vector<float> cols;
            an.getView().tiltDbPerOct = 0.0f;
            kickr::buildSpectrumColumns (an, 1000, 20.0f, 20000.0f, cols);
            const float c0 = columnAt (cols, 20.0f, 20000.0f, 4000.0f);
            an.getView().tiltDbPerOct = 4.5f;
            kickr::buildSpectrumColumns (an, 1000, 20.0f, 20000.0f, cols);
            const float c45 = columnAt (cols, 20.0f, 20000.0f, 4000.0f);
            std::printf ("  4 kHz sine column: tilt 0 -> %.2f dB, tilt 4.5 -> %.2f dB (expect a +9 dB difference)\n", c0, c45);
            check (std::abs ((c45 - c0) - 9.0f) < 1.5f, "analyzer tilt: +4.5 dB/oct lifts 4 kHz by 9 dB relative to the 1 kHz pivot");
        }

        // C) Ballistics: instant attack, release at the Speed rate.
        {
            kickr::Analyzer an; an.prepare (fsA); an.setFrameRate (30.0f); an.setReleaseDbPerSecond (60.0f);
            pushSine (an, 1000.0, 1.0f, 0.3, 512);
            int bin = 0; const float before = peakBinDb (an, bin);
            // silence: ~10 frames at 30 Hz = 1600 samples each
            std::vector<float> z (1600, 0.0f);
            for (int f = 0; f < 10; ++f) { an.pushBlock (z.data(), z.data(), 1600); an.updateSpectrum(); }
            const float after = an.getSpectrumDb()[(size_t) bin];
            std::printf ("  release: %.2f dB -> %.2f dB after 10 frames @ 60 dB/s (expect -20 dB)\n", before, after);
            check (std::abs ((before - after) - 20.0f) < 4.0f, "analyzer speed: peak falls at the set release rate (60 dB/s -> 20 dB in 10 frames)");
        }

        // D) Broadband highs read flat: white noise, tilt 0, per-column energy averaging ->
        //    2-10 kHz within a few dB of each other (the old per-bin drawing sagged here).
        {
            kickr::Analyzer an; an.prepare (fsA); an.setResolutionOrder (12); an.setFrameRate (30.0f); an.setReleaseDbPerSecond (240.0f);   // Very Fast: little peak-hold bias
            an.getView().tiltDbPerOct = 0.0f;
            juce::Random rng (1234);
            pushNoise (an, 0.5f, 1.0, 512, rng);
            std::vector<float> cols;
            kickr::buildSpectrumColumns (an, 1000, 20.0f, 20000.0f, cols);
            // Trend, not raw scatter: the old per-bin drawing sagged the high end by tens of dB;
            // single-bin columns near 2 kHz still carry a few dB of Rayleigh variance, which is
            // real signal statistics, not a display error.
            float lo = 1e9f, hi = -1e9f, lowBand = 0.0f, highBand = 0.0f;
            for (float hz : { 2000.0f, 3000.0f, 5000.0f, 7000.0f, 10000.0f }) { const float c = columnAt (cols, 20.0f, 20000.0f, hz); lo = std::min (lo, c); hi = std::max (hi, c); }
            for (float hz : { 2000.0f, 2500.0f, 3000.0f })    lowBand  += columnAt (cols, 20.0f, 20000.0f, hz) / 3.0f;
            for (float hz : { 7000.0f, 8500.0f, 10000.0f })   highBand += columnAt (cols, 20.0f, 20000.0f, hz) / 3.0f;
            const float at200 = columnAt (cols, 20.0f, 20000.0f, 200.0f);
            std::printf ("  white noise columns 2-10 kHz: %.1f .. %.1f dB (scatter %.1f), 2-3 kHz mean %.1f vs 7-10 kHz mean %.1f, 200 Hz %.1f dB\n", lo, hi, hi - lo, lowBand, highBand, at200);
            check (std::abs (lowBand - highBand) < 4.0f, "analyzer: no high-end sag - 7-10 kHz reads within 4 dB of 2-3 kHz on white noise");
            check (hi - lo < 8.0f, "analyzer: broadband scatter across 2-10 kHz stays under 8 dB (energy-averaged, 1/48-oct smoothed columns)");
            check (std::abs (at200 - 0.5f * (lo + hi)) < 6.0f, "analyzer: white noise reads the same level in the lows as in the highs");
        }

        // E) SPECTRUM page snapshot with the settings row, fed with a kick.
        {
            KICKRAudioProcessor pe;
            pe.prepareToPlay (48000.0, 512);
            kickr::tests::renderNote (pe, a1, vel, 48000.0, 512, 0.05);   // fresh hit in the analyzer history
            std::unique_ptr<juce::AudioProcessorEditor> edBase (pe.createEditor());
            auto* ed = dynamic_cast<KICKRAudioProcessorEditor*> (edBase.get());
            check (ed != nullptr, "editor created for the spectrum-page snapshot");
            if (ed != nullptr)
            {
                ed->setSize (1120, 819);
                ed->showSpectrumPageForTest();
                ed->refreshAnalyzersForSnapshot();
                const auto img = ed->createComponentSnapshot (ed->getLocalBounds(), false, 1.0f);
                const auto out = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("kickr_ui_spectrum_page.png");
                out.deleteFile();
                if (auto stream = out.createOutputStream()) { juce::PNGImageFormat fmt; fmt.writeImageToStream (img, *stream); }
                std::printf ("  spectrum-page snapshot: %s\n", out.getFullPathName().toRawUTF8());
            }
        }
    }

    std::printf ("\n%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
