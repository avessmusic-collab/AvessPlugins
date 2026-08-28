/*
    KICKR — console test runner. Grows one block of checks per Stage-2 phase.
    Build:  cmake --build build --target KICKR_Tests
    Run:    ./build/plugins/KICKR/KICKR_Tests
    Writes kickr_phase*.wav next to the working directory for manual inspection.
*/
#include "PluginProcessor.h"
#include "Tests/OfflineRender.h"

#include <cmath>
#include <cstdio>

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
    const int    a1 = 33;                 // MIDI A1 = 55 Hz
    const float  vel = 100.0f / 127.0f;

    std::printf ("KICKR tests  (sr %.0f)\n", sr);
    std::printf ("\n[Phase 2.1] OS-region shell + MIDI-triggered basic kick\n");

    KICKRAudioProcessor proc;
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
        setP (p, "pitchTime", 900.0f);
        const auto b = kickr::tests::renderNote (p, a1, vel, sr, 512, 1.5);
        const double fHead = estFreq (b, sr, 0.002, 0.020);
        std::printf ("  start-ratio probe (pitchTime 900ms): f(2-20ms) = %.0f Hz   (target ~4x55)\n", fHead);
        check (fHead > 150.0,  "pitch starts near pitchStart x fundamental (>= ~3x up)");
    }

    // Medium pitchTime (200 ms) — the fall is slow enough to track by zero-crossings.
    {
        KICKRAudioProcessor p;
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
        const auto wav = juce::File::getCurrentWorkingDirectory().getChildFile ("kickr_phase2_2.wav");
        kickr::tests::renderNoteToWav (pw, wav, a1, vel, sr, 512, 1.0);
    }

    // curve = 0 (near-linear sweep) vs curve = 1 (snap) — measured mid-fall
    {
        KICKRAudioProcessor p0, p1;
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

    std::printf ("\n%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
