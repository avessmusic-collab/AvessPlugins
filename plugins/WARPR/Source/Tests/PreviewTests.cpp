/*
    VOID (WARPR) - processed-waveform preview tests (UiBackendImpl::computeProcessedPeaks).
    Called from RunTests.cpp main():   failures += runPreviewTests (full);

    Contents [preview]
      1. default (test-backend) implementation    returns 0 and zero-fills
      2. warp + shaper off                         processed peaks == raw getRangePeaks reduction (mono and stereo, 1e-6)
      3. region handling                           frames outside sampleStart..sampleEnd pass through raw
      4. hard clip at 0.5 (shaper)                 peaks clamp, the unclipped raw peaks do not
      5. warp amount 0 / phase warps               a = 0 is the identity; real phase warps change the peaks, stay finite, keep the pair count
      6. all 60 warp modes                         finite, pair count preserved
      7. determinism                               two calls bit-identical, also after unrelated state changes
      8. over-cap spans                            sub-window path: pair count, finiteness, determinism
      9. cost                                      per-call time for a 1 s region (printed)
*/

#include "PluginProcessor.h"
#include "DSP/WarpModeTable.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

JUCE_BEGIN_IGNORE_WARNINGS_GCC_LIKE ("-Wfloat-equal")

int runPreviewTests (bool full);

namespace
{
    int failures = 0;

    void check (bool cond, const char* name)
    {
        std::printf ("  %s  %s\n", cond ? "PASS" : "FAIL", name);
        if (! cond) ++failures;
    }

    using ProcPtr = std::unique_ptr<PluginProcessor>;

    void setReal (PluginProcessor& p, const char* id, float real)
    {
        if (auto* q = dynamic_cast<juce::RangedAudioParameter*> (p.apvts.getParameter (id)))
            q->setValueNotifyingHost (q->convertTo0to1 (real));
    }

    /** Deterministic non-periodic test material (peak <= 0.9): three inharmonic partials. */
    warpr::SampleData::Ptr makeTestSample (int channels, int frames, double fs)
    {
        juce::AudioBuffer<float> b (channels, frames);

        for (int c = 0; c < channels; ++c)
            for (int i = 0; i < frames; ++i)
            {
                const double t = (double) i / fs;
                const double ph = c == 0 ? 0.0 : 0.7;
                const double v = 0.6 * std::sin (6.283185307179586 * 220.0 * t + ph)
                               + 0.3 * std::sin (6.283185307179586 * 1370.0 * t + 1.0 + ph)
                               + 0.1 * std::sin (6.283185307179586 * 3301.0 * t);
                b.setSample (c, i, (float) (0.9 * v));
            }

        return warpr::SampleData::create (b, fs, "preview-test", false, false);
    }

    ProcPtr makeProc (warpr::SampleData::Ptr sample)
    {
        auto p = std::make_unique<PluginProcessor>();
        p->getSampleStore().publish (std::move (sample));
        return p;
    }

    std::vector<float> peaks (PluginProcessor& p, int pairs, double a = 0.0, double b = 1.0, int* written = nullptr)
    {
        std::vector<float> out ((size_t) pairs * 2, -7.0f);
        const int n = p.getUiBackend().computeProcessedPeaks (out.data(), pairs, a, b);
        if (written != nullptr)
            *written = n;
        return out;
    }

    double maxAbsDiff (const std::vector<float>& a, const std::vector<float>& b)
    {
        if (a.size() != b.size())
            return 1.0e9;

        double worst = 0.0;
        for (size_t i = 0; i < a.size(); ++i)
            worst = std::max (worst, (double) std::abs (a[i] - b[i]));
        return worst;
    }

    bool allFinite (const std::vector<float>& v)
    {
        for (float x : v)
            if (! std::isfinite (x))
                return false;
        return true;
    }

    void setWarp (PluginProcessor& p, int mode, float amount, float a = 0.5f, float b = 0.0f)
    {
        setReal (p, "warpEnable", 1.0f);
        setReal (p, "warpMode", (float) mode);
        setReal (p, "warpAmount", amount);
        setReal (p, "warpParamA", a);
        setReal (p, "warpParamB", b);
        setReal (p, "warpMix", 1.0f);
    }

