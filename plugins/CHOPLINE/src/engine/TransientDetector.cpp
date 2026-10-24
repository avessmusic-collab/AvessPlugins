// CHOPLINE — TransientDetector implementation. See header for the algorithm.

#include "TransientDetector.h"

#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <cmath>

namespace chopline::engine
{

TransientDetector::Analysis TransientDetector::analyze (const juce::AudioBuffer<float>& audio,
                                                        double sampleRate)
{
    Analysis out;
    out.hopSeconds = sampleRate > 0.0 ? hopSize / sampleRate : 0.0;
    out.frameSeconds = sampleRate > 0.0 ? frameSize / sampleRate : 0.0;

    const auto numSamples = audio.getNumSamples();
    if (sampleRate <= 0.0 || numSamples < frameSize)
        return out;

    // Mono mix.
    std::vector<float> mono (static_cast<size_t> (numSamples), 0.0f);
    const auto numCh = audio.getNumChannels();
    for (int ch = 0; ch < numCh; ++ch)
    {
        const auto* src = audio.getReadPointer (ch);
        for (int i = 0; i < numSamples; ++i)
            mono[static_cast<size_t> (i)] += src[i];
    }
    const auto chGain = numCh > 0 ? 1.0f / static_cast<float> (numCh) : 1.0f;
    for (auto& s : mono)
        s *= chGain;

    // Hann window.
    std::vector<float> window (frameSize);
    for (int i = 0; i < frameSize; ++i)
        window[static_cast<size_t> (i)] =
            0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi
                                    * static_cast<float> (i) / static_cast<float> (frameSize - 1));

    juce::dsp::FFT fft (10); // 2^10 = 1024
    jassert (fft.getSize() == frameSize);

    const int numFrames = 1 + (numSamples - frameSize) / hopSize;
    constexpr int numBins = frameSize / 2;

    std::vector<float> fftData (static_cast<size_t> (frameSize) * 2, 0.0f);
    std::vector<float> prevLogMag (numBins, 0.0f);
    std::vector<float> flux (static_cast<size_t> (numFrames), 0.0f);

    for (int f = 0; f < numFrames; ++f)
    {
        const auto* src = mono.data() + static_cast<size_t> (f) * hopSize;
        std::fill (fftData.begin(), fftData.end(), 0.0f);
        for (int i = 0; i < frameSize; ++i)
            fftData[static_cast<size_t> (i)] = src[i] * window[static_cast<size_t> (i)];

        fft.performRealOnlyForwardTransform (fftData.data(), true);

        float frameFlux = 0.0f;
        for (int b = 1; b < numBins; ++b)
        {
            const auto re = fftData[static_cast<size_t> (b) * 2];
            const auto im = fftData[static_cast<size_t> (b) * 2 + 1];
            const auto logMag = std::log1p (std::sqrt (re * re + im * im));
            const auto diff = logMag - prevLogMag[static_cast<size_t> (b)];
            if (diff > 0.0f)
                frameFlux += diff;
            prevLogMag[static_cast<size_t> (b)] = logMag;
        }
        // The very first frame's flux is just "signal appeared" — keep it, it
        // marks an onset at the start of the file.
        flux[static_cast<size_t> (f)] = frameFlux;
    }

    // Light 3-tap smoothing.
    out.envelope.resize (flux.size(), 0.0f);
    for (size_t i = 0; i < flux.size(); ++i)
    {
        const auto a = i > 0 ? flux[i - 1] : flux[i];
        const auto c = i + 1 < flux.size() ? flux[i + 1] : flux[i];
        out.envelope[i] = 0.25f * a + 0.5f * flux[i] + 0.25f * c;
    }

    // Normalise.
    const auto maxIt = std::max_element (out.envelope.begin(), out.envelope.end());
    if (maxIt != out.envelope.end() && *maxIt > 0.0f)
    {
        const auto inv = 1.0f / *maxIt;
        for (auto& v : out.envelope)
            v *= inv;
    }

    return out;
}

