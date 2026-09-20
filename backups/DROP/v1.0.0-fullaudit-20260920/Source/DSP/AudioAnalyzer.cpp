#include "AudioAnalyzer.h"
#include <cmath>

namespace drop
{

void AudioAnalyzer::prepare(const juce::dsp::ProcessSpec& spec)
{
    sampleRate = spec.sampleRate > 0.0 ? spec.sampleRate : 44100.0;

    auto makeRiseCoeff = [this](double timeSeconds) noexcept -> float
    {
        return 1.0f - std::exp((float) (-1.0 / (juce::jmax(1.0e-6, timeSeconds) * sampleRate)));
    };

    // architecture.md AudioAnalyzer config: "fast attack ~1ms/fast release
    // ~10ms envelope minus a slow attack ~50ms/slow release ~200ms envelope,
    // positive difference = transient energy."
    fastAttackCoeff = makeRiseCoeff(0.001);
    fastReleaseCoeff = makeRiseCoeff(0.010);
    slowAttackCoeff = makeRiseCoeff(0.050);
    slowReleaseCoeff = makeRiseCoeff(0.200);

    // architecture.md: "standard running RMS window (e.g. 50ms)."
    rmsCoeff = makeRiseCoeff(0.050);

    // Peak-hold with decay (architecture.md: "peak-hold with decay"), a
    // ~300ms exponential release time constant.
    peakDecayCoeff = std::exp((float) (-1.0 / (0.300 * sampleRate)));

    reset();
}

void AudioAnalyzer::reset()
{
    hopAccum.fill(0.0f);
    fftScratch.fill(0.0f);
    hopWriteIndex = 0;
    metrics = AudioAnalyzerMetrics {};
    sourceProfile = SourceProfile::FullMix;
    fastEnv = slowEnv = 0.0f;
    rmsState = 0.0f;
    peakState = 0.0f;
    corrSumLR = corrSumLL = corrSumRR = 0.0;
    silentSamplesInHop = 0;
}

void AudioAnalyzer::analyze(const juce::AudioBuffer<float>& buffer, int numSamples) noexcept
{
    const int numChannels = buffer.getNumChannels();
    if (numChannels <= 0 || numSamples <= 0)
        return;

    const float* left = buffer.getReadPointer(0);
    const float* right = numChannels > 1 ? buffer.getReadPointer(1) : left;

    for (int i = 0; i < numSamples; ++i)
    {
        const float l = left[i];
        const float r = right[i];
        const float mono = 0.5f * (l + r);
        const float absMono = std::abs(mono);

        fastEnv += (absMono - fastEnv) * ((absMono > fastEnv) ? fastAttackCoeff : fastReleaseCoeff);
        slowEnv += (absMono - slowEnv) * ((absMono > slowEnv) ? slowAttackCoeff : slowReleaseCoeff);
        metrics.transientEnergy = juce::jmax(0.0f, fastEnv - slowEnv);

        rmsState += (mono * mono - rmsState) * rmsCoeff;
        metrics.rms = std::sqrt(juce::jmax(0.0f, rmsState));

        peakState *= peakDecayCoeff;
        peakState = juce::jmax(peakState, absMono);
        metrics.peak = peakState;

        corrSumLR += (double) l * (double) r;
        corrSumLL += (double) l * (double) l;
        corrSumRR += (double) r * (double) r;

        if (absMono < kSilenceThreshold)
            ++silentSamplesInHop;

        hopAccum[(size_t) hopWriteIndex++] = mono;
        if (hopWriteIndex >= kFftSize)
        {
            runHop();
            hopWriteIndex = 0;
        }
    }
}

void AudioAnalyzer::runHop() noexcept
{
    // Denormal/silence protection (architecture.md Denormal Protection):
    // hard-reset the running accumulators instead of letting them decay
    // toward subnormal values whenever the WHOLE hop was silent.
    if (silentSamplesInHop >= kFftSize)
    {
        fastEnv = slowEnv = 0.0f;
        rmsState = 0.0f;
        peakState = 0.0f;
        corrSumLR = corrSumLL = corrSumRR = 0.0;
        metrics.correlation = 1.0f;
        metrics.spectralCentroidHz = 0.0f;
        metrics.lowEnergy01 = metrics.midEnergy01 = metrics.highEnergy01 = 0.0f;
        silentSamplesInHop = 0;
        // Deliberately do NOT reclassify on a silent hop: silence carries no
        // source information, and the zeroed metrics would otherwise score
        // as Bass (centroid 0 -> "(1-centroidNorm)" term dominates). Rhythmic
        // material with gaps (drums!) must not have its classification
        // overwritten by the gaps between hits - hold the last energetic
        // hop's profile instead. (Found via Phase 3.5 classification test:
        // burst gaps misclassified drum loops as Bass.)
        return;
    }
    silentSamplesInHop = 0;

    fftScratch.fill(0.0f);
    for (int i = 0; i < kFftSize; ++i)
        fftScratch[(size_t) i] = hopAccum[(size_t) i];

    window.multiplyWithWindowingTable(fftScratch.data(), (size_t) kFftSize);
    fft.performRealOnlyForwardTransform(fftScratch.data());

    double weightedFreqSum = 0.0, magSum = 0.0;
    double lowSum = 0.0, midSum = 0.0, highSum = 0.0;

    const int numBins = kFftSize / 2;
    for (int k = 0; k < numBins; ++k)
    {
        const float re = fftScratch[(size_t) (2 * k)];
        const float im = fftScratch[(size_t) (2 * k + 1)];
        const double mag = std::sqrt((double) re * (double) re + (double) im * (double) im);
        const double freq = (double) k * sampleRate / (double) kFftSize;

        weightedFreqSum += mag * freq;
        magSum += mag;

        const double energy = mag * mag;
        if (freq < 150.0) lowSum += energy;
        else if (freq < 2000.0) midSum += energy;
        else highSum += energy;
    }

    metrics.spectralCentroidHz = magSum > 1.0e-9 ? (float) (weightedFreqSum / magSum) : 0.0f;

    const double totalEnergy = lowSum + midSum + highSum;
    if (totalEnergy > 1.0e-12)
    {
        metrics.lowEnergy01 = (float) (lowSum / totalEnergy);
        metrics.midEnergy01 = (float) (midSum / totalEnergy);
        metrics.highEnergy01 = (float) (highSum / totalEnergy);
    }
    else
    {
        metrics.lowEnergy01 = metrics.midEnergy01 = metrics.highEnergy01 = 0.0f;
    }

    const double denom = std::sqrt(juce::jmax(1.0e-12, corrSumLL * corrSumRR));
    metrics.correlation = (corrSumLL > 1.0e-12 || corrSumRR > 1.0e-12)
                         ? juce::jlimit(-1.0f, 1.0f, (float) (corrSumLR / denom))
                         : 1.0f;
    // "over the same 512-sample analysis window" (architecture.md) - reset every hop.
    corrSumLR = corrSumLL = corrSumRR = 0.0;

    classifySource();
}

void AudioAnalyzer::classifySource() noexcept
{
    // Coarse, explicit weighted-scoring heuristic (architecture.md: "a
    // simple weighted scoring function (not ML)"). Not tuned against real
    // program material beyond the synthetic vocal-like/drum-like test
    // signals in Source/Tests/RunTests.cpp - directionally correct, not a
    // precision classifier.
    const float centroidNorm = juce::jlimit(0.0f, 1.0f, metrics.spectralCentroidHz / 8000.0f);
    const float transientNorm = juce::jlimit(0.0f, 1.0f, metrics.transientEnergy * 8.0f);
    const float lowFrac = metrics.lowEnergy01;
    const float midFrac = metrics.midEnergy01;
    const float highFrac = metrics.highEnergy01;

    std::array<float, (size_t) SourceProfile::Count> scores {};
    scores[(size_t) SourceProfile::Bass]    = lowFrac * 2.0f + (1.0f - centroidNorm) * 0.7f - transientNorm * 0.3f;
    scores[(size_t) SourceProfile::Drums]   = transientNorm * 2.0f + highFrac * 0.6f;
    scores[(size_t) SourceProfile::Vocal]   = midFrac * 1.5f + (1.0f - std::abs(centroidNorm - 0.35f) * 2.0f) - transientNorm * 0.4f;
    scores[(size_t) SourceProfile::FullMix] = 0.55f; // baseline - wins when nothing else stands out strongly

    int best = 0;
    for (int i = 1; i < (int) SourceProfile::Count; ++i)
        if (scores[(size_t) i] > scores[(size_t) best])
            best = i;

    sourceProfile = (SourceProfile) best;
}

} // namespace drop
