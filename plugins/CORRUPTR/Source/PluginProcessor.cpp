#include "PluginProcessor.h"
#include "PluginEditor.h"

//==============================================================================
// Parameter Layout
//==============================================================================
juce::AudioProcessorValueTreeState::ParameterLayout CORRUPTRAudioProcessor::createParameterLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    //=========================================================================
    // Module: Top Bar / Global
    //=========================================================================
    layout.add(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID { "qualityMode", 1 },
        "Quality Mode",
        juce::StringArray { "ECO", "NORMAL", "HIGH", "EXTREME", "AUTO" },
        1)); // NORMAL

    //=========================================================================
    // Module: Distortion
    //=========================================================================
    layout.add(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID { "graphBypassSaturation", 1 },
        "Bypass Saturation",
        false));

    layout.add(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID { "distortionAlgorithm", 1 },
        "Distortion Algorithm",
        juce::StringArray { "Saturation", "Soft Clip", "Hard Clip", "Tube", "Diode", "Fuzz",
                             "Digital Distortion", "Bitcrusher", "Sample Rate Reducer",
                             "Wavefolder", "Ring-Mod", "Foldback" },
        0)); // Saturation

    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "drive", 1 },
        "Drive",
        juce::NormalisableRange<float>(0.0f, 40.0f, 0.01f, 1.0f),
        6.0f,
        "dB"));

    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "tone", 1 },
        "Tone",
        juce::NormalisableRange<float>(-100.0f, 100.0f, 0.01f, 1.0f),
        0.0f));

    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "bias", 1 },
        "Bias",
        juce::NormalisableRange<float>(-100.0f, 100.0f, 0.01f, 1.0f),
        0.0f));

    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "distortionMix", 1 },
        "Distortion Mix",
        juce::NormalisableRange<float>(0.0f, 100.0f, 0.01f, 1.0f),
        100.0f,
        "%"));

    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "fold", 1 },
        "Fold",
        juce::NormalisableRange<float>(0.0f, 100.0f, 0.01f, 1.0f),
        0.0f,
        "%"));

    layout.add(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID { "graphBypassWaveshaper", 1 },
        "Bypass Waveshaper",
        false));

    //=========================================================================
    // Module: Bitcrush
    //=========================================================================
    layout.add(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID { "graphBypassBitcrush", 1 },
        "Bypass Bitcrush",
        false));

    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "bitDepth", 1 },
        "Bit Depth",
        juce::NormalisableRange<float>(1.0f, 16.0f, 0.01f, 1.0f),
        16.0f,
        "bit"));

    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "sampleRateReduction", 1 },
        "Sample Rate Reduction",
        juce::NormalisableRange<float>(1.0f, 48.0f, 0.01f, 1.0f),
        1.0f,
        "x"));

    //=========================================================================
    // Module: Glitch
    //=========================================================================
    layout.add(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID { "graphBypassGlitch", 1 },
        "Bypass Glitch",
        false));

    layout.add(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID { "glitchMode", 1 },
        "Glitch Mode",
        juce::StringArray { "Off", "Stutter", "Repeat", "Reverse", "Buffer Repeat", "Freeze",
                             "Slice", "Random Slice", "Micro Loop", "Granular Repeat",
                             "Tape Stop", "Tape Start", "Retrigger", "Silence", "Noise Burst",
                             "Bitcrush Burst", "Pitch Jump", "Random Repeat" },
        0)); // Off

    layout.add(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID { "glitchBufferLength", 1 },
        "Glitch Buffer Length",
        juce::StringArray { "1/64", "1/32", "1/16", "1/8", "1/4", "1/2", "1 bar", "2 bars", "4 bars" },
        2)); // 1/16

    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "glitchProbability", 1 },
        "Glitch Probability",
        juce::NormalisableRange<float>(0.0f, 100.0f, 0.01f, 1.0f),
        0.0f,
        "%"));

    //=========================================================================
    // Module: Center Master
    //=========================================================================
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "mix", 1 },
        "Mix",
        juce::NormalisableRange<float>(0.0f, 200.0f, 0.01f, 1.0f),
        100.0f,
        "%"));

    layout.add(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID { "outputLimiterStyle", 1 },
        "Output Limiter Style",
        juce::StringArray { "Transparent", "Colored" },
        0)); // Transparent

    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "inputGain", 1 },
        "Input Gain",
        juce::NormalisableRange<float>(-24.0f, 24.0f, 0.01f, 1.0f),
        0.0f,
        "dB"));

    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "outputGain", 1 },
        "Output Gain",
        juce::NormalisableRange<float>(-24.0f, 24.0f, 0.01f, 1.0f),
        0.0f,
        "dB"));

    //=========================================================================
    // Module: Filter & EQ
    //=========================================================================
    layout.add(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID { "graphBypassFilter", 1 },
        "Bypass Filter",
        false));

    layout.add(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID { "filterType", 1 },
        "Filter Type",
        juce::StringArray { "LP", "HP", "BP", "Notch", "Comb", "Resonant LP", "Resonant HP" },
        0)); // LP

    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "filterCutoff", 1 },
        "Filter Cutoff",
        juce::NormalisableRange<float>(20.0f, 20000.0f, 0.01f, 0.3f),
        20000.0f,
        "Hz"));

    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "filterResonance", 1 },
        "Filter Resonance",
        juce::NormalisableRange<float>(0.0f, 100.0f, 0.01f, 1.0f),
        0.0f,
        "%"));

    //=========================================================================
    // Module: Feedback + Delay
    //=========================================================================
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "feedbackAmount", 1 },
        "Feedback Amount",
        juce::NormalisableRange<float>(0.0f, 100.0f, 0.01f, 1.0f),
        0.0f,
        "%"));

    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "feedbackDamping", 1 },
        "Feedback Damping",
        juce::NormalisableRange<float>(0.0f, 100.0f, 0.01f, 1.0f),
        50.0f,
        "%"));

    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "microDelayTime", 1 },
        "Micro Delay Time",
        juce::NormalisableRange<float>(0.1f, 50.0f, 0.01f, 1.0f),
        10.0f,
        "ms"));

    //=========================================================================
    // Module: Macros
    //=========================================================================
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "macroDamage", 1 },
        "Macro Damage",
        juce::NormalisableRange<float>(0.0f, 100.0f, 0.01f, 1.0f),
        0.0f,
        "%"));

    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "macroCrush", 1 },
        "Macro Crush",
        juce::NormalisableRange<float>(0.0f, 100.0f, 0.01f, 1.0f),
        0.0f,
        "%"));

    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "macroGlitch", 1 },
        "Macro Glitch",
        juce::NormalisableRange<float>(0.0f, 100.0f, 0.01f, 1.0f),
        0.0f,
        "%"));

    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "macroChaos", 1 },
        "Macro Chaos",
        juce::NormalisableRange<float>(0.0f, 100.0f, 0.01f, 1.0f),
        0.0f,
        "%"));

    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "macroRhythm", 1 },
        "Macro Rhythm",
        juce::NormalisableRange<float>(0.0f, 100.0f, 0.01f, 1.0f),
        0.0f,
        "%"));

    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "macroMovement", 1 },
        "Macro Movement",
        juce::NormalisableRange<float>(0.0f, 100.0f, 0.01f, 1.0f),
        0.0f,
        "%"));

    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "macroWidth", 1 },
        "Macro Width",
        juce::NormalisableRange<float>(0.0f, 100.0f, 0.01f, 1.0f),
        50.0f,
        "%"));

    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "macroMix", 1 },
        "Macro Mix",
        juce::NormalisableRange<float>(0.0f, 200.0f, 0.01f, 1.0f),
        100.0f,
        "%"));

    //=========================================================================
    // Module: Performance Triggers
    //=========================================================================
    layout.add(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID { "performanceKill", 1 },
        "Performance Kill",
        false));

    layout.add(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID { "performanceGlitch", 1 },
        "Performance Glitch",
        false));

    layout.add(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID { "performanceDestroy", 1 },
        "Performance Destroy",
        false));

    layout.add(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID { "performanceFreeze", 1 },
        "Performance Freeze",
        false));

    layout.add(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID { "performanceReverse", 1 },
        "Performance Reverse",
        false));

    layout.add(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID { "performanceStutter", 1 },
        "Performance Stutter",
        false));

    layout.add(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID { "performanceChaos", 1 },
        "Performance Chaos",
        false));

    //=========================================================================
    // Module: XY Performance Pad
    //=========================================================================
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "xyPadX", 1 },
        "XY Pad X",
        juce::NormalisableRange<float>(0.0f, 100.0f, 0.01f, 1.0f),
        0.0f,
        "%"));

    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "xyPadY", 1 },
        "XY Pad Y",
        juce::NormalisableRange<float>(0.0f, 100.0f, 0.01f, 1.0f),
        0.0f,
        "%"));

    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "xyPadSmoothing", 1 },
        "XY Pad Smoothing",
        juce::NormalisableRange<float>(0.0f, 500.0f, 0.01f, 1.0f),
        80.0f,
        "ms"));

    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "chaos", 1 },
        "Chaos",
        juce::NormalisableRange<float>(0.0f, 100.0f, 0.01f, 1.0f),
        0.0f,
        "%"));

    //=========================================================================
    // Module: Rhythmic Sequencer
    //=========================================================================
    layout.add(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID { "sequencerEnabled", 1 },
        "Sequencer Enabled",
        true));

    layout.add(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID { "sequencerRate", 1 },
        "Sequencer Rate",
        juce::StringArray { "1/4", "1/8", "1/16", "1/32", "1/4T", "1/8T", "1/16T" },
        2)); // 1/16

    layout.add(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID { "sequencerSteps", 1 },
        "Sequencer Steps",
        juce::StringArray { "16", "32" },
        0)); // 16

    //=========================================================================
    // Module: Modulation Matrix (4 LFOs)
    //=========================================================================
    layout.add(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID { "modMatrixEnabled", 1 },
        "Mod Matrix Enabled",
        true));

    const juce::StringArray lfoShapeChoices { "Sine", "Triangle", "Saw", "Reverse Saw", "Square",
                                               "S&H", "Random", "Smooth Random", "Random Walk" };

    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "lfo1Rate", 1 },
        "LFO 1 Rate",
        juce::NormalisableRange<float>(0.01f, 20.0f, 0.001f, 1.0f),
        1.0f,
        "Hz"));

    layout.add(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID { "lfo1Shape", 1 },
        "LFO 1 Shape",
        lfoShapeChoices,
        0)); // Sine

    layout.add(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID { "lfo1Sync", 1 },
        "LFO 1 Sync",
        false));

    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "lfo2Rate", 1 },
        "LFO 2 Rate",
        juce::NormalisableRange<float>(0.01f, 20.0f, 0.001f, 1.0f),
        1.0f,
        "Hz"));

    layout.add(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID { "lfo2Shape", 1 },
        "LFO 2 Shape",
        lfoShapeChoices,
        0)); // Sine

    layout.add(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID { "lfo2Sync", 1 },
        "LFO 2 Sync",
        false));

    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "lfo3Rate", 1 },
        "LFO 3 Rate",
        juce::NormalisableRange<float>(0.01f, 20.0f, 0.001f, 1.0f),
        1.0f,
        "Hz"));

    layout.add(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID { "lfo3Shape", 1 },
        "LFO 3 Shape",
        lfoShapeChoices,
        0)); // Sine

    layout.add(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID { "lfo3Sync", 1 },
        "LFO 3 Sync",
        false));

    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "lfo4Rate", 1 },
        "LFO 4 Rate",
        juce::NormalisableRange<float>(0.01f, 20.0f, 0.001f, 1.0f),
        1.0f,
        "Hz"));

    layout.add(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID { "lfo4Shape", 1 },
        "LFO 4 Shape",
        lfoShapeChoices,
        0)); // Sine

    layout.add(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID { "lfo4Sync", 1 },
        "LFO 4 Sync",
        false));

    //=========================================================================
    // Module: Mod Matrix (8 Slots)
    //=========================================================================
    const juce::StringArray modSourceChoices { "LFO 1", "LFO 2", "LFO 3", "LFO 4", "Envelope",
                                                "Audio Level", "Sequencer", "Random", "MIDI CC", "Macro" };

    const juce::StringArray modDestinationChoices { "Drive", "Mix", "Filter Cutoff", "Bit Depth",
                                                      "Sample Rate", "Fold", "Feedback",
                                                      "Glitch Probability", "Glitch Size", "Pitch",
                                                      "Pan", "Width" };

    for (int slot = 1; slot <= 8; ++slot)
    {
        const juce::String slotStr = juce::String(slot);

        layout.add(std::make_unique<juce::AudioParameterChoice>(
            juce::ParameterID { "modSlot" + slotStr + "Source", 1 },
            "Mod Slot " + slotStr + " Source",
            modSourceChoices,
            0)); // LFO 1

        layout.add(std::make_unique<juce::AudioParameterChoice>(
            juce::ParameterID { "modSlot" + slotStr + "Destination", 1 },
            "Mod Slot " + slotStr + " Destination",
            modDestinationChoices,
            0)); // Drive

        layout.add(std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID { "modSlot" + slotStr + "Amount", 1 },
            "Mod Slot " + slotStr + " Amount",
            juce::NormalisableRange<float>(-100.0f, 100.0f, 0.01f, 1.0f),
            0.0f,
            "%"));

        layout.add(std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID { "modSlot" + slotStr + "Enable", 1 },
            "Mod Slot " + slotStr + " Enable",
            false));
    }

    return layout;
}

