#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <cmath>

namespace
{
    float smoothstep(float x, float edge0, float edge1)
    {
        const float t = juce::jlimit(0.0f, 1.0f, (x - edge0) / (edge1 - edge0));
        return t * t * (3.0f - 2.0f * t);
    }

    // Delay Sync division table (parameter-spec.md v2), expressed in
    // quarter-note beats. Index matches delaySync's AudioParameterChoice
    // order exactly: 1/16, 1/8T, 1/8, 1/8D, 1/4, 1/4D, 1/2, 1/2D, 1/1.
    constexpr float kDivisionBeatFractions[9] =
    {
        0.25f,      // 0: 1/16
        1.0f / 3.0f,// 1: 1/8 triplet
        0.5f,       // 2: 1/8
        0.75f,      // 3: 1/8 dotted
        1.0f,       // 4: 1/4
        1.5f,       // 5: 1/4 dotted
        2.0f,       // 6: 1/2
        3.0f,       // 7: 1/2 dotted
        4.0f        // 8: 1/1 (whole)
    };
}

juce::AudioProcessorValueTreeState::ParameterLayout TransitionistAudioProcessor::createParameterLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    // transition - big hero macro, unipolar 0-100%
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "transition", 1 },
        "Transition",
        juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f, 1.0f),
        0.0f,
        "%"
    ));

    // reverb - stage send/mix, unipolar 0-100%
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "reverb", 1 },
        "Reverb",
        juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f, 1.0f),
        0.0f,
        "%"
    ));

    // delay - stage send/mix, unipolar 0-100%
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "delay", 1 },
        "Delay",
        juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f, 1.0f),
        0.0f,
        "%"
    ));

    // delaySync - choice, 9 tempo divisions, default index 4 ("1/4")
    layout.add(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID { "delaySync", 1 },
        "Delay Sync",
        juce::StringArray { "1/16", "1/8T", "1/8", "1/8D", "1/4", "1/4D", "1/2", "1/2D", "1/1" },
        4
    ));

    // dryWet - final blend, unipolar 0-100%, default 50%
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "dryWet", 1 },
        "Dry/Wet",
        juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f, 1.0f),
        50.0f,
        "%"
    ));

    // volume - dB, -60 to +6 (max 6dB boost), default 0, LAST stage in the chain
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "volume", 1 },
        "Volume",
        juce::NormalisableRange<float>(-60.0f, 6.0f, 0.1f, 1.0f),
        0.0f,
        "dB"
    ));

    return layout;
}

TransitionistAudioProcessor::TransitionistAudioProcessor()
    : AudioProcessor(BusesProperties()
                        .withInput("Input", juce::AudioChannelSet::stereo(), true)
                        .withOutput("Output", juce::AudioChannelSet::stereo(), true))
    , parameters(*this, nullptr, "Parameters", createParameterLayout())
{
}

TransitionistAudioProcessor::~TransitionistAudioProcessor()
{
}

void TransitionistAudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    currentSampleRate = sampleRate;

    const int numChannels = getTotalNumOutputChannels();

    juce::dsp::ProcessSpec spec;
    spec.sampleRate = sampleRate;
    spec.maximumBlockSize = static_cast<juce::uint32>(samplesPerBlock);
    spec.numChannels = static_cast<juce::uint32>(numChannels);

    // --- Ping-Pong Delay lines (mono each - A/B, not per-channel) ---
    // Practical floor of 30 BPM for the longest division (whole note = 4
    // beats) bounds max delay to ~8 seconds at the current sample rate.
    maxDelaySamples = static_cast<int>(std::ceil(sampleRate * 8.0));

    juce::dsp::ProcessSpec monoSpec;
    monoSpec.sampleRate = sampleRate;
    monoSpec.maximumBlockSize = static_cast<juce::uint32>(samplesPerBlock);
    monoSpec.numChannels = 1;

    delayLineA.setMaximumDelayInSamples(maxDelaySamples);
    delayLineA.prepare(monoSpec);
    delayLineA.reset();
    delayLineB.setMaximumDelayInSamples(maxDelaySamples);
    delayLineB.prepare(monoSpec);
    delayLineB.reset();

    smoothedDelaySamples.reset(sampleRate, 0.05); // ~50ms ramp
    {
        // Initial delay time: 120 BPM fallback, division index 4 (1/4 note).
        const double samplesPerBeat = (60.0 / 120.0) * sampleRate;
        const float initialDelaySamples = static_cast<float>(samplesPerBeat * kDivisionBeatFractions[4]);
        smoothedDelaySamples.setCurrentAndTargetValue(initialDelaySamples);
        lastTargetDelaySamples = initialDelaySamples;
    }

    for (auto& f : feedbackFilter)
    {
        f.setType(juce::dsp::FirstOrderTPTFilter<float>::Type::lowpass);
        f.setCutoffFrequency(8000.0f);
        f.prepare(monoSpec);
        f.reset();
    }

    // --- Reverb ---
    reverb.prepare(spec);
    reverb.reset();

    // --- Output Glue (fixed, wet-path safety net) ---
    softClip.functionToUse = [](float x) { return std::tanh(1.15f * x); };
    softClip.prepare(spec);
    softClip.reset();

    limiter.setThreshold(-1.0f);
    limiter.setRelease(50.0f);
    limiter.prepare(spec);
    limiter.reset();

    // --- Preallocated scratch buffers (ALL allocation happens here, never
    // in processBlock()) ---
    dryBuffer.setSize(numChannels, samplesPerBlock, false, false, true);
    reverbWetBuffer.setSize(numChannels, samplesPerBlock, false, false, true);
    dryBuffer.clear();
    reverbWetBuffer.clear();
}

