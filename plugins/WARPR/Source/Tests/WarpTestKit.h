#pragma once
/*
  WarpTestKit.h - the class-level warp test machinery shared by the per-phase test blocks (D2, D3, ...):
  single-cycle fixtures, a one-oscillator renderer driving UnisonOsc::render() with an explicit WarpSpan
  (OS-rate bus, no decimator), span builders and signal measures.

  The "universal cube" (T1) is a loop over (mode, signal, OS, a, A, B) that calls renderOsc(); each phase
  supplies its own mode range and per-mode bounds.
*/

#include "DSP/RegionFold.h"
#include "DSP/UnisonOsc.h"
#include "DSP/WarpModeTable.h"
#include "DSP/WarpStage.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace warpr::tests::kit
{
    inline constexpr double kPi = 3.14159265358979323846;

    inline double rootHz() { return juce::MidiMessage::getMidiNoteInHertz (60); }

    inline size_t ix (int v) { return static_cast<size_t> (v); }

    //==========================================================================
    // fixtures
    //==========================================================================
    /** One cycle per root period: fileSampleRate = frames * midiToHz(60) -> Lc == frames. */
    inline warpr::SampleData::Ptr makeCycle (int frames, float amp, bool saw = false)
    {
        juce::AudioBuffer<float> b (1, frames);
        for (int i = 0; i < frames; ++i)
        {
            const double ph = (double) i / (double) frames;
            b.setSample (0, i, saw ? amp * (float) (2.0 * ph - 1.0) : amp * (float) std::sin (2.0 * kPi * ph));
        }
        return warpr::SampleData::create (b, (double) frames * rootHz(), saw ? "saw cycle" : "sine cycle", true, false);
    }

    /** Stereo cycle with identical (or different) channels. */
    inline warpr::SampleData::Ptr makeStereoCycle (int frames, bool identical)
    {
        juce::AudioBuffer<float> b (2, frames);
        for (int i = 0; i < frames; ++i)
        {
            const double ph = (double) i / (double) frames;
            const float l = 0.4f * (float) std::sin (2.0 * kPi * ph);
            b.setSample (0, i, l);
            b.setSample (1, i, identical ? l : 0.3f * (float) std::sin (4.0 * kPi * ph + 0.5));
        }
        return warpr::SampleData::create (b, (double) frames * rootHz(), "stereo cycle", true, false);
    }

    inline warpr::SampleData::Ptr makeNoise()
    {
        juce::Random rng (7);
        juce::AudioBuffer<float> b (1, 48000);
        for (int i = 0; i < 48000; ++i)
            b.setSample (0, i, 0.3f * (rng.nextFloat() * 2.0f - 1.0f));
        return warpr::SampleData::create (b, 48000.0, "noise", false, false);
    }

    //==========================================================================
    // class-level oscillator renderer
    //==========================================================================
    struct OscRun
    {
        std::vector<float> L, R;
    };

    /** Renders `totalBase` host samples (multiple of 32) of one oscillator at `hz`.  `spanFn (chunkIndex, span)`
        fills the span and returns whether the span is passed at all (false = nullptr = the D1 path). */
    template <class SpanFn>
    OscRun renderOsc (const warpr::SampleData& sd, double hz, double fs, int os, int totalBase, SpanFn&& spanFn)
    {
        warpr::RegionFold rg;
        rg.configure (sd, 0.0f, 1.0f, warpr::RegionFold::Mode::Forward);

        const double lc = sd.fileSampleRate / rootHz();
        warpr::UnisonOsc osc;
        osc.start (rg, false, 0, lc);

        const double rho = hz / rootHz() * sd.fileSampleRate / fs;
        constexpr int chunk = 32;
        const int total = (totalBase / chunk) * chunk;

        OscRun out;
        out.L.assign (ix (total) * ix (os), 0.0f);
        out.R.assign (ix (total) * ix (os), 0.0f);

        int ci = 0;
        for (int i = 0; i < total; i += chunk, ++ci)
        {
            warpr::WarpSpan sp;
            sp.Lc = lc;
            sp.os = os;
            sp.osRate = fs * (double) os;
            sp.fadeLen = (int) std::lround (0.010 * fs * (double) os);
            const bool use = spanFn (ci, sp);

            osc.render (sd, rg, 0, rho, rho, os, 1.0f, 1.0f, 1.0f, 1.0f,
                        out.L.data() + ix (i) * ix (os), out.R.data() + ix (i) * ix (os), chunk * os,
                        use ? &sp : nullptr);
        }

        return out;
    }

    /** Constant warp span for every chunk. */
    inline auto constSpan (int mode, float a, float A, float B, float mix)
    {
        return [=] (int, warpr::WarpSpan& sp)
        {
            sp.active = true;
            sp.mode = mode;
            sp.a0 = sp.a1 = a;
            sp.A0 = sp.A1 = A;
            sp.B0 = sp.B1 = B;
            sp.mix0 = sp.mix1 = mix;
            return true;
        };
    }

    inline auto noWarp()
    {
        return [] (int, warpr::WarpSpan&) { return false; };
    }

    //==========================================================================
    // measures
    //==========================================================================
    inline double maxStepOf (const std::vector<float>& x, size_t from = 0)
    {
        double m = 0.0;
        for (size_t i = from + 1; i < x.size(); ++i)
            m = std::max (m, (double) std::abs (x[i] - x[i - 1]));
        return m;
    }

    inline double peakOf (const std::vector<float>& x)
    {
        double m = 0.0;
        for (float v : x)
            m = std::max (m, (double) std::abs (v));
        return m;
    }

    inline bool allFinite (const std::vector<float>& x)
    {
        for (float v : x)
            if (! std::isfinite (v))
                return false;
        return true;
    }

    inline bool bitEqual (const std::vector<float>& a, const std::vector<float>& b)
    {
        return a.size() == b.size() && std::memcmp (a.data(), b.data(), a.size() * sizeof (float)) == 0;
    }

    /** 10 log10 of the power of (a - b) relative to a; -300 when they are identical. */
    inline double nullDb (const std::vector<float>& a, const std::vector<float>& b, size_t from = 0)
    {
        double num = 0.0, den = 0.0;
        for (size_t i = from; i < a.size() && i < b.size(); ++i)
        {
            const double d = (double) a[i] - (double) b[i];
            num += d * d;
            den += (double) a[i] * (double) a[i];
        }
        if (num <= 0.0)
            return -300.0;
        return 10.0 * std::log10 (num / std::max (den, 1.0e-30));
    }
}
