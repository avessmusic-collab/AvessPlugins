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

    // Stage 3 DSP Phase 3.3 modules.
    pitchModule.prepare(spec);
    stutterModule.prepare(spec);
    digitalModule.prepare(spec);
    vacuumModule.prepare(spec);
    noiseModule.prepare(spec);

    // Stage 3 DSP Phase 3.4.
    impactEngine.prepare(spec);

    dryCaptureBuffer.setSize((int) spec.numChannels, (int) spec.maximumBlockSize);
    dryCaptureBuffer.clear();

    dryWetSmoothed.reset(sampleRate, 0.02);
    dryWetSmoothed.setCurrentAndTargetValue(dryWetParam != nullptr ? dryWetParam->load() / 100.0f : 1.0f);
    outputGainSmoothed.reset(sampleRate, 0.02);
    outputGainSmoothed.setCurrentAndTargetValue(outputParam != nullptr ? juce::Decibels::decibelsToGain(outputParam->load()) : 1.0f);

    // Remaining DSP (ImpactEngine + real Style-driven IntensityBudget)
    // arrives in Stage 3 DSP Phases 3.4-3.5.
}

void DROPAudioProcessor::releaseResources()
{
    // Cleanup will be added alongside further DSP modules in Stage 3 DSP
    // Phases 3.3-3.5 if needed; the Phase 3.2 modules' preallocated buffers
    // are cheap to simply leave allocated until the processor is destroyed.
}

