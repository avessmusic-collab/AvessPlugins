#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <cmath>

namespace
{
    // Cubic Hermite smoothstep - used for the bipolar filter's dead-zone
    // crossfade (Component 6). (Reverb freeze's smoothstep is added in
    // Phase 4.2 and will reuse this same helper.)
    float smoothstep(float x, float edge0, float edge1)
    {
        const float t = juce::jlimit(0.0f, 1.0f, (x - edge0) / (edge1 - edge0));
        return t * t * (3.0f - 2.0f * t);
    }

    // Tempo-sync division table (architecture.md "Tempo Sync / Host Playhead"
    // section), expressed in quarter-note beats. Index 0 = shortest (1/32
    // note), index 9 = longest (whole note).
    constexpr float kDivisionBeatFractions[10] =
    {
        0.125f, // 0: 1/32 note
        0.25f,  // 1: 1/16 note
        0.375f, // 2: 1/16 dotted
        0.5f,   // 3: 1/8 note
        0.75f,  // 4: 1/8 dotted
        1.0f,   // 5: 1/4 note
        1.5f,   // 6: 1/4 dotted
        2.0f,   // 7: 1/2 note
        3.0f,   // 8: 1/2 dotted
        4.0f    // 9: 1/1 (whole) note
    };
}

juce::AudioProcessorValueTreeState::ParameterLayout TransitionistAudioProcessor::createParameterLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    // throw - Float, unipolar (0-100%, default 0.0)
    // Triple-role macro: dry/wet blend, delay feedback amount, reverb decay/size.
    // (DSP mapping implemented in Stage 2 - not used in Stage 1's pass-through.)
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "throw", 1 },
        "Throw",
        juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f, 1.0f),
        0.0f,
        "%"
    ));

    // space - Float, unipolar (0-100%, default 0.0)
    // Links tempo-synced delay division and reverb size into one "room" macro.
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "space", 1 },
        "Space",
        juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f, 1.0f),
        0.0f,
        "%"
    ));

    // sweep - Float, BIPOLAR (-100 to +100%, default 0.0 at the range MIDPOINT).
    // This codebase's first bipolar parameter - see parameter-spec.md's dedicated
    // implementation note. Normalised 0.5 MUST correspond to the raw value 0.0
    // (center detent = fully open filter, per creative-brief.md/architecture.md).
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "sweep", 1 },
        "Sweep",
        juce::NormalisableRange<float>(-100.0f, 100.0f, 0.1f, 1.0f),
        0.0f,
        "%"
    ));

    return layout;
}

