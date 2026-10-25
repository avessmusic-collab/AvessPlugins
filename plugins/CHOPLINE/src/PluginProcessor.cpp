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
    swingParam = state.getRawParameterValue (params::id::swing);

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
    if (swingParam != nullptr)
        context.swingPct = swingParam->load (std::memory_order_relaxed);

    // Host transport → scheduler view (spec §8: derive step times from the
    // host PPQ every block; no playhead at all = Standalone free-run).
    if (auto* playHead = getPlayHead())
    {
        if (const auto position = playHead->getPosition())
        {
            context.hasHostPosition = true;
            context.hostPlaying = position->getIsPlaying();
            if (const auto ppq = position->getPpqPosition())
            {
                context.hasPpq = true;
                context.ppqPosition = *ppq;
            }
            if (const auto bpm = position->getBpm(); bpm.hasValue() && *bpm > 0.0)
            {
                context.hasBpm = true;
                context.bpm = *bpm;
            }
            if (const auto timeSig = position->getTimeSignature())
            {
                context.timeSigNumerator = timeSig->numerator;
                context.timeSigDenominator = timeSig->denominator;
            }
        }
    }

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
    stateTree.removeChild (stateTree.getChildWithName (state::PatternModel::patternsTag), nullptr);
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
