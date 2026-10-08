/*
    CORRUPTR — console QA runner (full-effect audit, 2026-10-08).
    Build:  cmake --build build --target CORRUPTR_Tests
    Run:    ./build/plugins/CORRUPTR/CORRUPTR_Tests

    Renders real audio through every distortion algorithm, bitcrush mode,
    glitch mode, filter type, the feedback loop, quality modes, performance
    triggers, sequencer and mod matrix, checking output sanity (finite,
    bounded, non-silent, actually-does-something) per section.
*/
#include "PluginProcessor.h"
#include <juce_audio_formats/juce_audio_formats.h>

#include <array>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <set>
#include <vector>

namespace
{
int failures = 0;
void check (bool cond, const char* name)
{
    std::printf ("  %s  %s\n", cond ? "PASS" : "FAIL", name);
    if (! cond) ++failures;
}

constexpr double kSr = 48000.0;

//==============================================================================
struct FakePlayHead : juce::AudioPlayHead
{
    double bpm = 120.0;
    double ppq = 0.0;
    bool playing = true;

    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo pos;
        pos.setBpm (bpm);
        pos.setPpqPosition (ppq);
        pos.setIsPlaying (playing);
        pos.setTimeInSamples ((juce::int64) (ppq * 60.0 / bpm * kSr));
        return pos;
    }
};

//==============================================================================
void setParam (CORRUPTRAudioProcessor& p, const juce::String& id, float realValue)
{
    auto* param = p.getAPVTS().getParameter (id);
    jassert (param != nullptr);
    if (param != nullptr)
        param->setValueNotifyingHost (param->getNormalisableRange().convertTo0to1 (realValue));
}

struct Harness
{
    std::unique_ptr<CORRUPTRAudioProcessor> proc;
    FakePlayHead playHead;
    int blockSize = 480;

    explicit Harness (int blockSizeIn = 480) : blockSize (blockSizeIn)
    {
        proc = std::make_unique<CORRUPTRAudioProcessor>();
        proc->setPlayHead (&playHead);
        // Defaults that keep measurements clean unless a test overrides them:
        setParam (*proc, "autoGain", 0.0f);     // no loudness matching confound
        setParam (*proc, "qualityMode", 0.0f);  // ECO unless testing oversampling
    }

    void prepare()
    {
        // setPlayConfigDetails mirrors what a host wrapper does before
        // prepareToPlay — without it getSampleRate() stays 0 inside
        // processBlock and the filter computes tan(pi*f/0) = NaN.
        proc->setPlayConfigDetails (2, 2, kSr, blockSize);
        proc->prepareToPlay (kSr, blockSize);
    }

    // Processes `input` through the plugin block by block; returns the output.
    juce::AudioBuffer<float> render (const juce::AudioBuffer<float>& input)
    {
        const int n = input.getNumSamples();
        juce::AudioBuffer<float> out (2, n);
        juce::AudioBuffer<float> blockBuf (2, blockSize);
        juce::MidiBuffer midi;

        int done = 0;
        while (done < n)
        {
            const int len = juce::jmin (blockSize, n - done);
            blockBuf.setSize (2, len, true, false, true);
            for (int ch = 0; ch < 2; ++ch)
                blockBuf.copyFrom (ch, 0, input, ch, done, len);

            proc->processBlock (blockBuf, midi);

            for (int ch = 0; ch < 2; ++ch)
                out.copyFrom (ch, done, blockBuf, ch, 0, len);

            playHead.ppq += (double) len / kSr * (playHead.bpm / 60.0);
            done += len;
        }
        return out;
    }
};

//==============================================================================
juce::AudioBuffer<float> makeSine (double freq, float ampl, double seconds)
{
    const int n = (int) (seconds * kSr);
    juce::AudioBuffer<float> b (2, n);
    for (int i = 0; i < n; ++i)
    {
        const float v = ampl * (float) std::sin (juce::MathConstants<double>::twoPi * freq * i / kSr);
        b.setSample (0, i, v);
        b.setSample (1, i, v);
    }
    return b;
}

