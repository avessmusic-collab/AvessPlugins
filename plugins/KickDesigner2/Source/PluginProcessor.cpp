#include "PluginProcessor.h"
#include "PluginEditor.h"

KickDesigner2AudioProcessor::KickDesigner2AudioProcessor()
    : AudioProcessor (BusesProperties()
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, &undoManager, "PARAMETERS", kd2::createParameterLayout())
{
}

KickDesigner2AudioProcessor::~KickDesigner2AudioProcessor() = default;

void KickDesigner2AudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    currentSampleRate = sampleRate;
    currentBlockSize  = samplesPerBlock;

    // Stage 1: no engine yet. Latency stays 0 until real oversampling is enabled
    // (Stage 2 — Phase 2.10). AD-10: the whole voice renders inside the OS region.
    setLatencySamples (0);
}

void KickDesigner2AudioProcessor::releaseResources()
{
}

bool KickDesigner2AudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    // Instrument: output-only. Reject any input bus.
    if (layouts.getMainInputChannelSet() != juce::AudioChannelSet::disabled())
        return false;

    const auto out = layouts.getMainOutputChannelSet();
    return out == juce::AudioChannelSet::stereo()
        || out == juce::AudioChannelSet::mono();
}

void KickDesigner2AudioProcessor::processBlock (juce::AudioBuffer<float>& buffer,
                                                juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;

    // Instrument → start from silence. The KickEngine is implemented in Stage 2.
    buffer.clear();

    // MIDI is consumed but not yet acted on (synthesis arrives in Stage 2 — Phase 2.1).
    juce::ignoreUnused (midiMessages);
}

juce::AudioProcessorEditor* KickDesigner2AudioProcessor::createEditor()
{
    return new KickDesigner2AudioProcessorEditor (*this);
}

void KickDesigner2AudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();

    // Forward-migration tag + preset bookkeeping (carried through the round-trip).
    state.setProperty ("stateVersion", kStateVersion, nullptr);
    if (! state.hasProperty ("currentPresetName"))
        state.setProperty ("currentPresetName", juce::String(), nullptr);
    if (! state.hasProperty ("currentPresetPath"))
        state.setProperty ("currentPresetPath", juce::String(), nullptr);

    if (auto xml = std::unique_ptr<juce::XmlElement> (state.createXml()))
        copyXmlToBinary (*xml, destData);
}

void KickDesigner2AudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    auto xml = std::unique_ptr<juce::XmlElement> (getXmlFromBinary (data, sizeInBytes));
    if (xml == nullptr || ! xml->hasTagName (apvts.state.getType()))
        return;

    auto tree = juce::ValueTree::fromXml (*xml);

    // Reserved for future migration — only v1 exists today.
    const int stateVersion = (int) tree.getProperty ("stateVersion", kStateVersion);
    juce::ignoreUnused (stateVersion);

    apvts.replaceState (tree);
}

// Plugin factory entry point.
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new KickDesigner2AudioProcessor();
}
