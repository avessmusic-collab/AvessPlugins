#pragma once
#include <juce_dsp/juce_dsp.h>
#include <array>

/**
 * AudioAnalyzer - lightweight, real-time-safe source characterization.
 *
 * See plugins/DROP/.ideas/architecture.md > Core Components > AudioAnalyzer
 * and its "AudioAnalyzer FFT sizing" Algorithm Details section, and
 * plugins/DROP/.ideas/implementation-spec.md > AUDIOANALYZER /
 * SOURCE-AWARE PROCESSING.
 *
 * Runs on a PARALLEL, READ-ONLY tap of the dry pre-chain signal
 * (architecture.md Processing Order step 2 - "runs in parallel off the same
 * pre-chain signal... never blocking or altering the main signal path").
 * RMS/peak/transient-energy are cheap sample-domain envelope followers,
 * updated every sample. Spectral centroid + low/mid/high band energy are
 * computed via a 512-point juce::dsp::FFT (Hann-windowed), but only once per
 * 512-sample HOP - an internal accumulation ring buffer decouples this hop
 * from the host's actual block size (which may be 1, 32, 512, 8192...
 * samples per processBlock() call), per architecture.md's "AudioAnalyzer FFT
 * sizing" section: "the analyzer simply appends incoming samples to its own
 * ring accumulator and fires whenever it has a full 512-sample window,
 * independent of how many host processBlock() calls that took." Stereo
 * correlation is a running normalized cross-correlation over the SAME
 * 512-sample analysis window, reset every hop.
 *
 * SourceProfile classification (Vocal/Drums/Bass/FullMix) is a coarse,
 * explicit weighted-scoring heuristic (NOT ML, per implementation-spec.md
 * "Avoid ML in MVP") combining spectral centroid, transient density, and
 * band-energy ratios, re-evaluated once per FFT hop (architecture.md: "a
 * simple weighted scoring function").
 *
 * RT-safe: juce::dsp::FFT + all scratch buffers preallocated in prepare();
 * analyze() performs zero allocation. Denormal-safe (architecture.md Denormal
 * Protection): running accumulators are explicitly hard-reset (not left to
 * decay toward subnormal values) whenever an entire hop window was silent.
 */
namespace drop
{

enum class SourceProfile
{
    Vocal = 0,
    Drums,
    Bass,
    FullMix,
    Count
};

struct AudioAnalyzerMetrics
{
    float rms = 0.0f;
    float peak = 0.0f;
    float transientEnergy = 0.0f;   // positive difference of fast-minus-slow envelope followers (architecture.md's dual-envelope transient detector)
    float spectralCentroidHz = 0.0f;
    float lowEnergy01 = 0.0f;       // fraction of total FFT-bin energy below 150Hz
    float midEnergy01 = 0.0f;       // fraction of total FFT-bin energy 150Hz-2kHz
    float highEnergy01 = 0.0f;      // fraction of total FFT-bin energy above 2kHz
    float correlation = 1.0f;       // -1..1, running normalized L/R cross-correlation over the last completed hop window
};

class AudioAnalyzer
{
public:
    void prepare(const juce::dsp::ProcessSpec& spec);
    void reset();

    /**
     * Analyzes `buffer`'s first `numSamples` samples (READ-ONLY - never
     * modifies `buffer`). Call ONCE per host processBlock(), on the
     * pre-chain dry signal, BEFORE any DSP module touches the buffer
     * (architecture.md Processing Order step 1-2).
     */
    void analyze(const juce::AudioBuffer<float>& buffer, int numSamples) noexcept;

    const AudioAnalyzerMetrics& getMetrics() const noexcept { return metrics; }
    SourceProfile getSourceProfile() const noexcept { return sourceProfile; }

    static constexpr int kFftOrder = 9;
    static constexpr int kFftSize = 1 << kFftOrder; // 512

private:
    void runHop() noexcept;
    void classifySource() noexcept;

    juce::dsp::FFT fft { kFftOrder };
    juce::dsp::WindowingFunction<float> window { (size_t) kFftSize, juce::dsp::WindowingFunction<float>::hann };

    std::array<float, (size_t) kFftSize> hopAccum {};
    std::array<float, (size_t) (kFftSize * 2)> fftScratch {};
    int hopWriteIndex = 0;

    double sampleRate = 44100.0;
    AudioAnalyzerMetrics metrics;
    SourceProfile sourceProfile = SourceProfile::FullMix;

    float fastEnv = 0.0f, slowEnv = 0.0f;
    float fastAttackCoeff = 0.0f, fastReleaseCoeff = 0.0f;
    float slowAttackCoeff = 0.0f, slowReleaseCoeff = 0.0f;

    float rmsState = 0.0f;
    float rmsCoeff = 0.0f;

    float peakState = 0.0f;
    float peakDecayCoeff = 0.0f;

    double corrSumLR = 0.0, corrSumLL = 0.0, corrSumRR = 0.0;

    int silentSamplesInHop = 0; // count of near-silent samples within the CURRENTLY-accumulating hop

    static constexpr float kSilenceThreshold = 1.0e-6f;
};

} // namespace drop
