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

    //=========================================================================
    // Stage 2 Phase 3.5: Glitch Engine RNG seed — architecture.md requires a
    // SEEDED juce::Random instance (not juce::Random::getSystemRandom())
    // whose seed persists through getStateInformation/setStateInformation so
    // presets reproduce identical glitch behavior across sessions. The
    // system RNG is used here ONCE, at construction (message thread, plugin
    // instantiation — not the audio thread), purely as a one-time entropy
    // source for the initial seed; every subsequent glitch-engine random
    // draw uses `glitchRandom` (seeded from this value in prepareToPlay),
    // never the system RNG again.
    //=========================================================================
    glitchRandomSeedAtomic.store(juce::Random::getSystemRandom().nextInt64(), std::memory_order_relaxed);
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

    //=========================================================================
    // Stage 2 Phase 3.3: Core Linear Chain
    //=========================================================================
    inputGainDsp.prepare(spec);
    inputGainDsp.reset();
    inputGainDsp.setRampDurationSeconds(0.02); // 10-20ms smoothed ramp per architecture.md component #1

    driveGainSmoothed.reset(sampleRate, 0.02);
    driveGainSmoothed.setCurrentAndTargetValue(juce::Decibels::decibelsToGain(6.0f)); // matches `drive` default (6dB)
    distortionMixSmoothed.reset(sampleRate, 0.02);
    distortionMixSmoothed.setCurrentAndTargetValue(1.0f); // matches `distortionMix` default (100%)

    distortionToneLpState.fill(0.0f);
    distortionDcBlockerX1.fill(0.0f);
    distortionDcBlockerY1.fill(0.0f);
    // Fixed 1kHz pole for the tone-tilt filter's internal one-pole LP.
    distortionToneLpCoeff = 1.0f - std::exp(-2.0f * juce::MathConstants<float>::pi * 1000.0f / static_cast<float>(sampleRate));
    // Fixed ~5Hz pole for the DC blocker (architecture.md component #2).
    distortionDcBlockerR = std::exp(-2.0f * juce::MathConstants<float>::pi * 5.0f / static_cast<float>(sampleRate));

    for (auto& osc : ringModOsc)
    {
        osc.initialise([](float phase) { return std::sin(phase); }, 128);
        osc.prepare(monoSpec);
        osc.reset();
    }

    bitcrushHeldSample.fill(0.0f);
    bitcrushHoldCounter.fill(0);

    //=========================================================================
    // Stage 2 Phase 3.7: Rhythmic Sequencer — Volume+Gate combined-gain
    // smoothing. 8ms ramp: fast enough to feel rhythmically tight/gate-like
    // (this lane pair is meant to read as a step-sequenced level/gate, not
    // a slow automation curve) while still avoiding a hard click at step
    // boundaries — see PluginProcessor.h's Phase 3.7 doc comment.
    // `rhythmicSequencer` itself needs no prepareToPlay() work (its two
    // snapshot buffers and working pattern are preallocated fixed-size
    // members, sized at construction, never resized here).
    //=========================================================================
    sequencerVolumeGateGainSmoothed.reset(sampleRate, 0.008);
    sequencerVolumeGateGainSmoothed.setCurrentAndTargetValue(1.0f);

    //=========================================================================
    // Stage 2 Phase 3.5: Glitch / Buffer Engine — ring buffer + sample-rate-
    // derived constants (see PluginProcessor.h's Phase 3.5 doc comment for
    // full design rationale).
    //
    // Ring buffer sizing: architecture.md "sized for the maximum musical
    // division - 4 bars at slowest supported tempo, e.g. 4 bars @ 40bpm @
    // 192kHz - allocate generously in prepareToPlay, never on audio
    // thread." Computed against the ACTUAL prepared `sampleRate` (not a
    // hardcoded 192kHz constant) combined with kGlitchMinSupportedBpm
    // (40bpm, architecture.md's own worst-case example) and a 4/4 time-
    // signature assumption for "1 bar" = 1 whole note = 4 quarter notes,
    // so the buffer is correctly generous whatever sample rate the host
    // actually uses while still covering the slowest-tempo/longest-
    // division case at that rate. Allocated ONCE here; never
    // resized/reallocated in processBlock().
    //=========================================================================
    {
        const double wholeNoteSecondsAtMinBpm = (60.0 / kGlitchMinSupportedBpm) * 4.0; // 4/4 time assumption
        const double maxDivisionSeconds = wholeNoteSecondsAtMinBpm * 4.0; // "4 bars" (the longest glitchBufferLength choice)
        glitchRingBufferLength = static_cast<int>(std::ceil(maxDivisionSeconds * sampleRate)) + samplesPerBlock + 4;
        glitchRingBuffer.setSize(numChannels, glitchRingBufferLength, false, true, true);
        glitchRingBuffer.clear();
    }
    glitchRingWritePos = 0;
    glitchCycleSampleCounter = 0;            // forces a cycle-boundary recompute on the very first processed sample
    glitchCurrentCycleLengthSamples = 0;
    glitchEventActiveThisCycle = false;
    glitchActiveWindowLengthSamples = 0;
    glitchWindowReadStartIndex = 0;
    glitchReverseDirection = false;
    glitchWindowReadPos.fill(0);

    glitchMinWindowSamples   = juce::jmax(8, static_cast<int>(std::round(sampleRate * 0.001)));                       // >=1ms floor, avoids degenerate near-zero windows
    glitchFreezeWindowSamples = juce::jmax(glitchMinWindowSamples, static_cast<int>(std::round(sampleRate * kGlitchFreezeWindowMs / 1000.0)));
    glitchLoopEdgeFadeSamples = juce::jmax(1, static_cast<int>(std::round(sampleRate * kGlitchLoopEdgeFadeMs / 1000.0)));

    glitchActiveMixSmoothed.reset(sampleRate, kGlitchCrossfadeMs / 1000.0);
    glitchActiveMixSmoothed.setCurrentAndTargetValue(0.0f);
    lastGlitchModeIndex = -1; // force clean per-block init on the first processBlock() call

    // RNG: apply whatever seed currently lives in the cross-thread atomic
    // (set in the constructor, or by a prior setStateInformation preset
    // restore). prepareToPlay always runs before processBlock and is not
    // itself real-time, so reseeding here (rather than lazily detecting a
    // change on the first block) is safe and simplest.
    {
        const juce::int64 seed = glitchRandomSeedAtomic.load(std::memory_order_relaxed);
        glitchRandom.setSeed(seed);
        glitchLastAppliedRandomSeed = seed;
    }

    //=========================================================================
    // Stage 2 Phase 3.6: Glitch Engine Tier 2 — Slice/Random Repeat shared
    // state + Granular Repeat's grain-voice scheduler + Hann window table.
    // See PluginProcessor.h's Phase 3.6 doc comment for full design
    // rationale. Grain length is FIXED (kGlitchGrainLengthMs, independent
    // of glitchBufferLength) specifically so the Hann table can be sized
    // and filled exactly once here — never resized/refilled in
    // processBlock().
    //=========================================================================
    glitchSliceOrder = { 0, 1, 2, 3, 4, 5, 6, 7 };
    glitchSliceCurrentIndex = 0;
    glitchTier2SharedReadPos = 0;
    glitchPitchJumpRatio = 1.0f;

    glitchGrainLengthSamples = juce::jmax(4, static_cast<int>(std::round(sampleRate * kGlitchGrainLengthMs / 1000.0)));
    glitchGrainSpawnIntervalSamples = juce::jmax(1, glitchGrainLengthSamples / kGlitchGranularVoiceCount);
    glitchGrainWindowFn.fillWindowingTables(static_cast<size_t>(glitchGrainLengthSamples),
                                             juce::dsp::WindowingFunction<float>::WindowingMethod::hann,
                                             true);
    glitchGrainWindowTable.assign(static_cast<size_t>(glitchGrainLengthSamples), 1.0f);
    glitchGrainWindowFn.multiplyWithWindowingTable(glitchGrainWindowTable.data(),
                                                    static_cast<size_t>(glitchGrainLengthSamples));
    for (auto& v : glitchGrainVoices)
    {
        v.active = false;
        v.startRingIndex = 0;
        v.phase = 0;
    }
    glitchGrainNextVoiceSlot = 0;
    glitchGrainSpawnCountdown = 0;

    for (auto& f : svfFilter)
    {
        f.prepare(monoSpec);
        f.reset();
    }
    notchX1.fill(0.0f); notchX2.fill(0.0f);
    notchY1.fill(0.0f); notchY2.fill(0.0f);

    combMaxDelaySamples = static_cast<int>(std::ceil(0.025 * sampleRate)) + 4; // 25ms headroom for the ~1-20ms comb spacing range
    for (auto& d : combDelay)
    {
        d.setMaximumDelayInSamples(combMaxDelaySamples);
        d.prepare(monoSpec);
        d.reset();
    }
    lastFilterTypeIndex = -1; // force a state reset + coefficient recompute on the first processBlock() call

    dryBuffer.setSize(numChannels, samplesPerBlock, false, true, true);
    dryBuffer.clear();
    masterDryGainSmoothed.reset(sampleRate, 0.02);
    masterDryGainSmoothed.setCurrentAndTargetValue(0.0f); // matches `mix` default (100% = fully wet)
    masterWetGainSmoothed.reset(sampleRate, 0.02);
    masterWetGainSmoothed.setCurrentAndTargetValue(1.0f);

    outputGainDsp.prepare(spec);
    outputGainDsp.reset();
    outputGainDsp.setRampDurationSeconds(0.02);

    coloredLimiterSaturation.functionToUse = [](float x)
    {
        // Fixed, gentle, always-on-in-Colored-mode saturation (architecture.md
        // component #13's recommended MVP approximation for GR-proportional
        // coloring, since juce::dsp::Limiter doesn't expose a GR signal).
        constexpr float driveAmt = 1.6f;
        return std::tanh(x * driveAmt) / std::tanh(driveAmt);
    };

    outputLimiter.prepare(spec);
    outputLimiter.reset();
    outputLimiter.setThreshold(-0.3f);
    outputLimiter.setRelease(50.0f);
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

    const int numSamples = buffer.getNumSamples();
    const int numChannels = juce::jmin(buffer.getNumChannels(), 2); // dual-mono per architecture.md

    //=========================================================================
    // Stage 2 Phase 3.3: Master Mix dry-signal capture (component #14)
    //
    // Captured HERE — the very first thing that happens in processBlock(),
    // before Input Gain and before Phase 3.2's diagnostic block — per
    // architecture.md component #14: "capturing dry signal at the very
    // start of processBlock(), before Input Gain, to match user expectation
    // that 'dry' means the original unprocessed signal". As of Phase 3.4,
    // `buffer` is no longer overwritten by an isolated feedback-loop test
    // harness before the real chain runs (that workaround was retired when
    // the feedback loop was integrated directly into the live per-sample
    // chain below), so no restore-from-dryBuffer step is needed anymore —
    // `dryBuffer` is read (not written back into `buffer`) only at the
    // Master Mix step inside the merged per-sample loop further down.
    //=========================================================================
    {
        const int channelsToCapture = juce::jmin(buffer.getNumChannels(), dryBuffer.getNumChannels());
        for (int ch = 0; ch < channelsToCapture; ++ch)
            dryBuffer.copyFrom(ch, 0, buffer, ch, 0, buffer.getNumSamples());
    }

    //=========================================================================
    // Stage 2 Phase 3.7: Rhythmic Sequencer — resolves this block's step
    // index (host-synced, or the explicit no-transport freeze-at-step-0
    // fallback) and computes all 10 lanes' raw contribution scalars, BEFORE
    // the Phase 3.2/3.7 drive-accumulate block below (which consumes
    // `sequencerDriveContributionDb`) and before Phase 3.3/3.5's per-block
    // sections further down (which consume the other lanes' contributions).
    // See PluginProcessor.h's Phase 3.7 doc comment and this method's own
    // definition (below processBlock()) for the full design rationale.
    //=========================================================================
    updateSequencerStepAndContributions();

    //=========================================================================
    // Stage 2 Phase 3.2 / 3.7: Unified Modulation Accumulator — `drive`
    // destination.
    //
    // Computes the modulated `drive` value using the generic
    // ModulationAccumulator (see dsp/ModulationAccumulator.h) fed with:
    //   - base:        real `drive` APVTS parameter
    //   - Sequencer:   AS OF PHASE 3.7, the REAL Rhythmic Sequencer's
    //                  `drive`-lane contribution (`sequencerDriveContributionDb`,
    //                  computed just above by updateSequencerStepAndContributions())
    //                  — replaces Phase 3.2's synthetic sine stand-in
    //   - Mod Matrix:  SYNTHETIC stand-in (Mod Matrix doesn't exist until
    //                  Phase 3.8) - unchanged from Phase 3.2
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
    // AS OF PHASE 3.7: the result is no longer diagnostic-only — it is
    // stored in `phase32DriveModulationObservation` and CONSUMED by
    // `driveGainSmoothed`'s target below (Phase 3.3's per-block parameter-
    // read section), the first destination where the unified modulation
    // accumulator's output reaches live DSP. This deliberately does not
    // change the Phase 3.4 feedback-routing integration's own logic.
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

        // Advance the Mod Matrix synthetic phase once per block (bounded,
        // deterministic, no allocation - real-time safe). Rate is an
        // arbitrary placeholder chosen only to be slow enough to be
        // musically-plausible modulation. (Sequencer's own synthetic phase
        // was retired in Phase 3.7 — see PluginProcessor.h.)
        const double blockDurationSeconds = getSampleRate() > 0.0
                                                 ? static_cast<double>(buffer.getNumSamples()) / getSampleRate()
                                                 : 0.0;
        phase32SyntheticModMatrixPhase += blockDurationSeconds * (2.0 * juce::MathConstants<double>::pi) * 0.13; // 0.13 Hz synthetic stand-in
        phase32SyntheticModMatrixPhase = std::fmod(phase32SyntheticModMatrixPhase, 2.0 * juce::MathConstants<double>::pi);

        const float syntheticModMatrixContribution = 2.0f * static_cast<float>(std::sin(phase32SyntheticModMatrixPhase)); // +/-2dB
        const float macroContribution = juce::jlimit(0.0f, 100.0f, macroDamagePct) * 0.08f; // 0-100% -> 0 to +8dB placeholder curve

        const float performanceDestroyOverrideValue = driveRangeMax; // placeholder ("Destroy" -> max drive); real diff decided in Phase 3.9

        const float modulatedDrive = ModulationAccumulator::accumulate(
            driveBase,
            sequencerDriveContributionDb, // REAL Sequencer `drive`-lane contribution (Phase 3.7)
            syntheticModMatrixContribution,
            macroContribution,
            performanceDestroyActive,
            performanceDestroyOverrideValue,
            driveRangeMin,
            driveRangeMax);

        phase32DriveModulationObservation.store(modulatedDrive, std::memory_order_relaxed);
    }

    //=========================================================================
    // Stage 2 Phase 3.4: Feedback Routing Integration (architecture.md
    // component #7) — parameter reads + smoothing-target setup only.
    //
    // Phase 3.1's isolated validation harness (which ran the loop's safety
    // math against a synthetic passthrough and discarded its result) has
    // been RETIRED. The loop's safety math itself — soft-clamp gain,
    // in-loop damping filter, continuous RMS-envelope limiter, per-sample
    // isfinite() circuit breaker — is unchanged and is now spliced directly
    // into the live per-sample chain below (see the merged Distortion ->
    // Bitcrush -> Filter -> Feedback-tap -> Master-Mix loop further down),
    // operating on REAL Distortion Engine / Filter Stage output instead of
    // a placeholder passthrough.
    //
    // TAP-POINT CONTRADICTION RESOLUTION (documented per Phase 3.3's own
    // precedent for resolving prose-vs-numbered-list conflicts in
    // architecture.md, e.g. its Output Gain/Limiter ordering note above
    // outputGainDsp's declaration): architecture.md component #7's prose
    // says the tap is "post-Distortion, routed through a copy of the Filter
    // stage's current settings, through a short DelayLine... then summed
    // back into the Distortion stage's input", while the "Sequential DSP
    // chain (REQUIRED order)" section's explicit numbered list places the
    // tap at step 10, immediately AFTER step 9 (Filter Stage) and BEFORE
    // step 11 (Master Mix) — i.e. post-Filter, not post-Distortion. This
    // implementation follows the Sequential DSP chain's explicit numbered
    // order as authoritative (same precedent as Phase 3.3's resolution),
    // meaning: the tap happens post-Filter-Stage (step 9's output), and the
    // already-built in-loop damping filter (`feedbackDampingFilter`) IS
    // component #7's "copy of the Filter stage" referenced in the prose —
    // NOT a second real juce::dsp::StateVariableTPTFilter instance running
    // the main Filter Stage's exact topology/resonance. This also matches
    // the Processing Chain ASCII diagram, which draws the tap arrow leaving
    // AFTER "Filter Stage" and the loop's own internal
    // "[Filter copy -> short Delay -> safety soft-clamp/limiter, NaN
    // guard]" box summing back into "Distortion Engine input, next block".
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
    // Unconditional — not bypassed or weakened by this integration.
    const float targetInternalGain = kFeedbackMaxSafeGain * std::tanh(feedbackAmountPct / 100.0f);
    feedbackInternalGainSmoothed.setTargetValue(targetInternalGain);

    // In-loop damping: feedbackDamping -> one-pole lowpass cutoff.
    // 100% damping -> kFeedbackDampingMinHz (heaviest HF cut per pass)
    // 0%   damping -> kFeedbackDampingMaxHz (near-transparent)
    // Unconditional — not bypassed or weakened by this integration.
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

    //=========================================================================
    // Stage 2 Phase 3.3: Core Linear Chain — per-block parameter reads
    //=========================================================================
    auto* inputGainParam          = parameters.getRawParameterValue("inputGain");
    auto* graphBypassSatParam     = parameters.getRawParameterValue("graphBypassSaturation");
    auto* distortionAlgoParam     = parameters.getRawParameterValue("distortionAlgorithm");
    // (`drive` is read in the Phase 3.2/3.7 merged accumulator block above.)
    auto* toneParam                = parameters.getRawParameterValue("tone");
    auto* biasParam                = parameters.getRawParameterValue("bias");
    auto* foldParam                = parameters.getRawParameterValue("fold");
    auto* distortionMixParam       = parameters.getRawParameterValue("distortionMix");
    auto* graphBypassWSParam       = parameters.getRawParameterValue("graphBypassWaveshaper");
    auto* graphBypassBCParam       = parameters.getRawParameterValue("graphBypassBitcrush");
    auto* bitDepthParam            = parameters.getRawParameterValue("bitDepth");
    auto* srrParam                 = parameters.getRawParameterValue("sampleRateReduction");
    auto* graphBypassGlitchParam   = parameters.getRawParameterValue("graphBypassGlitch");
    auto* glitchModeParam          = parameters.getRawParameterValue("glitchMode");
    auto* glitchBufferLenParam     = parameters.getRawParameterValue("glitchBufferLength");
    auto* glitchProbabilityParam   = parameters.getRawParameterValue("glitchProbability");
    auto* chaosParam               = parameters.getRawParameterValue("chaos");
    auto* graphBypassFiltParam     = parameters.getRawParameterValue("graphBypassFilter");
    auto* filterTypeParam          = parameters.getRawParameterValue("filterType");
    auto* filterCutoffParam        = parameters.getRawParameterValue("filterCutoff");
    auto* filterResonanceParam     = parameters.getRawParameterValue("filterResonance");
    auto* mixParam                 = parameters.getRawParameterValue("mix");
    auto* outputGainParam          = parameters.getRawParameterValue("outputGain");
    auto* outputLimiterStyleParam  = parameters.getRawParameterValue("outputLimiterStyle");

    distortionEngineBypassed  = graphBypassSatParam->load() > 0.5f;
    waveshaperCurveBypassed   = graphBypassWSParam->load() > 0.5f;
    distortionAlgorithmIndex  = static_cast<int>(distortionAlgoParam->load());
    distortionToneNorm        = juce::jlimit(-1.0f, 1.0f, toneParam->load() / 100.0f);
    distortionBiasOffset      = juce::jlimit(-1.0f, 1.0f, biasParam->load() / 100.0f) * 0.3f; // moderate pre-shaper DC offset range
    distortionFoldPct         = foldParam->load();

    // Stage 2 Phase 3.7: consumes the modulated `drive` value computed
    // above (Phase 3.2/3.7 merged accumulator block: base APVTS `drive` +
    // REAL Sequencer Drive-lane contribution + Mod Matrix stub (Phase 3.8)
    // + real macroDamage + real performanceDestroy override), replacing
    // the raw `driveParam->load()` read Phase 3.2 used before this
    // destination was wired live.
    driveGainSmoothed.setTargetValue(juce::Decibels::decibelsToGain(phase32DriveModulationObservation.load(std::memory_order_relaxed)));
    distortionMixSmoothed.setTargetValue(juce::jlimit(0.0f, 100.0f, distortionMixParam->load()) / 100.0f);

    bitcrushBypassed = graphBypassBCParam->load() > 0.5f;
    {
        // Stage 2 Phase 3.7: bitDepth's Sequencer contribution combines
        // through ModulationAccumulator, same "base + sequencer, Mod
        // Matrix/Macro/Performance stubbed at 0/false until Phase 3.8/3.9"
        // pattern as `drive` above.
        const float modulatedBitDepth = ModulationAccumulator::accumulate(
            bitDepthParam->load(), sequencerBitDepthContributionBits, 0.0f, 0.0f, false, 0.0f, 1.0f, 16.0f);
        const float bitDepth = juce::jlimit(1.0f, 16.0f, modulatedBitDepth);
        bitcrushLevels = juce::jmax(1.0f, std::pow(2.0f, bitDepth) - 1.0f);
    }
    {
        // Stage 2 Phase 3.7: sampleRateReduction's Sequencer contribution,
        // same pattern.
        const float modulatedSrr = ModulationAccumulator::accumulate(
            srrParam->load(), sequencerSampleRateContributionFactor, 0.0f, 0.0f, false, 0.0f, 1.0f, 48.0f);
        const float srr = juce::jlimit(1.0f, 48.0f, modulatedSrr);
        bitcrushHoldSamples = juce::jmax(1, static_cast<int>(std::round(srr)));
    }

    //=========================================================================
    // Stage 2 Phase 3.5: Glitch / Buffer Engine — per-block parameter reads
    // + housekeeping. Per-sample cycle-boundary/trigger/window-read logic
    // lives in the merged per-sample loop below (and in
    // setUpGlitchEventForThisCycle()/readGlitchWindowSample()); this block
    // only handles things that are correct to resolve once per block, per
    // this file's established convention (mirrors `lastFilterTypeIndex`'s
    // change-detection pattern and the feedback loop's per-block
    // smoothing-target setup above).
    //=========================================================================
    glitchBypassed          = graphBypassGlitchParam->load() > 0.5f;
    glitchModeIndex         = static_cast<int>(glitchModeParam->load());
    glitchBufferLengthIndex = static_cast<int>(glitchBufferLenParam->load());
    // Stage 2 Phase 3.7: glitchProbability's Sequencer contribution combines
    // through ModulationAccumulator, same pattern as drive/bitDepth/srr above.
    glitchProbabilityPct = juce::jlimit(0.0f, 100.0f, ModulationAccumulator::accumulate(
        glitchProbabilityParam->load(), sequencerGlitchProbabilityContributionPct, 0.0f, 0.0f, false, 0.0f, 0.0f, 100.0f));
    glitchChaosPct          = juce::jlimit(0.0f, 100.0f, chaosParam->load());

    // Mode-change detection (plan.md Phase 3.5 Test Criteria: "Mode
    // switching mid-playback doesn't produce discontinuities"): forces the
    // current cycle to end immediately (glitchEventActiveThisCycle=false,
    // which drives glitchActiveMixSmoothed's target to 0 below and ramps
    // cleanly to dry over kGlitchCrossfadeMs even if a window was
    // mid-playback) and restarts cycle timing cleanly under the new mode.
    if (glitchModeIndex != lastGlitchModeIndex)
    {
        glitchEventActiveThisCycle = false;
        glitchCycleSampleCounter = 0;      // forces a fresh cycle-boundary recompute on the very next sample
        glitchCurrentCycleLengthSamples = 0;
        lastGlitchModeIndex = glitchModeIndex;
    }

    // RNG reseed detection — audio-thread reads the cross-thread atomic
    // once per block (architecture.md Thread Boundaries: "read once per
    // relevant trigger event, not per-sample") and only touches the real
    // `glitchRandom` instance if a message-thread write (setStateInformation
    // preset restore) actually changed the seed since last block.
    {
        const juce::int64 pendingSeed = glitchRandomSeedAtomic.load(std::memory_order_relaxed);
        if (pendingSeed != glitchLastAppliedRandomSeed)
        {
            glitchRandom.setSeed(pendingSeed);
            glitchLastAppliedRandomSeed = pendingSeed;
        }
    }

    // Host BPM (architecture.md: "glitchBufferLength converts to sample
    // count via host BPM (from juce::AudioPlayHead)"). JUCE 8
    // std::optional<PositionInfo> API (architecture.md's own Sequencer
    // section flags the older getCurrentPosition() pattern as deprecated/
    // error-prone — using the current API here). Real-time-safe: no
    // allocation in getPlayHead()/getPosition(), and the fallback path
    // below (no host, or host doesn't report tempo) is a plain constant,
    // also allocation-free.
    float glitchHostBpm = kGlitchFallbackBpm;
    if (auto* playHead = getPlayHead())
    {
        if (const auto position = playHead->getPosition())
        {
            if (const auto bpm = position->getBpm())
                if (*bpm > 0.0)
                    glitchHostBpm = static_cast<float>(*bpm);
        }
    }

    // Nominal cycle length in samples for `glitchBufferLength`'s currently-
    // selected musical division, at `glitchHostBpm`. Chaos-scaled jitter
    // and the Bernoulli probability trial are applied per-CYCLE (not
    // per-block) in the per-sample loop below, using this nominal value as
    // their base — see PluginProcessor.h's Phase 3.5 doc comment for the
    // exact formula.
    const float glitchNominalCycleLengthSamplesF = [&]() -> float
    {
        const double wholeNoteSeconds = (60.0 / juce::jmax(1.0f, glitchHostBpm)) * 4.0; // 4/4 time assumption, documented in PluginProcessor.h
        const int divIdx = juce::jlimit(0, 8, glitchBufferLengthIndex);
        const double divisionSeconds = wholeNoteSeconds * static_cast<double>(kGlitchDivisionMultiplier[(size_t) divIdx]);
        return static_cast<float>(juce::jmax(1.0, divisionSeconds * sr));
    }();

    filterBypassed  = graphBypassFiltParam->load() > 0.5f;
    filterTypeIndex = static_cast<int>(filterTypeParam->load());
    {
        // Stage 2 Phase 3.7: filterCutoff's Sequencer contribution combines
        // through ModulationAccumulator, same pattern as the other lanes.
        const float modulatedCutoff = ModulationAccumulator::accumulate(
            filterCutoffParam->load(), sequencerFilterCutoffContributionHz, 0.0f, 0.0f, false, 0.0f, 20.0f, 20000.0f);
        const float cutoffHz     = juce::jlimit(20.0f, 20000.0f, modulatedCutoff);
        const float resonancePct = juce::jlimit(0.0f, 100.0f, filterResonanceParam->load());
        updateFilterParameters(cutoffHz, resonancePct, getSampleRate());
    }

    if (filterTypeIndex != lastFilterTypeIndex)
    {
        // architecture.md: "Filter must reset state on filterType change to
        // prevent transient bursts" — reset ALL topology states regardless
        // of which one is newly active, since any of them could hold stale
        // energy from the previously-active type. All three reset calls
        // are bounded/allocation-free (DelayLine::reset() and
        // StateVariableTPTFilter::reset() clear their already-allocated
        // internal buffers; the Notch state is just four floats).
        for (auto& f : svfFilter) f.reset();
        notchX1.fill(0.0f); notchX2.fill(0.0f);
        notchY1.fill(0.0f); notchY2.fill(0.0f);
        for (auto& d : combDelay) d.reset();
        lastFilterTypeIndex = filterTypeIndex;
    }

    // Master Mix target gains (0-200% per architecture.md component #14)
    {
        // Stage 2 Phase 3.7: mix's Sequencer contribution combines through
        // ModulationAccumulator, same pattern as the other lanes.
        const float modulatedMix = ModulationAccumulator::accumulate(
            mixParam->load(), sequencerMixContributionPct, 0.0f, 0.0f, false, 0.0f, 0.0f, 200.0f);
        const float mixPct = juce::jlimit(0.0f, 200.0f, modulatedMix);
        float dryGain, wetGain;
        if (mixPct <= 100.0f)
        {
            const float t = mixPct / 100.0f;
            dryGain = 1.0f - t;
            wetGain = t;
        }
        else
        {
            const float t = (mixPct - 100.0f) / 100.0f; // 0..1 across the 100-200% range
            constexpr float wetCeilingLinear = 1.9953f;  // +6dB at mix=200%, per architecture.md component #14
            dryGain = 0.0f;
            wetGain = 1.0f + t * (wetCeilingLinear - 1.0f);
        }
        masterDryGainSmoothed.setTargetValue(dryGain);
        masterWetGainSmoothed.setTargetValue(wetGain);
    }

    outputGainDsp.setGainDecibels(outputGainParam->load());
    const bool coloredLimiterMode = static_cast<int>(outputLimiterStyleParam->load()) == 1;

    // Input Gain (block-level, via juce::dsp::Gain — built-in 10-20ms ramp)
    {
        juce::dsp::AudioBlock<float> block(buffer);
        juce::dsp::ProcessContextReplacing<float> context(block);
        inputGainDsp.setGainDecibels(inputGainParam->load());
        inputGainDsp.process(context);
    }

    // Distortion Engine -> Bitcrusher/SRR -> Filter Stage -> Feedback Routing
    // tap (component #7) -> Master Mix. Single per-sample pass; ALL
    // sample-rate-shared SmoothedValues (including the two Phase 3.4
    // feedback-loop ones, `feedbackInternalGainSmoothed` and
    // `feedbackDampingCutoffSmoothed`) are advanced exactly ONCE per sample
    // index (outer loop), NOT once per channel (inner loop) — this keeps
    // their ramps correctly paced regardless of channel count, since a
    // single juce::SmoothedValue instance is shared across both channels
    // here. (Phase 3.1's original isolated harness advanced the two
    // feedback SmoothedValues once per channel-per-sample instead of once
    // per sample — a ramp-rate bug relative to this file's own stated
    // convention — corrected here as part of the Phase 3.4 merge.)
    for (int n = 0; n < numSamples; ++n)
    {
        const float driveGain            = driveGainSmoothed.getNextValue();
        const float distortionMixAmt     = distortionMixSmoothed.getNextValue();
        const float masterDryGain        = masterDryGainSmoothed.getNextValue();
        const float masterWetGain        = masterWetGainSmoothed.getNextValue();
        const float feedbackInternalGain = feedbackInternalGainSmoothed.getNextValue();
        const float feedbackCutoffHz     = feedbackDampingCutoffSmoothed.getNextValue();
        const float sequencerVolumeGateGain = sequencerVolumeGateGainSmoothed.getNextValue(); // Stage 2 Phase 3.7 -- see PluginProcessor.h doc comment

        //=====================================================================
        // Stage 2 Phase 3.5: Glitch / Buffer Engine — per-sample cycle/
        // trigger bookkeeping. Runs exactly ONCE per sample index (not once
        // per channel), matching this loop's established convention for
        // channel-shared state (see the block comment above this `for (int
        // n ...)` loop). This step is purely timing/RNG-based — it does NOT
        // touch audio data (the actual ring-buffer WRITE of each channel's
        // post-Bitcrush sample happens inside processGlitchEngine(), called
        // per-channel below, using `glitchWritePosThisSample` captured here
        // BEFORE any channel writes so both channels land at the same
        // ring-buffer index for this sample).
        //=====================================================================
        const int glitchWritePosThisSample = glitchRingWritePos;

        if (glitchCycleSampleCounter <= 0)
        {
            // New cycle boundary reached — recompute this cycle's length
            // (chaos-scaled jitter around the nominal BPM-derived division
            // length) and run the Bernoulli trigger trial (chaos-scaled
            // probability variance). Both RNG draws happen UNCONDITIONALLY
            // every cycle boundary, regardless of glitchMode/
            // graphBypassGlitch, so the draw sequence — and therefore
            // reproducibility from a given seed — depends only on tempo/
            // sample-rate/probability/chaos, never on mode selection or
            // bypass state (see PluginProcessor.h's Phase 3.5 doc comment).
            const float chaosNorm = glitchChaosPct / 100.0f;

            const float lengthJitter = (glitchRandom.nextFloat() * 2.0f - 1.0f) * chaosNorm * kGlitchChaosWindowLengthJitterRange;
            const int jitteredCycleLength = static_cast<int>(std::round(glitchNominalCycleLengthSamplesF * (1.0f + lengthJitter)));
            glitchCurrentCycleLengthSamples = juce::jlimit(1, glitchRingBufferLength - 4, jitteredCycleLength);

            const float probBase = glitchProbabilityPct / 100.0f;
            const float probJitter = (glitchRandom.nextFloat() * 2.0f - 1.0f) * chaosNorm * kGlitchChaosProbabilityJitterRange;
            const float effectiveProbability = juce::jlimit(0.0f, 1.0f, probBase + probJitter);
            const bool triggered = glitchRandom.nextFloat() < effectiveProbability;

            // Phase 3.6: all 17 non-Off glitchMode indices (1-17) are now
            // implemented (Tier 1 modes from Phase 3.5 + Tier 2 modes from
            // this phase) — widened from Phase 3.5's `isTier1PlayableMode`
            // (which only covered 6 indices) to `isImplementedMode`. Off
            // (0) remains the only index that can never go "active."
            const bool isImplementedMode = (glitchModeIndex >= 1 && glitchModeIndex <= 17);

            if (triggered && isImplementedMode && ! glitchBypassed)
            {
                glitchEventActiveThisCycle = true;
                setUpGlitchEventForThisCycle(glitchWritePosThisSample);
            }
            else
            {
                glitchEventActiveThisCycle = false;
            }

            glitchCycleSampleCounter = glitchCurrentCycleLengthSamples;
        }
        --glitchCycleSampleCounter;

        // graphBypassGlitch toggled ON mid-active-window: `glitchEventActiveThisCycle`
        // is only recomputed at cycle boundaries, so re-check bypass here too —
        // this drives the crossfade to dry smoothly (via glitchActiveMixSmoothed's
        // ramp below) even if bypass is engaged mid-window, rather than an abrupt cut.
        const bool glitchWantsWet = glitchEventActiveThisCycle && ! glitchBypassed;
        glitchActiveMixSmoothed.setTargetValue(glitchWantsWet ? 1.0f : 0.0f);
        const float glitchMixAmt = glitchActiveMixSmoothed.getNextValue();

        // Stage 2 Phase 3.6: shared (NOT per-channel) once-per-sample
        // advance for the three Tier-2 modes whose wrap-triggered state
        // change must execute exactly once per sample, not once per
        // channel-call — see PluginProcessor.h's Phase 3.6 "Shared vs.
        // per-channel state" doc note for why duplicating these per
        // channel would be incorrect (double RNG draws / double
        // index-advances / double grain spawns).
        if (glitchWantsWet)
        {
            if (glitchModeIndex == 9)
                advanceGlitchGranularVoicesForThisSample();
            else if (glitchModeIndex == 6 || glitchModeIndex == 7 || glitchModeIndex == 17)
                advanceGlitchTier2SharedState();
        }

        for (int channel = 0; channel < numChannels; ++channel)
        {
            auto* channelData = buffer.getWritePointer(channel);
            const float dry = dryBuffer.getReadPointer(channel)[n];

            // Feedback loop's stored/delayed sample — the recirculated
            // output of a PREVIOUS sample's Filter Stage, already run
            // through this loop's damping/soft-clamp/RMS-limiter/delay
            // chain. Read BEFORE this sample's Distortion Engine call so it
            // can be summed directly into this sample's Distortion input,
            // per architecture.md's Sequential DSP chain step 10 -> step 5
            // ("this block's feedback-loop-recirculated sample as part of
            // its input, from step 10 of the PREVIOUS block").
            const float delayedFeedback = feedbackDelayLine.popSample(channel);

            float s = channelData[n] + delayedFeedback;
            s = processDistortionEngine(s, channel, driveGain, distortionMixAmt);
            s = processBitcrusher(s, channel);
            s = processGlitchEngine(s, channel, glitchWritePosThisSample, glitchMixAmt);
            s = processFilterStage(s, channel);

            // `s` is now the Filter Stage's output (Sequential DSP chain
            // step 9) — this is BOTH (a) what continues forward to Master
            // Mix below, unmodified, AND (b) what gets tapped for the
            // Feedback Routing Path (step 10). See the tap-point
            // contradiction resolution documented above this loop's
            // parameter-read section.
            const float filterStageOutput = s;

            // --- Feedback Routing Path (architecture.md component #7) ---
            // In-loop HF damping — functions as component #7's "copy of
            // the Filter stage" per this file's tap-point resolution — real,
            // unconditional, always active (not gated by any bypass here).
            auto& dampingFilter = feedbackDampingFilter[(size_t) channel];
            dampingFilter.setCutoffFrequency(feedbackCutoffHz);
            float loopSample = dampingFilter.processSample(0, filterStageOutput);

            // Soft-clamp gain — real, unconditional, always active. tanh()
            // is bounded in [-1,1] for all finite inputs, so the *parameter
            // itself* can never command unity/runaway gain regardless of
            // setting, even now that real nonlinear Distortion/Filter
            // output feeds this loop instead of a synthetic passthrough.
            loopSample *= feedbackInternalGain;

            // Continuous RMS-envelope limiter on the feedback path itself —
            // architecture.md's recommended belt-and-suspenders addition,
            // independent of both the tanh gain clamp above and the main
            // Output Limiter (component #13) downstream. This is the
            // primary safety net for the flagged "high filterResonance +
            // high feedbackAmount" danger case (a resonant Filter Stage can
            // boost energy at the cutoff frequency beyond the Distortion
            // stage's own saturation ceiling) and for unbounded Distortion
            // algorithms (e.g. Ring-Mod at high Drive, which is NOT
            // amplitude-bounded like the tanh/clip algorithms) reaching the
            // loop before the main Output Limiter ever sees them, since the
            // feedback tap sits upstream of Master Mix/Output Limiter.
            auto& rmsEnv = feedbackRmsEnvelope[(size_t) channel];
            rmsEnv = feedbackRmsOnePoleCoeff * rmsEnv + (1.0f - feedbackRmsOnePoleCoeff) * (loopSample * loopSample);
            const float currentRms = std::sqrt(juce::jmax(0.0f, rmsEnv));
            if (currentRms > kFeedbackRmsLimitThreshold)
                loopSample *= (kFeedbackRmsLimitThreshold / currentRms);

            // Circuit breaker — checked every sample (not just once per
            // block), unconditional, real. A NaN/Inf sample can persist
            // indefinitely once it enters a feedback loop, unlike a purely
            // feedforward chain, so this must never be skipped. Now guards
            // REAL nonlinear Distortion/Filter output flowing through the
            // loop (previously only ever saw a synthetic passthrough).
            if (! std::isfinite(loopSample))
            {
                resetFeedbackLoopChannel(channel);
                loopSample = 0.0f;
                feedbackCircuitBreakerTripped.store(true, std::memory_order_relaxed);
            }

            // DELAY stage write: closes the loop — this pushed sample is
            // what popSample() will read back `delaySamples` from now,
            // becoming a FUTURE sample's/block's Distortion Engine input
            // contribution (the "DELAY -> DISTORTION" half of the
            // DISTORTION -> FILTER -> DELAY -> DISTORTION loop).
            feedbackDelayLine.pushSample(channel, loopSample);

            // Master Mix (component #14): the main signal path continues
            // forward using the Filter Stage's own output (untouched by the
            // feedback tap above, which only READ it) — dry captured at
            // step 1, before Input Gain.
            float mixed = dry * masterDryGain + filterStageOutput * masterWetGain;

            // Stage 2 Phase 3.7: Sequencer Volume+Gate lanes -- see
            // updateSequencerStepAndContributions() for the combined-gain
            // computation and PluginProcessor.h's Phase 3.7 doc comment for
            // the "new destination, full range" scaling rationale. Applied
            // here (post-Master-Mix, pre-Output-Gain, pre-final-finite-
            // check) so a corrupt combined gain is still caught by the
            // existing isfinite() guard immediately below.
            mixed *= sequencerVolumeGateGain;

            // Final output-stage guard: catches a non-finite value arriving
            // via the raw host input itself (e.g. a misbehaving upstream
            // plugin) or generated anywhere in the now-real, nonlinear
            // linear chain, so the "never propagates to output" guarantee
            // holds regardless of where the NaN/Inf originated. This does
            // not replace the feedback loop's own internal circuit breaker
            // above (which specifically protects the recirculating delay
            // line's state) — it is a second, independent guard at the
            // point where a sample is about to leave this function.
            if (! std::isfinite(mixed))
            {
                mixed = 0.0f;
                feedbackCircuitBreakerTripped.store(true, std::memory_order_relaxed);
            }

            channelData[n] = mixed;
        }

        // Advance the Glitch ring buffer's shared write index exactly ONCE
        // per sample, AFTER both channels have written their post-Bitcrush
        // sample at `glitchWritePosThisSample` inside processGlitchEngine()
        // above (called once per channel, same index each time this sample).
        glitchRingWritePos = (glitchRingWritePos + 1) % glitchRingBufferLength;
    }

    // Output Gain (block-level). CONTRADICTION RESOLUTION: applied BEFORE
    // the Output Limiter, per the Sequential DSP chain's explicit numbered
    // order (step 12 before step 13) — see header comment above
    // outputGainDsp's declaration for the full contradiction note.
    {
        juce::dsp::AudioBlock<float> block(buffer);
        juce::dsp::ProcessContextReplacing<float> context(block);
        outputGainDsp.process(context);
    }

    // Output Limiter (dual-mode). Colored mode runs a fixed, gentle,
    // always-on saturation stage just before the limiter (architecture.md
    // component #13's recommended MVP approximation for GR-proportional
    // coloring); Transparent mode is the limiter alone.
    {
        juce::dsp::AudioBlock<float> block(buffer);
        juce::dsp::ProcessContextReplacing<float> context(block);
        if (coloredLimiterMode)
            coloredLimiterSaturation.process(context);
        outputLimiter.process(context);
    }

    // Defensive only (shouldn't occur for this stereo-in/stereo-out effect):
    // mirror channel 0's final processed result into any additional
    // channels rather than leaving them unprocessed.
    for (int channel = numChannels; channel < buffer.getNumChannels(); ++channel)
        buffer.copyFrom(channel, 0, buffer, 0, 0, numSamples);
}