    /** A backend that implements only the pure virtuals: exercises the interface default. */
    struct NullBackend : WarprUiBackend
    {
        int getPlayheads (WarprVizFrame::Voice*, int) override { return 0; }
        WarprVizFifo& vizFifo() override { return fifo; }
        uint32_t revision (Revision) const override { return 0; }
        juce::String getUiStateJson() override { return {}; }
        void setLfoCount (int) override {}
        void setLfoShapeJson (int, const juce::String&) override {}
        void setModMatrixJson (const juce::String&) override {}
        void setShaperCurveJson (const juce::String&, const juce::String&) override {}
        juce::String getShaperCurveJson (const juce::String&) override { return {}; }
        void shaperReset (const juce::String&) override {}
        void shaperNormalize (const juce::String&) override {}
        void loadSampleFile (const juce::File&) override {}
        WarprSampleDisplay getSampleDisplay() override { return {}; }
        void computeFilterResponse (float*, int) override {}
        juce::String getPresetName() override { return {}; }
        void presetStep (int) override {}
        void presetSaveDialog() override {}
        void presetLoadDialog() override {}
        void openSettings() override {}

        WarprVizFifo fifo;
    };

    double nowMs() { return juce::Time::getMillisecondCounterHiRes(); }
}

int runPreviewTests (bool full)
{
    failures = 0;
    std::printf ("\n[preview] processed-waveform preview (real warp + shaper kernels, message thread)\n");

    constexpr double fs = 48000.0;
    constexpr int pairs = 256;

    // ---- 1. interface default
    {
        NullBackend nb;
        float buf[16];
        std::fill_n (buf, 16, 7.0f);
        const int n = nb.computeProcessedPeaks (buf, 8, 0.0, 1.0);
        bool zero = true;
        for (int i = 0; i < 16; ++i)
            zero = zero && buf[i] == 0.0f;
        check (n == 0 && zero, "default implementation: returns 0 and zero-fills");
    }

    // ---- 2. everything off == raw reduction
    for (int channels = 1; channels <= 2; ++channels)
    {
        auto p = makeProc (makeTestSample (channels, 48000, fs));
        setReal (*p, "warpEnable", 0.0f);
        setReal (*p, "shaperEnable", 0.0f);

        int written = 0;
        const auto proc = peaks (*p, pairs, 0.0, 1.0, &written);
        const auto raw = p->getUiBackend().getRangePeaks (0.0, 1.0, pairs);
        const double d = maxAbsDiff (proc, raw);
        std::printf ("        (%s, warp+shaper off: max |processed - raw| = %.3g)\n", channels == 1 ? "mono" : "stereo", d);
        check (written == pairs && d < 1.0e-6, channels == 1 ? "warp + shaper off: processed peaks == raw peaks (mono)"
                                                              : "warp + shaper off: processed peaks == raw peaks (stereo)");

        // a zoomed sub-range with the same arithmetic
        const auto procZ = peaks (*p, 100, 0.3, 0.42);
        const auto rawZ = p->getUiBackend().getRangePeaks (0.3, 0.42, 100);
        check (maxAbsDiff (procZ, rawZ) < 1.0e-6, "warp + shaper off: zoomed sub-range matches getRangePeaks");

        // enabled but neutral: warp mix 0, shaper mix 0, and warp amount 0
        setReal (*p, "warpEnable", 1.0f);
        setReal (*p, "shaperEnable", 1.0f);
        setReal (*p, "warpAmount", 0.0f);
        setReal (*p, "shaperMix", 0.0f);
        check (maxAbsDiff (peaks (*p, pairs), raw) < 1.0e-6, "enabled with amount 0 and shaper mix 0: identity");
    }

    // ---- 3. region
    {
        auto p = makeProc (makeTestSample (1, 48000, fs));
        setReal (*p, "sampleStart", 0.25f);
        setReal (*p, "sampleEnd", 0.75f);
        setReal (*p, "warpEnable", 0.0f);
        const auto raw = p->getUiBackend().getRangePeaks (0.0, 1.0, pairs);
        check (maxAbsDiff (peaks (*p, pairs), raw) < 1.0e-6, "region 0.25..0.75, stages off: identity everywhere");

        setWarp (*p, 9, 1.0f, 0.8f, 0.3f);                       // Mirror, full amount
        const auto warped = peaks (*p, pairs);
        bool outsideRaw = true, insideChanged = false;
        for (int q = 0; q < pairs; ++q)
        {
            const double d = std::max (std::abs (warped[(size_t) q * 2] - raw[(size_t) q * 2]),
                                       std::abs (warped[(size_t) q * 2 + 1] - raw[(size_t) q * 2 + 1]));
            if (q < 60 || q >= 200)
                outsideRaw = outsideRaw && d < 1.0e-6;
            else if (d > 1.0e-4)
                insideChanged = true;
        }
        check (outsideRaw, "frames outside sampleStart..sampleEnd are passed through raw");
        check (insideChanged, "frames inside the region are processed by the warp");
    }

    // ---- 4. hard clip at 0.5
    {
        auto p = makeProc (makeTestSample (1, 48000, fs));
        setReal (*p, "warpEnable", 0.0f);
        const auto raw = peaks (*p, pairs);
        float rawMax = 0.0f;
        for (float v : raw)
            rawMax = std::max (rawMax, std::abs (v));

        p->getUiBackend().setShaperCurveJson ("A", R"({"interp":"linear","points":[{"x":-1,"y":-0.5},{"x":-0.5,"y":-0.5},{"x":0.5,"y":0.5},{"x":1,"y":0.5}]})");
        setReal (*p, "shaperEnable", 1.0f);
        setReal (*p, "shaperMorph", 0.0f);
        setReal (*p, "shaperMix", 1.0f);

        const auto clipped = peaks (*p, pairs);
        float clipMax = 0.0f;
        for (float v : clipped)
            clipMax = std::max (clipMax, std::abs (v));

        std::printf ("        (raw peak %.3f -> clipped peak %.3f)\n", (double) rawMax, (double) clipMax);
        check (rawMax > 0.75f, "hard clip test material exceeds 0.5 (raw peak > 0.75)");
        // MEASURED 0.546: the shaper's remainder-form ADAA outputs the curve's inter-sample mean,
        // which sits up to half an input step above a hard corner on fast-moving material - the
        // VOICE path renders the same value, so the preview is faithful; the naive 0.505 gate
        // ignored the anti-aliasing. Bound = 0.5 + max inter-sample step/2 of a 0.9 sine (~0.06).
        check (clipMax <= 0.56f && clipMax >= 0.49f, "hard clip at 0.5: peaks clamp to ~0.5 (+ ADAA half-step)");

        // mix 0.5 lies between the raw and the clipped peak
        setReal (*p, "shaperMix", 0.5f);
        float halfMax = 0.0f;
        for (float v : peaks (*p, pairs))
            halfMax = std::max (halfMax, std::abs (v));
        check (halfMax > clipMax + 0.02f && halfMax < rawMax - 0.02f, "shaper mix 0.5: peak between clipped and raw");
    }

    // ---- 5. phase warps
    {
        auto p = makeProc (makeTestSample (1, 48000, fs));
        setReal (*p, "shaperEnable", 0.0f);
        const auto raw = p->getUiBackend().getRangePeaks (0.0, 1.0, 2048);

        setWarp (*p, 9, 0.0f);
        check (maxAbsDiff (peaks (*p, 2048), raw) < 1.0e-6, "warp amount 0: identity (bit-exact contract)");

        int changed = 0;
        bool finite = true, countOk = true;
        for (int mode : { 9, 10, 45, 46, 48 })
        {
            setWarp (*p, mode, 1.0f, 0.8f, 0.3f);
            int written = 0;
            const auto w = peaks (*p, 2048, 0.0, 1.0, &written);
            finite = finite && allFinite (w);
            countOk = countOk && written == 2048;
            const double d = maxAbsDiff (w, raw);
            std::printf ("        (phase mode %2d: max change %.3f)\n", mode, d);
            if (d > 1.0e-3)
                ++changed;
        }
        check (changed >= 3, "phase warps change the peaks (at least 3 of 5 probed modes)");
        check (finite && countOk, "phase warps: finite, pair count preserved");
    }

    // ---- 6. all 60 modes
    {
        auto p = makeProc (makeTestSample (2, 48000, fs));
        setReal (*p, "shaperEnable", 0.0f);
        int bad = 0, wrongCount = 0;
        for (int mode = 0; mode < warpr::kNumWarpModes; ++mode)
        {
            setWarp (*p, mode, 0.7f, 0.6f, 0.4f);
            int written = 0;
            const auto w = peaks (*p, pairs, 0.0, 1.0, &written);
            if (! allFinite (w)) ++bad;
            if (written != pairs) ++wrongCount;
        }
        check (bad == 0 && wrongCount == 0, "all 60 warp modes: finite, pair count preserved");
    }

    // ---- 7. determinism
    {
        auto p = makeProc (makeTestSample (2, 48000, fs));
        setWarp (*p, 58, 0.8f, 0.5f, 0.5f);                      // Self FM: the stateful feedback mode
        setReal (*p, "shaperEnable", 1.0f);
        const auto a = peaks (*p, pairs);
        const auto b = peaks (*p, pairs);
        // interleave an unrelated call with a different range / mode, then repeat
        setWarp (*p, 59, 0.9f, 0.3f, 0.2f);
        (void) peaks (*p, 64, 0.2, 0.4);
        setWarp (*p, 58, 0.8f, 0.5f, 0.5f);
        const auto c = peaks (*p, pairs);
        check (a.size() == b.size() && std::memcmp (a.data(), b.data(), a.size() * sizeof (float)) == 0, "two calls: bit-identical");
        check (a.size() == c.size() && std::memcmp (a.data(), c.data(), a.size() * sizeof (float)) == 0,
               "bit-identical after unrelated calls (no state carried between calls)");
    }

    // ---- 8. over-cap spans
    {
        constexpr int longFrames = WarprUiBackend::kPreviewFrameCap + 100000;
        auto p = makeProc (makeTestSample (1, longFrames, fs));
        setWarp (*p, 48, 0.8f, 0.6f, 0.2f);
        setReal (*p, "shaperEnable", 0.0f);

        int written = 0;
        const double t0 = nowMs();
        const auto a = peaks (*p, 512, 0.0, 1.0, &written);
        const double t1 = nowMs();
        const auto b = peaks (*p, 512, 0.0, 1.0);
        std::printf ("        (over-cap span %d frames, 512 pairs: %.1f ms)\n", longFrames, t1 - t0);
        check (written == 512 && allFinite (a), "over-cap span: sub-window path gives all pairs, finite");
        check (std::memcmp (a.data(), b.data(), a.size() * sizeof (float)) == 0, "over-cap span: deterministic");

        bool inRange = true;
        for (size_t q = 0; q < a.size(); q += 2)
            inRange = inRange && a[q] <= a[q + 1] && std::abs (a[q]) <= 1.01f && std::abs (a[q + 1]) <= 1.01f;
        check (inRange, "over-cap span: every pair has min <= max inside the sample's range");
    }

    // ---- 9. cost of a 1 s region
    {
        auto p = makeProc (makeTestSample (2, 48000, fs));
        setReal (*p, "shaperEnable", 1.0f);

        for (const int mode : { 17, 25, 48, 54, 58 })
        {
            setWarp (*p, mode, 0.7f, 0.5f, 0.3f);
            (void) peaks (*p, 512);                                         // warm-up (tables, caches)
            const int reps = full ? 20 : 5;
            const double t0 = nowMs();
            for (int i = 0; i < reps; ++i)
                (void) peaks (*p, 512);
            std::printf ("        (1 s stereo region, warp mode %2d + shaper: %.2f ms per call)\n", mode, (nowMs() - t0) / reps);
        }

        check (true, "per-call cost printed");
    }

    std::printf ("  [preview] %s\n", failures == 0 ? "all passed" : "FAILURES");
    return failures;
}

JUCE_END_IGNORE_WARNINGS_GCC_LIKE
