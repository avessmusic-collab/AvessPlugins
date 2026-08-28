#include "Tests/OfflineRender.h"

#include <cmath>
#include <memory>

namespace kickr::tests
{
    juce::AudioBuffer<float> renderNote (juce::AudioProcessor& processor,
                                         int    midiNoteNumber,
                                         float  velocity,
                                         double sampleRate,
                                         int    blockSize,
                                         double seconds)
    {
        const int numChannels  = juce::jmax (1, processor.getTotalNumOutputChannels());
        const int totalSamples = juce::jmax (1, static_cast<int> (std::ceil (sampleRate * seconds)));
        const int maxBlock     = juce::jmax (1, blockSize);

        processor.setRateAndBufferSizeDetails (sampleRate, maxBlock);
        processor.prepareToPlay (sampleRate, maxBlock);

        juce::AudioBuffer<float> output (numChannels, totalSamples);
        output.clear();

        juce::AudioBuffer<float> scratch (numChannels, maxBlock);

        juce::MidiBuffer midi;
        midi.addEvent (juce::MidiMessage::noteOn (1, midiNoteNumber, velocity), 0);

        for (int pos = 0; pos < totalSamples;)
        {
            const int numThisBlock = juce::jmin (maxBlock, totalSamples - pos);

            juce::AudioBuffer<float> block (scratch.getArrayOfWritePointers(), numChannels, numThisBlock);
            block.clear();

            processor.processBlock (block, midi);
            midi.clear(); // note-on only fires in the first block

            for (int ch = 0; ch < numChannels; ++ch)
                output.copyFrom (ch, pos, block, ch, 0, numThisBlock);

            pos += numThisBlock;
        }

        processor.releaseResources();
        return output;
    }

    bool renderNoteToWav (juce::AudioProcessor& processor,
                          const juce::File&     outputFile,
                          int    midiNoteNumber,
                          float  velocity,
                          double sampleRate,
                          int    blockSize,
                          double seconds)
    {
        const auto rendered = renderNote (processor, midiNoteNumber, velocity,
                                          sampleRate, blockSize, seconds);

        outputFile.deleteFile();
        outputFile.getParentDirectory().createDirectory();

        std::unique_ptr<juce::OutputStream> stream = outputFile.createOutputStream();
        if (stream == nullptr)
            return false;

        juce::WavAudioFormat format;
        auto writer = format.createWriterFor (stream,
                                              juce::AudioFormatWriterOptions{}
                                                  .withSampleRate (sampleRate)
                                                  .withNumChannels (rendered.getNumChannels())
                                                  .withBitsPerSample (24));

        if (writer == nullptr)
            return false;

        // On success createWriterFor transferred ownership of the stream to the writer.
        return writer->writeFromAudioSampleBuffer (rendered, 0, rendered.getNumSamples());
    }
}