//==============================================================================
// Stage 2 Phase 3.7: Rhythmic Sequencer — helper method implementation.
// See PluginProcessor.h's Phase 3.7 doc comment for the full design
// rationale (double-buffered snapshot mechanism, host-sync + no-transport
// fallback, per-block step-boundary resolution, destination wiring, and
// per-lane modulation-depth scaling).
//==============================================================================
void CORRUPTRAudioProcessor::updateSequencerStepAndContributions()
{
    auto* enabledParam = parameters.getRawParameterValue("sequencerEnabled");
    auto* rateParam     = parameters.getRawParameterValue("sequencerRate");
    auto* stepsParam    = parameters.getRawParameterValue("sequencerSteps");

    sequencerEnabledFlag = enabledParam->load() > 0.5f;
    sequencerRateIndex   = static_cast<int>(rateParam->load());
    sequencerNumSteps    = (static_cast<int>(stepsParam->load()) == 1) ? 32 : 16; // sequencerSteps choice index 0="16", 1="32"

    // Host-synced step-rate divisions, expressed as a fraction of one
    // quarter note (beats) — matches parameter-spec.md's sequencerRate
    // choice order exactly: 1/4, 1/8, 1/16, 1/32, 1/4T, 1/8T, 1/16T.
    static constexpr std::array<double, 7> kSequencerRateBeats {
        1.0, 0.5, 0.25, 0.125, (1.0 / 1.5), (0.5 / 1.5), (0.25 / 1.5)
    };
    const double stepLengthBeats = kSequencerRateBeats[(size_t) juce::jlimit(0, 6, sequencerRateIndex)];

    // Host sync (architecture.md component #5): uses the SAME JUCE 8
    // std::optional<PositionInfo> AudioPlayHead::getPosition() API Phase
    // 3.5 already established for glitchHostBpm's per-block read (getBpm())
    // — here reading getIsPlaying()/getPpqPosition() instead. This method
    // runs BEFORE Phase 3.5's own per-block BPM read further down in
    // processBlock(), so it issues its own getPosition() call rather than
    // sharing Phase 3.5's; both are cheap, allocation-free, real-time-safe
    // stack-based std::optional reads, so querying getPosition() twice per
    // block has no meaningful cost — this reuses the SAME established API
    // pattern, not a second different playhead-query mechanism.
    //
    // NO-TRANSPORT FALLBACK (architecture.md, explicit MVP requirement,
    // verified here as an EXPLICIT branch, not an accidental fallthrough):
    // if the host provides no AudioPlayHead, no PositionInfo, isPlaying()
    // is false, or the host doesn't report ppqPosition, `transportValid`
    // stays false and the sequencer FREEZES AT STEP 0 below — the simpler,
    // unambiguous option of architecture.md's "freeze at step 0 / hold
    // last value" phrasing (see PluginProcessor.h's Phase 3.7 doc comment
    // for the full reasoning).
    int stepIndex = 0;
    bool transportValid = false;
    if (auto* playHead = getPlayHead())
    {
        if (const auto position = playHead->getPosition())
        {
            if (position->getIsPlaying())
            {
                if (const auto ppq = position->getPpqPosition())
                {
                    const double stepIndexRaw = std::floor(*ppq / stepLengthBeats);
                    const int wrapped = static_cast<int>(std::fmod(stepIndexRaw, static_cast<double>(sequencerNumSteps)));
                    stepIndex = wrapped < 0 ? wrapped + sequencerNumSteps : wrapped;
                    transportValid = true;
                }
            }
        }
    }
    if (! transportValid)
        stepIndex = 0; // explicit freeze-at-step-0 fallback (see comment above) — not a default left over from an untaken branch

    sequencerLastStepIndex = stepIndex;

    //=========================================================================
    // PER-BLOCK (not per-sample) step-boundary resolution — FLAGGED DESIGN
    // RESOLUTION: architecture.md does not specify per-sample vs. per-block
    // granularity for sequencer step advancement. This method is called
    // exactly ONCE per processBlock() (see the call site above), unlike
    // Phase 3.5's Glitch Engine cycle counter (which advances per-SAMPLE
    // from a per-block BPM read). Reasoning:
    //   1. Even the fastest rate division (1/32T) at realistic tempos is
    //      still many samples long relative to a typical block size (64-
    //      1024 samples) — a step boundary landing mid-block is rare, and
    //      its audible impact is already bounded by the existing per-
    //      destination SmoothedValue ramps each consumed lane feeds into
    //      (driveGainSmoothed, masterDryGainSmoothed/masterWetGainSmoothed,
    //      sequencerVolumeGateGainSmoothed) — the same click-avoidance
    //      mechanism already relied on for ordinary host automation of
    //      those same parameters.
    //   2. This matches this file's OTHER established per-block-cache
    //      convention for "discrete, changes-are-relatively-rare" control
    //      state (glitchMode/filterTypeIndex change-detection, both
    //      resolved once per block), rather than the per-sample-counter
    //      convention reserved specifically for the Glitch Engine's own
    //      continuously-recirculating, self-timed cycle state.
    //   3. plan.md's Phase 3.7 test criterion is "steps advance correctly
    //      synced to host tempo/PPQ at all 7 rate divisions" — it does not
    //      require sample-accurate step-boundary placement, only correct
    //      tempo/PPQ synchronization, which this satisfies (the step index
    //      is computed directly from the host's live ppqPosition every
    //      block, not from a free-running internal counter that could
    //      drift out of sync).
    //=========================================================================

    const auto* snapshot = rhythmicSequencer.getActiveSnapshot(); // single relaxed atomic load, real-time-safe

    auto laneValue = [&](int laneIndex) -> float
    {
        if (snapshot == nullptr)
            return RhythmicSequencer::laneDefaultValue(laneIndex);
        const int s = juce::jlimit(0, RhythmicSequencer::kMaxSteps - 1, stepIndex);
        return snapshot->values[(size_t) laneIndex][(size_t) s];
    };

    // sequencerEnabled=false -> every lane reads as its own neutral/default
    // value (0 for bipolar lanes, 1.0/"fully open" for Gate) — matches
    // sequencerEnabled's documented ENABLE semantic (parameter-spec.md:
    // "module off -> modulation output disabled entirely").
    const float driveRaw        = sequencerEnabledFlag ? laneValue(RhythmicSequencer::laneDrive)             : 0.0f;
    const float mixRaw          = sequencerEnabledFlag ? laneValue(RhythmicSequencer::laneMix)               : 0.0f;
    const float filterCutoffRaw = sequencerEnabledFlag ? laneValue(RhythmicSequencer::laneFilterCutoff)      : 0.0f;
    const float bitDepthRaw     = sequencerEnabledFlag ? laneValue(RhythmicSequencer::laneBitDepth)          : 0.0f;
    const float sampleRateRaw   = sequencerEnabledFlag ? laneValue(RhythmicSequencer::laneSampleRate)        : 0.0f;
    const float glitchProbRaw   = sequencerEnabledFlag ? laneValue(RhythmicSequencer::laneGlitchProbability) : 0.0f;
    const float volumeRaw       = sequencerEnabledFlag ? laneValue(RhythmicSequencer::laneVolume)            : 0.0f;
    const float panRaw          = sequencerEnabledFlag ? laneValue(RhythmicSequencer::lanePan)               : 0.0f;
    const float pitchRaw        = sequencerEnabledFlag ? laneValue(RhythmicSequencer::lanePitch)             : 0.0f;
    const float gateRaw         = sequencerEnabledFlag ? laneValue(RhythmicSequencer::laneGate)
                                                        : RhythmicSequencer::laneDefaultValue(RhythmicSequencer::laneGate);

    //=========================================================================
    // Per-lane raw-value -> destination-units scaling (FLAGGED DESIGN
    // RESOLUTION — architecture.md does not specify modulation depth per
    // lane; see PluginProcessor.h's Phase 3.7 doc comment for the full
    // rationale of the two conventions used below).
    //=========================================================================
    sequencerDriveContributionDb              = driveRaw        * (40.0f    - 0.0f)  * kSequencerModDepthFraction;
    sequencerMixContributionPct               = mixRaw          * (200.0f   - 0.0f)  * kSequencerModDepthFraction;
    sequencerFilterCutoffContributionHz       = filterCutoffRaw * (20000.0f - 20.0f) * kSequencerModDepthFraction;
    sequencerBitDepthContributionBits         = bitDepthRaw     * (16.0f    - 1.0f)  * kSequencerModDepthFraction;
    sequencerSampleRateContributionFactor     = sampleRateRaw   * (48.0f    - 1.0f)  * kSequencerModDepthFraction;
    sequencerGlitchProbabilityContributionPct = glitchProbRaw   * (100.0f   - 0.0f)  * kSequencerModDepthFraction;

    // Volume (new destination, base=0dB/unity, full +/-kSequencerVolumeRangeDb
    // range) + Gate (new destination, base=1.0/fully-open, full 0..1 range)
    // both combine via ModulationAccumulator too, per this phase's
    // "sequencer output enters modulation ONLY through accumulate()"
    // requirement, even though there's no separate Mod Matrix/Macro/
    // Performance Trigger contributor for either yet (all stubbed at
    // 0/false, same as every other lane above — Phase 3.8/3.9's job to add
    // real contributions).
    const float modulatedVolumeDb = ModulationAccumulator::accumulate(
        0.0f, volumeRaw * kSequencerVolumeRangeDb, 0.0f, 0.0f, false, 0.0f, -kSequencerVolumeRangeDb, kSequencerVolumeRangeDb);
    const float modulatedGate = ModulationAccumulator::accumulate(
        1.0f, gateRaw - 1.0f, 0.0f, 0.0f, false, 0.0f, 0.0f, 1.0f);

    sequencerVolumeGateGainTarget = juce::Decibels::decibelsToGain(modulatedVolumeDb) * modulatedGate;
    sequencerVolumeGateGainSmoothed.setTargetValue(sequencerVolumeGateGainTarget);

    // Pan / Pitch: NOT YET CONSUMED — Stereo and Pitch/Frequency FX are
    // explicitly post-MVP per architecture.md's Scope Reconciliation Note
    // ("Pitch/Frequency FX and Stereo... are explicitly post-MVP"). Still
    // computed and clamped through the SAME ModulationAccumulator path as
    // every consumed lane, stored in diagnostic atomics for when a future
    // Stage wires a real destination — matches Phase 3.2's own "computed
    // but not yet applied to any DSP" precedent exactly.
    const float modulatedPan = ModulationAccumulator::accumulate(
        0.0f, panRaw * 100.0f, 0.0f, 0.0f, false, 0.0f, -100.0f, 100.0f);
    const float modulatedPitch = ModulationAccumulator::accumulate(
        0.0f, pitchRaw * 24.0f, 0.0f, 0.0f, false, 0.0f, -24.0f, 24.0f);
    sequencerPanObservation.store(modulatedPan, std::memory_order_relaxed);
    sequencerPitchObservation.store(modulatedPitch, std::memory_order_relaxed);
}

