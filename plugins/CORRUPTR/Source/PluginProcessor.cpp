#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <cmath>

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
    //=========================================================================
    // Stage 2 Phase 3.2: Unified Modulation Accumulator (Isolated) - self-test
    //
    // Runs on EVERY plugin instantiation (including under pluginval, this
    // repo's closest equivalent to an executable unit-test harness - see
    // dsp/ModulationAccumulator.h's runSelfTest() doc comment for why).
    // Debug builds hard-stop at the exact failing check via the jassert()s
    // inside runSelfTest() itself; the aggregate result is also stored in a
    // diagnostic atomic (same convention as Phase 3.1's
    // feedbackCircuitBreakerTripped) so release builds never crash but the
    // outcome remains inspectable.
    //=========================================================================
    const bool modAccumulatorSelfTestOk = ModulationAccumulator::runSelfTest();
    modulationAccumulatorSelfTestPassed.store(modAccumulatorSelfTestOk, std::memory_order_relaxed);
    jassert(modAccumulatorSelfTestOk);
}

CORRUPTRAudioProcessor::~CORRUPTRAudioProcessor()
{
}

void CORRUPTRAudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    const int numChannels = juce::jmax(getTotalNumOutputChannels(), 2); // dual-mono per architecture.md's stereo-compatible processing note

    juce::dsp::ProcessSpec spec;
    spec.sampleRate = sampleRate;
    spec.maximumBlockSize = static_cast<juce::uint32>(samplesPerBlock);
    spec.numChannels = static_cast<juce::uint32>(numChannels);

    //=========================================================================
    // Stage 2 Phase 3.1: Feedback Routing Safety Validation (Isolated)
    //=========================================================================
    feedbackMaxDelaySamples = static_cast<int>(std::ceil(kFeedbackDelayHeadroomSeconds * sampleRate)) + 4;
    feedbackDelayLine.setMaximumDelayInSamples(feedbackMaxDelaySamples);
    feedbackDelayLine.prepare(spec);
    feedbackDelayLine.reset();

    juce::dsp::ProcessSpec monoSpec = spec;
    monoSpec.numChannels = 1;
    for (auto& f : feedbackDampingFilter)
    {
        f.prepare(monoSpec);
        f.setType(juce::dsp::FirstOrderTPTFilterType::lowpass);
        f.reset();
    }

    feedbackRmsEnvelope.fill(0.0f);

    // One-pole coefficient for the continuous RMS-envelope limiter
    // (architecture.md's recommended "additional block-level RMS-based
    // limiter on the feedback path" — implemented as a continuous one-pole
    // tracker rather than strictly per-block for smoother, artifact-free
    // gain reduction).
    const float rmsTimeConstantSeconds = kFeedbackRmsTimeConstantMs / 1000.0f;
    feedbackRmsOnePoleCoeff = std::exp(-1.0f / (rmsTimeConstantSeconds * static_cast<float>(sampleRate)));

    feedbackInternalGainSmoothed.reset(sampleRate, 0.02); // 20ms ramp - avoids zipper noise on feedbackAmount automation
    feedbackInternalGainSmoothed.setCurrentAndTargetValue(0.0f);

    feedbackDampingCutoffSmoothed.reset(sampleRate, 0.02);
    feedbackDampingCutoffSmoothed.setCurrentAndTargetValue(kFeedbackDampingMaxHz);

    feedbackCircuitBreakerTripped.store(false);
}

void CORRUPTRAudioProcessor::releaseResources()
{
    // Nothing to release for Phase 3.1 — the feedback delay line and
    // damping filters stay allocated at their prepared size. Revisit once
    // later DSP phases add larger buffers (e.g. the Glitch ring buffer).
}

void CORRUPTRAudioProcessor::resetFeedbackLoopChannel(int channel)
{
    // Real-time-safe circuit-breaker reset: bounded loop over the
    // already-allocated delay buffer (no allocation, deterministic
    // execution time), fills that channel's delay history with zero so no
    // NaN/Inf can recirculate.
    for (int i = 0; i < feedbackMaxDelaySamples; ++i)
        feedbackDelayLine.pushSample(channel, 0.0f);

    feedbackDampingFilter[(size_t) channel].reset();
    feedbackRmsEnvelope[(size_t) channel] = 0.0f;
}

void CORRUPTRAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;
    juce::ignoreUnused(midiMessages);

    for (int i = getTotalNumInputChannels(); i < getTotalNumOutputChannels(); ++i)
        buffer.clear(i, 0, buffer.getNumSamples());

    //=========================================================================
    // Stage 2 Phase 3.2: Unified Modulation Accumulator (Isolated)
    //
    // Read-only observation point: computes what a modulated `drive` value
    // WOULD be this block, using the generic ModulationAccumulator (see
    // dsp/ModulationAccumulator.h) fed with:
    //   - base:        real `drive` APVTS parameter
    //   - Sequencer:   SYNTHETIC stand-in (Sequencer doesn't exist until
    //                  Phase 3.7) - a slow, bounded, deterministic sine so
    //                  this is a genuine per-block computation, not a
    //                  compile-time constant, without needing a real clock
    //   - Mod Matrix:  SYNTHETIC stand-in (Mod Matrix doesn't exist until
    //                  Phase 3.8) - same idea, different rate, so the two
    //                  synthetic contributors are independently verifiable
    //   - Macro:       REAL `macroDamage` APVTS parameter (macros ARE real
    //                  Stage 1 parameters already) mapped through a
    //                  PLACEHOLDER 0-100% -> 0 to +8dB curve; the real
    //                  macroDamage->drive routing/weighting is MacroEngine's
    //                  job in Phase 3.8, not decided here
    //   - Performance Trigger: REAL `performanceDestroy` bool APVTS
    //                  parameter gates a PLACEHOLDER override value
    //                  (driveRangeMax); the real Performance Trigger
    //                  preset-diff semantics for "Destroy" are Phase 3.9's
    //                  job, not decided here - this only exercises the
    //                  override-wins-then-releases code path with a real,
    //                  automatable boolean instead of a hardcoded constant
    //
    // Result is stored in `phase32DriveModulationObservation` for
    // diagnostic/future-use only - it is NOT applied to any DSP processing
    // below. The Distortion Engine (architecture.md component #2, which
    // would actually consume a modulated `drive` value) doesn't exist yet -
    // built in Phase 3.3. This deliberately does NOT change Phase 3.1's
    // feedback-loop code or output below it in any way.
    //=========================================================================
    {
        constexpr float driveRangeMin = 0.0f;
        constexpr float driveRangeMax = 40.0f;

        auto* driveParam = parameters.getRawParameterValue("drive");
        auto* macroDamageParam = parameters.getRawParameterValue("macroDamage");
        auto* performanceDestroyParam = parameters.getRawParameterValue("performanceDestroy");

        const float driveBase = driveParam->load();
        const float macroDamagePct = macroDamageParam->load();
        const bool performanceDestroyActive = performanceDestroyParam->load() > 0.5f;

        // Advance the two synthetic phases once per block (bounded,
        // deterministic, no allocation - real-time safe). Rates are
        // arbitrary placeholders chosen only to be slow enough to be
        // musically-plausible modulation and mutually distinguishable.
        const double blockDurationSeconds = getSampleRate() > 0.0
                                                 ? static_cast<double>(buffer.getNumSamples()) / getSampleRate()
                                                 : 0.0;
        phase32SyntheticSequencerPhase += blockDurationSeconds * (2.0 * juce::MathConstants<double>::pi) * 0.5;  // 0.5 Hz synthetic stand-in
        phase32SyntheticModMatrixPhase += blockDurationSeconds * (2.0 * juce::MathConstants<double>::pi) * 0.13; // 0.13 Hz synthetic stand-in
        phase32SyntheticSequencerPhase = std::fmod(phase32SyntheticSequencerPhase, 2.0 * juce::MathConstants<double>::pi);
        phase32SyntheticModMatrixPhase = std::fmod(phase32SyntheticModMatrixPhase, 2.0 * juce::MathConstants<double>::pi);

        const float syntheticSequencerContribution = 3.0f * static_cast<float>(std::sin(phase32SyntheticSequencerPhase)); // +/-3dB
        const float syntheticModMatrixContribution = 2.0f * static_cast<float>(std::sin(phase32SyntheticModMatrixPhase)); // +/-2dB
        const float macroContribution = juce::jlimit(0.0f, 100.0f, macroDamagePct) * 0.08f; // 0-100% -> 0 to +8dB placeholder curve

        const float performanceDestroyOverrideValue = driveRangeMax; // placeholder ("Destroy" -> max drive); real diff decided in Phase 3.9

        const float modulatedDrive = ModulationAccumulator::accumulate(
            driveBase,
            syntheticSequencerContribution,
            syntheticModMatrixContribution,
            macroContribution,
            performanceDestroyActive,
            performanceDestroyOverrideValue,
            driveRangeMin,
            driveRangeMax);

        phase32DriveModulationObservation.store(modulatedDrive, std::memory_order_relaxed);
    }

    //=========================================================================
    // Stage 2 Phase 3.1: Feedback Routing Safety Validation (Isolated)
    //
    // Signal routing for THIS ISOLATED PHASE ONLY (per plan.md/architecture.md):
    //   input -> [SPLICE: Distortion Engine, placeholder passthrough for now]
    //         -> [SPLICE: Filter Stage, placeholder passthrough for now]
    //         -> in-loop damping filter (real, always active)
    //         -> soft-clamp gain (real, always active)
    //         -> RMS limiter (real, always active)
    //         -> circuit breaker (real, always active)
    //         -> DELAY stage write/read (real, always active - this is what
    //            closes the loop back into "Distortion input" next sample)
    //         -> summed back into output signal for this isolated test
    //
    // The two SPLICE points are marked with TODO comments below. Real
    // Distortion (#2) / Filter (#6) processing wires in during Phase
    // 3.3/3.4 — do NOT build throwaway versions of those here.
    //=========================================================================
    auto* feedbackAmountParam = parameters.getRawParameterValue("feedbackAmount");
    auto* feedbackDampingParam = parameters.getRawParameterValue("feedbackDamping");
    auto* microDelayTimeParam = parameters.getRawParameterValue("microDelayTime");

    const float feedbackAmountPct = feedbackAmountParam->load();
    const float feedbackDampingPct = feedbackDampingParam->load();
    const float microDelayMs = microDelayTimeParam->load();

    // Soft-clamp gain mapping (architecture.md "Feedback Safety Soft-Clamp"):
    // tanh() is bounded in [-1,1] for every finite input, so
    // internalGain <= kFeedbackMaxSafeGain (0.85) regardless of parameter value.
    const float targetInternalGain = kFeedbackMaxSafeGain * std::tanh(feedbackAmountPct / 100.0f);
    feedbackInternalGainSmoothed.setTargetValue(targetInternalGain);

    // In-loop damping: feedbackDamping -> one-pole lowpass cutoff.
    // 100% damping -> kFeedbackDampingMinHz (heaviest HF cut per pass)
    // 0%   damping -> kFeedbackDampingMaxHz (near-transparent)
    const float dampingNorm = juce::jlimit(0.0f, 1.0f, feedbackDampingPct / 100.0f);
    const double nyquistGuardHz = getSampleRate() > 0.0 ? getSampleRate() * 0.45 : kFeedbackDampingMaxHz;
    const float targetCutoffHz = juce::jmin(
        kFeedbackDampingMinHz * std::pow(kFeedbackDampingMaxHz / kFeedbackDampingMinHz, 1.0f - dampingNorm),
        static_cast<float>(nyquistGuardHz));
    feedbackDampingCutoffSmoothed.setTargetValue(targetCutoffHz);

    // microDelayTime binds directly to the feedback loop's internal DELAY
    // stage — resolves architecture.md's flagged spec gap ("feedback loop's
    // internal delay TIME not yet an explicit parameter"); parameter-spec.md
    // confirms this exact binding for microDelayTime.
    const double sr = getSampleRate() > 0.0 ? getSampleRate() : 44100.0;
    const float delaySamples = juce::jlimit(1.0f, static_cast<float>(feedbackMaxDelaySamples - 1),
                                             static_cast<float>(microDelayMs / 1000.0 * sr));
    feedbackDelayLine.setDelay(delaySamples);

    const int numSamples = buffer.getNumSamples();
    const int numChannels = juce::jmin(buffer.getNumChannels(), 2); // dual-mono per architecture.md

    for (int channel = 0; channel < numChannels; ++channel)
    {
        auto* channelData = buffer.getWritePointer(channel);
        auto& dampingFilter = feedbackDampingFilter[(size_t) channel];
        auto& rmsEnv = feedbackRmsEnvelope[(size_t) channel];

        for (int n = 0; n < numSamples; ++n)
        {
            const float internalGain = feedbackInternalGainSmoothed.getNextValue();
            const float cutoffHz = feedbackDampingCutoffSmoothed.getNextValue();
            dampingFilter.setCutoffFrequency(cutoffHz);

            const float x = channelData[n];

            // Read the currently-delayed feedback sample - the loop's memory.
            const float delayedFeedback = feedbackDelayLine.popSample(channel);

            // --- SPLICE POINT: Distortion Engine (architecture.md component #2) ---
            // TODO(Phase 3.3/3.4): replace this passthrough with the real
            // Distortion Engine's output. Feedback sums into the distortion
            // stage's input per architecture.md's DISTORTION -> FILTER ->
            // DELAY -> DISTORTION loop description.
            const float distortionInput = x + delayedFeedback;

            // --- SPLICE POINT: Filter Stage (architecture.md component #6) ---
            // TODO(Phase 3.3/3.4): replace this passthrough with a copy of
            // the Filter Stage's current settings. NOTE: high filterResonance
            // combined with high feedbackAmount is flagged as the single most
            // dangerous parameter combination in the plugin (architecture.md
            // Parameter Interactions) — re-validate safety once this splice
            // is real.
            const float filterOutput = distortionInput;

            // In-loop HF damping — real, always active (NOT a placeholder).
            float loopSample = dampingFilter.processSample(0, filterOutput);

            // Soft-clamp gain — real, always active.
            loopSample *= internalGain;

            // Recommended additional safety net: continuous RMS-envelope
            // limiter on the feedback path itself (architecture.md component #7).
            rmsEnv = feedbackRmsOnePoleCoeff * rmsEnv + (1.0f - feedbackRmsOnePoleCoeff) * (loopSample * loopSample);
            const float currentRms = std::sqrt(juce::jmax(0.0f, rmsEnv));
            if (currentRms > kFeedbackRmsLimitThreshold)
                loopSample *= (kFeedbackRmsLimitThreshold / currentRms);

            // Circuit breaker: checked every sample (not just once per
            // block) since a NaN/Inf sample can persist indefinitely once
            // it enters a feedback loop, unlike a purely feedforward chain.
            // This covers corruption INTERNAL to the loop (damping filter /
            // gain stage / RMS math).
            if (! std::isfinite(loopSample))
            {
                resetFeedbackLoopChannel(channel);
                loopSample = 0.0f;
                feedbackCircuitBreakerTripped.store(true, std::memory_order_relaxed);
            }

            // DELAY stage write: this pushed sample is what popSample()
            // above will read back `delaySamples` from now, closing the loop.
            feedbackDelayLine.pushSample(channel, loopSample);

            // Sum feedback contribution back into the signal so this
            // isolated phase is self-contained and testable end-to-end.
            // Final signal-chain mix position (post-Filter, pre-Master-Mix
            // per architecture.md's Processing Chain) is finalized during
            // Phase 3.4 integration into the full core chain.
            float finalOutput = x + loopSample;

            // Final output-stage guard: the isfinite() check above only
            // covers corruption generated INSIDE the loop - it does not
            // cover a non-finite value arriving via the raw host input `x`
            // itself (e.g. a misbehaving upstream plugin). Catch that case
            // here too so the "never propagates to output" guarantee holds
            // regardless of where the NaN/Inf originated.
            if (! std::isfinite(finalOutput))
            {
                finalOutput = 0.0f;
                feedbackCircuitBreakerTripped.store(true, std::memory_order_relaxed);
            }

            channelData[n] = finalOutput;
        }
    }

    // Defensive only (shouldn't occur for this stereo-in/stereo-out effect):
    // mirror channel 0's processed result into any additional channels
    // rather than leaving them unprocessed.
    for (int channel = numChannels; channel < buffer.getNumChannels(); ++channel)
        buffer.copyFrom(channel, 0, buffer, 0, 0, numSamples);
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
