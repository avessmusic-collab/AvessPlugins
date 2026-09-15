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
    // Cache all 19 raw parameter pointers once (Stage 3 DSP Phase 3.2
    // validation carry-over from Phase 3.1 - see PluginProcessor.h comment).
    triggerParam = parameters.getRawParameterValue("trigger");
    buildLengthParam = parameters.getRawParameterValue("buildLength");
    styleParam = parameters.getRawParameterValue("style");
    intensityParam = parameters.getRawParameterValue("intensity");
    tensionParam = parameters.getRawParameterValue("tension");
    chaosParam = parameters.getRawParameterValue("chaos");
    impactParam = parameters.getRawParameterValue("impact");
    filterParam = parameters.getRawParameterValue("filter");
    widthParam = parameters.getRawParameterValue("width");
    pitchParam = parameters.getRawParameterValue("pitch");
    gateParam = parameters.getRawParameterValue("gate");
    stutterParam = parameters.getRawParameterValue("stutter");
    delayParam = parameters.getRawParameterValue("delay");
    reverbParam = parameters.getRawParameterValue("reverb");
    distortionParam = parameters.getRawParameterValue("distortion");
    digitalParam = parameters.getRawParameterValue("digital");
    noiseParam = parameters.getRawParameterValue("noise");
    dryWetParam = parameters.getRawParameterValue("dryWet");
    outputParam = parameters.getRawParameterValue("output");
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
    currentSampleRate = sampleRate;
    musicalClock.prepare(sampleRate);

    // DropStateMachine is NEVER restored mid-transition (architecture.md
    // "State Persistence" > Restore behavior) - every (re)prepare always
    // starts from Idle, regardless of what setStateInformation() may have
    // run before or after this call.
    dropStateMachine.reset();
    previousTriggerParamValue = false;

    // Stage 3 DSP Phase 3.2: prepare the six always-process DSP modules.
    // numChannels/maximumBlockSize drive every module's preallocated
    // scratch-buffer sizing (RT-safety: no allocation in processBlock()).
    juce::dsp::ProcessSpec spec;
    spec.sampleRate = sampleRate;
    spec.maximumBlockSize = (juce::uint32) juce::jmax(1, samplesPerBlock);
    spec.numChannels = (juce::uint32) juce::jmax(1, getTotalNumOutputChannels());

    filterModule.prepare(spec);
    widthModule.prepare(spec);
    gateModule.prepare(spec);
    delayModule.prepare(spec);
    saturationModule.prepare(spec);
    reverbModule.prepare(spec);

    dryCaptureBuffer.setSize((int) spec.numChannels, (int) spec.maximumBlockSize);
    dryCaptureBuffer.clear();

    dryWetSmoothed.reset(sampleRate, 0.02);
    dryWetSmoothed.setCurrentAndTargetValue(dryWetParam != nullptr ? dryWetParam->load() / 100.0f : 1.0f);
    outputGainSmoothed.reset(sampleRate, 0.02);
    outputGainSmoothed.setCurrentAndTargetValue(outputParam != nullptr ? juce::Decibels::decibelsToGain(outputParam->load()) : 1.0f);

    // Remaining DSP modules (Pitch/Stutter/Digital/Noise/Vacuum/ImpactEngine
    // + real IntensityBudget) arrive in Stage 3 DSP Phases 3.3-3.5.
}

void DROPAudioProcessor::releaseResources()
{
    // Cleanup will be added alongside further DSP modules in Stage 3 DSP
    // Phases 3.3-3.5 if needed; the Phase 3.2 modules' preallocated buffers
    // are cheap to simply leave allocated until the processor is destroyed.
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
    const int buildLengthChoiceIndex = (int) buildLengthParam->load();
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

    // Pre-chain dry capture (architecture.md Processing Order step 1:
    // "must happen before any module touches the buffer") - used by the
    // final Dry/Wet Mixer stage at the very end of this function.
    for (int channel = 0; channel < totalNumOutputChannels; ++channel)
        dryCaptureBuffer.copyFrom(channel, 0, buffer, channel, 0, numSamples);

    // MusicalClock: refresh bpm/time-signature/bar-position once per host
    // block (architecture.md: "recomputes samplesPerBeat once per block...
    // never cache across blocks"). Falls back to a fixed 120 BPM / 4/4
    // internal transport (driven by a sample counter, never wall-clock)
    // when getPlayHead() is null or reports no valid tempo - e.g. a bare
    // AudioProcessor under test, or a host with no active transport.
    musicalClock.update(getPlayHead(), numSamples);

    // TransitionEngine: kept in sync with the `tension` parameter every
    // block (architecture.md: tension "shapes curve steepness /
    // late-acceleration bias"). Consumed via computeIntensityProgress()
    // during Building (Stage 3 DSP Phase 3.2).
    transitionEngine.setTension(tensionParam->load() / 100.0f);

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

    const bool triggerParamNow = (triggerParam->load() > 0.5f);
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
    // state machine sample-accurately at each boundary. Stage 3 DSP Phase
    // 3.2: each segment is now also processed through the fixed series DSP
    // chain (processSegment()) using the state/progress valid for THOSE
    // samples, before the state machine advances past them.
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

        processSegment(buffer, segmentStart, (int) segmentLength);

        dropStateMachine.advance(segmentLength);
        segmentStart += (int) segmentLength;
    }

    // Drain any transition that lands exactly on the block's last sample so
    // the state observed at the start of the NEXT block is already correct.
    while (dropStateMachine.tick()) {}

    // ------------------------------------------------------------------
    // Final Dry/Wet Mixer + Output Gain (architecture.md Processing Order
    // steps 9-10, always last) - smoothed to avoid zipper per
    // implementation-spec.md's PARAMETER SMOOTHING. Applied once across the
    // whole block (not per-segment) since dryWet/output are global,
    // non-tempo-synced controls with no sample-accuracy requirement.
    // ------------------------------------------------------------------
    dryWetSmoothed.setTargetValue(juce::jlimit(0.0f, 1.0f, dryWetParam->load() / 100.0f));
    outputGainSmoothed.setTargetValue(juce::Decibels::decibelsToGain(outputParam->load()));

    for (int i = 0; i < numSamples; ++i)
    {
        const float mixAmt = dryWetSmoothed.getNextValue();
        const float gain = outputGainSmoothed.getNextValue();

        for (int channel = 0; channel < totalNumOutputChannels; ++channel)
        {
            auto* data = buffer.getWritePointer(channel);
            const float dry = dryCaptureBuffer.getSample(channel, i);
            const float wet = data[i];
            data[i] = (dry + (wet - dry) * mixAmt) * gain;
        }
    }
}

