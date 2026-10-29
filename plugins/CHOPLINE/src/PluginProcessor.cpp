// CHOPLINE — PluginProcessor implementation (Phase 1).

#include "PluginProcessor.h"

#include "PluginEditor.h"

namespace chopline
{

ChoplineAudioProcessor::ChoplineAudioProcessor()
    : AudioProcessor (BusesProperties()
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      state (*this, nullptr, juce::Identifier ("CHOPLINE"), params::createParameterLayout())
{
    baseNoteParam = state.getRawParameterValue (params::id::midiBaseNote);
    swingParam = state.getRawParameterValue (params::id::swing);
    patternSlotParam = state.getRawParameterValue (params::id::patternSlot);
    qualityParam = state.getRawParameterValue (params::id::quality);

    // Phase 4 raw values (audio thread reads only these atomics).
    namespace pid = params::id;
    p4.brMode = state.getRawParameterValue (pid::brMode);
    p4.brHold = state.getRawParameterValue (pid::brHold);
    p4.brInterval = state.getRawParameterValue (pid::brInterval);
    p4.brOffset = state.getRawParameterValue (pid::brOffset);
    p4.brChance = state.getRawParameterValue (pid::brChance);
    p4.brGrid = state.getRawParameterValue (pid::brGrid);
    p4.brGate = state.getRawParameterValue (pid::brGate);
    p4.brDecay = state.getRawParameterValue (pid::brDecay);
    p4.brPitchDecay = state.getRawParameterValue (pid::brPitchDecay);
    p4.brAccel = state.getRawParameterValue (pid::brAccel);
    p4.brReverse = state.getRawParameterValue (pid::brReverse);
    p4.brFilterMode = state.getRawParameterValue (pid::brFilterMode);
    p4.brSweep = state.getRawParameterValue (pid::brSweep);
    p4.brMix = state.getRawParameterValue (pid::brMix);
    p4.brFreeze = state.getRawParameterValue (pid::brFreeze);
    p4.brRndGrid = state.getRawParameterValue (pid::brRndGrid);
    p4.brMidiBase = state.getRawParameterValue (pid::brMidiBase);

    p4.filterOn = state.getRawParameterValue (pid::masterFilterOn);
    p4.filterType = state.getRawParameterValue (pid::masterFilterType);
    p4.filterCutoff = state.getRawParameterValue (pid::masterFilterCutoff);
    p4.filterReso = state.getRawParameterValue (pid::masterFilterReso);
    p4.distOn = state.getRawParameterValue (pid::masterDistOn);
    p4.distType = state.getRawParameterValue (pid::masterDistType);
    p4.distDrive = state.getRawParameterValue (pid::masterDistDrive);
    p4.distMix = state.getRawParameterValue (pid::masterDistMix);
    p4.distAutoGain = state.getRawParameterValue (pid::masterDistAutoGain);
    p4.widthOn = state.getRawParameterValue (pid::masterWidthOn);
    p4.width = state.getRawParameterValue (pid::masterWidth);
    p4.mono = state.getRawParameterValue (pid::masterMono);
    p4.limiterOn = state.getRawParameterValue (pid::masterLimiterOn);
    p4.output = state.getRawParameterValue (pid::masterOutput);
    p4.ceiling = state.getRawParameterValue (pid::masterCeiling);
    p4.delayOn = state.getRawParameterValue (pid::masterDelayOn);
    p4.delayTime = state.getRawParameterValue (pid::masterDelayTime);
    p4.delayFb = state.getRawParameterValue (pid::masterDelayFb);
    p4.delayRtn = state.getRawParameterValue (pid::masterDelayRtn);
    p4.verbOn = state.getRawParameterValue (pid::masterVerbOn);
    p4.verbDecay = state.getRawParameterValue (pid::masterVerbDecay);
    p4.verbDamp = state.getRawParameterValue (pid::masterVerbDamp);
    p4.verbRtn = state.getRawParameterValue (pid::masterVerbRtn);

    // Dev-only convenience for the Standalone app: CHOPLINE_AUTOLOAD_SAMPLE=<path>
    // loads a file at startup (used for manual smoke testing; no effect in hosts).
    if (wrapperType == wrapperType_Standalone)
    {
        const auto autoload = juce::SystemStats::getEnvironmentVariable ("CHOPLINE_AUTOLOAD_SAMPLE", {});
        if (autoload.isNotEmpty() && juce::File::isAbsolutePath (autoload))
            engine.loadFile (juce::File (autoload));
    }

    startTimerHz (20); // Phase 5 housekeeping (program change / slot / quality)
}

void ChoplineAudioProcessor::timerCallback()
{
    // MIDI program change → pattern slot parameter (spec §5 "MIDI note
    // switching" family; captured lock-free in processBlock, applied here on
    // the message thread with host notification).
    const auto program = pendingProgramChange.exchange (-1, std::memory_order_relaxed);
    if (program >= 0)
    {
        if (auto* param = state.getParameter (params::id::patternSlot))
        {
            param->beginChangeGesture();
            param->setValueNotifyingHost (
                param->convertTo0to1 (static_cast<float> (program % 8)));
            param->endChangeGesture();
        }
    }

    // Pattern slot parameter → the model's UI-edited slot (playback switching
    // is handled bar-quantised by the scheduler from the BlockContext).
    if (patternSlotParam != nullptr)
    {
        const auto slot = juce::jlimit (0, 7,
            static_cast<int> (patternSlotParam->load (std::memory_order_relaxed)));
        if (slot != engine.getPatternModel().getActiveSlot())
            engine.setUiActiveSlot (slot);
    }

    // Eco/HQ: re-warming the stretcher pool is not audio-thread safe, so the
    // switch suspends processing for its duration (documented brief silence).
    if (qualityParam != nullptr)
    {
        const auto eco = static_cast<int> (qualityParam->load (std::memory_order_relaxed)) == 0;
        if (eco != engine.isQualityEco())
        {
            suspendProcessing (true);
            engine.setQualityEco (eco);
            suspendProcessing (false);
        }
    }
}

ChoplineAudioProcessor::~ChoplineAudioProcessor()
{
    stopTimer(); // before members die — a pending timerCallback must not fire
    *aliveFlag = false;
}

void ChoplineAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    engine.prepare (sampleRate, samplesPerBlock, getTotalNumOutputChannels());
    // Constant while playing (spec §8): the zero-latency master chain reports
    // the rounded oversampler group delay (≈ 0 for the IIR polyphase 2×).
    setLatencySamples (engine.getLatencySamples());
}

void ChoplineAudioProcessor::releaseResources()
{
    engine.release();
}

bool ChoplineAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    // Instrument: stereo output only, no input buses.
    return layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo()
        && layouts.getMainInputChannelSet().isDisabled();
}

