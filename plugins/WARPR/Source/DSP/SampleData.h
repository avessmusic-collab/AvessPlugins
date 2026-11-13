#pragma once
/*
  SampleData - immutable, ref-counted sample (architecture C1).

  Built OFF the audio thread (loader thread / message thread) and published through
  SampleStore -> LockFreeSwap.  Never modified after creation, so the audio thread can read
  it without synchronisation.

  Layout: planar float, one `Level` per octave of the mip pyramid.  Level 0 is the source;
  level k is the source band-limited to 0.5 / 2^k cycles per source frame and decimated by 2^k
  (see MipBuilder.h).  Every channel carries `pad` guard frames on each side so an 8-tap
  interpolation window never needs bounds checks: the guard is zero, or - for periodic
  (single-cycle) data - a circular copy of the opposite end.

  `numChannels` is 1 or 2.  A mono sample is stored once and duplicated by the reader, so a
  mono sample is bit-identical in L and R by construction.
*/

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <vector>

namespace warpr
{
    class SampleData : public juce::ReferenceCountedObject
    {
    public:
        using Ptr = juce::ReferenceCountedObjectPtr<SampleData>;

        static constexpr int pad            = 8;
        static constexpr int maxLevels      = 9;       // level 0 + up to 8 octave decimations
        static constexpr int maxPeakPairs   = 2048;
        static constexpr int maxChannels    = 2;
        static constexpr int defaultWaveFrames = 2048;

        struct Level
        {
            int numFrames = 0;
            int stride    = 0;                          // numFrames + 2 * pad
            std::vector<float> storage;                 // channel c starts at storage[c * stride]

            const float* channel (int c) const noexcept
            {
                return storage.data() + (size_t) c * (size_t) stride + (size_t) pad;
            }

            float* channel (int c) noexcept
            {
                return storage.data() + (size_t) c * (size_t) stride + (size_t) pad;
            }
        };

        SampleData() = default;

        /** Process-unique build serial (assigned by create()).  State capture keys its encoded-blob cache on it:
            unlike the object address it can never be reused by a later sample. */
        juce::uint64 serial = 0;

        juce::String name;
        double fileSampleRate = 44100.0;
        int numChannels = 1;
        int numFrames   = 0;
        bool isDefaultWave = false;
        bool singleCycle   = false;                     // periodic data: guards wrap, full-region loops are seamless
        int numLevels   = 1;
        std::array<Level, (size_t) maxLevels> levels;
        std::vector<float> peaksMinMax;                 // [min0, max0, min1, max1, ...] mono mix, <= 2048 pairs

        const Level& level (int k) const noexcept { return levels[(size_t) k]; }

        /** Builds a sample (non-finite values are scrubbed to 0, > 2 channels -> first two).
            Returns nullptr for empty input or an invalid sample rate.  `periodic` marks single-cycle
            data (circular guards and mips). */
        static Ptr create (const juce::AudioBuffer<float>& source, double sampleRate,
                           const juce::String& sampleName, bool periodic, bool defaultWave);

        /** 2048-frame additive saw (64 harmonics, 1/n, peak 0.9), periodic.
            fileSampleRate = 2048 * midiToHz(60) so rootNote 60 maps one cycle to one note period. */
        static Ptr createDefaultWave();
    };
}