//==============================================================================
// Stage 2 Phase 3.3: Core Linear Chain — helper method implementations
//==============================================================================
float CORRUPTRAudioProcessor::processDistortionEngine(float xIn, int channel, float driveGain, float distortionMixAmt)
{
    if (distortionEngineBypassed)
        return xIn; // graphBypassSaturation: full stage bypass (dry passthrough of this graph node)

    const float xBiased = xIn * driveGain + distortionBiasOffset;

    float y;
    if (waveshaperCurveBypassed)
    {
        // graphBypassWaveshaper ("Fold Bypass" in the UI, ADV panel): skips
        // ONLY the nonlinear transfer function below, keeping drive/bias/
        // tone/distortionMix active. This is intentionally NOT the same as
        // graphBypassSaturation above (see PluginProcessor.h comment / JSON
        // report for the full non-redundancy reasoning).
        y = xBiased;
    }
    else
    {
        switch (distortionAlgorithmIndex)
        {
            case 0: // Saturation
                y = std::tanh(xBiased);
                break;

            case 1: // Soft Clip (cubic soft-knee)
                if (xBiased <= -1.0f)      y = -2.0f / 3.0f;
                else if (xBiased >= 1.0f)  y =  2.0f / 3.0f;
                else                       y = xBiased - (xBiased * xBiased * xBiased) / 3.0f;
                break;

            case 2: // Hard Clip
                y = juce::jlimit(-1.0f, 1.0f, xBiased);
                break;

            case 3: // Tube (asymmetric tanh w/ bias)
                y = xBiased >= 0.0f ? std::tanh(xBiased) : std::tanh(xBiased * 1.5f) * 0.75f;
                break;

            case 4: // Diode (piecewise exponential diode I-V curve approximation)
                y = xBiased >= 0.0f ? (1.0f - std::exp(-xBiased))
                                    : -(1.0f - std::exp(xBiased * 3.0f)) * 0.3f;
                break;

            case 5: // Fuzz (2-stage hard-clip cascade with extra gain)
            {
                const float stage1 = juce::jlimit(-1.0f, 1.0f, xBiased * 2.0f);
                y = juce::jlimit(-1.0f, 1.0f, stage1 * 2.0f);
                break;
            }

            case 6: // Digital Distortion (hard clip + fixed quantizer, reuses the bitcrush quantizer at a fixed depth)
            {
                const float clipped = juce::jlimit(-1.0f, 1.0f, xBiased);
                constexpr float fixedLevels = 15.0f; // fixed 4-bit-style quantizer
                y = std::round(clipped * fixedLevels) / fixedLevels;
                break;
            }

            case 7:  // Bitcrusher (distortionAlgorithm choice)
            case 8:  // Sample Rate Reducer (distortionAlgorithm choice)
                // architecture.md's explicit flagged resolution: these two
                // choices are a no-op passthrough in the waveshaper — the
                // actual crush/reduce happens in the always-on component #3
                // (Bitcrusher/SRR stage) regardless of algorithm selection.
                // Full passthrough (bypasses drive/bias/tone/mix too, since
                // this is meant to be a literal no-op node).
                return xIn;

            case 9: // Wavefolder (triangle fold, closed-form asin(sin()))
            {
                const float k = 1.0f + (distortionFoldPct / 100.0f) * 4.0f;
                const float folded = xBiased * k;
                y = (2.0f / juce::MathConstants<float>::pi)
                        * std::asin(std::sin(folded * juce::MathConstants<float>::halfPi));
                break;
            }

            case 10: // Ring-Mod (x * sine carrier). NOTE: no dedicated APVTS
                     // carrier-frequency parameter exists in parameter-spec.md
                     // for this MVP phase (architecture.md flags carrier freq
                     // as a future Mod Matrix destination) — `fold` is
                     // repurposed here as the carrier-frequency control
                     // (20Hz-5000Hz exponential) since Fold has no other
                     // effect on Ring-Mod. Documented in the JSON report.
            {
                auto& osc = ringModOsc[(size_t) channel];
                const float carrierNorm = juce::jlimit(0.0f, 1.0f, distortionFoldPct / 100.0f);
                const float carrierFreq = 20.0f * std::pow(250.0f, carrierNorm); // 20Hz .. 5000Hz
                osc.setFrequency(carrierFreq);
                const float carrierSample = osc.processSample(0.0f);
                y = xBiased * carrierSample;
                break;
            }

            case 11: // Foldback (reflect-at-threshold recursive fold, bounded loop — softer knee than Wavefolder)
            default:
            {
                const float threshold = juce::jmax(0.05f, 1.0f - (distortionFoldPct / 100.0f) * 0.5f);
                float v = xBiased;
                for (int iter = 0; iter < 8 && (v > threshold || v < -threshold); ++iter)
                {
                    if (v > threshold)       v = 2.0f * threshold - v;
                    else if (v < -threshold) v = -2.0f * threshold - v;
                }
                y = v;
                break;
            }
        }
    }

    // Post-shaper tone tilt (fixed 1kHz pole, hand-rolled one-pole — see
    // PluginProcessor.h for the real-time-safety rationale).
    auto& lpState = distortionToneLpState[(size_t) channel];
    lpState += distortionToneLpCoeff * (y - lpState);
    const float hp = y - lpState;
    y = y + distortionToneNorm * (hp - lpState) * 0.5f;

    // Post-shaper DC blocker (~5Hz, hand-rolled one-pole highpass) — always
    // active regardless of `bias` value, per architecture.md component #2.
    auto& x1 = distortionDcBlockerX1[(size_t) channel];
    auto& y1 = distortionDcBlockerY1[(size_t) channel];
    const float dcBlocked = y - x1 + distortionDcBlockerR * y1;
    x1 = y;
    y1 = dcBlocked;
    y = dcBlocked;

    // Stage-local dry/wet (`distortionMix`, independent of master `mix`).
    return xIn * (1.0f - distortionMixAmt) + y * distortionMixAmt;
}

