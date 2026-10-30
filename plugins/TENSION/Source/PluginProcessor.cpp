#include "PluginProcessor.h"
#include "PluginEditor.h"

const juce::Identifier TENSIONAudioProcessor::stateType { "TENSION_STATE" };

juce::AudioProcessorValueTreeState::ParameterLayout TENSIONAudioProcessor::createParameterLayout()
{
    // Qualified names (not a using-directive): `sync` would be ambiguous with
    // POSIX ::sync() from <unistd.h> on macOS/Linux.
    namespace P = TensionParams;

    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    auto percentToText = [] (float value, int)
    {
        return juce::String (juce::roundToInt (value * 100.0f)) + " %";
    };
    auto percentFromText = [] (const juce::String& text)
    {
        return juce::jlimit (0.0f, 1.0f, text.retainCharacters ("0123456789.-").getFloatValue() / 100.0f);
    };
    auto dbToText = [] (float value, int)
    {
        return juce::String (value, 1) + " dB";
    };
    auto dbFromText = [] (const juce::String& text)
    {
        return text.retainCharacters ("0123456789.-").getFloatValue();
    };

    // tension - Float, 0..1 (0-100 %), linear, default 0
    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { P::tension, P::parameterVersion },
        "Tension",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.0f),
        0.0f,
        juce::AudioParameterFloatAttributes()
            .withLabel ("%")
            .withStringFromValueFunction (percentToText)
            .withValueFromStringFunction (percentFromText)));

    // mix - Float, 0..1 (0-100 %), linear, default 50 % (changed from 100 %,
    // user-directed 2026-10-30; parameter-spec.md carries the amendment)
    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { P::mix, P::parameterVersion },
        "Mix",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.0f),
        0.5f,
        juce::AudioParameterFloatAttributes()
            .withLabel ("%")
            .withStringFromValueFunction (percentToText)
            .withValueFromStringFunction (percentFromText)));

    // output - Float, -24..+12 dB, step 0.1, default 0 dB
    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { P::output, P::parameterVersion },
        "Output",
        juce::NormalisableRange<float> (-24.0f, 12.0f, 0.1f),
        0.0f,
        juce::AudioParameterFloatAttributes()
            .withLabel ("dB")
            .withStringFromValueFunction (dbToText)
            .withValueFromStringFunction (dbFromText)));

    // sync - Bool, default On
    layout.add (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { P::sync, P::parameterVersion },
        "Sync",
        true));

    // riseMode - Choice, default Cinematic (index 1)
    layout.add (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID { P::riseMode, P::parameterVersion },
        "Rise Mode",
        juce::StringArray { "Slow Burn", "Cinematic", "Aggressive" },
        1));

    return layout;
}

TENSIONAudioProcessor::TENSIONAudioProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true))
    , parameters (*this, nullptr, stateType, createParameterLayout())
{
    parameters.state.setProperty ("schemaVersion", TensionParams::stateSchemaVersion, nullptr);

    tensionParam  = parameters.getRawParameterValue (TensionParams::tension);
    mixParam      = parameters.getRawParameterValue (TensionParams::mix);
    outputParam   = parameters.getRawParameterValue (TensionParams::output);
    syncParam     = parameters.getRawParameterValue (TensionParams::sync);
    riseModeParam = parameters.getRawParameterValue (TensionParams::riseMode);
}

TENSIONAudioProcessor::~TENSIONAudioProcessor() = default;

void TENSIONAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    // Allocation happens here (never in processBlock). Zero latency.
    engine.prepare (sampleRate, samplesPerBlock, 2);
    setLatencySamples (0);
}

void TENSIONAudioProcessor::releaseResources()
{
}

HostTransport TENSIONAudioProcessor::readHostTransport() const
{
    HostTransport transport;

    // JUCE 8: getPosition() returns Optional<PositionInfo>. The deprecated
    // getCurrentPosition() is intentionally never used.
    if (auto* playHead = getPlayHead())
    {
        if (const auto position = playHead->getPosition())
        {
            if (const auto bpm = position->getBpm())
            {
                if (*bpm > 0.0)
                {
                    transport.bpm    = *bpm;
                    transport.hasBpm = true;
                }
            }

            if (const auto ppq = position->getPpqPosition())
            {
                transport.ppq    = *ppq;
                transport.hasPpq = true;
            }

            transport.isPlaying = position->getIsPlaying();
        }
    }

    return transport;
}

bool TENSIONAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& in  = layouts.getMainInputChannelSet();
    const auto& out = layouts.getMainOutputChannelSet();

    if (in != out)
        return false;

    return out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo();
}

void TENSIONAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;
    juce::ignoreUnused (midiMessages);

    // Clear any output channels without matching input (not reachable with
    // supported layouts, but safe).
    for (int ch = getTotalNumInputChannels(); ch < getTotalNumOutputChannels(); ++ch)
        buffer.clear (ch, 0, buffer.getNumSamples());

    // Atomic parameter reads (real-time safe)
    EngineParams params;
    params.tension  = tensionParam->load (std::memory_order_relaxed);
    params.mix      = mixParam->load (std::memory_order_relaxed);
    params.outputDb = outputParam->load (std::memory_order_relaxed);
    params.sync     = syncParam->load (std::memory_order_relaxed) > 0.5f;
    params.riseMode = juce::roundToInt (riseModeParam->load (std::memory_order_relaxed));

    engine.process (buffer, params, readHostTransport());
}

juce::AudioProcessorEditor* TENSIONAudioProcessor::createEditor()
{
    return new TENSIONAudioProcessorEditor (*this);
}

void TENSIONAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = parameters.copyState();
    state.setProperty ("schemaVersion", TensionParams::stateSchemaVersion, nullptr);

    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void TENSIONAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xml (getXmlFromBinary (data, sizeInBytes));

    if (xml == nullptr || ! xml->hasTagName (stateType.toString()))
        return; // invalid data ignored, defaults retained

    auto restored = juce::ValueTree::fromXml (*xml);

    if (! restored.isValid())
        return;

    restored.setProperty ("schemaVersion", TensionParams::stateSchemaVersion, nullptr);
    parameters.replaceState (restored);
}

// Factory function
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new TENSIONAudioProcessor();
}