TransitionistAudioProcessor::TransitionistAudioProcessor()
    : AudioProcessor(BusesProperties()
                        .withInput("Input", juce::AudioChannelSet::stereo(), true)
                        .withOutput("Output", juce::AudioChannelSet::stereo(), true))
    , parameters(*this, nullptr, "Parameters", createParameterLayout())
{
    // Verify the bipolar "sweep" parameter's range midpoint is correctly
    // centered, per parameter-spec.md's mandated verification step.
    jassert(parameters.getParameter("sweep")->convertTo0to1(0.0f) == 0.5f);
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

    // --- Component 1: Tempo-Synced Delay Line ---
    // Practical floor of 30 BPM for the longest division (whole note = 4
    // beats) bounds the maximum delay to ~8 seconds at the current sample
    // rate (per architecture.md Component 1 Configuration).
    maxDelaySamples = static_cast<int>(std::ceil(sampleRate * 8.0));
    delayLine.setMaximumDelayInSamples(maxDelaySamples);
    delayLine.prepare(spec);
    delayLine.reset();

    smoothedDelaySamples.reset(sampleRate, 0.05); // ~50ms ramp, per architecture.md
    {
        // Initial delay time: 120 BPM fallback, space = 0% (division index 0,
        // 1/32 note) - avoids an initial ramp-from-zero click before the
        // first block's real target is computed.
        const double samplesPerBeat = (60.0 / 120.0) * sampleRate;
        const float initialDelaySamples = static_cast<float>(samplesPerBeat * kDivisionBeatFractions[0]);
        smoothedDelaySamples.setCurrentAndTargetValue(initialDelaySamples);
        lastTargetDelaySamples = initialDelaySamples;
    }

    // --- Components 2-3: Feedback Damping Filter (one instance per channel) ---
    juce::dsp::ProcessSpec monoSpec;
    monoSpec.sampleRate = sampleRate;
    monoSpec.maximumBlockSize = static_cast<juce::uint32>(samplesPerBlock);
    monoSpec.numChannels = 1;

    for (auto& f : feedbackFilter)
    {
        f.setType(juce::dsp::FirstOrderTPTFilter<float>::Type::lowpass);
        f.setCutoffFrequency(8000.0f);
        f.prepare(monoSpec);
        f.reset();
    }

    // --- Component 4: Reverb Engine (basic, no freeze until Phase 4.2) ---
    reverb.prepare(spec);
    reverb.reset();

    // --- Component 6: Bipolar DJ Filter (dual always-running LadderFilter instances) ---
    lpfFilter.setMode(juce::dsp::LadderFilter<float>::Mode::LPF24);
    hpfFilter.setMode(juce::dsp::LadderFilter<float>::Mode::HPF24);
    lpfFilter.prepare(spec);
    hpfFilter.prepare(spec);
    lpfFilter.reset();
    hpfFilter.reset();

    // --- Preallocated scratch buffers (real-time safety: ALL allocation
    // happens here, never in processBlock()) ---
    dryBuffer.setSize(numChannels, samplesPerBlock, false, false, true);
    lpfBuffer.setSize(numChannels, samplesPerBlock, false, false, true);
    hpfBuffer.setSize(numChannels, samplesPerBlock, false, false, true);
    dryBuffer.clear();
    lpfBuffer.clear();
    hpfBuffer.clear();
}

void TransitionistAudioProcessor::releaseResources()
{
    // Release large preallocated buffers when the plugin is not in use.
    dryBuffer.setSize(0, 0);
    lpfBuffer.setSize(0, 0);
    hpfBuffer.setSize(0, 0);
}

void TransitionistAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    // Required: multiple stateful/feedback components in this chain (delay
    // feedback loop, TPT damping filter, reverb, both ladder filters).
    juce::ScopedNoDenormals noDenormals;
    juce::ignoreUnused(midiMessages);

    const int numChannels = buffer.getNumChannels();
    const int numSamples = buffer.getNumSamples();

    if (numChannels == 0 || numSamples == 0)
        return;

    // ------------------------------------------------------------------
    // Read all 3 parameters ONCE per block into local floats (atomic reads),
    // per architecture.md's Thread Boundaries section - critical for the
    // delay's per-sample feedback loop below.
    // ------------------------------------------------------------------
    const float throwValue = parameters.getRawParameterValue("throw")->load();  // 0-100
    const float spaceValue = parameters.getRawParameterValue("space")->load();  // 0-100
    const float sweepValue = parameters.getRawParameterValue("sweep")->load();  // -100 to +100

    const float throwNorm = throwValue / 100.0f;
    const float spaceNorm = spaceValue / 100.0f;
    const float sweepNorm = sweepValue / 100.0f;
    const float absSweep = std::abs(sweepNorm);

    // ------------------------------------------------------------------
    // Capture dry tap BEFORE any wet-path processing (Component 9 requirement).
    // ------------------------------------------------------------------
    for (int ch = 0; ch < numChannels; ++ch)
        dryBuffer.copyFrom(ch, 0, buffer, ch, 0, numSamples);

    // ------------------------------------------------------------------
    // Component 1 (tempo sync): compute target delay time in samples from
    // host BPM (JUCE 8 non-deprecated API, 120 BPM fallback) and `space`'s
    // division index.
    // ------------------------------------------------------------------
    double bpm = 120.0;
    if (auto* playHead = getPlayHead())
    {
        if (auto position = playHead->getPosition())
        {
            if (auto hostBpm = position->getBpm())
                if (*hostBpm > 0.0)
                    bpm = *hostBpm;
        }
    }

    const int divisionIndex = juce::jlimit(0, 9, static_cast<int>(std::round(spaceNorm * 9.0f)));
    const double samplesPerBeat = (60.0 / bpm) * currentSampleRate;
    const double targetDelaySamplesD = samplesPerBeat * static_cast<double>(kDivisionBeatFractions[divisionIndex]);
    const float targetDelaySamples = static_cast<float>(
        juce::jlimit(1.0, static_cast<double>(maxDelaySamples - 1), targetDelaySamplesD));

    // SmoothedValue::setTargetValue() internally no-ops (no ramp restart) if
    // the new target equals the current target, so calling it unconditionally
    // here still satisfies "retargeted whenever the computed target changes"
    // (architecture.md Component 1) without a separate manual epsilon check.
    if (std::abs(targetDelaySamples - lastTargetDelaySamples) > 0.0001f)
    {
        smoothedDelaySamples.setTargetValue(targetDelaySamples);
        lastTargetDelaySamples = targetDelaySamples;
    }

    // ------------------------------------------------------------------
    // Components 1-3: hand-built per-sample delay feedback loop.
    // NOTE: the smoothed delay time is advanced ONCE per sample (not once
    // per channel-sample as a literal reading of architecture.md's
    // channel-outer/sample-inner pseudocode would do) so that all channels
    // share exactly the same delay time within a given sample and the ramp
    // completes in the documented ~50ms regardless of channel count - a
    // deliberate correctness refinement of the pseudocode, not a deviation
    // from its intent (drive = 1.0 fixed for this phase; feedbackGain =
    // 0.85 * throwNorm per architecture.md Component 1-3 Algorithm Details).
    // ------------------------------------------------------------------
    constexpr float drive = 1.0f; // FIXED for Phase 4.1 (Phase 4.2 makes this throw-dependent)
    const float feedbackGain = 0.85f * throwNorm;

    for (int n = 0; n < numSamples; ++n)
    {
        const float delaySamples = smoothedDelaySamples.getNextValue();

        for (int ch = 0; ch < numChannels; ++ch)
        {
            auto* channelData = buffer.getWritePointer(ch);
            const float inputSample = channelData[n];

            const float delayed = delayLine.popSample(ch, delaySamples, true);
            const float saturated = std::tanh(drive * delayed);
            const float damped = feedbackFilter[static_cast<size_t>(juce::jmin(ch, 1))].processSample(0, saturated);
            const float toWrite = inputSample + damped * feedbackGain;
            delayLine.pushSample(ch, toWrite);

            channelData[n] = delayed; // tap BEFORE re-injecting feedback for this sample
        }
    }

    // ------------------------------------------------------------------
    // Component 4: Reverb Engine (basic - no freezeMode ramp until Phase 4.2).
    // Processes the delay's wet output (buffer now holds delay output).
    // ------------------------------------------------------------------
    juce::dsp::Reverb::Parameters reverbParams;
    reverbParams.roomSize = juce::jlimit(0.0f, 1.0f, 0.30f + 0.40f * spaceNorm + 0.30f * throwNorm);
    reverbParams.damping = juce::jmap(throwNorm, 0.0f, 1.0f, 0.60f, 0.05f);
    reverbParams.wetLevel = 1.0f;
    reverbParams.dryLevel = 0.0f;
    reverbParams.width = 1.0f;
    reverbParams.freezeMode = 0.0f; // Phase 4.2 adds the smoothstep freeze ramp
    reverb.setParameters(reverbParams);

    {
        juce::dsp::AudioBlock<float> reverbBlock(buffer);
        juce::dsp::ProcessContextReplacing<float> reverbContext(reverbBlock);
        reverb.process(reverbContext);
    }

    // ------------------------------------------------------------------
    // Component 6: Bipolar DJ Filter - dead-zone + smoothstep crossfade
    // between a true dry-bypass tap (the post-reverb signal currently in
    // `buffer`) and whichever always-running LadderFilter instance matches
    // sweep's sign. BOTH LadderFilter instances process every sample
    // regardless of which is audible (deliberate CPU-for-click-avoidance
    // tradeoff, per architecture.md).
    // ------------------------------------------------------------------
    constexpr float deadzone = 0.01f;
    constexpr float transitionWidth = 0.04f;

    float filterGain;
    float bypassGain;

    if (absSweep < deadzone)
    {
        filterGain = 0.0f;
        bypassGain = 1.0f;
    }
    else if (absSweep < deadzone + transitionWidth)
    {
        filterGain = smoothstep(absSweep, deadzone, deadzone + transitionWidth);
        bypassGain = 1.0f - filterGain;
    }
    else
    {
        filterGain = 1.0f;
        bypassGain = 0.0f;
    }

    const bool useHPF = sweepValue > 0.0f;
    const float resonanceActive = juce::jlimit(0.0f, 1.0f, 0.10f + 0.65f * absSweep);
    constexpr float resonanceMin = 0.10f;

    float lpfCutoffHz;
    float hpfCutoffHz;
    float lpfResonance;
    float hpfResonance;

    if (sweepValue < 0.0f)
    {
        lpfCutoffHz = juce::jmap(sweepValue, -100.0f, 0.0f, 200.0f, 20000.0f);
        lpfResonance = resonanceActive;
        hpfCutoffHz = 20.0f;   // parked fully open (HPF passes everything)
        hpfResonance = resonanceMin;
    }
    else if (sweepValue > 0.0f)
    {
        hpfCutoffHz = juce::jmap(sweepValue, 0.0f, 100.0f, 20.0f, 10000.0f);
        hpfResonance = resonanceActive;
        lpfCutoffHz = 20000.0f; // parked fully open (LPF passes everything)
        lpfResonance = resonanceMin;
    }
    else
    {
        lpfCutoffHz = 20000.0f;
        hpfCutoffHz = 20.0f;
        lpfResonance = resonanceMin;
        hpfResonance = resonanceMin;
    }

    lpfFilter.setCutoffFrequencyHz(lpfCutoffHz);
    lpfFilter.setResonance(lpfResonance);
    hpfFilter.setCutoffFrequencyHz(hpfCutoffHz);
    hpfFilter.setResonance(hpfResonance);

    for (int ch = 0; ch < numChannels; ++ch)
    {
        lpfBuffer.copyFrom(ch, 0, buffer, ch, 0, numSamples);
        hpfBuffer.copyFrom(ch, 0, buffer, ch, 0, numSamples);
    }

    {
        juce::dsp::AudioBlock<float> lpfBlock(lpfBuffer);
        lpfBlock = lpfBlock.getSubBlock(0, static_cast<size_t>(numSamples));
        juce::dsp::ProcessContextReplacing<float> lpfContext(lpfBlock);
        lpfFilter.process(lpfContext);

        juce::dsp::AudioBlock<float> hpfBlock(hpfBuffer);
        hpfBlock = hpfBlock.getSubBlock(0, static_cast<size_t>(numSamples));
        juce::dsp::ProcessContextReplacing<float> hpfContext(hpfBlock);
        hpfFilter.process(hpfContext);
    }

    const juce::AudioBuffer<float>& selectedFilterBuffer = useHPF ? hpfBuffer : lpfBuffer;

    // buffer currently holds the bypass (unfiltered, post-reverb) signal.
    buffer.applyGain(bypassGain);
    for (int ch = 0; ch < numChannels; ++ch)
        buffer.addFrom(ch, 0, selectedFilterBuffer, ch, 0, numSamples, filterGain);

    // ------------------------------------------------------------------
    // Component 9: Dry/Wet Mixer (equal-power crossfade, throw-driven).
    // `buffer` now holds the fully-processed wet signal; `dryBuffer` holds
    // the untouched dry tap captured at the top of this function.
    // ------------------------------------------------------------------
    const float wetGain = std::sin(throwNorm * juce::MathConstants<float>::halfPi);
    const float dryGain = std::cos(throwNorm * juce::MathConstants<float>::halfPi);

    buffer.applyGain(wetGain);
    for (int ch = 0; ch < numChannels; ++ch)
        buffer.addFrom(ch, 0, dryBuffer, ch, 0, numSamples, dryGain);
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

// Factory function
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new TransitionistAudioProcessor();
}
