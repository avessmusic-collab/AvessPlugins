#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>

/**
    Headless offline-render utility for KickDesigner2 (architecture.md -> Testing).

    Stage 1 scaffold: the host loop below is fully functional (prepare -> note-on at
    sample 0 -> render N seconds -> release). Until the engine exists (Stage 2) it
    simply captures silence. Later phases use it for deterministic WAV inspection and
    envelope-shape assertions.
*/
namespace kd2::tests
{
    /** Render `seconds` of `processor` output following a single MIDI note-on at sample 0. */
    juce::AudioBuffer<float> renderNote (juce::AudioProcessor& processor,
                                         int    midiNoteNumber,
                                         float  velocity,
                                         double sampleRate,
                                         int    blockSize,
                                         double seconds);

    /** As renderNote(), then write a 24-bit WAV to `outputFile`. Returns true on success. */
    bool renderNoteToWav (juce::AudioProcessor& processor,
                          const juce::File&     outputFile,
                          int    midiNoteNumber,
                          float  velocity,
                          double sampleRate,
                          int    blockSize,
                          double seconds);
}