void TransitionistAudioProcessor::releaseResources()
{
    dryBuffer.setSize(0, 0);
    reverbWetBuffer.setSize(0, 0);
}

bool TransitionistAudioProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    // Pure stereo-in/stereo-out effect (see class doc comment / architecture.md).
    // Explicitly enforced here (rather than relying on JUCE's permissive
    // default of accepting any host-proposed layout) because processBlock()'s
    // per-sample ping-pong delay loop aliases its "left"/"right" write
    // pointers to the SAME buffer when numChannels == 1, silently discarding
    // half of the ping-pong computation every sample in a mono bus - see
    // troubleshooting/dsp-issues/ for the writeup. Restricting the supported
    // layout to stereo/stereo makes that impossible instead of just assumed.
    return layouts.getMainInputChannelSet() == juce::AudioChannelSet::stereo()
        && layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

void TransitionistAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;
    juce::ignoreUnused(midiMessages);

    const int numChannels = buffer.getNumChannels();
    const int numSamples = buffer.getNumSamples();

    if (numChannels == 0 || numSamples == 0)
        return;

    // ------------------------------------------------------------------
    // Read all parameters ONCE per block (atomic reads).
    // ------------------------------------------------------------------
    const float transitionValue = parameters.getRawParameterValue("transition")->load();
    const float reverbValue     = parameters.getRawParameterValue("reverb")->load();
    const float delayValue      = parameters.getRawParameterValue("delay")->load();
    const int   delaySyncIndex  = static_cast<int>(parameters.getRawParameterValue("delaySync")->load());
    const float dryWetValue     = parameters.getRawParameterValue("dryWet")->load();
    const float volumeDb        = parameters.getRawParameterValue("volume")->load();

    const float transitionNorm = transitionValue / 100.0f;
    const float reverbNorm = reverbValue / 100.0f;
    const float delayNorm = delayValue / 100.0f;
    const float dryWetNorm = dryWetValue / 100.0f;

    // ------------------------------------------------------------------
    // True dry tap for the FINAL dry/wet blend - captured before any
    // effects stage (no input gain stage in this revision).
    // ------------------------------------------------------------------
    for (int ch = 0; ch < numChannels; ++ch)
        dryBuffer.copyFrom(ch, 0, buffer, ch, 0, numSamples);

    // ------------------------------------------------------------------
    // Ping-Pong Delay stage. The stage's own wet/dry blend (below) reads
    // the live buffer values directly (fully evaluated on the right-hand
    // side before reassignment) - no separate pre-stage tap buffer needed.
    // ------------------------------------------------------------------
    double bpm = 120.0;
    if (auto* playHead = getPlayHead())
        if (auto position = playHead->getPosition())
            if (auto hostBpm = position->getBpm())
                if (*hostBpm > 0.0)
                    bpm = *hostBpm;

    const int divIndex = juce::jlimit(0, 8, delaySyncIndex);
    const double samplesPerBeat = (60.0 / bpm) * currentSampleRate;
    const double targetDelaySamplesD = samplesPerBeat * static_cast<double>(kDivisionBeatFractions[divIndex]);
    const float targetDelaySamples = static_cast<float>(
        juce::jlimit(1.0, static_cast<double>(maxDelaySamples - 1), targetDelaySamplesD));

    if (std::abs(targetDelaySamples - lastTargetDelaySamples) > 0.0001f)
    {
        smoothedDelaySamples.setTargetValue(targetDelaySamples);
        lastTargetDelaySamples = targetDelaySamples;
    }

    // Feedback amount: base from `delay`, extra buildup from `transition`,
    // clamped safely below runaway. Stage wet/dry mix is driven purely by
    // `delayNorm` (0% = fully bypassed regardless of feedback amount).
    const float pingPongDrive = 1.0f + 0.5f * transitionNorm;
    const float pingPongFeedback = juce::jlimit(0.0f, 0.92f, 0.35f + 0.55f * delayNorm + 0.10f * transitionNorm);

    // numChannels is guaranteed >= 2 - isBusesLayoutSupported() only accepts stereo/stereo.
    auto* left = buffer.getWritePointer(0);
    auto* right = numChannels > 1 ? buffer.getWritePointer(1) : buffer.getWritePointer(0);

    for (int n = 0; n < numSamples; ++n)
    {
        const float delaySamples = smoothedDelaySamples.getNextValue();

        const float delayedA = delayLineA.popSample(0, delaySamples, true);
        const float delayedB = delayLineB.popSample(0, delaySamples, true);

        // Feedback path saturation + damping per repeat (analog-thickening
        // character, carried forward from v1's delay).
        const float fbAtoB = feedbackFilter[0].processSample(0, std::tanh(pingPongDrive * delayedA)) * pingPongFeedback;
        const float fbBtoA = feedbackFilter[1].processSample(0, std::tanh(pingPongDrive * delayedB)) * pingPongFeedback;

        const float monoIn = 0.5f * (left[n] + right[n]);
        delayLineA.pushSample(0, monoIn + fbBtoA); // fresh input ONLY into A
        delayLineB.pushSample(0, fbAtoB);          // B never receives fresh input directly

        // Stage wet/dry blend, driven purely by delayNorm.
        left[n] = left[n] * (1.0f - delayNorm) + delayedA * delayNorm;
        right[n] = right[n] * (1.0f - delayNorm) + delayedB * delayNorm;
    }

    // ------------------------------------------------------------------
    // Reverb stage. Uses the preallocated reverbWetBuffer member (real-time
    // safe - NEVER allocate an AudioBuffer inside processBlock()).
    // ------------------------------------------------------------------
    const float freeze = smoothstep(transitionNorm, 0.90f, 1.00f);
    const float reverbInputGain = 1.0f - freeze;

    juce::dsp::Reverb::Parameters reverbParams;
    reverbParams.roomSize = juce::jlimit(0.0f, 1.0f, 0.30f + 0.40f * reverbNorm + 0.30f * transitionNorm);
    reverbParams.damping = juce::jmap(transitionNorm, 0.0f, 1.0f, 0.60f, 0.05f);
    reverbParams.wetLevel = 1.0f;
    reverbParams.dryLevel = 0.0f;
    reverbParams.width = 1.0f;
    reverbParams.freezeMode = freeze;
    reverb.setParameters(reverbParams);

    // Mute the NEW input feeding the reverb as freeze engages - does not
    // mute the reverb's own sustaining output.
    for (int ch = 0; ch < numChannels; ++ch)
        reverbWetBuffer.copyFrom(ch, 0, buffer, ch, 0, numSamples);
    reverbWetBuffer.applyGain(reverbInputGain);

    {
        auto reverbSubBlock = juce::dsp::AudioBlock<float>(reverbWetBuffer).getSubBlock(0, static_cast<size_t>(numSamples));
        juce::dsp::ProcessContextReplacing<float> reverbContext(reverbSubBlock);
        reverb.process(reverbContext);
    }

    // Stage wet/dry blend, driven purely by reverbNorm.
    for (int ch = 0; ch < numChannels; ++ch)
    {
        buffer.applyGain(ch, 0, numSamples, 1.0f - reverbNorm);
        buffer.addFrom(ch, 0, reverbWetBuffer, ch, 0, numSamples, reverbNorm);
    }

    // ------------------------------------------------------------------
    // Output Glue (fixed safety net: soft-clip + limiter).
    // ------------------------------------------------------------------
    {
        juce::dsp::AudioBlock<float> glueBlock(buffer);
        juce::dsp::ProcessContextReplacing<float> glueContext(glueBlock);
        softClip.process(glueContext);
        limiter.process(glueContext);
    }

    // ------------------------------------------------------------------
    // Final Dry/Wet Mix (equal-power) against the TRUE dry tap (pre-effects,
    // post input gain).
    // ------------------------------------------------------------------
    const float wetGain = std::sin(dryWetNorm * juce::MathConstants<float>::halfPi);
    const float dryGain = std::cos(dryWetNorm * juce::MathConstants<float>::halfPi);

    buffer.applyGain(wetGain);
    for (int ch = 0; ch < numChannels; ++ch)
        buffer.addFrom(ch, 0, dryBuffer, ch, 0, numSamples, dryGain);

    // ------------------------------------------------------------------
    // Volume - LAST stage in the chain, -60 to +6dB (max 6dB boost).
    // Replaces the old separate input/output gain sliders.
    // ------------------------------------------------------------------
    buffer.applyGain(juce::Decibels::decibelsToGain(volumeDb));
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

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new TransitionistAudioProcessor();
}