std::vector<double> TransientDetector::pickOnsets (const Analysis& analysis,
                                                   float sensitivity01,
                                                   double minGapSeconds)
{
    std::vector<double> times;
    const auto& env = analysis.envelope;
    if (env.empty() || analysis.hopSeconds <= 0.0)
        return times;

    const auto sens = juce::jlimit (0.0f, 1.0f, sensitivity01);
    // Low sensitivity: high absolute floor + strong local-mean multiplier.
    const auto delta = juce::jmap (sens, 0.0f, 1.0f, 0.30f, 0.015f);
    const auto mult = juce::jmap (sens, 0.0f, 1.0f, 2.50f, 1.05f);

    constexpr int meanHalfWidth = 8; // local mean window: ±8 hops (~±43 ms @48k)
    const auto n = static_cast<int> (env.size());

    std::vector<float> strengths;

    for (int i = 0; i < n; ++i)
    {
        // Local maximum over ±2 hops (strict on the left to split plateaus).
        bool isPeak = true;
        for (int j = juce::jmax (0, i - 2); j <= juce::jmin (n - 1, i + 2); ++j)
        {
            if (j < i && env[static_cast<size_t> (j)] >= env[static_cast<size_t> (i)]) { isPeak = false; break; }
            if (j > i && env[static_cast<size_t> (j)] >  env[static_cast<size_t> (i)]) { isPeak = false; break; }
        }
        if (! isPeak)
            continue;

        float localSum = 0.0f;
        int count = 0;
        for (int j = juce::jmax (0, i - meanHalfWidth); j <= juce::jmin (n - 1, i + meanHalfWidth); ++j)
        {
            localSum += env[static_cast<size_t> (j)];
            ++count;
        }
        const auto localMean = count > 0 ? localSum / static_cast<float> (count) : 0.0f;

        if (env[static_cast<size_t> (i)] <= localMean * mult + delta)
            continue;

        // Window-centre compensation: the flux of frame i (starting at
        // i * hop) peaks when the onset sits near the centre of that frame,
        // so the onset time is roughly frame-start + half a frame.
        const auto t = juce::jmax (0.0, static_cast<double> (i) * analysis.hopSeconds
                                            + analysis.frameSeconds * 0.5);
        const auto strength = env[static_cast<size_t> (i)];

        if (! times.empty() && t - times.back() < minGapSeconds)
        {
            if (strength > strengths.back()) // keep the stronger of the pair
            {
                times.back() = t;
                strengths.back() = strength;
            }
            continue;
        }

        times.push_back (t);
        strengths.push_back (strength);
    }

    return times;
}

double TransientDetector::estimateBpm (const std::vector<double>& onsetTimes, double fallbackBpm)
{
    if (onsetTimes.size() < 3)
        return fallbackBpm;

    // Fold every adjacent inter-onset interval into 70–180 BPM and histogram.
    constexpr int loBpm = 70, hiBpm = 180;
    std::vector<int> histogram (hiBpm - loBpm, 0);
    std::vector<double> sums (hiBpm - loBpm, 0.0);

    for (size_t i = 1; i < onsetTimes.size(); ++i)
    {
        const auto interval = onsetTimes[i] - onsetTimes[i - 1];
        if (interval <= 0.01)
            continue;
        auto bpm = 60.0 / interval;
        while (bpm < loBpm) bpm *= 2.0;
        while (bpm >= hiBpm) bpm *= 0.5;
        if (bpm < loBpm) // pathological interval that cannot fold in
            continue;
        const auto bin = juce::jlimit (0, hiBpm - loBpm - 1, static_cast<int> (bpm) - loBpm);
        ++histogram[static_cast<size_t> (bin)];
        sums[static_cast<size_t> (bin)] += bpm;
    }

    int bestBin = -1, bestCount = 0;
    for (int b = 0; b < hiBpm - loBpm; ++b)
    {
        if (histogram[static_cast<size_t> (b)] > bestCount)
        {
            bestCount = histogram[static_cast<size_t> (b)];
            bestBin = b;
        }
    }

    if (bestBin < 0 || bestCount < 2)
        return fallbackBpm;

    const auto bpm = sums[static_cast<size_t> (bestBin)] / bestCount;
    return std::round (bpm * 10.0) / 10.0;
}

} // namespace chopline::engine
