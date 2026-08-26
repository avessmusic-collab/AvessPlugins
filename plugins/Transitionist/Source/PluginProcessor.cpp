#include "PluginProcessor.h"
#include "PluginEditor.h"

juce::AudioProcessorValueTreeState::ParameterLayout TransitionistAudioProcessor::createParameterLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    // throw - Float, unipolar (0-100%, default 0.0)
    // Triple-role macro: dry/wet blend, delay feedback amount, reverb decay/size.
    // (DSP mapping implemented in Stage 2 - not used in Stage 1's pass-through.)
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "throw", 1 },
        "Throw",
        juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f, 1.0f),
        0.0f,
        "%"
    ));

    // space - Float, unipolar (0-100%, default 0.0)
    // Links tempo-synced delay division and reverb size into one "room" macro.
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "space", 1 },
        "Space",
        juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f, 1.0f),
        0.0f,
        "%"
    ));

    // sweep - Float, BIPOLAR (-100 to +100%, default 0.0 at the range MIDPOINT).
    // This codebase's first bipolar parameter - see parameter-spec.md's dedicated
    // implementation note. Normalised 0.5 MUST correspond to the raw value 0.0
    // (center detent = fully open filter, per creative-brief.md/architecture.md).
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "sweep", 1 },
        "Sweep",
        juce::NormalisableRange<float>(-100.0f, 100.0f, 0.1f, 1.0f),
        0.0f,
        "%"
    ));

    return layout;
}

TransitionistAudioProcessor::TransitionistAudioProcessor()
    : AudioProcessor(BusesProperties()
                        .withInput("Input", juce::AudioChannelSet::stereo(), true)
                        .withOutput("Output", juce::AudioChannelSet::stereo(), true))
    , parameters(*this, nullptr, "Parameters", createParameterLayout())
{
    // Verify the bipolar "sweep" parameter's range midpoint is correctly
    // centered, per parameter-spec.md's mandated verification step.
    jassert(parameters.getParameter("sweep")->convertTo0to1(0.0f) == 0.5f);
}

TransitionistAudioProcessor::~TransitionistAudioProcessor()
{
}

void TransitionistAudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    // DSP initialization (delay line, reverb, ladder filters, output glue,
    // width) is added in Stage 2. No allocation needed for Stage 1's
    // pass-through processBlock().
    juce::ignoreUnused(sampleRate, samplesPerBlock);
}

void TransitionistAudioProcessor::releaseResources()
{
    // DSP cleanup will be added in Stage 2.
}

void TransitionistAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;
    juce::ignoreUnused(midiMessages);

    // Parameter access example (for Stage 2 DSP implementation):
    // auto* throwParam = parameters.getRawParameterValue("throw");
    // float throwValue = throwParam->load();  // Atomic read (real-time safe)

    // Pass-through for Stage 1 (all 9 DSP components from architecture.md are
    // implemented in Stage 2). Audio routing is already handled by JUCE since
    // input and output buses are both stereo.
    juce::ignoreUnused(buffer);
}

juce::AudioProcessorEditor* TransitionistAudioProcessor::createEditor()
{
    return new TransitionistAudioProcessorEditor(*this);
}

void TransitionistAudioProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    auto state = parameters.copyState();
    std::unique_ptr<juce::XmlElement> xml(state.createXml());
    copyXmlToBinary(*xml, destData);
}

void TransitionistAudioProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xmlState(getXmlFromBinary(data, sizeInBytes));

    if (xmlState != nullptr && xmlState->hasTagName(parameters.state.getType()))
        parameters.replaceState(juce::ValueTree::fromXml(*xmlState));
}

// Factory function
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new TransitionistAudioProcessor();
}
