#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

namespace kickr
{
    /**
        One decoded user sample, owned by the processor (message thread), handed to the
        audio thread by a bare `const SampleBuffer*` (see PluginProcessor + SamplePlayer,
        architecture.md AD-11).

        - `audio`      : the whole file decoded to float (<= 5 s, <= 2 channels).
        - `sourceRate` : the file's native sample rate — SamplePlayer does the rate
                         conversion to `fsOversampled`, it never resamples the file here.
        - `rootNote`   : MIDI note the sample is considered to be "at" for `sampleMidiTrack`
                         — the note that plays it back unpitched (fixed at C3 = 60, MIDI's
                         middle C and Ableton's default note name for it, in v1; a
                         per-sample root is deferred).

        Shared header so SamplePlayer and PluginProcessor both see the exact type.
    */
    struct SampleBuffer
    {
        juce::AudioBuffer<float> audio;
        double                   sourceRate { 44100.0 };
        int                      rootNote   { 60 };   // C3 (MIDI 60) — unpitched trigger note
    };

    /** Hard length cap for an importable / decodable sample (architecture.md AD-11). */
    inline constexpr double kMaxSampleSeconds = 5.0;
}
