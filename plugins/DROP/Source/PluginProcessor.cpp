#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <algorithm>
#include <cmath>
#include <limits>

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
    juce::ignoreUnused(samplesPerBlock);

    currentSampleRate = sampleRate;
    musicalClock.prepare(sampleRate);

    // DropStateMachine is NEVER restored mid-transition (architecture.md
    // "State Persistence" > Restore behavior) - every (re)prepare always
    // starts from Idle, regardless of what setStateInformation() may have
    // run before or after this call.
    dropStateMachine.reset();
    previousTriggerParamValue = false;

    // Remaining DSP module preparation (the 17 audible DSP modules) arrives
    // in Stage 3 DSP Phases 3.2-3.5.
}

void DROPAudioProcessor::releaseResources()
{
    // Cleanup will be added alongside DSP modules in Stage 3 DSP Phases 3.2-3.5.
}

drop::DropStateMachine::Durations DROPAudioProcessor::computeDurationsForNewBuild()
{
    drop::DropStateMachine::Durations d;

    // Arming: quantize the build's start to the next bar boundary
    // (architecture.md DropStateMachine > Arming: "snaps to the next
    // MusicalClock-quantized start point (default: next bar boundary...)").
    // Uses the bar position MusicalClock last observed at the top of THIS
    // block (musicalClock.update() is called once per host block, per
    // architecture.md's "recomputed every block" rule) - a MIDI trigger
    // landing mid-block is therefore quantized against the block-start bar
    // position, not its own exact sample offset; documented simplification,
    // not expected to be audible at any reasonable block size.
    d.armingToBuildingSamples = (int64_t) std::llround(musicalClock.samplesToNextBarBoundary());

    // Building: buildLength (bars, choice index -> bar count) -> samples,
    // tempo/time-signature latched now (architecture.md: "a tempo change
    // mid-build does not shorten/lengthen an in-flight build's
    // sample-position targets").
    static constexpr int kBuildLengthBars[] = { 1, 2, 4, 8, 16 };
    const int buildLengthChoiceIndex = (int) parameters.getRawParameterValue("buildLength")->load();
    const int bars = kBuildLengthBars[(size_t) juce::jlimit(0, 4, buildLengthChoiceIndex)];
    d.buildingSamples = (int64_t) std::llround(musicalClock.barsToSamples(bars));

    // Peak/Impact/Recovery: fixed placeholder windows until VacuumModule/
    // ImpactEngine/DropStyle land (Stage 3 DSP Phases 3.3-3.5, per plan.md -
    // this phase is scoped to the timing/control skeleton only). These are
    // deterministic and sample-accurate, not final musical values.
    d.peakSamples = (int64_t) std::llround(currentSampleRate * kPlaceholderPeakSeconds);
    d.impactSamples = (int64_t) std::llround(currentSampleRate * kPlaceholderImpactSeconds);
    d.recoverySamples = (int64_t) std::llround(currentSampleRate * kPlaceholderRecoverySeconds);

    return d;
}

void DROPAudioProcessor::fireTriggerIfIdle()
{
    // DropStateMachine::requestTrigger() itself already enforces re-trigger
    // safety (no-op unless Idle) - this wrapper exists so
    // computeDurationsForNewBuild() (which reads parameters/MusicalClock) is
    // only ever called when a trigger will actually be accepted.
    if (dropStateMachine.getState() != drop::DropState::Idle)
        return;

    dropStateMachine.requestTrigger(computeDurationsForNewBuild());
}

void DROPAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;

    // Clear any output channels that didn't come from an input channel
    // (matches JUCE plugin template convention for buses that may not
    // exactly match, e.g. mono <-> stereo hosts that still call through).
    auto totalNumInputChannels = getTotalNumInputChannels();
    auto totalNumOutputChannels = getTotalNumOutputChannels();

    for (auto channel = totalNumInputChannels; channel < totalNumOutputChannels; ++channel)
        buffer.clear(channel, 0, buffer.getNumSamples());

    const int numSamples = buffer.getNumSamples();
    if (numSamples <= 0)
        return;

    // MusicalClock: refresh bpm/time-signature/bar-position once per host
    // block (architecture.md: "recomputes samplesPerBeat once per block...
    // never cache across blocks"). Falls back to a fixed 120 BPM / 4/4
    // internal transport (driven by a sample counter, never wall-clock)
    // when getPlayHead() is null or reports no valid tempo - e.g. a bare
    // AudioProcessor under test, or a host with no active transport.
    musicalClock.update(getPlayHead(), numSamples);

    // TransitionEngine: kept in sync with the `tension` parameter every
    // block (architecture.md: tension "shapes curve steepness /
    // late-acceleration bias"). Its evaluate() output isn't consumed by any
    // audible DSP yet (Phase 3.1 scope - no audio-affecting modules exist),
    // but the component itself is fully implemented and unit-tested this
    // phase per plan.md's Phase 3.1 component list, ready for Phase 3.2+ to
    // read `transitionEngine.evaluate(progressWithinBuildingState)`.
    transitionEngine.setTension(parameters.getRawParameterValue("tension")->load() / 100.0f);

    // ------------------------------------------------------------------
    // Sample-accurate trigger-event detection (architecture.md
    // "Sample-Accurate Events / Block Segmentation" + "MIDI Routing").
    //
    // `trigger` parameter: AudioProcessorValueTreeState's raw-value
    // accessor exposes only the CURRENT (already-applied) value, not a list
    // of sample-accurate automation points within this block - JUCE does
    // not expose that resolution through APVTS. Its edge is therefore
    // detected at block granularity (treated as occurring at sample offset
    // 0 of the block in which it's first observed high) - the finest
    // resolution actually available without custom per-sample automation
    // plumbing, and "Host automation Trigger parameter" per
    // implementation-spec.md always works regardless of triggerMode.
    //
    // MIDI trigger note: genuinely sample-accurate via MidiBuffer's
    // per-event sample offsets. Only scanned when triggerMode == Midi (a
    // note matching the configured trigger pitch shouldn't accidentally arm
    // a build if the user hasn't opted into MIDI triggering - e.g. a track
    // also carrying unrelated MIDI content). TriggerMode::AutoSync is a
    // documented stub for this phase (architecture.md/plan.md Phase 3.1
    // scope: "auto may be a stub") - it currently behaves like Manual
    // (MIDI listener inactive, no auto-triggering logic yet).
    // ------------------------------------------------------------------
    int triggerEventOffset = -1; // -1 == no trigger event this block

    const bool triggerParamNow = (parameters.getRawParameterValue("trigger")->load() > 0.5f);
    if (triggerParamNow && !previousTriggerParamValue)
        triggerEventOffset = 0;
    previousTriggerParamValue = triggerParamNow;

    if (getTriggerMode() == TriggerMode::Midi)
    {
        const int configuredMidiNote = getMidiTriggerNote();
        for (const auto metadata : midiMessages)
        {
            const auto message = metadata.getMessage();
            if (message.isNoteOn() && message.getNoteNumber() == configuredMidiNote)
            {
                const int offset = metadata.samplePosition;
                if (triggerEventOffset < 0 || offset < triggerEventOffset)
                    triggerEventOffset = offset;
            }
        }
    }

    // ------------------------------------------------------------------
    // Sub-block segmentation loop (architecture.md "Sample-Accurate Events
    // / Block Segmentation"). Splits the block at the trigger-event offset
    // (if any) and every DropStateMachine state transition, advancing the
    // state machine sample-accurately at each boundary. Audio remains a
    // real-time-safe PASS-THROUGH this phase - no audible DSP module exists
    // yet (Stage 3 DSP Phases 3.2-3.5 add FilterModule..NoiseModule, which
    // will read weights recomputed at these same segment boundaries).
    // ------------------------------------------------------------------
    int segmentStart = 0;
    while (segmentStart < numSamples)
    {
        if (triggerEventOffset == segmentStart)
            fireTriggerIfIdle();

        // Cascade through any exactly-zero-length states (e.g. a bar
        // boundary already reached, or a zero-duration Peak placeholder)
        // before consuming any further samples in this segment.
        while (dropStateMachine.tick()) {}

        int64_t samplesToNextEvent = std::numeric_limits<int64_t>::max();
        if (triggerEventOffset > segmentStart)
            samplesToNextEvent = (int64_t) triggerEventOffset - (int64_t) segmentStart;

        int64_t segmentLength = (int64_t) (numSamples - segmentStart);
        segmentLength = std::min(segmentLength, samplesToNextEvent);
        segmentLength = std::min(segmentLength, dropStateMachine.samplesUntilNextTransition());

        if (segmentLength <= 0)
            segmentLength = 1; // defensive: guarantee forward progress (tick() above should already have resolved any zero-length boundary)

        dropStateMachine.advance(segmentLength);
        segmentStart += (int) segmentLength;
    }

    // Drain any transition that lands exactly on the block's last sample so
    // the state observed at the start of the NEXT block is already correct.
    while (dropStateMachine.tick()) {}
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

    // DropStateMachine itself is NEVER restored mid-transition (architecture.md
    // "Restore behavior") - it always initializes to Idle. Deliberately NOT
    // reset here: setStateInformation() can be called off the audio thread
    // (per JUCE, and possibly while the audio thread is concurrently
    // running), and DropStateMachine has no locks/atomics (by design - it's
    // audio-thread-only, per architecture.md "Thread Boundaries"), so
    // touching it from here would be a data race. prepareToPlay() - which
    // every host calls before/around a state load in the normal plugin
    // lifecycle - already unconditionally resets it to Idle.
}

//==============================================================================
// JUCE plugin entry point - creates the processor instance for every format
// wrapper (VST3/AU/Standalone).
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new DROPAudioProcessor();
}
