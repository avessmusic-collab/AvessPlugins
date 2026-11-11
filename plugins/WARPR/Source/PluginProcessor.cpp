#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "State/StateIO.h"

//==============================================================================
PluginProcessor::PluginProcessor()
    : AudioProcessor (BusesProperties()
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),   // instrument: output only (pattern #4)
      apvts (*this, nullptr, juce::Identifier (warpr::identity::apvtsStateId), warpr::createParameterLayout())
{
    paramSnapshot.bind (apvts);
}

PluginProcessor::~PluginProcessor()
{
    retireList.drain();
}

//==============================================================================
void PluginProcessor::prepareToPlay (double, int)
{
    drainRetired();
    backend.bumpRevision (WarprUiBackend::Revision::filterResponse);   // sample-rate dependent display
    // Voice engine / oversampling / filters are prepared here from D1 onward.
}

void PluginProcessor::releaseResources()
{
}

bool PluginProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    // Stereo output only; no input bus (mono-only host layouts are not supported).
    return layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo()
        && layouts.getMainInputChannelSet().isDisabled();
}

void PluginProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;
    juce::ignoreUnused (midiMessages);

    // Stage 1: no DSP.  Honest silence (the voice engine arrives in phase D1).
    buffer.clear();
}

//==============================================================================
juce::AudioProcessorEditor* PluginProcessor::createEditor()
{
    return new PluginEditor (*this);
}

//==============================================================================
void PluginProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    destData = warpr::state::write (apvts, backend.copyModel());
}

void PluginProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (data == nullptr || sizeInBytes <= 0)
        return;

    warpr::UiModel model;

    if (warpr::state::readContainer (data, static_cast<std::size_t> (sizeInBytes), apvts, model))
    {
        backend.adoptModel (model);
        return;
    }

    // Fallback: plain XML written by getXmlFromBinary-style hosts / older builds (no sample).
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (warpr::state::readXml (*xml, apvts, model))
            backend.adoptModel (model);

    // Anything else: keep the current (default) state.
}

//==============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new PluginProcessor();
}