float CORRUPTRAudioProcessor::processBitcrusher(float xIn, int channel)
{
    if (bitcrushBypassed)
        return xIn;

    auto& held = bitcrushHeldSample[(size_t) channel];
    auto& counter = bitcrushHoldCounter[(size_t) channel];

    if (counter <= 0)
    {
        held = xIn;
        counter = bitcrushHoldSamples;
    }
    --counter;

    // architecture.md Algorithm Details: "y[n] = quantize(hold(x[n], srrFactor), bitDepth)".
    return std::round(held * bitcrushLevels) / bitcrushLevels;
}

//==============================================================================
// Stage 2 Phase 3.5: Glitch / Buffer Engine (18 Modes) — Tier 1 helper method
// implementations. See PluginProcessor.h's Phase 3.5 doc comment for the
// full design rationale (ring buffer, trigger mechanism, chaos formula, RNG
// seeding, per-mode window semantics, click-free guarantees).
//==============================================================================
float CORRUPTRAudioProcessor::processGlitchEngine(float xIn, int channel, int writePos, float mixAmt)
{
    // Continuous ring-buffer write — unconditional, every sample, every
    // channel, regardless of whether a glitch is currently active or
    // graphBypassGlitch is on. architecture.md: "so there's always a
    // recent-history window available to capture from when a trigger
    // fires." (writePos is the same index for both channels this sample,
    // captured once in the outer per-sample loop before this call.)
    glitchRingBuffer.setSample(channel, writePos, xIn);

    if (mixAmt <= 0.0f)
        return xIn; // fully dry: Off/Tier-2 mode, no event active, bypassed, or mid-crossfade-release-to-dry — cheap early-out, no ring-buffer read needed

    const float wet = readGlitchWindowSample(channel);
    return xIn * (1.0f - mixAmt) + wet * mixAmt;
}

