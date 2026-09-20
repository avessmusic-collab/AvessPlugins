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
    // POST-1.0 (2026-09-17, user-directed: "ad a 32 bar choice"): "32"
    // appended as a 6th choice. APVTS session state stores the actual
    // choice index, so saved sessions (indices 0-4) reload identically;
    // only NORMALIZED host automation lanes recorded on this parameter
    // remap (the old top-of-lane 16 now sits at ~0.8, 1.0 is now 32).
    layout.add(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID { "buildLength", 1 },
        "Build Length",
        juce::StringArray { "1", "2", "4", "8", "16", "32" },
        4)); // default: 16 bars (index 4; was 4 bars/index 2 - changed 2026-09-19, user-directed "make default 16 bar selected". Saved sessions carry their own value.)

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
        // POST-1.0 SEVENTH IMPROVEMENT SESSION (2026-09-18, user-directed:
        // "make default dry wet 50%") - default 100.0f -> 50.0f (factory
        // presets updated to author 50% too, see DropPreset.cpp). Saved
        // sessions carry their own value and are unaffected.
        50.0f,
        "%"));

    // --- Output (Advanced page) -----------------------------------------

    // output - final output gain stage, smoothed.
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "output", 1 },
        "Output",
        juce::NormalisableRange<float>(-60.0f, 12.0f, 0.1f, 1.0f),
        0.0f,
        "dB"));

    // --- POST-1.0 addition (2026-09-16, user-directed improvement) --------
    // tail - the 20th parameter, added STRICTLY additively at the END of the
    // layout (parameter IDs 1-19 above are byte-for-byte untouched). See
    // PluginProcessor.h's top doc comment "POST-1.0 IMPROVEMENT" and
    // computeDurationsForNewBuild()'s doc comment for the exact exponential
    // seconds mapping. A 1.0.0 session saved before this parameter existed
    // loads with the default below (APVTS's standard, well-documented
    // behavior for a ValueTree missing a since-added parameter's node -
    // parameters.replaceState() simply leaves any parameter absent from the
    // loaded state at its constructor-supplied default, verified against
    // this repo's existing getStateInformation()/setStateInformation()
    // round-trip pattern, which already relies on the same guarantee for
    // every parameter).
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "tail", 1 },
        "Tail",
        juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f, 1.0f),
        // POST-1.0 SIXTH IMPROVEMENT SESSION (2026-09-17, user-directed:
        // "make the default tail zero") - default 35.0f -> 0.0f: a fresh
        // instance now CUTS cleanly at the drop (tail==0 maps to the
        // ~5ms kTailCutSeconds fast-fade window, see
        // computeDurationsForNewBuild()); ring-out is opt-in via the knob.
        0.0f,
        "%"));

    // --- POST-1.0 FOURTH IMPROVEMENT SESSION addition (2026-09-17, user-
    // directed) --------------------------------------------------------
    // noiseTension - the 21st parameter, added STRICTLY additively at the
    // END of the layout (parameter IDs 1-20 above are byte-for-byte
    // untouched). Shapes HOW NoiseModule's white-noise riser builds (see
    // PluginProcessor.h's top doc comment and NoiseModule.h's
    // riseAmountForProgress() doc comment for the exact shape family) -
    // does NOT change the noise LEVEL (still the `noise` macro above). At
    // its 50% default, drop::NoiseModule::riseAmountForProgress() reproduces
    // the pre-noiseTension smoothstep bit-for-bit, so a session saved before
    // this parameter existed (APVTS's standard missing-parameter-node ->
    // constructor-default behavior, same guarantee `tail` above already
    // relies on) loads and sounds byte-identical to the pre-noiseTension
    // build.
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "noiseTension", 1 },
        "Noise Curve", // UI chip reads "NOISE"; DAW name stays distinct from the "Noise" macro so automation lists are unambiguous (ID unchanged - sessions/automation intact)
        juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f, 1.0f),
        // Default 50 -> 0, POST-1.0 2026-09-19 (user-directed: "make the
        // noise curve to be default where the 0% is") - a fresh instance
        // uses the earliest-bloom shape (k = 2^-1.2, riser audible sooner
        // in the build); 50% remains the neutral point where the shape
        // family reproduces the base dB-linear curve exactly. Saved
        // sessions carry their own value and are unaffected.
        0.0f,
        "%"));

    // --- POST-1.0 SEVENTH IMPROVEMENT SESSION addition (2026-09-18, user-
    // directed: "add choices for different kinds of noise, search what
    // noises and sounds are used to make risers") ------------------------
    // noiseType - the 22nd parameter, added STRICTLY additively at the END
    // of the layout (IDs 1-21 untouched). Selects NoiseModule's riser
    // SOURCE, grounded in the standard riser sound-design palette (see
    // NoiseModule.h's NoiseType enum doc). Index 0 "STYLE" (the DEFAULT)
    // defers to the active DropStyle's authored NoiseColor - the exact
    // pre-noiseType behavior, so old sessions (missing node -> constructor
    // default, same guarantee as `tail`/`noiseTension`) load bit-identical.
    layout.add(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID { "noiseType", 1 },
        "Noise Type",
        juce::StringArray { "STYLE", "WHITE", "PINK", "BROWN", "SWEEP", "SAW", "SHEPARD" },
        // Default 0 (STYLE) -> 1 (WHITE), POST-1.0 2026-09-18 (user-
        // directed: "make the default noise white") - a fresh instance now
        // always risers with classic white noise; the Style-authored color
        // remains available as the explicit STYLE choice. Choice indices
        // unchanged (sessions saved with any selection reload identically).
        1));

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
    tailParam = parameters.getRawParameterValue("tail"); // POST-1.0 addition, 20th parameter
    noiseTensionParam = parameters.getRawParameterValue("noiseTension"); // POST-1.0 FOURTH IMPROVEMENT SESSION addition, 21st parameter
    noiseTypeParam = parameters.getRawParameterValue("noiseType"); // POST-1.0 SEVENTH IMPROVEMENT SESSION addition, 22nd parameter

    // Stage 3 GUI Phase 4.2: event-latched trigger edge-detection (see
    // PluginProcessor.h's parameterChanged()/triggerEventPendingFlag doc
    // comments) - registered on "trigger" only, not all 19 parameters.
    parameters.addParameterListener("trigger", this);
    triggerParamObject = parameters.getParameter("trigger");
}

DROPAudioProcessor::~DROPAudioProcessor()
{
    parameters.removeParameterListener("trigger", this);
}