//==============================================================================
CORRUPTRAudioProcessor::CORRUPTRAudioProcessor()
    : AudioProcessor(BusesProperties()
                        .withInput("Input", juce::AudioChannelSet::stereo(), true)
                        .withOutput("Output", juce::AudioChannelSet::stereo(), true))
    , parameters(*this, nullptr, "Parameters", createParameterLayout())
{
}

CORRUPTRAudioProcessor::~CORRUPTRAudioProcessor()
{
}

void CORRUPTRAudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    // Initialization will be added in Stage 2 (DSP)
    juce::ignoreUnused(sampleRate, samplesPerBlock);
}

void CORRUPTRAudioProcessor::releaseResources()
{
    // Cleanup will be added in Stage 2 (DSP)
}

void CORRUPTRAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;
    juce::ignoreUnused(midiMessages);

    // Parameter access example (for Stage 2 DSP implementation):
    // auto* driveParam = parameters.getRawParameterValue("drive");
    // float driveValue = driveParam->load();  // Atomic read (real-time safe)

    // Pass-through for Stage 1 (DSP implementation happens in Stage 2)
    // Audio routing is already handled by JUCE
    juce::ignoreUnused(buffer);
}

//==============================================================================
juce::AudioProcessorEditor* CORRUPTRAudioProcessor::createEditor()
{
    return new CORRUPTRAudioProcessorEditor(*this);
}

//==============================================================================
void CORRUPTRAudioProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    auto state = parameters.copyState();
    std::unique_ptr<juce::XmlElement> xml(state.createXml());
    copyXmlToBinary(*xml, destData);
}

void CORRUPTRAudioProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xmlState(getXmlFromBinary(data, sizeInBytes));

    if (xmlState != nullptr && xmlState->hasTagName(parameters.state.getType()))
        parameters.replaceState(juce::ValueTree::fromXml(*xmlState));
}

//==============================================================================
// Factory function
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new CORRUPTRAudioProcessor();
}
