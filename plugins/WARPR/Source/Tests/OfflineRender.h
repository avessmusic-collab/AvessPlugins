#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

namespace warpr::tests
{
    /** Renders `seconds` of audio from `processor` offline.

        House lesson (console harness): setPlayConfigDetails() is called BEFORE prepareToPlay(),
        otherwise getSampleRate() == 0 inside the processor -> NaN -> silent renders.

        `midi` holds the events to play; their sample positions are absolute from the start of
        the render (they are split across blocks with the correct per-block offset).
        The output has one channel per processor output channel (stereo for WARPR). */
    juce::AudioBuffer<float> render (juce::AudioProcessor& processor,
                                     const juce::MidiBuffer& midi,
                                     double sampleRate,
                                     int blockSize,
                                     double seconds);

    /** Convenience: a single note-on at sample 0, note-off after `noteSeconds`. */
    juce::MidiBuffer singleNote (int midiNote, float velocity, double sampleRate, double noteSeconds);

    /** Writes a 24-bit WAV for manual inspection.  Returns false on failure. */
    bool writeWav (const juce::AudioBuffer<float>& audio, const juce::File& file, double sampleRate);
}
