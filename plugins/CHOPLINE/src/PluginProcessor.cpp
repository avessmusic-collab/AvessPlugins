// CHOPLINE — PluginProcessor implementation (Phase 0 silent shell).

#include "PluginProcessor.h"

#include "PluginEditor.h"

namespace chopline
{

ChoplineAudioProcessor::ChoplineAudioProcessor()
    : AudioProcessor (BusesProperties()
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      state (*this, nullptr, juce::Identifier ("CHOPLINE"), params::createParameterLayout())
{
}

void ChoplineAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    engine.prepare (sampleRate, samplesPerBlock, getTotalNumOutputChannels());
}

void ChoplineAudioProcessor::releaseResources()
{
    engine.release();
}

bool ChoplineAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    // Instrument: stereo output only, no input buses.
    return layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo()
        && layouts.getMainInputChannelSet().isDisabled();
}

void ChoplineAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer,
                                           juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;

    // Phase 0: the engine clears the buffer (silence) and ignores MIDI.
    engine.process (buffer, midi);
}

juce::AudioProcessorEditor* ChoplineAudioProcessor::createEditor()
{
    return new ChoplineAudioProcessorEditor (*this);
}

void ChoplineAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    // Versioned from day one (spec §10): the schema version rides on the root.
    auto stateTree = state.copyState();
    stateTree.setProperty ("schemaVersion", params::kSchemaVersion, nullptr);

    if (auto xml = stateTree.createXml())
        copyXmlToBinary (*xml, destData);
}

void ChoplineAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (state.state.getType()))
            state.replaceState (juce::ValueTree::fromXml (*xml));
}

} // namespace chopline

// This creates new instances of the plugin.
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new chopline::ChoplineAudioProcessor();
}
