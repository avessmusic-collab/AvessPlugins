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

    std::printf ("\n%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