void CORRUPTRAudioProcessor::setUpGlitchEventForThisCycle(int writePosThisSample)
{
    // Called once, at the moment a new glitch event is triggered (cycle
    // boundary + Bernoulli trial succeeded + a Tier-1 mode is selected).
    // Captures the historical ring-buffer window this event will play back
    // for its duration — see PluginProcessor.h's "Per-mode window
    // semantics" doc comment for the rationale behind each mode's window
    // length.
    glitchReverseDirection = false;
    glitchTier2SharedReadPos = 0; // Slice/Random Slice/Random Repeat shared position — reset every new event regardless of mode (harmless no-op for modes that don't use it)
    int windowLen = glitchCurrentCycleLengthSamples;

    switch (glitchModeIndex)
    {
        case 1: // Stutter — short rapid-repeat slice, captured once, looped forward
            windowLen = juce::jmax(glitchMinWindowSamples,
                                    static_cast<int>(std::round(glitchCurrentCycleLengthSamples * kGlitchStutterFraction)));
            break;

        case 2: // Repeat — the full captured division, captured once, looped forward
            windowLen = glitchCurrentCycleLengthSamples;
            break;

        case 3: // Reverse — the full captured division, read back-to-front every pass
            windowLen = glitchCurrentCycleLengthSamples;
            glitchReverseDirection = true;
            break;

        case 4: // Buffer Repeat (Phase 3.6) — captured ONCE (fixed, like Stutter/Repeat), a
                // moderate-length chunk (kGlitchBufferRepeatFraction of the cycle), looped
                // forward every pass — NOT re-captured/re-selected (see Random Repeat, 17).
            windowLen = juce::jmax(glitchMinWindowSamples,
                                    static_cast<int>(std::round(glitchCurrentCycleLengthSamples * kGlitchBufferRepeatFraction)));
            break;

        case 5: // Freeze — very short ("~single-cycle" approximation), captured once, looped forward
            windowLen = juce::jmax(glitchMinWindowSamples, juce::jmin(glitchCurrentCycleLengthSamples, glitchFreezeWindowSamples));
            break;

        case 6:  // Slice (Phase 3.6) — full captured window subdivided into kGlitchSliceCount
                 // equal slices, played in natural order, looping.
        case 7:  // Random Slice (Phase 3.6) — same subdivision, but the play ORDER is
                 // shuffled ONCE here (Fisher-Yates, seeded glitchRandom) then held fixed
                 // for the rest of the event.
            windowLen = glitchCurrentCycleLengthSamples;
            {
                for (int i = 0; i < kGlitchSliceCount; ++i)
                    glitchSliceOrder[(size_t) i] = i;

                if (glitchModeIndex == 7)
                {
                    for (int i = kGlitchSliceCount - 1; i > 0; --i)
                    {
                        const int j = glitchRandom.nextInt(i + 1);
                        std::swap(glitchSliceOrder[(size_t) i], glitchSliceOrder[(size_t) j]);
                    }
                }
            }
            glitchSliceCurrentIndex = 0;
            break;

        case 8: // Micro Loop (Phase 3.6) — captured ONCE, finer-grained (shorter) than
                // Stutter, and TEMPO-RELATIVE (unlike Freeze's fixed-ms window).
            windowLen = juce::jmax(glitchMinWindowSamples,
                                    static_cast<int>(std::round(glitchCurrentCycleLengthSamples * kGlitchMicroLoopFraction)));
            break;

        case 9: // Granular Repeat (Phase 3.6) — reset all grain voices/spawn scheduler;
                // the per-sample scheduler (advanceGlitchGranularVoicesForThisSample())
                // spawns the first grain on its very next call.
            windowLen = glitchCurrentCycleLengthSamples;
            for (auto& v : glitchGrainVoices)
            {
                v.active = false;
                v.phase = 0;
            }
            glitchGrainNextVoiceSlot = 0;
            glitchGrainSpawnCountdown = 0;
            break;

        case 10: // Tape Stop (Phase 3.6) — full-cycle captured window; playback rate ramps
                 // 1.0->0.0 via a closed-form position integral (see readGlitchFractionalRateSample()).
            windowLen = glitchCurrentCycleLengthSamples;
            break;

        case 11: // Tape Start (Phase 3.6) — full-cycle captured window; playback rate ramps
                 // 0.0->1.0 via a closed-form position integral (see readGlitchFractionalRateSample()).
            windowLen = glitchCurrentCycleLengthSamples;
            break;

        case 12: // Retrigger — short slice; the ACTUAL capture is re-done fresh at the
                 // start of every loop pass inside readGlitchWindowSample(), so this
                 // initial capture is only what plays for the FIRST pass.
            windowLen = juce::jmax(glitchMinWindowSamples,
                                    static_cast<int>(std::round(glitchCurrentCycleLengthSamples * kGlitchRetriggerFraction)));
            break;

        case 13: // Silence — window length only paces the mute duration; readGlitchWindowSample() never reads the ring buffer for this mode
            windowLen = glitchCurrentCycleLengthSamples;
            break;

        case 14: // Noise Burst (Phase 3.6) — window length only paces the burst duration;
                 // readGlitchWindowSample() draws fresh noise instead of reading the ring buffer.
            windowLen = glitchCurrentCycleLengthSamples;
            break;

        case 15: // Bitcrush Burst (Phase 3.6) — full captured window (like Repeat), quantized
                 // at a fixed extreme depth AFTER the standard windowed read (self-contained
                 // to the Glitch Engine, not Component #3's bitDepth/sampleRateReduction).
            windowLen = glitchCurrentCycleLengthSamples;
            break;

        case 16: // Pitch Jump (Phase 3.6) — full-cycle captured window; choose a musical-interval
                 // playback ratio ONCE here (extra draw at event-setup/fired-event time, per the
                 // established RNG-invariant convention — see PluginProcessor.h's Phase 3.6 doc note).
            windowLen = glitchCurrentCycleLengthSamples;
            {
                const int idx = glitchRandom.nextInt(static_cast<int>(kGlitchPitchJumpRatios.size()));
                glitchPitchJumpRatio = kGlitchPitchJumpRatios[(size_t) idx];
            }
            break;

        case 17: // Random Repeat (Phase 3.6) — same window SIZE as Buffer Repeat, but
                 // readGlitchRandomRepeatSample()/advanceGlitchTier2SharedState() re-select a
                 // NEW random historical position every time the loop wraps.
            windowLen = juce::jmax(glitchMinWindowSamples,
                                    static_cast<int>(std::round(glitchCurrentCycleLengthSamples * kGlitchBufferRepeatFraction)));
            break;

        default:
            jassertfalse; // unreachable — guarded by isImplementedMode at the call site
            break;
    }

    windowLen = juce::jlimit(1, glitchRingBufferLength - 4, windowLen);
    glitchActiveWindowLengthSamples = windowLen;
    glitchWindowReadStartIndex = (writePosThisSample - windowLen + glitchRingBufferLength) % glitchRingBufferLength;
    glitchWindowReadPos[0] = 0;
    glitchWindowReadPos[1] = 0;
}

