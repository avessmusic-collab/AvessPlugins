// CHOPLINE — PluginProcessor implementation (Phase 1).

#include "PluginProcessor.h"

#include "PluginEditor.h"

namespace chopline
{

ChoplineAudioProcessor::ChoplineAudioProcessor()
    : AudioProcessor (BusesProperties()
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      state (*this, nullptr, juce::Identifier ("CHOPLINE"), params::createParameterLayout())
{
    baseNoteParam = state.getRawParameterValue (params::id::midiBaseNote);

    // Dev-only convenience for the Standalone app: CHOPLINE_AUTOLOAD_SAMPLE=<path>
    // loads a file at startup (used for manual smoke testing; no effect in hosts).
    if (wrapperType == wrapperType_Standalone)
    {
        const auto autoload = juce::SystemStats::getEnvironmentVariable ("CHOPLINE_AUTOLOAD_SAMPLE", {});
        if (autoload.isNotEmpty() && juce::File::isAbsolutePath (autoload))
            engine.loadFile (juce::File (autoload));
    }
}

ChoplineAudioProcessor::~ChoplineAudioProcessor()
{
    *aliveFlag = false;
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

    engine::Engine::BlockContext context;
    if (baseNoteParam != nullptr)
        context.midiBaseNote = params::baseNoteForChoice (
            static_cast<int> (baseNoteParam->load (std::memory_order_relaxed)));

    engine.process (buffer, midi, context);
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

    // Replace any stale engine children from a previous restore, then append
    // the live sample reference + slice map (thread-safe inside the engine).
    stateTree.removeChild (stateTree.getChildWithName (engine::Engine::sampleTag), nullptr);
    stateTree.removeChild (stateTree.getChildWithName (engine::Engine::slicesTag), nullptr);
    engine.serializeTo (stateTree);

    if (auto xml = stateTree.createXml())
        copyXmlToBinary (*xml, destData);
}

void ChoplineAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    auto xml = getXmlFromBinary (data, sizeInBytes);
    if (xml == nullptr || ! xml->hasTagName (state.state.getType()))
        return;

    const auto newState = juce::ValueTree::fromXml (*xml);
    state.replaceState (newState);

    // Engine restore touches message-thread structures; hosts (and pluginval at
    // strictness 10) may call setStateInformation from other threads.
    if (juce::MessageManager::getInstance()->isThisTheMessageThread())
    {
        engine.restoreFrom (newState);
    }
    else
    {
        juce::MessageManager::callAsync ([alive = aliveFlag, this, newState]
        {
            if (*alive)
                engine.restoreFrom (newState);
        });
    }
}

} // namespace chopline

// This creates new instances of the plugin.
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new chopline::ChoplineAudioProcessor();
}
