#include "Tests/OfflineRender.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <cmath>
#include <memory>

namespace warpr::tests
{
    juce::AudioBuffer<float> render (juce::AudioProcessor& processor,
                                     const juce::MidiBuffer& midi,
                                     double sampleRate,
                                     int blockSize,
                                     double seconds,
                                     bool pinGateDefault)
    {
        const int maxBlock     = juce::jmax (1, blockSize);
        const int totalSamples = juce::jmax (1, static_cast<int> (std::ceil (sampleRate * seconds)));

        // Instrument: no inputs.  setPlayConfigDetails MUST precede prepareToPlay.
        processor.setPlayConfigDetails (0, 2, sampleRate, maxBlock);
        processor.prepareToPlay (sampleRate, maxBlock);

        if (pinGateDefault)
            pinForwardLoop (processor);          // the corpus assumes GATED playback (plugin default: One-Shot)

        const int numChannels = juce::jmax (1, processor.getTotalNumOutputChannels());

        juce::AudioBuffer<float> output (numChannels, totalSamples);
        output.clear();

        juce::AudioBuffer<float> scratch (numChannels, maxBlock);
        juce::MidiBuffer blockMidi;

        for (int pos = 0; pos < totalSamples;)
        {
            const int numThisBlock = juce::jmin (maxBlock, totalSamples - pos);

            blockMidi.clear();
            for (const auto metadata : midi)
            {
                const int at = metadata.samplePosition;
                if (at >= pos && at < pos + numThisBlock)
                    blockMidi.addEvent (metadata.getMessage(), at - pos);
            }

            juce::AudioBuffer<float> block (scratch.getArrayOfWritePointers(), numChannels, numThisBlock);
            block.clear();

            processor.processBlock (block, blockMidi);

            for (int ch = 0; ch < numChannels; ++ch)
                output.copyFrom (ch, pos, block, ch, 0, numThisBlock);

            pos += numThisBlock;
        }

        processor.releaseResources();
        return output;
    }

    juce::MidiBuffer singleNote (int midiNote, float velocity, double sampleRate, double noteSeconds)
    {
        juce::MidiBuffer midi;
        midi.addEvent (juce::MidiMessage::noteOn (1, midiNote, velocity), 0);
        midi.addEvent (juce::MidiMessage::noteOff (1, midiNote), static_cast<int> (sampleRate * noteSeconds));
        return midi;
    }

    bool writeWav (const juce::AudioBuffer<float>& audio, const juce::File& file, double sampleRate)
    {
        file.deleteFile();
        file.getParentDirectory().createDirectory();

        std::unique_ptr<juce::OutputStream> stream = file.createOutputStream();
        if (stream == nullptr)
            return false;

        juce::WavAudioFormat format;
        auto writer = format.createWriterFor (stream,
                                              juce::AudioFormatWriterOptions{}
                                                  .withSampleRate (sampleRate)
                                                  .withNumChannels (audio.getNumChannels())
                                                  .withBitsPerSample (24));
        if (writer == nullptr)
            return false;

        // On success createWriterFor transferred ownership of the stream to the writer.
        return writer->writeFromAudioSampleBuffer (audio, 0, audio.getNumSamples());
    }
}