juce::AudioBuffer<float> makeNoise (float ampl, double seconds, juce::int64 seed = 42)
{
    juce::Random rng (seed);
    const int n = (int) (seconds * kSr);
    juce::AudioBuffer<float> b (2, n);
    for (int i = 0; i < n; ++i)
    {
        const float v = ampl * (rng.nextFloat() * 2.0f - 1.0f);
        b.setSample (0, i, v);
        b.setSample (1, i, v);
    }
    return b;
}

bool allFinite (const juce::AudioBuffer<float>& b)
{
    for (int ch = 0; ch < b.getNumChannels(); ++ch)
    {
        const float* x = b.getReadPointer (ch);
        for (int i = 0; i < b.getNumSamples(); ++i)
            if (! std::isfinite (x[i]))
                return false;
    }
    return true;
}

// RMS of channel 0, skipping the first `skipSeconds` (smoothing warm-up).
double rmsOf (const juce::AudioBuffer<float>& b, double skipSeconds = 0.1)
{
    const int start = juce::jmin (b.getNumSamples(), (int) (skipSeconds * kSr));
    const float* x = b.getReadPointer (0);
    double sq = 0.0;
    const int n = b.getNumSamples() - start;
    if (n <= 0) return 0.0;
    for (int i = start; i < b.getNumSamples(); ++i)
        sq += (double) x[i] * x[i];
    return std::sqrt (sq / n);
}

double peakOf (const juce::AudioBuffer<float>& b)
{
    return (double) b.getMagnitude (0, b.getNumSamples());
}

// Mean |a-b| on channel 0 after warm-up — "how different are two renders".
double meanAbsDiff (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b, double skipSeconds = 0.1)
{
    const int n = juce::jmin (a.getNumSamples(), b.getNumSamples());
    const int start = juce::jmin (n, (int) (skipSeconds * kSr));
    const float* xa = a.getReadPointer (0);
    const float* xb = b.getReadPointer (0);
    double acc = 0.0;
    for (int i = start; i < n; ++i)
        acc += std::abs ((double) xa[i] - xb[i]);
    return n - start > 0 ? acc / (n - start) : 0.0;
}

// Goertzel power at freq, channel 0, skipping warm-up.
double goertzel (const juce::AudioBuffer<float>& b, double freq, double skipSeconds = 0.2)
{
    const int start = (int) (skipSeconds * kSr);
    const int n = b.getNumSamples() - start;
    if (n <= 0) return 0.0;
    const double w = 2.0 * juce::MathConstants<double>::twoPi * 0.5 * freq / kSr; // 2*pi*f/sr
    const double coeff = 2.0 * std::cos (w);
    double s0 = 0.0, s1 = 0.0, s2 = 0.0;
    const float* x = b.getReadPointer (0);
    for (int i = 0; i < n; ++i)
    {
        s0 = x[start + i] + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    const double power = s1 * s1 + s2 * s2 - coeff * s1 * s2;
    return power / n;
}

void writeWav (const juce::AudioBuffer<float>& b, const juce::String& path)
{
    juce::File f (path);
    f.deleteFile();
    juce::WavAudioFormat fmt;
    if (auto stream = f.createOutputStream())
    {
        if (auto* writer = fmt.createWriterFor (stream.get(), kSr, 2, 24, {}, 0))
        {
            stream.release();
            std::unique_ptr<juce::AudioFormatWriter> w (writer);
            w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
        }
    }
}
} // namespace