void DROPAudioProcessor::parameterChanged(const juce::String& parameterID, float newValue)
{
    // Real-time-safety: this callback can be invoked from ANY thread
    // depending on who changed the parameter - the message thread for a UI
    // click (WebToggleButtonParameterAttachment -> setValueNotifyingHost()),
    // or potentially the audio thread for host automation (VST3/AU wrappers
    // typically apply automation immediately before calling processBlock()).
    // Only atomic exchange/store operations are performed here, safe on
    // every thread (see this method's declaration-site doc comment in
    // PluginProcessor.h for the full rationale).
    //
    // RISING-EDGE test performed HERE (per actual value-change event), not
    // in processBlock() - see triggerEventEdgeSeenHigh's doc comment in
    // PluginProcessor.h for why a host re-sending an already-high value must
    // NOT be treated as a fresh edge.
    if (parameterID != "trigger")
        return;

    const bool high = (newValue > 0.5f);
    const bool wasHigh = triggerEventEdgeSeenHigh.exchange(high, std::memory_order_acq_rel);
    if (high && !wasHigh)
        triggerEventPendingFlag.store(true, std::memory_order_release);
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
    // Clear any stale unconsumed rising-edge latch/state (Stage 3 GUI Phase
    // 4.2 - see PluginProcessor.h's triggerEventEdgeSeenHigh/
    // triggerEventPendingFlag doc comments) so a (re)prepare never auto-
    // fires a trigger the user didn't just press, and so the "previous
    // value" half of the edge test starts fresh (matches the old polling
    // code's own `previousTriggerParamValue = false;` reset here).
    triggerEventEdgeSeenHigh.store(false, std::memory_order_relaxed);
    triggerEventPendingFlag.store(false, std::memory_order_relaxed);

    // POST-1.0 bugfix state (see this class's top doc comment "POST-1.0
    // IMPROVEMENT") - mirrors dropStateMachine.reset() above: a (re)prepare
    // never resumes mid-build, so the tail-envelope carryover state must
    // restart fresh too.
    previousObservedState = drop::DropState::Idle;
    frozenDelayReturnWeight = 0.0f;
    frozenReverbReturnWeight = 0.0f;
    impactCarryoverElapsedSamples = -1;

    // POST-1.0 TRANSPORT-STOP SAFETY state (see PluginProcessor.h's top doc
    // comment "POST-1.0 SECOND IMPROVEMENT SESSION", Bug E) - mirrors the
    // reset block immediately above: a (re)prepare never resumes an
    // in-flight abort fade either.
    transportStopAbortActive = false;
    transportStopFadeElapsedSamples = 0;
    transportStopFadeTotalSamples = 0;

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

    // Stage 3 DSP Phase 3.4/3.5: Peak's ENTIRE duration IS the Vacuum window
    // (architecture.md VacuumModule: "1/64-1/16 note... e.g. 1/32 note at
    // current tempo"; see processSegment()'s ImpactEngine doc comment for
    // this phase's Peak/Impact handoff design). Clamped to a Style-authored
    // millisecond floor so extreme tempos never produce a too-short-to-be-
    // click-free window.
    //
    // POST-1.0 GRID-LOCK FIX (2026-09-16, user-reported real-DAW bug: "not
    // locking to the grid at all - as if it's using its own clock" - see
    // PluginProcessor.h's top doc comment "POST-1.0 SECOND IMPROVEMENT
    // SESSION", Bug D): computed HERE, BEFORE `buildingSamples` below (moved
    // up from its original position after Building), because Building now
    // needs this value to carve the Vacuum window OUT of the requested
    // bar-length duration instead of appending it afterward - see
    // `buildingSamples`'s own comment immediately below for the musical
    // rationale/exact math.
    const double vacuumWindowSamples = musicalClock.divisionToSamples(style.vacuumDivision, false);
    const double vacuumFloorSamples = currentSampleRate * style.vacuumFloorSeconds;
    d.peakSamples = (int64_t) std::llround(juce::jmax(vacuumWindowSamples, vacuumFloorSamples));

    // Building: buildLength (bars, choice index -> bar count) -> samples,
    // tempo/time-signature latched now (architecture.md: "a tempo change
    // mid-build does not shorten/lengthen an in-flight build's
    // sample-position targets").
    static constexpr int kBuildLengthBars[] = { 1, 2, 4, 8, 16, 32 }; // POST-1.0: "32" appended 2026-09-17 (user-directed)
    const int buildLengthChoiceIndex = (int) buildLengthParam->load();
    const int bars = kBuildLengthBars[(size_t) juce::jlimit(0, 5, buildLengthChoiceIndex)];
    const int64_t barsSamples = (int64_t) std::llround(musicalClock.barsToSamples(bars));

    // POST-1.0 GRID-LOCK FIX (Bug D, continued): the user builds toward a
    // downbeat that is `bars` bars past the quantized Arming start point -
    // i.e. Impact's first sample (the actual "drop") must land EXACTLY at
    // armingToBuildingSamples + barsSamples, measured from the moment
    // Arming began. Previously `buildingSamples` was set to the FULL
    // barsSamples, and Peak's Vacuum-collapse window ran AFTERWARD on top of
    // that - so Impact always arrived `peakSamples` samples LATE relative to
    // the downbeat (>= the Style's vacuumFloorSeconds floor, i.e. >= 25ms
    // for the default Style, more at slow tempi/long vacuum divisions).
    // Fixed: the Vacuum window is carved OUT of the requested bar-length
    // duration instead of appended after it, so
    // (armingToBuildingSamples + buildingSamples + peakSamples) ==
    // (armingToBuildingSamples + barsSamples) EXACTLY, landing Impact's
    // first sample precisely on the downbeat.
    //
    // CLAMP: floor-clamped to >= 1 sample (never <= 0) so Building always
    // exists as a genuine, nonzero-duration state - matters only in the
    // pathological edge case where the Style's Vacuum window is LONGER than
    // the ENTIRE requested build (e.g. a 1-bar build at a very slow tempo
    // combined with a large vacuumFloorSeconds), which makes perfect
    // grid-lock mathematically impossible (the vacuum collapse alone
    // doesn't fit inside the requested bar count) - in that rare case Impact
    // overruns the downbeat by (peakSamples - barsSamples + 1) samples as
    // the least-bad fallback, exactly mirroring the ORIGINAL (pre-fix)
    // behavior's own kind of overrun, just bounded to the unavoidable
    // minimum instead of always paying the full peakSamples penalty.
    d.buildingSamples = juce::jmax((int64_t) 1, barsSamples - d.peakSamples);

    // Impact: ImpactEngine's own burst/sub/tail window, Style-authored
    // (architecture.md: "typically 50-300ms").
    d.impactSamples = (int64_t) std::llround(currentSampleRate * style.impactWindowSeconds);

    // Recovery: POST-1.0 bugfix (2026-09-16, user-directed improvement) -
    // the `tail` APVTS parameter now SETS Recovery's duration directly,
    // OVERRIDING the active Style's own `recoveryDivision` timing preference
    // (style.recoveryDivision is intentionally left unused here - see
    // Source/Core/DropStyle.h's doc comment at that field for the override
    // note). Recovery IS the tail's ring-out window (PluginProcessor.h's
    // top doc comment "POST-1.0 IMPROVEMENT" / processSegment()'s "tail
    // envelope" doc comment): DelayModule/ReverbModule/ImpactEngine's
    // wet-return decays via a raised cosine across exactly this duration.
    //
    // EXACT MAPPING (exponential/log-linear, so equal steps in `tail` feel
    // like roughly equal PROPORTIONAL changes in ring-out time, matching how
    // decay-time controls are conventionally scaled):
    //   tailSeconds = kTailMinSeconds * (kTailMaxSeconds / kTailMinSeconds) ^ (tail / 100)
    // tail=0% -> exactly kTailMinSeconds (0.15s); tail=100% -> exactly
    // kTailMaxSeconds (4.0s); tail=35% (the default) -> ~0.47s. Independent
    // of tempo/time-signature (unlike the other Style-driven timings above,
    // which are all musical-division-based) - a deliberate choice, since a
    // ring-out/decay-time control is conventionally an absolute-time
    // parameter (like a reverb's decay time), not a musical subdivision.
    //
    // POST-1.0 "TAIL = CUT" FIX (2026-09-16, user-directed - see
    // PluginProcessor.h's top doc comment "POST-1.0 SECOND IMPROVEMENT
    // SESSION", Bug F): tail == 0% (within kTailCutEpsilon, guarding only
    // against float-representation slack at the parameter's exact minimum -
    // not a UI deadzone) is special-cased to a very short kTailCutSeconds
    // (~5ms) Recovery duration INSTEAD of kTailMinSeconds (150ms) - the
    // EXISTING raised-cosine tail envelope in processSegment() (completely
    // unchanged) then produces a fast, click-free fade-to-silence over that
    // short window, audibly an instant clean stop, followed by the same
    // guaranteed module-hygiene reset() every Recovery->Idle transition
    // already performs. Every tail value ABOVE the epsilon is byte-identical
    // to the pre-fix exponential formula (the `else` branch below is exactly
    // the original single-line computation) - "no discontinuity in feel"
    // just above 0% (still ~kTailMinSeconds) is therefore automatic, since
    // only the single point AT (within epsilon of) 0% is redirected to a
    // different value, not blended into the curve.
    static constexpr double kTailMinSeconds = 0.15;
    static constexpr double kTailMaxSeconds = 4.0;
    static constexpr double kTailCutSeconds = 0.005; // ~5ms - matches kTransportStopFadeSeconds's own "stop everything cleanly, fast" order of magnitude (PluginProcessor.h)
    static constexpr float kTailCutEpsilon = 1.0e-4f; // `tail` is a 0-100 percent value here (tailParam->load(), NOT the /100 tail01 below) - this guards only against float slack at the parameter's exact minimum, not a perceptible UI deadzone
    const float tailRaw = tailParam->load();
    double tailSeconds;
    if (tailRaw <= kTailCutEpsilon)
    {
        tailSeconds = kTailCutSeconds;

        // POST-1.0 "TRUE CUT" FIX (2026-09-18, user-directed: "count the
        // bars with bpm and make it precise to miliseconds. so when the
        // effect of 8 bars for example ends. when there is no tail and cut
        // is active. so the sound stops imidietly volumewise"). TAIL=CUT
        // previously shortened only RECOVERY - the Impact STATE still ran
        // its full Style-authored window (~220-240ms) past the downbeat,
        // with the dedicated shortTailReverb being excited at the drop and
        // the drive overlays active, so the effect audibly lingered ~a
        // quarter second past the bar line it had just landed on. With CUT
        // active the ENTIRE post-drop window now collapses to the same
        // ~5ms: Impact state = kTailCutSeconds (its only remaining job is
        // handing the untouched dry return through), Recovery =
        // kTailCutSeconds (the existing per-sample dB envelope fades any
        // residual wet to an exact 0), and tailCutActiveForBuild (latched
        // here, like every other per-build duration decision) additionally
        // gates the shortTailReverb's SEND to 0 during Impact so there is
        // nothing to ring in the first place. Net: the effect's own sound
        // ends within ~10ms (~= "millisecond-precise") of the bar-counted
        // BPM-exact downbeat; every tail ABOVE the epsilon keeps the full
        // Style-authored Impact window byte-identically.
        d.impactSamples = (int64_t) std::llround(currentSampleRate * kTailCutSeconds);
        tailCutActiveForBuild = true;
    }
    else
    {
        const float tail01 = juce::jlimit(0.0f, 1.0f, tailRaw / 100.0f);
        tailSeconds = kTailMinSeconds * std::pow(kTailMaxSeconds / kTailMinSeconds, (double) tail01);
        tailCutActiveForBuild = false;
    }
    d.recoverySamples = (int64_t) std::llround(currentSampleRate * tailSeconds);

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

    // POST-1.0 TRANSPORT-STOP SAFETY (see this method's declaration-site doc
    // comment in PluginProcessor.h and processBlock()'s "transport-stop" doc
    // comment): refuse to arm a NEW build while a VALID host playhead
    // reports the transport stopped. The standalone/no-playhead fallback
    // transport (isUsingFallbackTransport() == true) is NEVER blocked here -
    // it has no concept of "stopped" and must remain fully usable.
    if (!musicalClock.isUsingFallbackTransport() && !musicalClock.isHostPlaying())
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

void DROPAudioProcessor::advanceStateMachineTicks() noexcept
{
    // See this method's declaration-site doc comment in PluginProcessor.h.
    // previousObservedState is updated on every ACTUAL transition (not
    // polled once per block), so a Recovery state shorter than one segment
    // can never be missed - mirrors this class's existing trigger-edge-
    // detection rationale (parameterChanged()'s doc comment), applied here
    // to a different edge (Recovery->Idle instead of the trigger parameter's
    // low->high).
    while (dropStateMachine.tick())
    {
        const auto newState = dropStateMachine.getState();
        if (previousObservedState == drop::DropState::Recovery && newState == drop::DropState::Idle)
        {
            // POST-1.0 bugfix (Bug C "stale tails" - see this class's top
            // doc comment "POST-1.0 IMPROVEMENT"): the tail envelope
            // (processSegment()'s doc comment) has, by construction, reached
            // EXACTLY 0 at this precise transition - a guaranteed click-free
            // instant to clear internal DSP state so nothing stale replays
            // into the NEXT build. Deliberately NOT done at Idle->Arming
            // (audio is still live then, unlike this Recovery-exhausted
            // moment where the tail's own output contribution is already 0).
            //
            // POST-1.0 SECOND-PASS FIX (coordinator-reported build/test
            // feedback, "Bug C reset not effective"): the reset scope is
            // widened beyond the original four modules. Root cause: EVERY
            // module in the fixed series chain is legitimately fed a
            // small-but-nonzero live weight even while "ducked" (see
            // processSegment()'s doc comment above - DropStyle.cpp's
            // budgetEnvelope/moduleCurves are authored with nonzero
            // startWeight, e.g. 0.05, for Filter/Delay/Reverb/Digital - this
            // was already true, unmodified, throughout the ENTIRE prior
            // architecture, including Idle/Arming/Building/Peak/Impact of
            // every build). Every module with persistent internal filter/
            // oscillator state (StateVariableTPTFilter, crossover one-poles,
            // DC blockers, the granular delay line, etc.) was therefore
            // ALREADY live-processing real signal throughout the previous
            // build, and NONE of them were ever reset between builds prior
            // to this improvement session - only DelayModule/ReverbModule/
            // ImpactEngine/StutterModule were in the originally-approved
            // reset scope, which was insufficient: FilterModule's resonant
            // StateVariableTPTFilter in particular can hold audible energy
            // well past the excitation that produced it. Resetting the
            // FULL set of owned DSP modules here (not just the four
            // explicitly tail-capable ones) is the only way to genuinely
            // guarantee "nothing stale replays into the next build" for
            // every module that can carry inter-build memory - measured to
            // be necessary via source inspection (DropStyle.cpp's authored
            // curve data) since a live rebuild/test cycle wasn't available
            // in this pass.
            //
            // POST-1.0 refactor (2026-09-16, TRANSPORT-STOP SAFETY session):
            // the twelve module .reset() calls that used to live directly
            // here are now factored into resetAllOwnedDspModules() so the
            // new transport-stop-abort finalize path (processBlock()) can
            // share the EXACT same sweep - see that method's declaration-site
            // doc comment in PluginProcessor.h.
            resetAllOwnedDspModules();

            // Frozen tail-envelope weights are pure per-build snapshots
            // (re-captured fresh every Building segment of the NEXT build
            // anyway - see the "if (dropState == Building)" capture above in
            // processSegment()) - reset to 0 here purely for defensive
            // hygiene/consistency with everything else in this block, not
            // because a stale value could otherwise leak (it provably
            // cannot: Recovery is the only state that reads them, and the
            // next build's Recovery cannot begin before its OWN Building
            // segment has already overwritten them).
            frozenDelayReturnWeight = 0.0f;
            frozenReverbReturnWeight = 0.0f;
            impactCarryoverElapsedSamples = -1;
        }
        previousObservedState = newState;
    }
}

void DROPAudioProcessor::resetAllOwnedDspModules() noexcept
{
    // See this method's declaration-site doc comment in PluginProcessor.h.
    // Identical set/order to the original inline sweep this was factored out
    // of (advanceStateMachineTicks()'s Recovery->Idle branch) - every owned
    // DSP module that carries persistent inter-block filter/oscillator/
    // buffer state.
    filterModule.reset();
    widthModule.reset();
    gateModule.reset();
    delayModule.reset();
    saturationModule.reset();
    reverbModule.reset();
    pitchModule.reset();
    stutterModule.reset();
    digitalModule.reset();
    vacuumModule.reset();
    noiseModule.reset();
    impactEngine.reset();
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
    // POST-1.0 TRANSPORT-STOP SAFETY (2026-09-16, user-directed - see this
    // class's top doc comment "POST-1.0 SECOND IMPROVEMENT SESSION", Bug E:
    // "artifacts then the sound stops"). A transition mid-flight (state !=
    // Idle) left running against a host transport that has STOPPED produces
    // exactly that symptom (noise riser still rising, GateModule still
    // chopping, delay/reverb tails still ringing, with no more musically-
    // continuous host audio arriving) - detected here, once per block,
    // immediately after musicalClock.update() has refreshed
    // isUsingFallbackTransport()/isHostPlaying() for this block.
    //
    // `wasAlreadyAborting` is captured BEFORE possibly flipping
    // transportStopAbortActive true this block, and gates whether the
    // normal trigger-detection/segmentation/processSegment() pipeline below
    // still runs this block:
    //   - false (not aborting, or THIS is the detection block): pipeline
    //     runs completely normally, generating exactly what would have
    //     played anyway - the abort fade below then converges THAT content
    //     to dry, so nothing before the fade point is altered/clicked.
    //   - true (continuing a fade that didn't finish within the detection
    //     block - only possible at unusually tiny host block sizes given
    //     kTransportStopFadeSeconds' ~5ms length): the pipeline is skipped
    //     entirely (the abort already decided "this build is over" - no
    //     further wet content should be generated); `buffer` therefore
    //     already holds raw dry input, and the fade loop below is a
    //     harmless dry->dry no-op that simply continues advancing the
    //     shared elapsed-sample counter until it completes.
    //
    // Gated on a VALID host playhead (!isUsingFallbackTransport()) - the
    // standalone app / bare-AudioProcessor fallback path has no concept of
    // "stopped" and must remain fully usable (every existing test that never
    // calls setPlayHead() uses this fallback and is therefore completely
    // unaffected by this feature).
    // ------------------------------------------------------------------
    const bool wasAlreadyAborting = transportStopAbortActive;

    if (!wasAlreadyAborting
        && dropStateMachine.getState() != drop::DropState::Idle
        && !musicalClock.isUsingFallbackTransport()
        && !musicalClock.isHostPlaying())
    {
        transportStopAbortActive = true;
        transportStopFadeElapsedSamples = 0;
        transportStopFadeTotalSamples = juce::jmax(
            (int64_t) 1, (int64_t) std::llround(currentSampleRate * kTransportStopFadeSeconds));
    }

    // NOTE: intentionally NOT re-indented below (avoids reflowing this
    // entire pre-existing block just to add one wrapping brace pair) - the
    // contained statements keep their original indentation despite now
    // being nested one level deeper inside this `if`.
    if (!wasAlreadyAborting)
    {
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
    // MIDI trigger: genuinely sample-accurate via MidiBuffer's per-event
    // sample offsets. Only scanned when triggerMode == Midi (unrelated MIDI
    // content on the same track shouldn't accidentally arm a build if the
    // user hasn't opted into MIDI triggering). TriggerMode::AutoSync is a
    // documented stub for this phase (architecture.md/plan.md Phase 3.1
    // scope: "auto may be a stub") - it currently behaves like Manual
    // (MIDI listener inactive, no auto-triggering logic yet).
    //
    // POST-1.0 THIRD IMPROVEMENT SESSION (2026-09-17, user-directed: "remove
    // midi note selection"): with no UI element left to pick a specific
    // trigger note (see PluginEditor.cpp - the "MIDI NOTE" chip and its
    // native-function bridge were removed), MIDI trigger mode now responds
    // to ANY note-on message, DOCUMENTED SUPERSESSION of the prior
    // note-number-matching behavior (`message.getNoteNumber() ==
    // configuredMidiNote`). This is the most useful behavior for the
    // remaining realistic use case - triggering from a MIDI clip or a pad on
    // a controller, where the exact note number is incidental. The
    // underlying `midiTriggerNote` atomic + its `<DropCustomState
    // midiNote="...">` persisted field are KEPT, unused by this detection
    // logic, purely so a session saved before this change (which may have
    // stored a non-default note) still loads without error (APVTS/ValueTree
    // convention: an unused-but-present field is harmless) - see
    // getMidiTriggerNote()/setMidiTriggerNote() and
    // setStateInformation()/getStateInformation() in this file, both
    // unchanged.
    // ------------------------------------------------------------------
    int triggerEventOffset = -1; // -1 == no trigger event this block

    // Stage 3 GUI Phase 4.2 fix: consume the event-latched flag set by
    // parameterChanged() (see PluginProcessor.h's doc comments on both that
    // override and triggerEventPendingFlag) rather than polling
    // `triggerParam->load()` once per call - the old polling approach could
    // miss a short UI pulse entirely at large block sizes since it only ever
    // sampled the parameter's value once per processBlock() call. The
    // resulting event is still applied at this block's sample offset 0 (the
    // finest granularity APVTS exposes for this parameter either way) -
    // exchange() atomically consumes the flag so it can only fire once per
    // rising edge, matching the old polling logic's semantics exactly.
    // POST-1.0 (2026-09-17, user DAW report: "the drop button isn't mapped
    // correctly" - host automation). ROOT CAUSE: juce::AudioProcessorValueTreeState
    // notifies its Listeners for HOST-driven parameter changes via a coalescing
    // message-thread timer, not synchronously per change. A drawn automation step
    // can therefore have its rising edge collapsed (the listener sees only the
    // settled value) and parameterChanged() never observes false->true, so the
    // drop never fires. A UI click still works because its setValueNotifyingHost()
    // call notifies immediately - which is exactly why this only showed up under
    // host automation.
    //
    // FIX: in addition to the listener, poll the cached raw parameter value once
    // per block here and run the SAME rising-edge test against the SAME shared
    // edge state. Whichever path observes the edge first wins; the atomic exchange
    // guarantees exactly one fire per genuine false->true transition, so the two
    // paths can never double-trigger. RT-safe (atomics only).
    // NOTE the poll reads the PARAMETER OBJECT (getValue()), not APVTS's cached
    // raw atomic: that cached atomic is itself only refreshed through the same
    // notification path this backstop exists to survive, so polling it would
    // inherit the very failure mode being guarded against. getValue() is an
    // atomic read of the parameter's own current value - RT-safe and always
    // current no matter how the host wrote it.
    if (triggerParamObject != nullptr)
    {
        const bool rawHigh = triggerParamObject->getValue() > 0.5f;
        const bool rawWasHigh = triggerEventEdgeSeenHigh.exchange(rawHigh, std::memory_order_acq_rel);
        if (rawHigh && !rawWasHigh)
            triggerEventPendingFlag.store(true, std::memory_order_release);
    }

    if (triggerEventPendingFlag.exchange(false, std::memory_order_acq_rel))
        triggerEventOffset = 0;

    if (getTriggerMode() == TriggerMode::Midi)
    {
        // POST-1.0 (2026-09-17): ANY note-on triggers - see this block's
        // doc comment above. getMidiTriggerNote() is deliberately NOT
        // consulted here any more.
        for (const auto metadata : midiMessages)
        {
            const auto message = metadata.getMessage();
            if (message.isNoteOn())
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
        advanceStateMachineTicks();

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
    advanceStateMachineTicks();

    // Stage 3 DSP Phase 3.5: push one visualization frame per processBlock()
    // call (architecture.md Thread Boundaries: "pushed once per processed
    // block (or once per sub-block segment)" - block granularity chosen
    // here, simplest option explicitly allowed by that sentence). Uses the
    // LAST segment's computed weights/progress within this block (cached by
    // processSegment()) - a per-block snapshot is sufficient for a
    // ~30-60Hz GUI drain rate (Stage 3 GUI Phase 4.3, not wired yet).
    {
        VisualizationFrame frame;
        // POST-1.0 ("curve progression" fix): frame.progress is now the
        // LINEAR elapsed-build fraction (same basis as the audio weights -
        // see lastComputedProgress's doc comment in PluginProcessor.h);
        // frame.tensionLevel is the CURVED progress (DOCUMENTED
        // SUPERSESSION of the pre-fix flat `tensionParam->load() / 100.0f`
        // readout, which was time-invariant despite representing itself as
        // "the real curve-evaluation output" in PluginEditor.cpp's own
        // pre-existing doc comment).
        frame.progress = lastComputedProgress;
        frame.tensionLevel = lastComputedTensionLevel;
        frame.currentState = (int) dropStateMachine.getState();
        frame.inputRMS = audioAnalyzer.getMetrics().rms;
        for (int i = 0; i < 10; ++i)
            frame.activeModuleWeights[i] = lastComputedWeights.value[(size_t) i];
        // POST-1.0 additive fields (BPM diagnostic feature - see
        // VisualizationFrame's doc comment in PluginProcessor.h).
        frame.bpm = (float) musicalClock.getBpm();
        frame.hostPlayheadValid = !musicalClock.isUsingFallbackTransport();
        pushVisualizationFrame(frame);
    }
    } // if (!wasAlreadyAborting)

    // ------------------------------------------------------------------
    // POST-1.0 TRANSPORT-STOP SAFETY - abort fade (see this method's
    // "transport-stop" doc comment above). Runs whenever an abort is in
    // progress (whether just started this block or continuing from a
    // previous one), converging `buffer` toward the pre-chain
    // dryCaptureBuffer (the SAME dry signal the final Dry/Wet Mixer below
    // already uses) via a raised-cosine gain from 1.0 down to exactly 0.0
    // across kTransportStopFadeSeconds - a guaranteed click-free, bounded-
    // duration instant, matching the tail envelope's own raised-cosine
    // convention elsewhere in this file. Once buffer == dryCaptureBuffer
    // exactly, this fade is a fixed point of the Dry/Wet Mixer's own
    // crossfade too (dry + (dry-dry)*mixAmt == dry regardless of mixAmt) -
    // so the abort's audible result is pure dry pass-through, independent
    // of the user's Dry/Wet knob setting. Any samples in THIS block beyond
    // the fade's completion point are forced to gain 0.0 (not left at
    // whatever processSegment() may have already written for them, if this
    // is the detection block that ran the full pipeline for the ENTIRE
    // block above) - the transport-stop condition is only ever observed at
    // block granularity (musicalClock.update() samples it once per block),
    // so treating "stopped as of this block's start" as applying for the
    // block's full duration is the finest resolution actually available,
    // matching this file's established block-granularity convention for
    // the trigger parameter's own edge detection.
    // ------------------------------------------------------------------
    if (transportStopAbortActive)
    {
        const int64_t fadeTotal = transportStopFadeTotalSamples;
        for (int i = 0; i < numSamples; ++i)
        {
            const float t = (transportStopFadeElapsedSamples < fadeTotal)
                ? juce::jlimit(0.0f, 1.0f, (float) ((double) transportStopFadeElapsedSamples / (double) fadeTotal))
                : 1.0f;
            const float gain = 0.5f * (1.0f + std::cos(juce::MathConstants<float>::pi * t)); // 1 -> 0 raised cosine

            for (int channel = 0; channel < totalNumOutputChannels; ++channel)
            {
                auto* data = buffer.getWritePointer(channel);
                const float dry = dryCaptureBuffer.getSample(channel, i);
                data[i] = dry + (data[i] - dry) * gain;
            }

            if (transportStopFadeElapsedSamples < fadeTotal)
                ++transportStopFadeElapsedSamples;
        }

        if (transportStopFadeElapsedSamples >= fadeTotal)
        {
            // Click-free instant reached (gain has decayed to exactly 0,
            // matching the raised-cosine's t==1 limit) - finalize: force the
            // state machine back to Idle and run the SAME module-hygiene
            // reset sweep advanceStateMachineTicks() uses at the natural
            // Recovery->Idle transition, so nothing generated by the aborted
            // transition rings into whatever comes next.
            dropStateMachine.reset();
            resetAllOwnedDspModules();
            previousObservedState = drop::DropState::Idle;
            frozenDelayReturnWeight = 0.0f;
            frozenReverbReturnWeight = 0.0f;
            impactCarryoverElapsedSamples = -1;
            transportStopAbortActive = false;
            transportStopFadeElapsedSamples = 0;
            transportStopFadeTotalSamples = 0;
        }
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

// POST-1.0 SIXTH IMPROVEMENT SESSION (2026-09-17): the Recovery tail
// envelope as a PURE function of normalized recovery position t in [0,1] -
// dB-linear over kTailFadeRangeDb (60dB), raised-cosine landing to an EXACT
// 0.0 over the final (1 - kTailLandingStartT) of the window. Same math as
// the previous inline computation (RunTests.cpp's "[POST-1.0] Tail
// envelope" block mirrors it verbatim); factored out so processSegment()
// can evaluate it PER-SAMPLE (see the blend loop at the end of
// processSegment() for the root-cause story of why per-segment evaluation
// was an audible bug).
static float tailEnvelopeForT(float recoveryT) noexcept
{
    // POST-1.0 (2026-09-20, user-directed - final form after two same-day
    // iterations): a PURE DECAYING SLOPE, no sustain plateau.
    //   Iteration 1 (superseded): -60dB dB-linear across the whole window
    //     - compounded with the tails' own natural decay (5th instance of
    //     the compounded-curves class) and measured inaudible by ~40% of
    //     the window ("when i have tail active i get no tail").
    //   Iteration 2 (superseded): hold at 1.0 for 70% then fade - fixed
    //     audibility, but the plateau reads as SUSTAIN ("tail shouldnt
    //     have sustain. it sould be a slope. decaing").
    //   Current: decays from the very first sample, dB-LINEARLY (the
    //     plugin-wide "volume in dB" convention), but over a GENTLE
    //     kTailFadeRangeDb across the window - shallow enough that
    //     the ring stays audible deep into the window even stacked on the
    //     natural decay (the -60dB version's failure), steep enough to be
    //     an unmistakable continuous decay (the hold version's failure).
    //   (kTailLandingStartT .. 1]: raised-cosine landing from -30dB to an
    //   EXACT 0.0 - the click-free Recovery->Idle reset invariant.
    // TAIL=CUT (~5ms window) keeps its clean instant-stop character: the
    // same shape compressed into 240 samples (~0.13dB/sample) is a fast,
    // click-free stop.
    // 30dB -> 24dB, 2026-09-20 same-day user follow-up ("a bit longer"):
    // a ~20% shallower slope keeps the ring audibly present longer
    // within the same window; shape/landing unchanged.
    constexpr float kTailFadeRangeDb = 24.0f;
    constexpr float kTailLandingStartT = 0.97f;
    recoveryT = juce::jlimit(0.0f, 1.0f, recoveryT);
    if (recoveryT <= kTailLandingStartT)
        return std::pow(10.0f, -(kTailFadeRangeDb / 20.0f) * (recoveryT / kTailLandingStartT));
    const float envAtLandingStart = std::pow(10.0f, -(kTailFadeRangeDb / 20.0f));
    const float landingT = (recoveryT - kTailLandingStartT) / (1.0f - kTailLandingStartT);
    // Raised-cosine 1->0 over the landing window, scaled by the (already
    // -24dB-quiet) level reached at its start - C0-continuous with the
    // dB-linear branch at the seam, and EXACTLY 0 at recoveryT == 1.
    return envAtLandingStart * 0.5f * (1.0f + std::cos(juce::MathConstants<float>::pi * landingT));
}

void DROPAudioProcessor::processSegment(juce::AudioBuffer<float>& buffer, int startSample, int numSamplesInSegment) noexcept
{
    if (numSamplesInSegment <= 0)
        return;

    juce::dsp::AudioBlock<float> fullBlock(buffer);
    auto segmentBlock = fullBlock.getSubBlock((size_t) startSample, (size_t) numSamplesInSegment);

    // POST-1.0 ("curve progression" fix): TWO distinct progress values, per
    // the decision matrix documented at computeIntensityProgress()'s
    // declaration in PluginProcessor.h -
    //   `progress`       - CURVED (TransitionEngine-shaped) - feeds every
    //                       fixed-series module's own internal-sweep
    //                       `progress` argument below, unchanged from
    //                       pre-fix behavior.
    //   `linearProgress` - LINEAR elapsed fraction - feeds ONLY
    //                       IntensityBudget::computeWeights() below (the
    //                       single point where a Style's own ModuleCurve/
    //                       budgetEnvelope shapes are applied), so that
    //                       shape is applied exactly ONCE, not composed on
    //                       top of TransitionEngine's curve too.
    const float progress = computeIntensityProgress();
    const float linearProgress = computeLinearBuildProgress();
    const auto dropState = dropStateMachine.getState(); // POST-1.0 addition: cached once, reused by the tail-envelope/ImpactEngine-carryover logic below

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
    // POST-1.0 ("curve progression" fix): `linearProgress`, NOT `progress` -
    // see this method's doc comment above and computeIntensityProgress()'s
    // decision-matrix doc comment in PluginProcessor.h. This is the single
    // point where style.moduleCurves[i]/style.budgetEnvelope's own authored
    // shapes are evaluated; feeding them the ALREADY-curved `progress` here
    // (the pre-fix behavior) composed two end-weighted curves together,
    // which was the exact reported bug.
    auto weights = drop::IntensityBudget::computeWeights(style, linearProgress, macroPercent, intensity01,
                                                           tension01, chaos01, audioAnalyzer.getSourceProfile(),
                                                           chaosJitter);

    // POST-1.0 IDLE-TRANSPARENCY BUGFIX (2026-09-19 bug-check round).
    // MEASURED BUG: an UNTRIGGERED instance colored the signal at only
    // ~14dB below the input at 100% wet (~-20dB at the 50% dryWet
    // default) - clearly audible. ROOT CAUSE: the Style curves author
    // small nonzero START weights (budgetEnvelope startWeight ~0.05,
    // module-curve starts up to ~0.1) so computeWeights() at progress==0
    // hands every module a small live weight - correct DURING a build
    // (architecture.md: budget "near-zero at Idle/early Building" refers
    // to the build envelope's shape), but Idle/Arming are NOT part of any
    // build: the plugin must be inert until the transition actually runs.
    // FIX: hard-zero all shared-pool weights outside an active transition.
    // Entry into Building steps from 0 to the tiny authored start weights
    // under each module's own ~20ms weight smoother, exactly at the bar
    // boundary where the build begins - inaudible, and it also means the
    // delay/reverb buffers stay UNFED while idle (no stale pre-trigger
    // echoes) and the post-cut return to Idle stays bit-dry. Peak/Impact/
    // Recovery keep their live progress==0 weights untouched (the
    // transient-preservation behavior is calibrated against them).
    if (dropState == drop::DropState::Idle || dropState == drop::DropState::Arming)
        for (size_t wi = 0; wi < (size_t) drop::BudgetModule::Count; ++wi)
            weights.value[wi] = 0.0f;

    // Cached for the visualization-frame push at the end of processBlock()
    // (see that call site's doc comment, and the lastComputedProgress/
    // lastComputedTensionLevel member doc comments in PluginProcessor.h).
    lastComputedWeights = weights;
    lastComputedProgress = linearProgress; // same basis as the weights above (requirement: visual dot / progress strip must agree with the audio)
    lastComputedTensionLevel = progress;   // curved - "what the user feels"

    // POST-1.0 FIFTH IMPROVEMENT SESSION bugfix (2026-09-17, user bug-audit
    // report: "the white noise rises too fast and too strong, the curve
    // shows one thing the white noise does more than it shows"). See
    // drop::IntensityBudget::computeNoiseWeight()'s doc comment for the full
    // root-cause/fix rationale: `weights.value[Noise]` above is a SECOND,
    // independently progress-shaped curve that used to be multiplied
    // straight into NoiseModule's own `riseAmountForProgress(progress)`,
    // compounding two live progressions. `noiseWeight` below is now the SOLE
    // source of the audible noise level (used at the noiseModule.process()
    // call site instead of `weights.value[Noise]`) - overwriting
    // lastComputedWeights's Noise slot here (AFTER the line above, BEFORE
    // the visualization-frame push at the end of processBlock() reads it)
    // keeps the Advanced-page glow truthful too, matching what is actually
    // audible.
    const float noiseWeight = drop::IntensityBudget::computeNoiseWeight(
        style, macroPercent[(size_t) drop::BudgetModule::Noise], intensity01,
        audioAnalyzer.getSourceProfile(), chaosJitter[(size_t) drop::BudgetModule::Noise]);
    lastComputedWeights.value[(size_t) drop::BudgetModule::Noise] = noiseWeight;

    const float impactAmount01 = juce::jlimit(0.0f, 1.0f, impactParam->load() / 100.0f);

    // ------------------------------------------------------------------
    // POST-1.0 bugfix: "tail envelope" (see PluginProcessor.h's top doc
    // comment "POST-1.0 IMPROVEMENT"). CORRECTED (second pass,
    // coordinator-reported regression): this envelope now governs
    // DelayModule/ReverbModule's WET-RETURN and ImpactEngine's effective
    // strength during RECOVERY ONLY - value 1.0 exactly at Recovery's first
    // sample, decaying 1->0 across EXACTLY Recovery's (tail-parameter-
    // derived) duration, reaching precisely 0 at Recovery's last sample (the
    // click-free instant advanceStateMachineTicks() uses to reset every
    // owned DSP module - Bug C "stale tails" fix). POST-1.0 FIFTH
    // IMPROVEMENT SESSION (2026-09-17): the exact SHAPE of this decay is
    // decibel-linear (natural exponential-decay character), not a raised
    // cosine or a squared curve - see the shape's own doc comment at its
    // computation below for the full history/rationale. Peak and
    // Impact are UNCHANGED/untouched by this envelope - they use the LIVE
    // IntensityBudget weight directly for both send and return, exactly like
    // the pre-existing (already-verified-correct) legacy behavior; holding
    // this envelope at 1.0 through Peak+Impact was the first-pass bug (see
    // this method's "REGRESSION FIX" doc comment below, at the
    // delaySendWeight/delayReturnWeight computation) - it substituted a
    // potentially much larger frozen weight for the small live one during
    // Impact specifically, diluting the transient.
    //
    // frozenDelayReturnWeight/frozenReverbReturnWeight are continuously
    // re-captured every segment below while state == Building, so by the
    // time state advances past Building each holds exactly Building's OWN
    // final weight - multiplying the envelope by this frozen value (rather
    // than a flat 1.0) is what makes Recovery's tail start exactly where the
    // source content left off, C0/C1-continuous through the return-weight
    // smoother's own ~20ms ramp (NOT instantaneously - see the "post-impact
    // continuity" test's doc comment in RunTests.cpp for the expected
    // attack-then-decay shape this produces).
    // ------------------------------------------------------------------
    // POST-1.0 SIXTH IMPROVEMENT SESSION bugfix (2026-09-17, user DAW
    // report: "fix the sound. It doesnt cut immedietly. it has artifacts in
    // the end."). ROOT CAUSE: the previous code computed a SINGLE scalar
    // `tailEnvelope` here from recoveryT at the SEGMENT'S START and applied
    // it as a constant across the whole segment. Segments are cut only at
    // state boundaries, so a short Recovery window sits entirely inside ONE
    // segment - at tail==0 (the ~5ms kTailCutSeconds window, now also the
    // DEFAULT) recoveryT evaluated to 0 -> envelope 1.0 for the ENTIRE
    // window: the fade never actually happened at all, and the
    // Recovery->Idle transition then hard-muted the still-full-level
    // delay/reverb/impact tails in a single sample - the reported
    // "doesn't cut immediately" + end-of-effect click artifact. (Same
    // segment-granularity bug class as the GateModule note in NOTES.md; at
    // larger host block sizes even LONG tails staircased audibly, up to
    // ~-8dB instantaneous steps per 1024-sample block for the 150ms
    // minimum tail.)
    //
    // FIX (single-application rule, per this plugin's recurring
    // compounded-curves lesson): the envelope math (unchanged - dB-linear
    // 60dB + raised-cosine landing to exact 0, see tailEnvelopeForT()
    // above) is no longer multiplied into ANY per-segment weight. Instead
    // the frozen Delay/Reverb return weights and ImpactEngine's return run
    // UNSCALED through Recovery, and one PER-SAMPLE blend at the very end
    // of this method applies the envelope to the segment's entire effect
    // contribution (out = dry + (wet - dry) * env(t_sample)) - every
    // wet-return stage it replaces was linear in its return weight, so this
    // is mathematically the intended per-sample product, now reaching an
    // EXACT 0.0 on Recovery's final sample regardless of block size (the
    // click-free Recovery->Idle reset invariant, now actually honored at
    // audio rate).
    const int64_t recoveryTotalSamples = (dropState == drop::DropState::Recovery)
        ? dropStateMachine.currentStateLengthSamples() : 0;
    const int64_t recoveryElapsedAtSegmentStart = (recoveryTotalSamples > 0)
        ? (recoveryTotalSamples - dropStateMachine.samplesUntilNextTransition()) : 0;

    if (dropState == drop::DropState::Building)
    {
        frozenDelayReturnWeight = weights.value[(size_t) drop::BudgetModule::Delay];
        frozenReverbReturnWeight = weights.value[(size_t) drop::BudgetModule::Reverb];
    }

    // POST-1.0 REGRESSION FIX (2026-09-16, second pass - coordinator-reported
    // build/test feedback): the tail envelope/frozen-weight treatment below
    // applies ONLY during Recovery, NOT Peak/Impact. Peak and Impact are
    // UNCHANGED/BYTE-IDENTICAL to the pre-tail-bugfix behavior - both send
    // and return equal the LIVE weights.value[...] (whatever
    // IntensityBudget::computeWeights() genuinely produces at progress==0 -
    // NOT necessarily exactly 0.0f: DropStyle.cpp's budgetEnvelope curves are
    // authored with startWeight=0.05, not 0.0, so Filter/Delay/Reverb/Digital
    // legitimately carry small-but-nonzero weight even when "ducked" - this
    // was already true, unmodified, throughout Peak/Impact BEFORE this
    // improvement session and is what the already-passing Phase 3.4
    // transient-preservation test was calibrated against).
    //
    // ROOT CAUSE of the first-pass regression: the previous version of this
    // code used `frozenDelayReturnWeight * tailEnvelope` (tailEnvelope==1.0
    // throughout Peak+Impact) as the RETURN weight for ALL THREE states,
    // instead of only Recovery. Since Building's captured frozen weight can
    // be substantially larger than the tiny live Peak/Impact weight (e.g.
    // Clean EDM's Delay curve ends Building at 0.5 vs. its progress==0 value
    // of ~0.1), AND the delay line/reverb's "wet" content was legitimately
    // near-silent during a silence-except-one-impulse test run (nothing had
    // been written to send-gated buffers), the crossfade
    // `dry + (wet - dry) * returnWeight` with wet≈0 and returnWeight
    // substantially > the original small value DILUTED the dry impulse
    // itself (`dry * (1 - returnWeight)`) well below the required 50% floor -
    // this is the exact mechanism that broke the Phase 3.4 transient-
    // preservation test. Recovery is the ONLY state where wet-return legitimately
    // needs to exceed the live IntensityBudget weight (to let the tail ring
    // out under user control) - restoring Peak/Impact to the untouched
    // legacy path fixes the regression while keeping Recovery's new tail
    // behavior intact.
    const bool inRecovery = (dropState == drop::DropState::Recovery);
    // POST-1.0 SIXTH IMPROVEMENT SESSION: during Recovery the frozen return
    // weights are now applied UNSCALED - the tail decay is applied once,
    // per-sample, at the end-of-segment blend (see the tailEnvelopeForT()
    // root-cause comment above). Multiplying a segment-start envelope in
    // here as well would both re-introduce the segment-granularity
    // staircase AND double-apply the decay.
    const float delaySendWeight = inRecovery ? 0.0f : weights.value[(size_t) drop::BudgetModule::Delay];
    const float delayReturnWeight = inRecovery ? frozenDelayReturnWeight : delaySendWeight;
    const float reverbSendWeight = inRecovery ? 0.0f : weights.value[(size_t) drop::BudgetModule::Reverb];
    const float reverbReturnWeight = inRecovery ? frozenReverbReturnWeight : reverbSendWeight;

    // Stage 3 DSP Phase 3.4: ImpactEngine's coordination-drive values
    // (architecture.md: ImpactEngine "drives existing modules into a
    // specific configuration") are computed BEFORE the series chain below
    // so the SAME-segment WidthModule/SaturationModule calls can be driven
    // by them. Non-zero ONLY during the Impact state (state-gated, like
    // VacuumModule below) - architecture.md Processing Chain "Routing
    // notes": "VacuumModule and ImpactEngine are state-gated, not
    // weight-blended."
    drop::ImpactEngine::DriveValues impactDrive {};
    if (dropState == drop::DropState::Impact)
    {
        const int64_t total = dropStateMachine.currentStateLengthSamples();
        const int64_t remaining = dropStateMachine.samplesUntilNextTransition();
        const int64_t elapsedAtSegmentStart = total - remaining;
        impactDrive = drop::ImpactEngine::computeDriveValues((double) elapsedAtSegmentStart, (double) total, impactAmount01);
    }

    // Fixed series order per architecture.md Processing Order step 4:
    // Filter -> Width -> Gate -> Pitch -> Stutter -> Delay -> Saturation ->
    // Digital, then Reverb (Phase 3.2's placeholder Building/Recovery-
    // ambience position - Impact's own SEPARATE short-tail reverb instance
    // lives inside ImpactEngine, not here). Noise is a PARALLEL ADD (step 5,
    // not a series insert) - DOCUMENTED SUPERSESSION: architecture.md step 5
    // originally placed this add AFTER Digital/BEFORE Reverb; the "NOISE
    // FIRST" change below (POST-1.0, 2026-09-17) moved the injection point to
    // the HEAD of the series chain instead, so it is no longer positioned
    // per step 5's original ordering - see that change's own comment
    // immediately below for the current, authoritative position (HEAD, not
    // "after Digital, before Reverb"). Every IntensityBudget-managed module
    // always processes (architecture.md "Always-Process, Weight-Mix") -
    // weight blends its contribution, never bypasses it.
    // NOISE FIRST (POST-1.0, 2026-09-17, user DAW report: "put the noise in the
    // chain before the effects, so it gets delay reverb and everything else").
    // The riser is now injected at the HEAD of the fixed series chain, so every
    // module downstream treats it as part of the material: the filter sweep
    // shapes it, the gate chops it in tempo, delay and reverb carry it, and
    // saturation/digital colour it. Previously it was added near the END (after
    // Digital, before Reverb), so it sat on top of the transition as a separate
    // layer that only ever reached the reverb.
    //
    // Feedback safety is unaffected: DelayModule's loop is bounded by its own
    // tanh soft-clip plus a hard <=0.92 loop-gain clamp (Phase 3.2), so feeding
    // it a rising noise source cannot run away. VacuumModule still attenuates
    // the whole bus during Peak, so the riser still collapses into the vacuum.
    //
    // `noiseTension` (21st parameter) shapes the rise-amount curve only - see
    // NoiseModule.h's riseAmountForProgress() doc comment. Read fresh every
    // segment (cheap atomic load, matching every other macro above).
    //
    // POST-1.0 FIFTH IMPROVEMENT SESSION bugfix (2026-09-17): `noiseWeight`
    // (computed above, NOT `weights.value[Noise]`) - see
    // drop::IntensityBudget::computeNoiseWeight()'s doc comment for why
    // passing the budget's own Noise slot here was the "curve dishonesty"
    // bug (two compounded progress-shaped gains).
    // POST-1.0 SEVENTH IMPROVEMENT SESSION (2026-09-18): live user-facing
    // riser-source selection (choice index -> NoiseType; 0 == STYLE defers
    // to the Style-authored color set at Arming). Set per segment so
    // automation takes effect immediately; the module's generators keep
    // their own continuous state across type switches.
    noiseModule.setType((drop::NoiseType) juce::jlimit(0, 6, (int) noiseTypeParam->load()));
    noiseModule.process(segmentBlock, noiseWeight, progress, 0.6f,
                         noiseTensionParam->load() / 100.0f);

    filterModule.process(segmentBlock, weights.value[(size_t) drop::BudgetModule::Filter], progress);
    widthModule.process(segmentBlock, weights.value[(size_t) drop::BudgetModule::Width], progress, impactDrive.widthExpansionAmount);
    gateModule.process(segmentBlock, weights.value[(size_t) drop::BudgetModule::Gate], progress, musicalClock);
    pitchModule.process(segmentBlock, weights.value[(size_t) drop::BudgetModule::Pitch], progress);
    stutterModule.process(segmentBlock, weights.value[(size_t) drop::BudgetModule::Stutter], progress, musicalClock);
    // POST-1.0 bugfix (Bug C "stale tails"): send/return split - see this
    // function's "tail envelope" doc comment above and DelayModule.h's class
    // doc comment. Identical to the prior single-weight process() call
    // whenever delaySendWeight == delayReturnWeight (always true outside
    // Peak/Impact/Recovery).
    delayModule.processWithSendReturn(segmentBlock, delaySendWeight, delayReturnWeight, progress, musicalClock);

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
    // (NoiseModule used to be injected HERE, after Digital and before Reverb.
    //  POST-1.0 2026-09-17: moved to the FRONT of the chain - see the
    //  injection site above filterModule.process().)
    // POST-1.0 bugfix (Bug C "stale tails"): send/return split, same
    // rationale as DelayModule above.
    reverbModule.processWithSendReturn(segmentBlock, reverbSendWeight, reverbReturnWeight, progress);

    // VacuumModule: state-gated (NOT IntensityBudget weight-blended,
    // architecture.md Processing Chain "Routing notes") - applied to the
    // WHOLE post-chain bus only during the DropStateMachine Peak state.
    // Stage 3 DSP Phase 3.4: Peak's ENTIRE latched duration now IS the
    // Vacuum window (see computeDurationsForNewBuild()) - the segmentation
    // loop in processBlock() guarantees the entire segment stays within
    // Peak before this runs (segments are split exactly at state
    // transitions).
    if (dropState == drop::DropState::Peak)
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
    //
    // POST-1.0 bugfix (Bug A "instant cut" - see PluginProcessor.h's top doc
    // comment "POST-1.0 IMPROVEMENT" and ImpactEngine::process()'s doc
    // comment): PluginProcessor now continues calling ImpactEngine::process()
    // through the Recovery state too (previously state-gated to Impact
    // ONLY), so the sub-hit/burst envelopes and - critically - the dedicated
    // shortTailReverb's own algorithmic decay are never abruptly cut off at
    // the exact Impact->Recovery boundary. `elapsed` is tracked CONTINUOUSLY
    // across that boundary via impactCarryoverElapsedSamples (member;
    // dropStateMachine's own per-state samplesIntoState resets to 0 at every
    // transition and would reintroduce a discontinuity if used directly for
    // the Recovery portion). The (re)start condition below
    // (elapsedAtSegmentStart == 0) relies on the SAME segmentation guarantee
    // the original code already depended on: a fresh segment always starts
    // exactly at Impact's first sample.
    if (dropState == drop::DropState::Impact)
    {
        const int64_t total = dropStateMachine.currentStateLengthSamples();
        const int64_t remaining = dropStateMachine.samplesUntilNextTransition();
        const int64_t elapsedAtSegmentStart = total - remaining;
        if (elapsedAtSegmentStart == 0)
            impactCarryoverElapsedSamples = 0; // fresh Impact entry - (re)start the continuous counter
    }

    if ((dropState == drop::DropState::Impact || dropState == drop::DropState::Recovery)
        && impactCarryoverElapsedSamples >= 0)
    {
        // POST-1.0 SIXTH IMPROVEMENT SESSION: impactAmount01 is now passed
        // UNSCALED through Recovery too (previously multiplied by the
        // segment-start tail envelope) - the Tail knob's decay is applied
        // once, PER-SAMPLE, at the end-of-segment blend below, which covers
        // ImpactEngine's linear reverb-return contribution exactly as it
        // covers Delay/Reverb's (see tailEnvelopeForT()'s root-cause
        // comment).
        const float impactAmount01Effective = impactAmount01;
        // reverbSendWeightOverride: -1 (sentinel, "no override") during
        // Impact - preserves the EXACT prior single-weight behavior
        // (send==return==impactAmount01). During Recovery, forced to 0.0f so
        // the live pass-through audio arriving at that point (unrelated to
        // the drop by then) never re-excites shortTailReverb - only its
        // ALREADY-excited internal state keeps ringing out, decayed by the
        // per-sample tail blend at the end of this method.
        // POST-1.0 "TRUE CUT" (2026-09-18): with tailCutActiveForBuild the
        // send is 0 during Impact TOO - the shortTailReverb is never
        // excited at the drop, so with CUT there is nothing to ring past
        // the bar line at all (see computeDurationsForNewBuild()'s CUT
        // branch doc comment).
        const float reverbSendWeightOverride =
            (dropState == drop::DropState::Recovery || tailCutActiveForBuild) ? 0.0f : -1.0f;
        // accentLevel: Style-authored scale on the synthesized sub-hit/burst
        // accents ONLY (POST-1.0 2026-09-17, user DAW report: Dark Techno
        // "adds a drum kick" - that Style authors this to 0.0; see
        // DropStyleData::impactAccentLevel). Uses the SAME
        // activeStyleIndexForBuild latched at Arming as every other
        // Style-driven decision in this build.
        const float accentLevel = drop::getStyle(activeStyleIndexForBuild).impactAccentLevel;
        impactEngine.process(segmentBlock, (double) impactCarryoverElapsedSamples,
                              (double) dropStateMachine.currentStateLengthSamples(),
                              impactAmount01Effective, reverbSendWeightOverride, accentLevel);
        impactCarryoverElapsedSamples += numSamplesInSegment;
    }

    // ------------------------------------------------------------------
    // POST-1.0 SIXTH IMPROVEMENT SESSION: the SINGLE application point of
    // the Recovery tail envelope, per-sample (see tailEnvelopeForT()'s
    // root-cause comment - the previous per-segment scalar meant the fade
    // never actually happened inside short Recovery windows, ending in a
    // full-level hard mute at Recovery->Idle: the reported "doesn't cut
    // immediately" + end artifact). Blends the segment's ENTIRE effect
    // contribution toward the untouched dry input captured at the top of
    // processBlock() (dryCaptureBuffer - same absolute sample offsets):
    //     out[i] = dry[i] + (out[i] - dry[i]) * env(t_i)
    // Every Recovery wet stage this replaces (Delay/Reverb frozen returns,
    // ImpactEngine's shortTailReverb return) is linear in its return
    // weight, so this equals the intended per-sample weight product - and
    // env(t) reaches an EXACT 0.0 on Recovery's final sample regardless of
    // host block size, so the Recovery->Idle module-hygiene reset() sweep
    // lands on a segment already bit-identical to dry: click-free by
    // construction. RT-safe: dryCaptureBuffer is preallocated in
    // prepareToPlay() and already written this block.
    if (dropState == drop::DropState::Recovery)
    {
        const int chToBlend = juce::jmin(buffer.getNumChannels(), dryCaptureBuffer.getNumChannels());
        for (int ch = 0; ch < chToBlend; ++ch)
        {
            float* out = buffer.getWritePointer(ch, startSample);
            const float* dry = dryCaptureBuffer.getReadPointer(ch, startSample);
            for (int i = 0; i < numSamplesInSegment; ++i)
            {
                const float t = (recoveryTotalSamples > 0)
                    ? (float) ((double) (recoveryElapsedAtSegmentStart + (int64_t) i) / (double) recoveryTotalSamples)
                    : 1.0f; // zero-length Recovery edge case: immediately fully dry, matches the env's t==1 limit
                out[i] = dry[i] + (out[i] - dry[i]) * tailEnvelopeForT(t);
            }
        }
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
            // POST-1.0 BUGFIX (2026-09-16, user-directed improvement, Bug B
            // "post-impact artifact" - see PluginProcessor.h's top doc
            // comment "POST-1.0 IMPROVEMENT"): DOCUMENTED SUPERSESSION of the
            // original `1.0f - t` ramp (architecture.md's literal "all
            // module weights ramp back to 0" during Recovery, taken at face
            // value, implied ramping FROM full weight DOWN to 0 across
            // Recovery - but the 10 shared-budget modules were already
            // settled to EXACTLY 0 through the whole preceding Peak+Impact
            // window per this same function's Peak/Impact case above, so a
            // `1.0f - t` ramp starting at Recovery's first sample actually
            // SLAMMED them from 0 back up toward full weight (through each
            // module's own ~20ms smoother) before ramping down again - an
            // audible post-impact effect blast, not the intended smooth
            // settle). Recovery now ALSO returns 0.0f, matching
            // implementation-spec.md's own IMPACT PHILOSOPHY section verbatim:
            // "BUILD -> TENSION -> VACUUM -> ORIGINAL/CLEAN IMPACT -> SHORT
            // TAIL -> RECOVERY" - Recovery stays clean (shared modules stay
            // ducked), TAILS ONLY ring out during it, via the separate
            // send/return-weight tail envelope in processSegment() (NOT this
            // shared-budget progress value at all - DelayModule/
            // ReverbModule/ImpactEngine's Recovery-state wet-return is driven
            // by the tail envelope, independently of computeWeights()'s
            // now-permanently-0 output during this state).
            return 0.0f;
    }
    return 0.0f;
}

float DROPAudioProcessor::computeLinearBuildProgress() noexcept
{
    // POST-1.0 addition ("curve progression" fix) - see this method's
    // declaration-site doc comment in PluginProcessor.h for the full
    // decision matrix. Deliberately a small, independent duplicate of
    // computeIntensityProgress()'s Building-case elapsed/total arithmetic
    // (NOT a shared/refactored helper) - the two methods differ ONLY in
    // whether transitionEngine.evaluate() is applied to `t`, and keeping
    // them fully independent avoids any risk of perturbing
    // computeIntensityProgress()'s own extensively-tested exact behavior.
    if (dropStateMachine.getState() != drop::DropState::Building)
        return 0.0f; // matches computeIntensityProgress()'s state-gating exactly: 0 for Idle/Arming/Peak/Impact/Recovery

    const int64_t total = dropStateMachine.currentStateLengthSamples();
    if (total <= 0)
        return 1.0f; // zero-length Building edge case -> full progress immediately (matches computeIntensityProgress())

    const int64_t elapsed = total - dropStateMachine.samplesUntilNextTransition();
    return juce::jlimit(0.0f, 1.0f, (float) ((double) elapsed / (double) total));
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
