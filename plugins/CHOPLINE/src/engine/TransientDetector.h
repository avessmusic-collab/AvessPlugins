// CHOPLINE — TransientDetector (spec §4)
// Spectral-flux onset detection tuned for vocals (syllable onsets).
//
// Algorithm: the sample is mixed to mono and analysed in 1024-sample Hann
// frames with a 256-sample hop. Per frame, the spectral flux is the sum over
// all bins of the half-wave-rectified increase in log-compressed magnitude
// (log1p) versus the previous frame — log compression makes quiet syllable
// onsets count as much as loud ones, which suits vocals. The flux envelope
// is lightly smoothed and normalised once per load; picking onsets for a
// given Sensitivity is then a cheap peak-picking pass over that envelope
// (local maximum above an adaptive local-mean threshold, minimum gap
// enforced by keeping the stronger peak), so the SENS knob can re-slice
// live on the message thread without re-running the FFT.

#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <vector>

namespace chopline::engine
{

class TransientDetector
{
public:
    struct Analysis
    {
        std::vector<float> envelope; // normalised 0..1 onset strength per hop
        double hopSeconds = 0.0;
        double frameSeconds = 0.0;
    };

    static constexpr int frameSize = 1024;
    static constexpr int hopSize = 256;

    /// Heavy part (FFT sweep). Run once per load on the background thread.
    static Analysis analyze (const juce::AudioBuffer<float>& audio, double sampleRate);

    /// Cheap part: pick onset times (seconds) for a sensitivity in [0, 1]
    /// (1 = most slices) with a minimum gap between onsets. Deterministic.
    static std::vector<double> pickOnsets (const Analysis& analysis,
                                           float sensitivity01,
                                           double minGapSeconds);

    /// Rough tempo estimate from onset times via a folded inter-onset-interval
    /// histogram (70–180 BPM). Returns fallback when there are too few onsets.
    static double estimateBpm (const std::vector<double>& onsetTimes,
                               double fallbackBpm = 120.0);
};

} // namespace chopline::engine