void DROPAudioProcessor::processSegment(juce::AudioBuffer<float>& buffer, int startSample, int numSamplesInSegment) noexcept
{
    if (numSamplesInSegment <= 0)
        return;

    juce::dsp::AudioBlock<float> fullBlock(buffer);
    auto segmentBlock = fullBlock.getSubBlock((size_t) startSample, (size_t) numSamplesInSegment);

    const float progress = computeIntensityProgress();

    // IntensityBudget input: all 10 macro parameters, normalized 0-1
    // (architecture.md "rawWeight[i] = styleWeight[i](progress) *
    // (macroParam[i] / 100)"). Pitch/Stutter/Digital/Noise are
    // budget-tracked here for forward compatibility but not yet consumed by
    // any DSP module (Phase 3.3 wires them in).
    std::array<float, (size_t) drop::BudgetModule::Count> macroPercent {};
    macroPercent[(size_t) drop::BudgetModule::Filter] = filterParam->load() / 100.0f;
    macroPercent[(size_t) drop::BudgetModule::Width] = widthParam->load() / 100.0f;
    macroPercent[(size_t) drop::BudgetModule::Pitch] = pitchParam->load() / 100.0f;
    macroPercent[(size_t) drop::BudgetModule::Gate] = gateParam->load() / 100.0f;
    macroPercent[(size_t) drop::BudgetModule::Stutter] = stutterParam->load() / 100.0f;
    macroPercent[(size_t) drop::BudgetModule::Delay] = delayParam->load() / 100.0f;
    macroPercent[(size_t) drop::BudgetModule::Reverb] = reverbParam->load() / 100.0f;
    macroPercent[(size_t) drop::BudgetModule::Distortion] = distortionParam->load() / 100.0f;
    macroPercent[(size_t) drop::BudgetModule::Digital] = digitalParam->load() / 100.0f;
    macroPercent[(size_t) drop::BudgetModule::Noise] = noiseParam->load() / 100.0f;

    const float intensity01 = intensityParam->load() / 100.0f;
    const auto weights = intensityBudget.compute(macroPercent, progress, intensity01);

    // Fixed series order per architecture.md Processing Order / plan.md
    // Phase 3.2 scope (Pitch/Stutter series slot deferred to Phase 3.3):
    // Filter -> Width -> Gate -> Delay -> Saturation -> Reverb. Every
    // module always processes (architecture.md "Always-Process,
    // Weight-Mix") - weight blends its contribution, never bypasses it.
    filterModule.process(segmentBlock, weights.value[(size_t) drop::BudgetModule::Filter], progress);
    widthModule.process(segmentBlock, weights.value[(size_t) drop::BudgetModule::Width], progress);
    gateModule.process(segmentBlock, weights.value[(size_t) drop::BudgetModule::Gate], progress, musicalClock);
    delayModule.process(segmentBlock, weights.value[(size_t) drop::BudgetModule::Delay], progress, musicalClock);
    saturationModule.process(segmentBlock, weights.value[(size_t) drop::BudgetModule::Distortion], progress);
    reverbModule.process(segmentBlock, weights.value[(size_t) drop::BudgetModule::Reverb], progress);
}

float DROPAudioProcessor::computeIntensityProgress() noexcept
{
    switch (dropStateMachine.getState())
    {
        case drop::DropState::Idle:
        case drop::DropState::Arming:
            return 0.0f;

        case drop::DropState::Building:
        {
            const int64_t total = dropStateMachine.currentStateLengthSamples();
            if (total <= 0)
                return 1.0f; // zero-length Building edge case -> full progress immediately
            const int64_t elapsed = total - dropStateMachine.samplesUntilNextTransition();
            const float t = juce::jlimit(0.0f, 1.0f, (float) ((double) elapsed / (double) total));
            return transitionEngine.evaluate(t);
        }

        case drop::DropState::Peak:
        case drop::DropState::Impact:
            // Phase 3.2 placeholder: VacuumModule/ImpactEngine (Phase
            // 3.3-3.4) will gate/override the bus during these states - the
            // six always-process modules simply hold at full budget here.
            return 1.0f;

        case drop::DropState::Recovery:
        {
            const int64_t total = dropStateMachine.currentStateLengthSamples();
            if (total <= 0)
                return 0.0f;
            const int64_t elapsed = total - dropStateMachine.samplesUntilNextTransition();
            const float t = juce::jlimit(0.0f, 1.0f, (float) ((double) elapsed / (double) total));
            return 1.0f - t; // architecture.md: "all module weights ramp back to 0" during Recovery
        }
    }
    return 0.0f;
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