void ChoplineAudioProcessor::fillContextFromParams (engine::Engine::BlockContext& context) const
{
    if (baseNoteParam != nullptr)
        context.midiBaseNote = params::baseNoteForChoice (
            static_cast<int> (baseNoteParam->load (std::memory_order_relaxed)));
    if (swingParam != nullptr)
        context.swingPct = swingParam->load (std::memory_order_relaxed);

    // Phase 5: pattern slot (bar-quantised switch in the scheduler), Eco
    // quality, and the Standalone Space transport-preview gate (hosts always
    // pass true — their transport is the gate).
    if (patternSlotParam != nullptr)
        context.patternSlot = juce::jlimit (0, 7,
            static_cast<int> (patternSlotParam->load (std::memory_order_relaxed)));
    context.eco = qualityParam != nullptr
        && static_cast<int> (qualityParam->load (std::memory_order_relaxed)) == 0;
    context.freeRunGate = wrapperType != wrapperType_Standalone
                       || engine.isPreviewHeld();

    // Phase 4: Beat Repeat + master chain values from the cached atomics.
    const auto f = [] (std::atomic<float>* a, float fallback = 0.0f)
    {
        return a != nullptr ? a->load (std::memory_order_relaxed) : fallback;
    };
    const auto b = [&f] (std::atomic<float>* a) { return f (a) >= 0.5f; };
    const auto i = [&f] (std::atomic<float>* a) { return static_cast<int> (f (a)); };

    auto& br = context.br;
    br.mode = i (p4.brMode);
    br.hold = b (p4.brHold);
    br.intervalIndex = i (p4.brInterval);
    br.offsetSteps = i (p4.brOffset);
    br.chancePct = f (p4.brChance, 60.0f);
    br.gridIndex = i (p4.brGrid);
    br.gateIndex = i (p4.brGate);
    br.decayPct = f (p4.brDecay, 25.0f);
    br.pitchDecaySt = f (p4.brPitchDecay);
    br.accelPct = f (p4.brAccel);
    br.reverseMode = i (p4.brReverse);
    br.filterMode = i (p4.brFilterMode);
    br.sweepPct = f (p4.brSweep, 50.0f);
    br.mixMode = i (p4.brMix);
    br.freeze = b (p4.brFreeze);
    br.rndGrid = b (p4.brRndGrid);
    context.brPerfBase = br.mode != 2 ? params::brBaseNoteForChoice (i (p4.brMidiBase)) : -1;

    auto& m = context.master;
    m.filterOn = b (p4.filterOn);
    m.filterType = i (p4.filterType);
    m.cutoffHz = f (p4.filterCutoff, 20000.0f);
    m.resoPct = f (p4.filterReso, 10.0f);
    m.distOn = b (p4.distOn);
    m.distType = i (p4.distType);
    m.drivePct = f (p4.distDrive, 10.0f);
    m.mixPct = f (p4.distMix, 100.0f);
    m.autoGain = b (p4.distAutoGain);
    m.widthOn = b (p4.widthOn);
    m.widthPct = f (p4.width, 100.0f);
    m.monoHz = f (p4.mono, 120.0f);
    m.outOn = b (p4.limiterOn);
    m.outDb = f (p4.output);
    m.ceilingDb = f (p4.ceiling, -0.3f);
    m.delayOn = b (p4.delayOn);
    m.delayTimeIndex = i (p4.delayTime);
    m.delayFbPct = f (p4.delayFb, 35.0f);
    m.delayRtnPct = f (p4.delayRtn);
    m.verbOn = b (p4.verbOn);
    m.verbDecayPct = f (p4.verbDecay, 40.0f);
    m.verbDampPct = f (p4.verbDamp, 50.0f);
    m.verbRtnPct = f (p4.verbRtn);
    m.ecoNoOversample = context.eco; // Phase 5 Eco: Soft/Tape at base rate
}

void ChoplineAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer,
                                           juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;

    engine::Engine::BlockContext context;
    fillContextFromParams (context);

    // MIDI program change → pattern slot (applied on the message thread by the
    // housekeeping timer; the store is lock-free).
    for (const auto metadata : midi)
    {
        const auto msg = metadata.getMessage();
        if (msg.isProgramChange())
            pendingProgramChange.store (msg.getProgramChangeNumber(),
                                        std::memory_order_relaxed);
    }

    // Host transport → scheduler view (spec §8: derive step times from the
    // host PPQ every block; no playhead at all = Standalone free-run).
    if (auto* playHead = getPlayHead())
    {
        if (const auto position = playHead->getPosition())
        {
            context.hasHostPosition = true;
            context.hostPlaying = position->getIsPlaying();
            if (const auto ppq = position->getPpqPosition())
            {
                context.hasPpq = true;
                context.ppqPosition = *ppq;
            }
            if (const auto bpm = position->getBpm(); bpm.hasValue() && *bpm > 0.0)
            {
                context.hasBpm = true;
                context.bpm = *bpm;
            }
            if (const auto timeSig = position->getTimeSignature())
            {
                context.timeSigNumerator = timeSig->numerator;
                context.timeSigDenominator = timeSig->denominator;
            }
        }
    }

    engine.process (buffer, midi, context);
}

juce::AudioProcessorEditor* ChoplineAudioProcessor::createEditor()
{
    return new ChoplineAudioProcessorEditor (*this);
}

juce::ValueTree ChoplineAudioProcessor::buildStateTree()
{
    // Versioned from day one (spec §10): the schema version rides on the root.
    auto stateTree = state.copyState();
    stateTree.setProperty ("schemaVersion", params::kSchemaVersion, nullptr);

    // Replace any stale engine children from a previous restore, then append
    // the live sample reference + slice map (thread-safe inside the engine).
    stateTree.removeChild (stateTree.getChildWithName (engine::Engine::sampleTag), nullptr);
    stateTree.removeChild (stateTree.getChildWithName (engine::Engine::slicesTag), nullptr);
    stateTree.removeChild (stateTree.getChildWithName (state::PatternModel::patternsTag), nullptr);
    engine.serializeTo (stateTree);
    return stateTree;
}

void ChoplineAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = buildStateTree().createXml())
        copyXmlToBinary (*xml, destData);
}

void ChoplineAudioProcessor::applyStateTree (const juce::ValueTree& newState)
{
    state.replaceState (newState);
    engine.restoreFrom (newState);
}

void ChoplineAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    auto xml = getXmlFromBinary (data, sizeInBytes);
    if (xml == nullptr || ! xml->hasTagName (state.state.getType()))
        return;

    const auto newState = juce::ValueTree::fromXml (*xml);

    // Engine restore touches message-thread structures; hosts (and pluginval at
    // strictness 10) may call setStateInformation from other threads. The APVTS
    // itself is replaced SYNCHRONOUSLY on the calling thread (as before the
    // Phase 5 refactor), so parameters are correct for the very next block even
    // when the restore came from an offline-bounce thread.
    if (juce::MessageManager::getInstance()->isThisTheMessageThread())
    {
        applyStateTree (newState);
    }
    else
    {
        state.replaceState (newState);
        juce::MessageManager::callAsync ([alive = aliveFlag, this, newState]
        {
            if (*alive)
                engine.restoreFrom (newState);
        });
    }
}

} // namespace chopline

// This creates new instances of the plugin.
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new chopline::ChoplineAudioProcessor();
}