float CORRUPTRAudioProcessor::readGlitchWindowSample(int channel)
{
    // ---- Phase 3.6: Tier-2 modes with bespoke read machinery (fractional-
    // rate / shared-position / no-ring-buffer-read) dispatch to dedicated
    // helpers below. See PluginProcessor.h's Phase 3.6 doc comment for the
    // full per-mode design rationale.
    if (glitchModeIndex == 9) // Granular Repeat
        return readGlitchGranularSample(channel);

    if (glitchModeIndex == 10 || glitchModeIndex == 11 || glitchModeIndex == 16) // Tape Stop / Tape Start / Pitch Jump
        return readGlitchFractionalRateSample(channel);

    if (glitchModeIndex == 6 || glitchModeIndex == 7) // Slice / Random Slice
        return readGlitchSliceSample(channel);

    if (glitchModeIndex == 17) // Random Repeat
        return readGlitchRandomRepeatSample(channel);

    if (glitchModeIndex == 14) // Noise Burst — juce::Random noise, per-channel independent draws, no ring-buffer read
        return (glitchRandom.nextFloat() * 2.0f - 1.0f) * kGlitchNoiseBurstAmplitude;

    if (glitchModeIndex == 13) // Silence — hard-gated mute for the window duration; the
        return 0.0f;           // glitchActiveMixSmoothed crossfade still provides click-free fade in/out around this.

    // ---- Tier-1 modes (1/2/3/5/12) + Tier-2's simple fixed/fixed-loop
    // modes (4 Buffer Repeat, 8 Micro Loop, 15 Bitcrush Burst) all share
    // this SAME per-channel captured-window loop-read machinery — they
    // only differ in window LENGTH (set in setUpGlitchEventForThisCycle)
    // or, for Bitcrush Burst, one extra quantize step applied below.
    const int windowLen = juce::jmax(1, glitchActiveWindowLengthSamples);
    auto& readPos = glitchWindowReadPos[(size_t) channel];

    if (glitchModeIndex == 12 && readPos == 0)
    {
        // Retrigger: re-capture a fresh window from the CURRENT ring-buffer
        // write position at the start of EVERY loop pass (not just the
        // first) — the deliberate differentiator from Stutter/Repeat's
        // fixed-loop behavior. See PluginProcessor.h's Phase 3.5 doc
        // comment for the full rationale. (Both channels reach `readPos ==
        // 0` on the same sample since they always advance in lockstep, so
        // this recompute — though run once per channel — always produces
        // the same shared index.)
        glitchWindowReadStartIndex = (glitchRingWritePos - windowLen + glitchRingBufferLength) % glitchRingBufferLength;
    }

    const int idx = glitchReverseDirection
                        ? (glitchWindowReadStartIndex + (windowLen - 1 - readPos)) % glitchRingBufferLength
                        : (glitchWindowReadStartIndex + readPos) % glitchRingBufferLength;

    float sample = glitchRingBuffer.getSample(channel, idx);

    // Per-loop-pass click guard (plan.md Phase 3.5 Test Criteria: "click-free
    // buffer manipulation"): linear fade-in/fade-out of
    // `glitchLoopEdgeFadeSamples` at the START and END of EVERY pass through
    // the window (not just the first), so the read pointer's wrap-to-start
    // is never a hard discontinuity, no matter how many times it loops.
    const int fadeSamples = juce::jmin(windowLen / 4, glitchLoopEdgeFadeSamples);
    if (fadeSamples > 0)
    {
        if (readPos < fadeSamples)
            sample *= static_cast<float>(readPos) / static_cast<float>(fadeSamples);
        else if (readPos >= windowLen - fadeSamples)
            sample *= static_cast<float>(windowLen - 1 - readPos) / static_cast<float>(fadeSamples);
    }

    if (glitchModeIndex == 15) // Bitcrush Burst (Phase 3.6) — extreme fixed quantization applied
                               // AFTER the standard windowed read, self-contained to the Glitch Engine.
        sample = std::round(sample * kGlitchBitcrushBurstLevels) / kGlitchBitcrushBurstLevels;

    readPos += 1;
    if (readPos >= windowLen)
        readPos = 0; // loop back to window start (Stutter/Repeat/Reverse/Freeze/Buffer Repeat/Micro Loop/Bitcrush Burst); Retrigger's readPos==0 branch above then re-captures a fresh window on the NEXT call

    return sample;
}

