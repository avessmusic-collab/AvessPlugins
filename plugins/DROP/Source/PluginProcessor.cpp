#include "PluginProcessor.h"
#include "PluginEditor.h"

juce::AudioProcessorValueTreeState::ParameterLayout DROPAudioProcessor::createParameterLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    // Parameters are added in the exact order documented by
    // parameter-spec.md ("Parameters are implemented in EXACT order..."),
    // matching v10-PluginEditor.h's relay/attachment declaration order:
    // Main-page controls first, then Advanced-page controls.

    // --- Trigger / Timing / Style (Main page) --------------------------

    // trigger - momentary/edge-triggered bool. UI pulses true->false (~30ms
    // apart); DropStateMachine ignores it while state != Idle (Stage 3 DSP).
    layout.add(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID { "trigger", 1 },
        "Trigger",
        false));

    // buildLength - tempo-synced Building-state duration.
    layout.add(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID { "buildLength", 1 },
        "Build Length",
        juce::StringArray { "1", "2", "4", "8", "16" },
        2)); // default: 4 bars

    // style - selects the data-driven DropStyle (16 factory styles).
    layout.add(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID { "style", 1 },
        "Style",
        juce::StringArray {
            "Clean EDM", "Big Room", "Techno Pressure", "Dark Techno",
            "Trap Pull", "DnB Rush", "Hyper", "Cinematic",
            "Minimal", "Vocal Build", "Drum Build", "Synth Build",
            "Full Mix", "Destroy", "Vacuum", "Reverse Energy" },
        0)); // default: Clean EDM

    // --- Hero macros (Main page) ----------------------------------------

    // intensity - scales IntensityBudget's total normalized budget envelope.
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "intensity", 1 },
        "Intensity",
        juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f, 1.0f),
        50.0f,
        "%"));

    // --- Shape (Advanced page) ------------------------------------------

    // tension - TransitionEngine progress-curve steepness / late-acceleration bias.
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "tension", 1 },
        "Tension",
        juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f, 1.0f),
        50.0f,
        "%"));

    // chaos - scales DropRandomizer's deterministic, seeded, musically-quantized jitter.
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "chaos", 1 },
        "Chaos",
        juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f, 1.0f),
        0.0f,
        "%"));

    // impact - ImpactEngine strength + VacuumModule depth/duration; excluded
    // from the shared IntensityBudget 1.0 pool (architecture.md).
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "impact", 1 },
        "Impact",
        juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f, 1.0f),
        50.0f,
        "%"));

    // --- 10 module macros (Advanced page, 2 rows x 5) -------------------
    // All ten share identical type/range/default/skew: Float, 0-100%,
    // default 100 (unipolar direct multiplier on the Style-authored weight
    // curve - see parameter-spec.md header note / architecture.md's
    // "Resolved Open Questions" #2). Kept as separate add() calls (not a
    // loop) so each parameter's declaration is individually greppable/
    // traceable to its DSP component cross-reference, matching
    // parameter-spec.md's per-parameter format.

    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "filter", 1 },
        "Filter",
        juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f, 1.0f),
        100.0f,
        "%"));

    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "width", 1 },
        "Width",
        juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f, 1.0f),
        100.0f,
        "%"));

    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "pitch", 1 },
        "Pitch",
        juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f, 1.0f),
        100.0f,
        "%"));

    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "gate", 1 },
        "Gate",
        juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f, 1.0f),
        100.0f,
        "%"));

    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "stutter", 1 },
        "Stutter",
        juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f, 1.0f),
        100.0f,
        "%"));

    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "delay", 1 },
        "Delay",
        juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f, 1.0f),
        100.0f,
        "%"));

    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "reverb", 1 },
        "Reverb",
        juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f, 1.0f),
        100.0f,
        "%"));

    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "distortion", 1 },
        "Distortion",
        juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f, 1.0f),
        100.0f,
        "%"));

    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "digital", 1 },
        "Digital",
        juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f, 1.0f),
        100.0f,
        "%"));

    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "noise", 1 },
        "Noise",
        juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f, 1.0f),
        100.0f,
        "%"));

    // --- Hero macro (Main page, right knob) -----------------------------

    // dryWet - global mix (juce::dsp::DryWetMixer), smoothed.
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "dryWet", 1 },
        "Dry/Wet",
        juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f, 1.0f),
        100.0f,
        "%"));

    // --- Output (Advanced page) -----------------------------------------

    // output - final output gain stage, smoothed.
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "output", 1 },
        "Output",
        juce::NormalisableRange<float>(-60.0f, 12.0f, 0.1f, 1.0f),
        0.0f,
        "dB"));

    return layout;
}

DROPAudioProcessor::DROPAudioProcessor()
    : AudioProcessor(BusesProperties()
                        .withInput("Input", juce::AudioChannelSet::stereo(), true)
                        .withOutput("Output", juce::AudioChannelSet::stereo(), true))
    , parameters(*this, nullptr, "Parameters", createParameterLayout())
{
}

DROPAudioProcessor::~DROPAudioProcessor()
{
}

bool DROPAudioProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    // Mono and stereo, matching input to output (architecture.md's
    // "44.1-192 kHz, mono+stereo" guarantee).
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::mono()
        && layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;

    return layouts.getMainOutputChannelSet() == layouts.getMainInputChannelSet();
}

void DROPAudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    // DSP module preparation (MusicalClock, TransitionEngine, DropStateMachine,
    // and the 17 DSP modules) arrives in Stage 3 DSP Phases 3.1-3.5.
    juce::ignoreUnused(sampleRate, samplesPerBlock);
}

void DROPAudioProcessor::releaseResources()
{
    // Cleanup will be added alongside DSP modules in Stage 3.
}

void DROPAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;
    juce::ignoreUnused(midiMessages);

    // Stage 1 scaffolding only: real-time-safe pass-through, no allocation,
    // no locks. The MusicalClock/TransitionEngine/DropStateMachine timing
    // skeleton and sample-accurate block segmentation (architecture.md
    // "Sample-Accurate Events / Block Segmentation") land in Stage 3 DSP
    // Phase 3.1; MIDI trigger-note detection lands alongside it.

    // Clear any output channels that didn't come from an input channel
    // (matches JUCE plugin template convention for buses that may not
    // exactly match, e.g. mono <-> stereo hosts that still call through).
    auto totalNumInputChannels = getTotalNumInputChannels();
    auto totalNumOutputChannels = getTotalNumOutputChannels();

    for (auto channel = totalNumInputChannels; channel < totalNumOutputChannels; ++channel)
        buffer.clear(channel, 0, buffer.getNumSamples());

    // Parameter access example (for Stage 3 DSP implementation):
    // auto* intensityParam = parameters.getRawParameterValue("intensity");
    // float intensityValue = intensityParam->load(); // atomic read, real-time safe
}

juce::AudioProcessorEditor* DROPAudioProcessor::createEditor()
{
    return new DROPAudioProcessorEditor(*this);
}

void DROPAudioProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    // APVTS parameters serialize automatically via parameters.copyState().
    auto state = parameters.copyState();

    // Non-parameter state (architecture.md "State Persistence"): nested as
    // a <DropCustomState> child under the APVTS root ValueTree so both
    // save/restore in a single pass.
    juce::ValueTree customState("DropCustomState");
    customState.setProperty("triggerMode", static_cast<int>(getTriggerMode()), nullptr);
    customState.setProperty("midiNote", getMidiTriggerNote(), nullptr);
    customState.setProperty("seed", static_cast<int>(getChaosSeed()), nullptr);
    customState.setProperty("uiAdvancedExpanded", getUiAdvancedRowExpanded(), nullptr);

    for (const auto& point : tensionCurvePoints)
    {
        juce::ValueTree curvePoint("CurvePoint");
        curvePoint.setProperty("x", point.x, nullptr);
        curvePoint.setProperty("y", point.y, nullptr);
        customState.appendChild(curvePoint, nullptr);
    }

    state.appendChild(customState, nullptr);

    std::unique_ptr<juce::XmlElement> xml(state.createXml());
    copyXmlToBinary(*xml, destData);
}

void DROPAudioProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xmlState(getXmlFromBinary(data, sizeInBytes));

    if (xmlState == nullptr || !xmlState->hasTagName(parameters.state.getType()))
        return;

    auto newState = juce::ValueTree::fromXml(*xmlState);

    // Extract (and detach) the custom child tree before handing the rest to
    // APVTS, so replaceState() doesn't have to know about our own extension.
    auto customState = newState.getChildWithName("DropCustomState");

    if (customState.isValid())
    {
        setTriggerMode(static_cast<TriggerMode>(static_cast<int>(customState.getProperty("triggerMode", 0))));
        setMidiTriggerNote(static_cast<int>(customState.getProperty("midiNote", kDefaultMidiTriggerNote)));
        setChaosSeed(static_cast<uint32_t>(static_cast<int>(customState.getProperty("seed", static_cast<int>(kDefaultChaosSeed)))));
        setUiAdvancedRowExpanded(static_cast<bool>(customState.getProperty("uiAdvancedExpanded", false)));

        std::vector<CurvePoint> points;
        for (int i = 0; i < customState.getNumChildren(); ++i)
        {
            auto child = customState.getChild(i);
            if (child.hasType("CurvePoint"))
            {
                CurvePoint point;
                point.x = static_cast<float>(child.getProperty("x", 0.0));
                point.y = static_cast<float>(child.getProperty("y", 0.0));
                points.push_back(point);
            }
        }
        setTensionCurvePoints(std::move(points));

        newState.removeChild(customState, nullptr);
    }
    else
    {
        // Missing/corrupt custom-state child: fall back to defaults
        // (architecture.md "Restore behavior") - silent, safe fallback.
        setTriggerMode(TriggerMode::Manual);
        setMidiTriggerNote(kDefaultMidiTriggerNote);
        setChaosSeed(kDefaultChaosSeed);
        setUiAdvancedRowExpanded(false);
        setTensionCurvePoints({});
    }

    parameters.replaceState(newState);

    // DropStateMachine itself is NEVER restored mid-transition - Stage 3
    // DSP always initializes it to Idle regardless of saved state
    // (architecture.md "Restore behavior"). No state-machine member exists
    // yet in Stage 1, so there is nothing to reset here.
}

//==============================================================================
// JUCE plugin entry point - creates the processor instance for every format
// wrapper (VST3/AU/Standalone).
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new DROPAudioProcessor();
}