drop::DropStateMachine::Durations DROPAudioProcessor::computeDurationsForNewBuild()
{
    // Stage 3 DSP Phase 3.5: Vacuum/Impact/Recovery timing now comes from
    // the ACTIVE (already-latched-this-call by fireTriggerIfIdle()) Style
    // instead of fixed constants (architecture.md: "Vacuum duration/floor,
    // Impact tail length, Recovery length are all Style-configurable").
    const auto& style = drop::getStyle(activeStyleIndexForBuild);

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

    // Stage 3 DSP Phase 3.4/3.5: Peak's ENTIRE duration IS the Vacuum window
    // (architecture.md VacuumModule: "1/64-1/16 note... e.g. 1/32 note at
    // current tempo"; see processSegment()'s ImpactEngine doc comment for
    // this phase's Peak/Impact handoff design). Clamped to a Style-authored
    // millisecond floor so extreme tempos never produce a too-short-to-be-
    // click-free window.
    const double vacuumWindowSamples = musicalClock.divisionToSamples(style.vacuumDivision, false);
    const double vacuumFloorSamples = currentSampleRate * style.vacuumFloorSeconds;
    d.peakSamples = (int64_t) std::llround(juce::jmax(vacuumWindowSamples, vacuumFloorSamples));

    // Impact: ImpactEngine's own burst/sub/tail window, Style-authored
    // (architecture.md: "typically 50-300ms").
    d.impactSamples = (int64_t) std::llround(currentSampleRate * style.impactWindowSeconds);

    // Recovery: Style-authored musical division (architecture.md:
    // "Style-configurable, e.g. 1/8-1 bar").
    d.recoverySamples = (int64_t) std::llround(musicalClock.divisionToSamples(style.recoveryDivision, false));

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

    // Stage 3 DSP Phase 3.5: the `style` APVTS parameter is latched for the
    // ENTIRE upcoming build right here (architecture.md DropStyle:
    // "Switching style while a build is in-flight... is deferred until the
    // state machine returns to Idle" - reading it only at Arming, and
    // reusing this value for the whole build via activeStyleIndexForBuild,
    // implements that deferral naturally).
    activeStyleIndexForBuild = juce::jlimit(0, drop::kNumStyles - 1, (int) styleParam->load());
    const auto& style = drop::getStyle(activeStyleIndexForBuild);

    // Stage 3 DSP Phase 3.3: Chaos RNG draws happen ONLY here, at Arming
    // (architecture.md DropRandomizer: "RNG draws happen only at
    // well-defined points: once at Arming... latching this build's jitter
    // values"). Reseeded from the persisted `chaos` seed EVERY Arming (see
    // DropRandomizer.h's class doc comment for the determinism rationale).
    dropRandomizer.setSeed(getChaosSeed());
    dropRandomizer.setChaosAmount01(chaosParam->load() / 100.0f);
    dropRandomizer.drawForNewBuild();

    pitchModule.setSemitoneTarget(dropRandomizer.getPitchSemitoneChoice());
    pitchModule.setMode(style.pitchMode);
    stutterModule.setMode(static_cast<drop::StutterMode>(dropRandomizer.getStutterModeChoice()));
    noiseModule.setSeed(dropRandomizer.getSeed());
    noiseModule.setColor(style.noiseColor);
    impactEngine.setSeed(dropRandomizer.getSeed()); // deterministic burst noise (a "seeded equivalent" of NoiseModule's generator, internally XORed - see ImpactEngine::kSeedXorConstant)

    // Stage 3 DSP Phase 3.5: Style-driven mode/curve-type selection (data-
    // driven, per architecture.md DROPSTYLE: "Do not hard-code large switch
    // statements throughout DSP" - the switch/table lives in DropStyle.cpp's
    // data authoring, not here or in any DSP module).
    filterModule.setMode(style.filterMode);
    saturationModule.setMode(style.saturationMode);
    transitionEngine.setCurveType(style.transitionCurveType);

    // Stage 3 DSP Phase 3.5: the previously-deferred Gate/Delay division
    // Chaos-jitter consumers (architecture.md Integration Points:
    // "DropRandomizer (Chaos) -> ... GateModule pattern/rate, DelayModule
    // division") are wired here - musically-constrained choices drawn above
    // by dropRandomizer.drawForNewBuild(), latched for this build's duration.
    gateModule.setChaosDivision(dropRandomizer.getGateDivisionChoice());
    delayModule.setChaosDivision(dropRandomizer.getDelayDivisionChoice());
    // The remaining deferred consumer - per-module IntensityBudget intensity
    // jitter (dropRandomizer.getIntensityJitter()) - is consumed every
    // segment inside processSegment() via IntensityBudget::computeWeights()'s
    // `chaosJitter01` argument, not here (it is read repeatedly for the
    // duration of the build, not a one-shot "apply at Arming" setter).

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

    // Stage 3 DSP Phase 3.5: AudioAnalyzer tap (architecture.md Processing
    // Order step 2: "runs in parallel off the same pre-chain signal... order
    // relative to [the dry capture] doesn't matter"). READ-ONLY - never
    // modifies dryCaptureBuffer. Its SourceProfile + metrics feed
    // IntensityBudget::computeWeights() below (via processSegment()).
    audioAnalyzer.analyze(dryCaptureBuffer, numSamples);

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

    // Stage 3 DSP Phase 3.5: push one visualization frame per processBlock()
    // call (architecture.md Thread Boundaries: "pushed once per processed
    // block (or once per sub-block segment)" - block granularity chosen
    // here, simplest option explicitly allowed by that sentence). Uses the
    // LAST segment's computed weights/progress within this block (cached by
    // processSegment()) - a per-block snapshot is sufficient for a
    // ~30-60Hz GUI drain rate (Stage 3 GUI Phase 4.3, not wired yet).
    {
        VisualizationFrame frame;
        frame.progress = lastComputedProgress;
        frame.tensionLevel = tensionParam->load() / 100.0f;
        frame.currentState = (int) dropStateMachine.getState();
        frame.inputRMS = audioAnalyzer.getMetrics().rms;
        for (int i = 0; i < 10; ++i)
            frame.activeModuleWeights[i] = lastComputedWeights.value[(size_t) i];
        pushVisualizationFrame(frame);
    }

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
    // (macroParam[i] / 100)"). As of Phase 3.3, all 10 weights are consumed
    // by a real DSP module below (Pitch/Stutter/Digital/Noise joined
    // Filter/Width/Gate/Delay/Reverb/Distortion from Phase 3.2).
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
    const float tension01 = tensionParam->load() / 100.0f;
    const float chaos01 = chaosParam->load() / 100.0f;

    // Stage 3 DSP Phase 3.5: the deferred per-module IntensityBudget
    // intensity-jitter consumer (architecture.md Integration Points:
    // "DropRandomizer (Chaos) -> IntensityBudget weight jitter") - read
    // every segment (pure getters, no new RNG draws; the underlying values
    // were latched once at Arming by dropRandomizer.drawForNewBuild()).
    std::array<float, (size_t) drop::BudgetModule::Count> chaosJitter {};
    for (size_t i = 0; i < (size_t) drop::BudgetModule::Count; ++i)
        chaosJitter[i] = dropRandomizer.getIntensityJitter((int) i);

    const auto& style = drop::getStyle(activeStyleIndexForBuild);
    const auto weights = drop::IntensityBudget::computeWeights(style, progress, macroPercent, intensity01,
                                                                 tension01, chaos01, audioAnalyzer.getSourceProfile(),
                                                                 chaosJitter);

    // Cached for the visualization-frame push at the end of processBlock()
    // (see that call site's doc comment).
    lastComputedWeights = weights;
    lastComputedProgress = progress;

    const float impactAmount01 = juce::jlimit(0.0f, 1.0f, impactParam->load() / 100.0f);

    // Stage 3 DSP Phase 3.4: ImpactEngine's coordination-drive values
    // (architecture.md: ImpactEngine "drives existing modules into a
    // specific configuration") are computed BEFORE the series chain below
    // so the SAME-segment WidthModule/SaturationModule calls can be driven
    // by them. Non-zero ONLY during the Impact state (state-gated, like
    // VacuumModule below) - architecture.md Processing Chain "Routing
    // notes": "VacuumModule and ImpactEngine are state-gated, not
    // weight-blended."
    drop::ImpactEngine::DriveValues impactDrive {};
    if (dropStateMachine.getState() == drop::DropState::Impact)
    {
        const int64_t total = dropStateMachine.currentStateLengthSamples();
        const int64_t remaining = dropStateMachine.samplesUntilNextTransition();
        const int64_t elapsedAtSegmentStart = total - remaining;
        impactDrive = drop::ImpactEngine::computeDriveValues((double) elapsedAtSegmentStart, (double) total, impactAmount01);
    }

    // Fixed series order per architecture.md Processing Order step 4:
    // Filter -> Width -> Gate -> Pitch -> Stutter -> Delay -> Saturation ->
    // Digital, then Noise as a PARALLEL ADD (step 5, not a series insert),
    // then Reverb (Phase 3.2's placeholder Building/Recovery-ambience
    // position - Impact's own SEPARATE short-tail reverb instance lives
    // inside ImpactEngine, not here). Every IntensityBudget-managed module
    // always processes (architecture.md "Always-Process, Weight-Mix") -
    // weight blends its contribution, never bypasses it.
    filterModule.process(segmentBlock, weights.value[(size_t) drop::BudgetModule::Filter], progress);
    widthModule.process(segmentBlock, weights.value[(size_t) drop::BudgetModule::Width], progress, impactDrive.widthExpansionAmount);
    gateModule.process(segmentBlock, weights.value[(size_t) drop::BudgetModule::Gate], progress, musicalClock);
    pitchModule.process(segmentBlock, weights.value[(size_t) drop::BudgetModule::Pitch], progress);
    stutterModule.process(segmentBlock, weights.value[(size_t) drop::BudgetModule::Stutter], progress, musicalClock);
    delayModule.process(segmentBlock, weights.value[(size_t) drop::BudgetModule::Delay], progress, musicalClock);

    // SaturationModule: Impact-state overlay (architecture.md ImpactEngine
    // "light SaturationModule pass") is blended in via max() - never LESS
    // saturation than the shared budget alone would produce, but also never
    // double-applies when the budget weight already exceeds the light
    // overlay amount (it won't, since IntensityBudget's weight is 0
    // throughout Peak AND Impact - see computeIntensityProgress() - but
    // max() is the correct, order-independent way to express "at least this
    // much").
    const float saturationWeight = juce::jmax(weights.value[(size_t) drop::BudgetModule::Distortion], impactDrive.saturationOverlayAmount);
    saturationModule.process(segmentBlock, saturationWeight, progress);

    digitalModule.process(segmentBlock, weights.value[(size_t) drop::BudgetModule::Digital], progress,
                           tension01, chaos01);
    noiseModule.process(segmentBlock, weights.value[(size_t) drop::BudgetModule::Noise], progress, 0.6f);
    reverbModule.process(segmentBlock, weights.value[(size_t) drop::BudgetModule::Reverb], progress);

    // VacuumModule: state-gated (NOT IntensityBudget weight-blended,
    // architecture.md Processing Chain "Routing notes") - applied to the
    // WHOLE post-chain bus only during the DropStateMachine Peak state.
    // Stage 3 DSP Phase 3.4: Peak's ENTIRE latched duration now IS the
    // Vacuum window (see computeDurationsForNewBuild()) - the segmentation
    // loop in processBlock() guarantees the entire segment stays within
    // Peak before this runs (segments are split exactly at state
    // transitions).
    if (dropStateMachine.getState() == drop::DropState::Peak)
    {
        const int64_t total = dropStateMachine.currentStateLengthSamples();
        const int64_t remaining = dropStateMachine.samplesUntilNextTransition();
        const int64_t elapsedAtSegmentStart = total - remaining;
        vacuumModule.process(segmentBlock, (double) elapsedAtSegmentStart, (double) total, impactAmount01);
    }

    // ImpactEngine: state-gated to Impact (Stage 3 DSP Phase 3.4). Peak's
    // Vacuum collapse ramps its floor back up to unity EXACTLY as Peak ends
    // (VacuumModule::computeGain() returns 1.0 at elapsed >= totalWindow) -
    // the segmentation loop guarantees a fresh segment starts at Impact's
    // very first sample, so ImpactEngine's own attack begins exactly where
    // Vacuum's release ends, with NO overlap and therefore no
    // double-envelope bump (architecture.md VacuumModule config: "the ramp
    // back up and Impact's attack are designed together so there is no
    // double-envelope bump"). computeIntensityProgress() already ducked the
    // 10 shared-budget modules to 0 starting back at Peak entry (so their
    // ~20ms internal smoothers are GENUINELY settled by now, not merely
    // targeting 0 - a post-3.4-review bugfix) and holds them there through
    // this Impact state too (TRANSIENT PRESERVATION: "duck the effect
    // chain, not the source") - ImpactEngine's own additive sub-hit/burst/
    // tail content is layered on top of that ducked (near-dry) bus here.
    if (dropStateMachine.getState() == drop::DropState::Impact)
    {
        const int64_t total = dropStateMachine.currentStateLengthSamples();
        const int64_t remaining = dropStateMachine.samplesUntilNextTransition();
        const int64_t elapsedAtSegmentStart = total - remaining;
        impactEngine.process(segmentBlock, (double) elapsedAtSegmentStart, (double) total, impactAmount01);
    }
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
            // Stage 3 DSP Phase 3.4 bugfix (TRANSIENT PRESERVATION,
            // implementation-spec.md: "duck the effect chain, not the
            // source"): the 10 shared-budget modules snap to 0 starting at
            // PEAK ENTRY (not Impact entry) - each module's OWN internal
            // weightSmoothed ramp (~20ms) needs real elapsed time to settle
            // fully to 0, and Peak's ENTIRE latched duration (>= the active
            // Style's vacuumFloorSeconds even at the floor-clamped
            // extreme-tempo edge case - see computeDurationsForNewBuild())
            // comfortably provides that, so by the
            // time Impact's first sample arrives the shared chain is
            // GENUINELY transparent (weight exactly 0, smoothers settled),
            // not merely "targeting 0." Ducking during Peak is also
            // musically free: VacuumModule multiplies the whole bus down
            // during this same window anyway (architecture.md ImpactEngine
            // config: "weights the state machine snaps toward ~0 for most
            // modules right at Impact" - interpreted here as starting the
            // snap at the Vacuum collapse that immediately precedes it, per
            // coordinator guidance, since a lingering ~20ms of stale
            // pre-collapse weight is exactly what let a test impulse placed
            // at Impact's first sample get partially reprocessed before this
            // fix). ImpactEngine's own sub/burst/width-expansion/light-
            // saturation/tail additions are layered on top of this ducked
            // bus in processSegment(), independently of this progress value.
            return 0.0f;

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

void DROPAudioProcessor::setCurrentProgram(int index)
{
    // Message-thread-only (standard JUCE AudioProcessor::setCurrentProgram()
    // contract - hosts call this off the audio thread; the actual parameter
    // writes below go through the normal APVTS setValueNotifyingHost() path,
    // identical to any other host-driven parameter change).
    index = juce::jlimit(0, drop::kNumPresets - 1, index);
    currentProgramIndex = index;
    const auto& preset = drop::getPreset(index);

    auto setParam = [this](const char* id, float rawValue)
    {
        if (auto* p = parameters.getParameter(id))
            p->setValueNotifyingHost(p->convertTo0to1(rawValue));
    };

    setParam("style", (float) preset.styleIndex);
    setParam("buildLength", (float) preset.buildLengthChoiceIndex);
    setParam("intensity", preset.intensity);
    setParam("tension", preset.tension);
    setParam("chaos", preset.chaos);
    setParam("impact", preset.impact);
    setParam("dryWet", preset.dryWet);
    setParam("output", preset.outputDb);

    // architecture.md "Resolved Open Questions" #2: the 10 module macros
    // default to 100% (unipolar direct multiplier, "module behaves exactly
    // as the selected DropStyle authored it") - a loaded preset is not a
    // separate data structure from its Style (architecture.md Presets: "the
    // 16 presets ARE the 16 DropStyles at their recommended macro
    // defaults"), so resetting all 10 macros to 100% here guarantees the
    // Style is heard exactly as authored, not pre-attenuated.
    static constexpr const char* kMacroIds[10] = {
        "filter", "width", "pitch", "gate", "stutter", "delay", "reverb", "distortion", "digital", "noise"
    };
    for (auto* id : kMacroIds)
        setParam(id, 100.0f);
}

void DROPAudioProcessor::pushVisualizationFrame(const VisualizationFrame& frame) noexcept
{
    // Audio-thread-only (see architecture.md Thread Boundaries). Lock-free:
    // juce::AbstractFifo's write-side is safe for exactly one writer
    // concurrent with exactly one reader (popVisualizationFrame(), message
    // thread) - matches this class's usage exactly.
    int start1, size1, start2, size2;
    visualizationFifo.prepareToWrite(1, start1, size1, start2, size2);
    if (size1 > 0)
        visualizationBuffer[(size_t) start1] = frame;
    else if (size2 > 0)
        visualizationBuffer[(size_t) start2] = frame;
    visualizationFifo.finishedWrite(size1 + size2);
    // size1+size2 == 0 (FIFO full - GUI not draining fast enough, or not yet
    // attached) is a silent, harmless drop of this one frame: the next
    // processBlock() call will simply try again. No allocation, no lock,
    // never blocks the audio thread.
}

bool DROPAudioProcessor::popVisualizationFrame(VisualizationFrame& outFrame) noexcept
{
    // Message-thread-only (see VisualizationFrame's public doc comment).
    int start1, size1, start2, size2;
    visualizationFifo.prepareToRead(1, start1, size1, start2, size2);
    if (size1 > 0)
        outFrame = visualizationBuffer[(size_t) start1];
    else if (size2 > 0)
        outFrame = visualizationBuffer[(size_t) start2];
    else
    {
        visualizationFifo.finishedRead(0);
        return false;
    }
    visualizationFifo.finishedRead(size1 + size2);
    return true;
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