//==============================================================================
// Stage 2 Phase 3.6: Glitch / Buffer Engine — Tier 2 helper method
// implementations. See PluginProcessor.h's Phase 3.6 doc comment for the
// full design rationale (per-mode window semantics, the DelayLine-vs-
// hand-rolled-Lagrange deviation, the shared-vs-per-channel state
// distinction, the RNG draw-sequence invariant, and the Granular windowing
// implementation note).
//==============================================================================
float CORRUPTRAudioProcessor::readGlitchRingBufferFractional(int channel, float fractionalIndex) const
{
    // 4-point, 3rd-order Lagrange interpolation — the same closed-form
    // cubic formula juce::dsp::DelayLineInterpolationTypes::Lagrange3rd
    // implements internally — applied directly to the already-captured,
    // static glitchRingBuffer content at an arbitrary fractional index.
    const int i0 = static_cast<int>(std::floor(fractionalIndex));
    const float frac = fractionalIndex - static_cast<float>(i0);

    const int len = juce::jmax(1, glitchRingBufferLength);
    auto wrap = [len](int idx)
    {
        idx %= len;
        if (idx < 0)
            idx += len;
        return idx;
    };

    const float ym1 = glitchRingBuffer.getSample(channel, wrap(i0 - 1));
    const float y0  = glitchRingBuffer.getSample(channel, wrap(i0));
    const float y1  = glitchRingBuffer.getSample(channel, wrap(i0 + 1));
    const float y2  = glitchRingBuffer.getSample(channel, wrap(i0 + 2));

    const float c0 = y0;
    const float c1 = y1 - (1.0f / 3.0f) * ym1 - 0.5f * y0 - (1.0f / 6.0f) * y2;
    const float c2 = 0.5f * (ym1 + y1) - y0;
    const float c3 = (1.0f / 6.0f) * (y2 - ym1) + 0.5f * (y0 - y1);

    return ((c3 * frac + c2) * frac + c1) * frac + c0;
}

float CORRUPTRAudioProcessor::readGlitchFractionalRateSample(int channel)
{
    // Elapsed samples within the current cycle — derived from the already-
    // shared glitchCurrentCycleLengthSamples/glitchCycleSampleCounter (no
    // new per-sample accumulator needed; independently, identically
    // computable by each channel's call — see PluginProcessor.h's Phase
    // 3.6 doc note).
    const int elapsed = juce::jlimit(0, juce::jmax(0, glitchCurrentCycleLengthSamples - 1),
                                      glitchCurrentCycleLengthSamples - 1 - glitchCycleSampleCounter);
    const float T = static_cast<float>(juce::jmax(1, glitchActiveWindowLengthSamples));
    const float t = static_cast<float>(elapsed);
    const float baseIndex = static_cast<float>(glitchWindowReadStartIndex);

    if (glitchModeIndex == 10) // Tape Stop: rate(t) = 1 - t/T -> position(t) = t - t^2/(2T)
    {
        const float position = juce::jlimit(0.0f, T - 1.0f, t - (t * t) / (2.0f * T));
        return readGlitchRingBufferFractional(channel, baseIndex + position);
    }

    if (glitchModeIndex == 11) // Tape Start: rate(t) = t/T -> position(t) = t^2/(2T)
    {
        const float position = juce::jlimit(0.0f, T - 1.0f, (t * t) / (2.0f * T));
        return readGlitchRingBufferFractional(channel, baseIndex + position);
    }

    // Pitch Jump (16): constant-rate read at glitchPitchJumpRatio, wrapping
    // within the window with the same linear loop-edge fade convention
    // Tier-1 uses at every wrap.
    const float rawPosition = t * glitchPitchJumpRatio;
    float wrapped = std::fmod(rawPosition, T);
    if (wrapped < 0.0f)
        wrapped += T;

    float gain = 1.0f;
    const float fadeF = static_cast<float>(juce::jmax(1, glitchLoopEdgeFadeSamples));
    if (wrapped < fadeF)
        gain = wrapped / fadeF;
    else if (wrapped > T - fadeF)
        gain = (T - wrapped) / fadeF;

    const float sample = readGlitchRingBufferFractional(channel, baseIndex + wrapped);
    return sample * juce::jlimit(0.0f, 1.0f, gain);
}

float CORRUPTRAudioProcessor::readGlitchSliceSample(int channel)
{
    // Slice (6) / Random Slice (7): glitchTier2SharedReadPos (position
    // within the CURRENT slice) and glitchSliceCurrentIndex (which slot in
    // glitchSliceOrder is currently playing) are both already resolved for
    // this sample by advanceGlitchTier2SharedState(), called once per
    // sample from processBlock() BEFORE the per-channel loop — this
    // function only reads, it never advances shared state itself.
    const int windowLen = juce::jmax(1, glitchActiveWindowLengthSamples);
    const int sliceLength = juce::jmax(1, windowLen / kGlitchSliceCount);
    const int sliceSlot = glitchSliceOrder[(size_t) juce::jlimit(0, kGlitchSliceCount - 1, glitchSliceCurrentIndex)];
    const int sliceStartOffset = sliceSlot * sliceLength;
    const int readPos = juce::jlimit(0, sliceLength - 1, glitchTier2SharedReadPos);

    const int idx = (glitchWindowReadStartIndex + sliceStartOffset + readPos) % glitchRingBufferLength;
    float sample = glitchRingBuffer.getSample(channel, idx);

    // Per-slice click guard (same fade convention as Tier-1's per-loop-pass
    // fade, applied within each slice's own boundaries rather than the
    // whole window).
    const int fadeSamples = juce::jmin(sliceLength / 4, glitchLoopEdgeFadeSamples);
    if (fadeSamples > 0)
    {
        if (readPos < fadeSamples)
            sample *= static_cast<float>(readPos) / static_cast<float>(fadeSamples);
        else if (readPos >= sliceLength - fadeSamples)
            sample *= static_cast<float>(sliceLength - 1 - readPos) / static_cast<float>(fadeSamples);
    }

    return sample;
}

float CORRUPTRAudioProcessor::readGlitchRandomRepeatSample(int channel)
{
    // Random Repeat (17): glitchTier2SharedReadPos and (on wrap)
    // glitchWindowReadStartIndex's random re-selection are both already
    // resolved for this sample by advanceGlitchTier2SharedState() — this
    // function only reads.
    const int windowLen = juce::jmax(1, glitchActiveWindowLengthSamples);
    const int readPos = juce::jlimit(0, windowLen - 1, glitchTier2SharedReadPos);
    const int idx = (glitchWindowReadStartIndex + readPos) % glitchRingBufferLength;
    float sample = glitchRingBuffer.getSample(channel, idx);

    const int fadeSamples = juce::jmin(windowLen / 4, glitchLoopEdgeFadeSamples);
    if (fadeSamples > 0)
    {
        if (readPos < fadeSamples)
            sample *= static_cast<float>(readPos) / static_cast<float>(fadeSamples);
        else if (readPos >= windowLen - fadeSamples)
            sample *= static_cast<float>(windowLen - 1 - readPos) / static_cast<float>(fadeSamples);
    }

    return sample;
}

float CORRUPTRAudioProcessor::readGlitchGranularSample(int channel)
{
    // Granular Repeat (9): sums every currently-active grain voice's
    // windowed contribution. Voice scheduling/phase-advance is handled
    // exclusively by advanceGlitchGranularVoicesForThisSample() (called
    // once per sample from processBlock(), NOT here) — this function only
    // reads the already-resolved voice state, once per channel.
    float sum = 0.0f;
    for (auto& v : glitchGrainVoices)
    {
        if (! v.active)
            continue;

        const int idx = (v.startRingIndex + v.phase) % glitchRingBufferLength;
        const float raw = glitchRingBuffer.getSample(channel, idx);
        const float win = glitchGrainWindowTable[(size_t) juce::jlimit(0, glitchGrainLengthSamples - 1, v.phase)];
        sum += raw * win;
    }

    // Fixed headroom scalar: with kGlitchGranularVoiceCount fixed
    // round-robin-spawned voices (not derived from Hann's exact constant-
    // overlap-add ratio), a small defensive scalar keeps overlapping grains
    // from summing to an unnecessarily hot level.
    return sum * 0.85f;
}

void CORRUPTRAudioProcessor::advanceGlitchGranularVoicesForThisSample()
{
    if (glitchGrainSpawnCountdown <= 0)
    {
        auto& voice = glitchGrainVoices[(size_t) glitchGrainNextVoiceSlot];

        // Random start position within the captured window — architecture.md:
        // "short overlapping grains" — a fresh draw from the seeded
        // glitchRandom instance per grain spawn while this event is active
        // (an "extra draw at the fired-event path", per the established RNG-
        // invariant convention documented in PluginProcessor.h).
        const int maxOffset = juce::jmax(1, glitchActiveWindowLengthSamples - glitchGrainLengthSamples);
        const int randomOffset = static_cast<int>(glitchRandom.nextFloat() * static_cast<float>(maxOffset));
        voice.startRingIndex = (glitchWindowReadStartIndex + randomOffset + glitchRingBufferLength) % glitchRingBufferLength;
        voice.phase = 0;
        voice.active = true;

        glitchGrainNextVoiceSlot = (glitchGrainNextVoiceSlot + 1) % kGlitchGranularVoiceCount;
        glitchGrainSpawnCountdown = glitchGrainSpawnIntervalSamples;
    }
    --glitchGrainSpawnCountdown;

    for (auto& v : glitchGrainVoices)
    {
        if (! v.active)
            continue;

        v.phase += 1;
        if (v.phase >= glitchGrainLengthSamples)
            v.active = false;
    }
}

