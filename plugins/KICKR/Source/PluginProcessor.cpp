#include "PluginProcessor.h"
#include "PluginEditor.h"

KICKRAudioProcessor::KICKRAudioProcessor()
    : AudioProcessor (BusesProperties()
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, &undoManager, "PARAMETERS", kickr::createParameterLayout())
{
}

KICKRAudioProcessor::~KICKRAudioProcessor() = default;

void KICKRAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    currentSampleRate = sampleRate;
    currentBlockSize  = samplesPerBlock;

    // Stage 1: no engine yet. Latency stays 0 until real oversampling is enabled
    // (Stage 2 — Phase 2.10). AD-10: the whole voice renders inside the OS region.
    setLatencySamples (0);
}

void KICKRAudioProcessor::releaseResources()
{
}

bool KICKRAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    // Instrument: output-only. Reject any input bus.
    if (layouts.getMainInputChannelSet() != juce::AudioChannelSet::disabled())
        return false;

    const auto out = layouts.getMainOutputChannelSet();
    return out == juce::AudioChannelSet::stereo()
        || out == juce::AudioChannelSet::mono();
}

void KICKRAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer,
                                                juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;

    // Instrument → start from silence. The KickEngine is implemented in Stage 2.
    buffer.clear();

    // MIDI is consumed but not yet acted on (synthesis arrives in Stage 2 — Phase 2.1).
    juce::ignoreUnused (midiMessages);
}

juce::AudioProcessorEditor* KICKRAudioProcessor::createEditor()
{
    return new KICKRAudioProcessorEditor (*this);
}

void KICKRAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();

    // Forward-migration tag + preset / sample bookkeeping (carried through the round-trip).
    state.setProperty ("stateVersion", kStateVersion, nullptr);
    if (! state.hasProperty ("currentPresetName"))
        state.setProperty ("currentPresetName", juce::String(), nullptr);
    if (! state.hasProperty ("currentPresetPath"))
        state.setProperty ("currentPresetPath", juce::String(), nullptr);
    // v2: name of the loaded sample from the managed bank (bare file name, never a path).
    // Buffer resolution / decode is Phase 2.7b; Stage 1 only persists the string.
    if (! state.hasProperty ("currentSampleName"))
        state.setProperty ("currentSampleName", currentSampleName, nullptr);

    if (auto xml = std::unique_ptr<juce::XmlElement> (state.createXml()))
        copyXmlToBinary (*xml, destData);
}

void KICKRAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    auto xml = std::unique_ptr<juce::XmlElement> (getXmlFromBinary (data, sizeInBytes));
    if (xml == nullptr || ! xml->hasTagName (apvts.state.getType()))
        return;

    auto tree = juce::ValueTree::fromXml (*xml);

    // Migration: v1 (pre-sample) states have no sample* params — JUCE applies their v2
    // defaults (sampleEnable off) so an old patch sounds identical. Nothing else to do yet;
    // Phase 2.7b adds resolve + decode of currentSampleName.
    const int stateVersion = (int) tree.getProperty ("stateVersion", kStateVersion);
    juce::ignoreUnused (stateVersion);

    currentSampleName = tree.getProperty ("currentSampleName", juce::String()).toString();

    apvts.replaceState (tree);
}

// Plugin factory entry point.
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new KICKRAudioProcessor();
}