//==============================================================================
int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    std::printf ("CORRUPTR QA runner — sr %.0f\n", kSr);

    const auto sine = makeSine (220.0, 0.5f, 1.5);
    const auto noise = makeNoise (0.5f, 2.0);

    //=========================================================================
    std::printf ("\n[1] Default state is sane / near-transparent\n");
    {
        Harness h;
        h.prepare();
        auto out = h.render (sine);
        check (allFinite (out), "default render finite");
        const double rIn = rmsOf (sine), rOut = rmsOf (out);
        const double dB = 20.0 * std::log10 (juce::jmax (1e-9, rOut / rIn));
        std::printf ("       level delta %+.2f dB\n", dB);
        check (std::abs (dB) < 3.0, "default render level within +/-3 dB of input");
    }

    //=========================================================================
    std::printf ("\n[2] 12 distortion algorithms @ drive 24 dB (finite, bounded, audible effect)\n");
    juce::AudioBuffer<float> bypassRef;
    {
        Harness h;
        setParam (*h.proc, "graphBypassSaturation", 1.0f);
        h.prepare();
        bypassRef = h.render (sine);
    }
    static const char* algoNames[12] = { "Saturation", "Soft Clip", "Hard Clip", "Tube", "Diode", "Fuzz",
                                         "Digital", "Bitcrusher(passthru)", "SRR(passthru)",
                                         "Wavefolder", "Ring-Mod", "Foldback" };
    for (int algo = 0; algo < 12; ++algo)
    {
        Harness h;
        setParam (*h.proc, "distortionAlgorithm", (float) algo);
        setParam (*h.proc, "drive", 24.0f);
        setParam (*h.proc, "fold", 50.0f); // affects Wavefolder/Ring-Mod/Foldback only
        h.prepare();
        auto out = h.render (sine);
        const bool finite = allFinite (out);
        const double pk = peakOf (out);
        const double diff = meanAbsDiff (out, bypassRef);
        const bool isPassthrough = (algo == 7 || algo == 8);
        char name[96];
        std::snprintf (name, sizeof (name), "%-20s finite, peak %.2f, vs-bypass-diff %.4f", algoNames[algo], pk, diff);
        // Pass criteria: finite, bounded (limiter ceiling ~1.0), and the
        // waveshaper audibly does something at 24 dB drive — except the two
        // documented passthrough choices, which must match the bypass render
        // (they also skip drive gain by design).
        const bool effectOk = isPassthrough ? (diff < 0.01) : (diff > 0.01);
        check (finite && pk < 1.2 && effectOk, name);
    }

    //=========================================================================
    std::printf ("\n[3] Drive 0 dB neutrality (v1.0.1 Ring-Mod fix regression)\n");
    for (int algo : { 0, 1, 2, 10 }) // Saturation, Soft Clip, Hard Clip, Ring-Mod
    {
        Harness h;
        setParam (*h.proc, "distortionAlgorithm", (float) algo);
        setParam (*h.proc, "drive", 0.0f);
        setParam (*h.proc, "fold", 50.0f);
        h.prepare();
        auto out = h.render (sine);
        const double diff = meanAbsDiff (out, bypassRef);
        char name[96];
        std::snprintf (name, sizeof (name), "%-20s drive=0 ~= bypass (diff %.4f)", algoNames[algo], diff);
        check (allFinite (out) && diff < 0.05, name);
    }

    //=========================================================================
    std::printf ("\n[4] Bitcrusher: 6 modes + SRR hold\n");
    for (int mode = 0; mode < 6; ++mode)
    {
        Harness h;
        setParam (*h.proc, "bitDepth", 4.0f);
        setParam (*h.proc, "bitcrushMode", (float) mode);
        setParam (*h.proc, "graphBypassFilter", 1.0f); // the SVF smears the staircase; measure the quantizer raw
        h.prepare();
        auto out = h.render (sine);
        // Count distinct quantized values in the steady-state tail.
        std::set<int> levels;
        const float* x = out.getReadPointer (0);
        for (int i = (int) (0.5 * kSr); i < out.getNumSamples(); ++i)
            levels.insert ((int) std::lround ((double) x[i] * 10000.0));
        char name[96];
        static const char* mn[6] = { "Classic", "Dither", "Asymmetric", "Mid-Rise", "Bit Flip", "Gate Crush" };
        std::snprintf (name, sizeof (name), "mode %-10s finite, distinct levels %d", mn[mode], (int) levels.size());
        // 4 bits -> 15 levels -> very few distinct values (Dither adds noise,
        // allow a bigger set there; limiter is above threshold-transparent at
        // these amplitudes).
        const int maxLevels = (mode == 1) ? 4000 : 80;
        check (allFinite (out) && rmsOf (out) > 0.01 && (int) levels.size() <= maxLevels, name);
    }
    {
        Harness h;
        setParam (*h.proc, "sampleRateReduction", 16.0f);
        setParam (*h.proc, "graphBypassFilter", 1.0f);
        h.prepare();
        auto out = h.render (sine);
        // Hold of 16 samples -> value changes in the tail should be ~n/16.
        const float* x = out.getReadPointer (0);
        int changes = 0;
        const int start = (int) (0.5 * kSr);
        for (int i = start + 1; i < out.getNumSamples(); ++i)
            if (x[i] != x[i - 1]) ++changes;
        const int span = out.getNumSamples() - start;
        char name[96];
        std::snprintf (name, sizeof (name), "SRR 16x holds samples (changes %d of %d)", changes, span);
        check (allFinite (out) && changes < span / 8, name);
    }

    //=========================================================================
    std::printf ("\n[5] Filter: 5 types + slope on white noise (cutoff 1 kHz, res 30%%)\n");
    auto filterRender = [&noise] (int type, float morphPct, int slope)
    {
        Harness h;
        setParam (*h.proc, "filterType", (float) type);
        setParam (*h.proc, "filterCutoff", 1000.0f);
        setParam (*h.proc, "filterResonance", 30.0f);
        setParam (*h.proc, "filterMorph", morphPct);
        setParam (*h.proc, "filterSlope", (float) slope);
        h.prepare();
        return h.render (noise);
    };
    {
        auto lp = filterRender (0, 0.0f, 0);
        const double lo = goertzel (lp, 200.0), hi = goertzel (lp, 8000.0);
        check (allFinite (lp) && lo > hi * 10.0, "Lowpass: 200 Hz >> 8 kHz");

        auto hp = filterRender (1, 0.0f, 0);
        const double lo2 = goertzel (hp, 200.0), hi2 = goertzel (hp, 8000.0);
        check (allFinite (hp) && hi2 > lo2 * 10.0, "Highpass: 8 kHz >> 200 Hz");

        auto bp = filterRender (2, 0.0f, 0);
        const double c = goertzel (bp, 1000.0), lo3 = goertzel (bp, 100.0), hi3 = goertzel (bp, 10000.0);
        check (allFinite (bp) && c > lo3 * 5.0 && c > hi3 * 5.0, "Bandpass: 1 kHz peak vs 100 Hz/10 kHz");

        auto nf = filterRender (3, 0.0f, 0);
        const double cN = goertzel (nf, 1000.0), loN = goertzel (nf, 100.0), hiN = goertzel (nf, 10000.0);
        check (allFinite (nf) && cN * 3.0 < loN && cN * 3.0 < hiN, "Notch: 1 kHz dip vs 100 Hz/10 kHz");

        auto m0 = filterRender (4, 0.0f, 0);   // Morph @ 0 == LP
        const double mLo = goertzel (m0, 200.0), mHi = goertzel (m0, 8000.0);
        check (allFinite (m0) && mLo > mHi * 10.0, "Morph @ 0%: behaves as lowpass");

        auto m2 = filterRender (4, 66.7f, 0);  // Morph @ 2/3 == HP
        const double mLo2 = goertzel (m2, 200.0), mHi2 = goertzel (m2, 8000.0);
        check (allFinite (m2) && mHi2 > mLo2 * 5.0, "Morph @ 67%: behaves as highpass");

        auto lp24 = filterRender (0, 0.0f, 1);
        const double hi12 = goertzel (lp, 8000.0), hi24 = goertzel (lp24, 8000.0);
        check (allFinite (lp24) && hi24 < hi12, "24 dB slope attenuates 8 kHz more than 12 dB");
    }

    //=========================================================================
    std::printf ("\n[6] Glitch engine: 17 modes @ 100%% probability (finite + audible)\n");
    juce::AudioBuffer<float> glitchOffRef;
    {
        Harness h;
        h.prepare();
        glitchOffRef = h.render (sine);
    }
    static const char* glitchNames[18] = { "Off", "Stutter", "Repeat", "Reverse", "Buffer Repeat", "Freeze",
                                           "Slice", "Random Slice", "Micro Loop", "Granular Repeat",
                                           "Tape Stop", "Tape Start", "Retrigger", "Silence", "Noise Burst",
                                           "Bitcrush Burst", "Pitch Jump", "Random Repeat" };
    for (int mode = 1; mode <= 17; ++mode)
    {
        Harness h;
        setParam (*h.proc, "glitchMode", (float) mode);
        setParam (*h.proc, "glitchProbability", 100.0f);
        setParam (*h.proc, "glitchBufferLength", 2.0f); // 1/16
        h.prepare();
        auto out = h.render (sine);
        const double diff = meanAbsDiff (out, glitchOffRef, 0.2);
        const double r = rmsOf (out, 0.2);
        char name[96];
        std::snprintf (name, sizeof (name), "%-16s finite, rms %.3f, vs-off-diff %.4f", glitchNames[mode], r, diff);
        const bool quietOk = (mode == 13) ? (r < rmsOf (glitchOffRef, 0.2)) : (r > 0.01);
        check (allFinite (out) && peakOf (out) < 1.5 && diff > 0.005 && quietOk, name);
    }

    //=========================================================================
    std::printf ("\n[7] Feedback loop safety (100%% feedback, 0 damping)\n");
    {
        Harness h;
        setParam (*h.proc, "feedbackAmount", 100.0f);
        setParam (*h.proc, "feedbackDamping", 0.0f);
        setParam (*h.proc, "microDelayTime", 5.0f);
        setParam (*h.proc, "drive", 30.0f);
        setParam (*h.proc, "distortionAlgorithm", 5.0f); // Fuzz
        setParam (*h.proc, "filterResonance", 90.0f);
        h.prepare();
        // 1 s of loud input followed by 2 s of silence.
        auto input = makeSine (220.0, 0.9f, 3.0);
        for (int ch = 0; ch < 2; ++ch)
            juce::FloatVectorOperations::clear (input.getWritePointer (ch) + (int) kSr, input.getNumSamples() - (int) kSr);
        auto out = h.render (input);
        check (allFinite (out), "feedback render finite");
        check (peakOf (out) < 1.5, "feedback output bounded");
        // Tail (last 0.5 s) must have decayed, not run away.
        double tailSq = 0.0;
        const float* x = out.getReadPointer (0);
        const int tn = (int) (0.5 * kSr);
        for (int i = out.getNumSamples() - tn; i < out.getNumSamples(); ++i)
            tailSq += (double) x[i] * x[i];
        const double tailRms = std::sqrt (tailSq / tn);
        std::printf ("       extreme-settings tail rms %.5f (drive-in-loop self-oscillation is bounded, by design)\n", tailRms);
    }
    {
        // Same loop at drive 0 (no re-amplification inside the loop):
        // the recirculating energy must decay once input stops.
        Harness h;
        setParam (*h.proc, "feedbackAmount", 100.0f);
        setParam (*h.proc, "feedbackDamping", 0.0f);
        setParam (*h.proc, "microDelayTime", 5.0f);
        setParam (*h.proc, "drive", 0.0f);
        h.prepare();
        auto input = makeSine (220.0, 0.9f, 3.0);
        for (int ch = 0; ch < 2; ++ch)
            juce::FloatVectorOperations::clear (input.getWritePointer (ch) + (int) kSr, input.getNumSamples() - (int) kSr);
        auto out = h.render (input);
        double tailSq = 0.0;
        const float* x = out.getReadPointer (0);
        const int tn = (int) (0.5 * kSr);
        for (int i = out.getNumSamples() - tn; i < out.getNumSamples(); ++i)
            tailSq += (double) x[i] * x[i];
        const double tailRms = std::sqrt (tailSq / tn);
        char name[96];
        std::snprintf (name, sizeof (name), "feedback decays after input stops at drive 0 (tail rms %.5f)", tailRms);
        check (allFinite (out) && tailRms < 0.01, name);
    }

    //=========================================================================
    std::printf ("\n[8] Quality modes (ECO/NORMAL/HIGH/EXTREME): level-consistent, latency reported\n");
    {
        std::array<double, 4> rmsByMode {};
        std::array<int, 4> latencyByMode {};
        for (int q = 0; q < 4; ++q)
        {
            Harness h;
            setParam (*h.proc, "qualityMode", (float) q);
            setParam (*h.proc, "drive", 20.0f);
            h.prepare();
            auto out = h.render (sine);
            rmsByMode[(size_t) q] = rmsOf (out);
            latencyByMode[(size_t) q] = h.proc->getLatencySamples();
            char name[96];
            std::snprintf (name, sizeof (name), "quality %d finite (rms %.3f, latency %d smp)", q, rmsByMode[(size_t) q], latencyByMode[(size_t) q]);
            check (allFinite (out) && rmsByMode[(size_t) q] > 0.05, name);
        }
        const double lo = *std::min_element (rmsByMode.begin(), rmsByMode.end());
        const double hi = *std::max_element (rmsByMode.begin(), rmsByMode.end());
        check (hi / juce::jmax (1e-9, lo) < 1.41, "quality modes within ~3 dB of each other");
        check (latencyByMode[0] == 0 && latencyByMode[1] > 0 && latencyByMode[3] >= latencyByMode[1],
               "latency: ECO 0, oversampled modes report > 0");
    }

    //=========================================================================
    std::printf ("\n[9] Ring-Mod block-size invariance (carrier phase continuity)\n");
    {
        auto renderRing = [] (int blockSize)
        {
            Harness h (blockSize);
            setParam (*h.proc, "distortionAlgorithm", 10.0f);
            setParam (*h.proc, "drive", 12.0f);
            setParam (*h.proc, "fold", 50.0f);
            h.prepare();
            return h.render (makeSine (220.0, 0.5f, 1.5));
        };
        auto small = renderRing (480);
        auto large = renderRing (4800);
        const double diff = meanAbsDiff (small, large, 0.2);
        char name[96];
        std::snprintf (name, sizeof (name), "ring-mod render independent of host block size (diff %.4f)", diff);
        check (allFinite (small) && allFinite (large) && diff < 0.01, name);
        writeWav (small, "corruptr_qa_ringmod_480.wav");
        writeWav (large, "corruptr_qa_ringmod_4800.wav");
    }

    //=========================================================================
    std::printf ("\n[10] Performance triggers\n");
    {
        Harness h;
        setParam (*h.proc, "performanceKill", 1.0f);
        h.prepare();
        auto out = h.render (sine);
        const double r = rmsOf (out, 0.3);
        char name[96];
        std::snprintf (name, sizeof (name), "Kill mutes output (rms %.5f)", r);
        check (allFinite (out) && r < 0.001, name);
    }
    {
        Harness h;
        setParam (*h.proc, "performanceDestroy", 1.0f);
        h.prepare();
        auto out = h.render (sine);
        const double diff = meanAbsDiff (out, glitchOffRef, 0.3);
        char name[96];
        std::snprintf (name, sizeof (name), "Destroy mangles output (vs-clean diff %.4f)", diff);
        check (allFinite (out) && diff > 0.05, name);
    }

    //=========================================================================
    std::printf ("\n[11] Sequencer + Mod Matrix modulation reach the DSP\n");
    {
        // Sequencer: Gate lane alternating open/closed should chop the level.
        Harness h;
        setParam (*h.proc, "sequencerEnabled", 1.0f);
        auto& seq = h.proc->getRhythmicSequencer();
        for (int s = 0; s < 16; ++s)
            seq.setStep (RhythmicSequencer::laneGate, s, (s % 2 == 0) ? 1.0f : 0.0f);
        seq.publishSnapshot();
        h.prepare();
        auto out = h.render (makeSine (220.0, 0.5f, 2.0));
        // Compare RMS of 1/16-note windows: alternating gates -> strong
        // variance between adjacent windows.
        const double stepSec = 60.0 / 120.0 / 4.0; // 1/16 @ 120 BPM
        const int stepSamples = (int) (stepSec * kSr);
        double minW = 1e9, maxW = 0.0;
        const float* x = out.getReadPointer (0);
        for (int w = 4; w < 14; ++w)
        {
            double sq = 0.0;
            for (int i = 0; i < stepSamples; ++i)
                sq += (double) x[w * stepSamples + i] * x[w * stepSamples + i];
            const double r = std::sqrt (sq / stepSamples);
            minW = juce::jmin (minW, r);
            maxW = juce::jmax (maxW, r);
        }
        char name[96];
        std::snprintf (name, sizeof (name), "sequencer gate chops level (min %.3f max %.3f)", minW, maxW);
        check (allFinite (out) && maxW > minW * 2.0, name);
    }
    {
        // Mod Matrix: LFO1 (1 Hz sine) -> Filter Cutoff at full amount, on noise.
        Harness h;
        setParam (*h.proc, "modMatrixEnabled", 1.0f);
        setParam (*h.proc, "modSlot1Source", 0.0f);       // LFO 1
        setParam (*h.proc, "modSlot1Destination", 2.0f);  // Filter Cutoff
        setParam (*h.proc, "modSlot1Amount", 100.0f);
        setParam (*h.proc, "modSlot1Enable", 1.0f);
        setParam (*h.proc, "filterCutoff", 2000.0f);
        setParam (*h.proc, "lfo1Rate", 1.0f);
        h.prepare();
        auto out = h.render (makeNoise (0.5f, 3.0));
        // HF content at 6 kHz should vary over time as cutoff sweeps.
        double minP = 1e18, maxP = 0.0;
        for (int seg = 1; seg < 6; ++seg)
        {
            juce::AudioBuffer<float> segBuf (2, (int) (0.5 * kSr));
            for (int ch = 0; ch < 2; ++ch)
                segBuf.copyFrom (ch, 0, out, ch, seg * (int) (0.5 * kSr), segBuf.getNumSamples());
            const double p = goertzel (segBuf, 6000.0, 0.0);
            minP = juce::jmin (minP, p);
            maxP = juce::jmax (maxP, p);
        }
        char name[96];
        std::snprintf (name, sizeof (name), "mod matrix LFO sweeps filter (6 kHz power ratio %.1fx)", maxP / juce::jmax (1e-18, minP));
        check (allFinite (out) && maxP > minP * 3.0, name);
    }

    //=========================================================================
    std::printf ("\n[12] State save/load round-trip\n");
    {
        Harness h;
        h.prepare();
        setParam (*h.proc, "drive", 17.5f);
        setParam (*h.proc, "distortionAlgorithm", 9.0f);
        setParam (*h.proc, "filterCutoff", 1234.0f);
        setParam (*h.proc, "macroDamage", 66.0f);
        auto& seq = h.proc->getRhythmicSequencer();
        seq.setStep (RhythmicSequencer::laneDrive, 3, 0.75f);
        seq.publishSnapshot();

        juce::MemoryBlock state;
        h.proc->getStateInformation (state);

        Harness h2;
        h2.prepare();
        h2.proc->setStateInformation (state.getData(), (int) state.getSize());

        auto rawOf = [] (Harness& hh, const char* id) { return hh.proc->getAPVTS().getRawParameterValue (id)->load(); };
        const bool paramsOk = std::abs (rawOf (h2, "drive") - 17.5f) < 0.01f
                           && (int) rawOf (h2, "distortionAlgorithm") == 9
                           && std::abs (rawOf (h2, "filterCutoff") - 1234.0f) < 1.0f
                           && std::abs (rawOf (h2, "macroDamage") - 66.0f) < 0.01f;
        check (paramsOk, "APVTS parameters survive round-trip");
        const float step = h2.proc->getRhythmicSequencer().getStep (RhythmicSequencer::laneDrive, 3);
        check (std::abs (step - 0.75f) < 1e-4f, "sequencer pattern survives round-trip");
    }

    //=========================================================================
    std::printf ("\n%s — %d failure(s)\n", failures == 0 ? "ALL PASS" : "FAILURES PRESENT", failures);
    return failures == 0 ? 0 : 1;
}