void CORRUPTRAudioProcessor::advanceGlitchTier2SharedState()
{
    if (glitchModeIndex == 17) // Random Repeat
    {
        const int windowLen = juce::jmax(1, glitchActiveWindowLengthSamples);
        glitchTier2SharedReadPos += 1;
        if (glitchTier2SharedReadPos >= windowLen)
        {
            glitchTier2SharedReadPos = 0;

            // Re-select a NEW random historical window position, bounded to
            // stay within already-written ring-buffer history relative to
            // the CURRENT write position — a single shared draw from
            // glitchRandom per wrap (architecture.md: "Buffer Repeat/Random
            // Repeat... optional random window re-selection").
            const int maxBackOffset = juce::jmax(windowLen, glitchRingBufferLength - windowLen - 4);
            const int span = juce::jmax(1, maxBackOffset - windowLen);
            const int randomBackOffset = windowLen + static_cast<int>(glitchRandom.nextFloat() * static_cast<float>(span));
            glitchWindowReadStartIndex = (glitchRingWritePos - randomBackOffset + glitchRingBufferLength) % glitchRingBufferLength;
        }
        return;
    }

    // Slice (6) / Random Slice (7): advance position within the current
    // slice; on wrap, move to the NEXT slot in glitchSliceOrder
    // (predetermined once at event setup — Random Slice's shuffle already
    // happened then, so no further RNG draw is needed here, only a
    // deterministic index advance).
    const int windowLen = juce::jmax(1, glitchActiveWindowLengthSamples);
    const int sliceLength = juce::jmax(1, windowLen / kGlitchSliceCount);
    glitchTier2SharedReadPos += 1;
    if (glitchTier2SharedReadPos >= sliceLength)
    {
        glitchTier2SharedReadPos = 0;
        glitchSliceCurrentIndex = (glitchSliceCurrentIndex + 1) % kGlitchSliceCount;
    }
}

void CORRUPTRAudioProcessor::updateFilterParameters(float cutoffHz, float resonancePct, double sampleRate)
{
    const float resonanceNorm = juce::jlimit(0.0f, 1.0f, resonancePct / 100.0f);

    // StateVariableTPTFilter-backed topologies (LP/HP/BP/Resonant LP/Resonant HP).
    // architecture.md: "clamped to prevent self-oscillation blowup" —
    // regular LP/HP/BP get a conservative resonance ceiling; the two
    // "Resonant" variants get a higher ceiling (closer to, but still
    // safely below, self-oscillation) since that's their entire purpose.
    constexpr float kStandardResonanceMax = 8.0f;
    constexpr float kResonantVariantMax = 18.0f;
    auto svfType = juce::dsp::StateVariableTPTFilterType::lowpass;
    float svfResonance = 0.7071f; // default Butterworth Q

    switch (filterTypeIndex)
    {
        case 0: svfType = juce::dsp::StateVariableTPTFilterType::lowpass;  svfResonance = juce::jmap(resonanceNorm, 0.5f, kStandardResonanceMax); break; // LP
        case 1: svfType = juce::dsp::StateVariableTPTFilterType::highpass; svfResonance = juce::jmap(resonanceNorm, 0.5f, kStandardResonanceMax); break; // HP
        case 2: svfType = juce::dsp::StateVariableTPTFilterType::bandpass; svfResonance = juce::jmap(resonanceNorm, 0.5f, kStandardResonanceMax); break; // BP
        case 5: svfType = juce::dsp::StateVariableTPTFilterType::lowpass;  svfResonance = juce::jmap(resonanceNorm, 0.5f, kResonantVariantMax); break; // Resonant LP
        case 6: svfType = juce::dsp::StateVariableTPTFilterType::highpass; svfResonance = juce::jmap(resonanceNorm, 0.5f, kResonantVariantMax); break; // Resonant HP
        default: break; // Notch (3) / Comb (4) don't use the SVF path
    }

    for (auto& f : svfFilter)
    {
        f.setType(svfType);
        f.setCutoffFrequency(cutoffHz);
        f.setResonance(svfResonance);
    }

    // Notch (custom biquad, manual RBJ formula — see PluginProcessor.h for
    // why this avoids juce::dsp::IIR::Filter::makeNotch()'s heap allocation).
    {
        const float q = juce::jmap(resonanceNorm, 0.7071f, 12.0f);
        const float w0 = juce::MathConstants<float>::twoPi * cutoffHz / static_cast<float>(sampleRate);
        const float alpha = std::sin(w0) / (2.0f * q);
        const float cosw0 = std::cos(w0);
        const float a0 = 1.0f + alpha;
        notchB0 = 1.0f / a0;
        notchB1 = (-2.0f * cosw0) / a0;
        notchB2 = 1.0f / a0;
        notchA1 = (-2.0f * cosw0) / a0;
        notchA2 = (1.0f - alpha) / a0;
    }

    // Comb (custom, via DelayLine + feedback gain). architecture.md:
    // "~1-20ms, tunable via filterCutoff reinterpreted as comb spacing".
    {
        const float delayMs = juce::jlimit(0.5f, 20.0f, 1000.0f / juce::jmax(50.0f, cutoffHz));
        const float delaySamples = juce::jlimit(1.0f, static_cast<float>(combMaxDelaySamples - 1),
                                                 static_cast<float>(delayMs / 1000.0 * sampleRate));
        for (auto& d : combDelay)
            d.setDelay(delaySamples);
        combFeedbackGain = juce::jlimit(0.0f, 0.92f, resonanceNorm * 0.92f); // clamped well below 1.0 to prevent self-oscillation blowup
    }
}

float CORRUPTRAudioProcessor::processFilterStage(float xIn, int channel)
{
    if (filterBypassed)
        return xIn;

    switch (filterTypeIndex)
    {
        case 3: // Notch (custom biquad)
        {
            auto& x1 = notchX1[(size_t) channel];
            auto& x2 = notchX2[(size_t) channel];
            auto& y1 = notchY1[(size_t) channel];
            auto& y2 = notchY2[(size_t) channel];
            const float y = notchB0 * xIn + notchB1 * x1 + notchB2 * x2 - notchA1 * y1 - notchA2 * y2;
            x2 = x1; x1 = xIn;
            y2 = y1; y1 = y;
            return y;
        }

        case 4: // Comb (custom DelayLine + feedback)
        {
            auto& delay = combDelay[(size_t) channel];
            const float delayed = delay.popSample(0);
            const float y = xIn + combFeedbackGain * delayed;
            delay.pushSample(0, y);
            return y;
        }

        default: // LP(0)/HP(1)/BP(2)/Resonant LP(5)/Resonant HP(6) via StateVariableTPTFilter
            return svfFilter[(size_t) channel].processSample(0, xIn);
    }
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

    //=========================================================================
    // Stage 2 Phase 3.5: Glitch Engine RNG seed persistence.
    //
    // architecture.md component #4: "must use a seeded juce::Random
    // instance... so presets reproduce identical glitch behavior across
    // sessions - seed stored in custom state." architecture.md's State
    // Persistence section describes the general mechanism as a nested
    // juce::ValueTree merged into the same state blob (DrumRoulette
    // pattern); at this phase's scope (a single int64 seed - no other
    // custom state exists yet, Sequencer pattern data is Phase 3.7's job)
    // a plain XML attribute on the root element achieves the same result
    // without introducing a second ValueTree-merge mechanism a phase
    // early. Stored as a String attribute (not `setAttribute(name, int)`)
    // to preserve full int64 precision - juce::XmlElement's integer
    // attribute API is 32-bit only. A `corruptrStateVersion` attribute is
    // added alongside it per architecture.md's own "recommend a version
    // attribute in the root ValueTree from day one" advice.
    //=========================================================================
    xml->setAttribute("corruptrStateVersion", 1);
    xml->setAttribute("glitchRandomSeed", juce::String(glitchRandomSeedAtomic.load(std::memory_order_relaxed)));

    //=========================================================================
    // Stage 2 Phase 3.7: Rhythmic Sequencer pattern persistence.
    //
    // architecture.md's State Persistence section: "Custom state: nested
    // juce::ValueTree merged into the same state blob returned from
    // getStateInformation(), following the DrumRoulette pattern (APVTS
    // ValueTree + custom child ValueTree combined into one XML document)."
    // Extends the exact same custom-state mechanism Phase 3.5 established
    // for glitchRandomSeed (a plain attribute on this same root element),
    // but for the Sequencer's much larger pattern data (10 lanes x 32
    // steps) a full nested child XML element is used instead of a single
    // attribute — matching architecture.md's own DrumRoulette-pattern
    // description precisely, now that the state is large/structured enough
    // to warrant it. Message-thread only (getStateInformation() always
    // runs on the message thread) — see dsp/RhythmicSequencer.h's
    // toValueTree() for the actual serialization format.
    //=========================================================================
    {
        const juce::ValueTree patternTree = rhythmicSequencer.toValueTree();
        if (std::unique_ptr<juce::XmlElement> patternXml { patternTree.createXml() })
            xml->addChildElement(patternXml.release());
    }

    copyXmlToBinary(*xml, destData);
}

void CORRUPTRAudioProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xmlState(getXmlFromBinary(data, sizeInBytes));

    if (xmlState != nullptr && xmlState->hasTagName(parameters.state.getType()))
    {
        parameters.replaceState(juce::ValueTree::fromXml(*xmlState));

        //=====================================================================
        // Stage 2 Phase 3.5: Glitch Engine RNG seed restore.
        //
        // Message-thread write to the cross-thread atomic ONLY — the real
        // `glitchRandom` instance is reseeded by the audio thread once per
        // block (processBlock()'s per-block Glitch parameter-read section),
        // never mutated directly here, per architecture.md's Thread
        // Boundaries convention ("Glitch/Randomization seeds:
        // std::atomic<uint32_t> or similar, read once per relevant trigger
        // event, not per-sample"). This is how a user reloading a saved
        // preset gets IDENTICAL glitch timing: the exact same seed value
        // round-trips through getStateInformation -> XML attribute ->
        // setStateInformation -> this atomic -> the next processBlock()'s
        // reseed of `glitchRandom`, so the sequence of Bernoulli-trial/
        // jitter draws from that point forward is bit-for-bit identical to
        // any other session that starts from the same seed (the exact
        // wall-clock/transport-position alignment of those draws still
        // depends on where playback starts, same as any tempo-synced
        // effect - the RNG STREAM itself, not draw timing relative to
        // playback start, is what's made reproducible here).
        //
        // Missing/corrupt attribute (e.g. a preset saved before this phase
        // existed): architecture.md's Restore Behavior - "fall back to...
        // a fresh random seed - never crash on malformed preset data." A
        // fresh system-RNG-sourced seed is generated here (message thread,
        // one-time, non-real-time - same justification as the
        // constructor's initial seed) so an old preset still gets SOME
        // seeded, reproducible-from-here-forward behavior.
        //=====================================================================
        if (xmlState->hasAttribute("glitchRandomSeed"))
        {
            const juce::int64 restoredSeed = xmlState->getStringAttribute("glitchRandomSeed").getLargeIntValue();
            glitchRandomSeedAtomic.store(restoredSeed, std::memory_order_relaxed);
        }
        else
        {
            glitchRandomSeedAtomic.store(juce::Random::getSystemRandom().nextInt64(), std::memory_order_relaxed);
        }

        //=====================================================================
        // Stage 2 Phase 3.7: Rhythmic Sequencer pattern restore.
        //
        // architecture.md Restore Behavior: "Missing/corrupt custom state:
        // fall back to a default empty pattern... never crash on malformed
        // preset data." RhythmicSequencer::fromValueTree() already
        // implements this fallback internally (see dsp/RhythmicSequencer.h)
        // — if no child element is found here at all (e.g. a preset saved
        // before this phase existed), fromValueTree() is called with a
        // default-constructed (invalid) ValueTree, which its own internal
        // tag-name/validity check turns into the same "default empty
        // pattern" fallback, so this call site never needs a separate
        // empty-state branch. fromValueTree() also republishes the restored
        // (or default) pattern to the audio thread internally.
        //=====================================================================
        const auto* patternXml = xmlState->getChildByName("SequencerPattern");
        const juce::ValueTree patternTree = patternXml != nullptr
                                                 ? juce::ValueTree::fromXml(*patternXml)
                                                 : juce::ValueTree();
        rhythmicSequencer.fromValueTree(patternTree);
    }
}

//==============================================================================
// Factory function
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new CORRUPTRAudioProcessor();
}
