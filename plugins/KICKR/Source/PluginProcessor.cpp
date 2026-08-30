#include "PluginProcessor.h"
#include "PluginEditor.h"

#include <algorithm>

KICKRAudioProcessor::KICKRAudioProcessor()
    : AudioProcessor (BusesProperties()
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, &undoManager, "PARAMETERS", kickr::createParameterLayout())
{
    // PHASE 2.10 — report OS-factor latency changes to the host on the message thread.
    apvts.addParameterListener (kickr::id::oversampling, this);

    // PHASE 3.3 — first instantiation with no restored session shows as "Default".
    presetManager.markDefaultIfUnnamed();
}

KICKRAudioProcessor::~KICKRAudioProcessor()
{
    apvts.removeParameterListener (kickr::id::oversampling, this);
}

void KICKRAudioProcessor::parameterChanged (const juce::String& parameterID, float)
{
    // Message thread only (APVTS dispatches parameter listeners there). Never mid-block.
    if (parameterID == kickr::id::oversampling)
        setLatencySamples (engine.pendingOsLatencySamples());
}

void KICKRAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    currentSampleRate = sampleRate;
    currentBlockSize  = samplesPerBlock;

    engine.prepare (sampleRate, samplesPerBlock);

    // PHASE 3.2 — build the FFT + Hann window once, size the ring, reset atomics
    // (message thread). The audio thread never touches any of that.
    analyzer.prepare (sampleRate);

    // Phase 2.1: oversampling is pinned to 1x, so this reports 0. Phase 2.10 enables
    // real factors and updates latency on every `oversampling` change (AD-2 / AD-10).
    setLatencySamples (engine.getLatencySamples());

    // PHASE 2.7b — resolve a pending sample name from a state restore that happened before
    // prepareToPlay, then drop any buffers no voice can still be referencing.
    if (currentSample.load() == nullptr && pendingSample.load() == nullptr
        && currentSampleName.isNotEmpty())
        loadSampleByName (currentSampleName);

    retireUnreferenced();
}

void KICKRAudioProcessor::releaseResources()
{
    engine.reset();
}

bool KICKRAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    // Instrument: output-only. Reject any input bus.
    if (layouts.getMainInputChannelSet() != juce::AudioChannelSet::disabled())
        return false;

    const auto out = layouts.getMainOutputChannelSet();
    return out == juce::AudioChannelSet::stereo()
        || out == juce::AudioChannelSet::mono();
}

void KICKRAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer,
                                                juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;

    // PHASE 2.7b — publish a newly-decoded sample buffer with a plain atomic pointer swap.
    // No allocation and no free on the audio thread: the previous object stays owned by the
    // message thread (retired list) until no voice references it.
    const auto* pend = pendingSample.load (std::memory_order_acquire);
    if (pend != currentSample.load (std::memory_order_relaxed))
        currentSample.store (pend, std::memory_order_release);

    engine.setSampleBuffer (currentSample.load (std::memory_order_relaxed));

    // PHASE 3.2 — arm a one-kick waveform capture on any note-on in this block. The
    // analyzer taps sit in the PROCESSOR (not KickEngine — the engine stays pure DSP)
    // and read the FINAL post-limiter output, so the display matches what leaves the
    // plugin. Bounded scan, no allocation.
    for (const auto meta : midiMessages)
    {
        if (meta.getMessage().isNoteOn())
        {
            analyzer.armCapture();
            break;
        }
    }

    // Phase 2.1: the engine clears the buffer, renders the voice inside the OS region
    // (processSamplesUp(zero) -> render @ fsOversampled -> processSamplesDown), then
    // runs the base-rate DC blocker + NaN/Inf guard.
    engine.processBlock (buffer, midiMessages);

    // PHASE 3.2 — feed the post-limiter output to the analyzer: bounded memcpy +
    // atomic store into the double-buffered capture + one AbstractFifo write. No
    // allocation, no locks, no FFT on the audio thread.
    if (buffer.getNumSamples() > 0 && buffer.getNumChannels() > 0)
    {
        const float* l = buffer.getReadPointer (0);
        const float* r = buffer.getNumChannels() > 1 ? buffer.getReadPointer (1) : l;
        analyzer.pushBlock (l, r, buffer.getNumSamples());
    }
}

juce::AudioProcessorEditor* KICKRAudioProcessor::createEditor()
{
    return new KICKRAudioProcessorEditor (*this);
}

void KICKRAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();

    // Forward-migration tag + preset / sample bookkeeping (carried through the round-trip).
    state.setProperty ("stateVersion", kStateVersion, nullptr);
    if (! state.hasProperty ("currentPresetName"))
        state.setProperty ("currentPresetName", juce::String(), nullptr);
    if (! state.hasProperty ("currentPresetPath"))
        state.setProperty ("currentPresetPath", juce::String(), nullptr);
    // v2: name of the loaded sample from the managed bank (bare file name, never a path).
    state.setProperty ("currentSampleName", currentSampleName, nullptr);

    // PHASE 2.7b — a good moment to free retired sample buffers (message thread).
    retireUnreferenced();

    if (auto xml = std::unique_ptr<juce::XmlElement> (state.createXml()))
        copyXmlToBinary (*xml, destData);
}

void KICKRAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    auto xml = std::unique_ptr<juce::XmlElement> (getXmlFromBinary (data, sizeInBytes));
    if (xml == nullptr || ! xml->hasTagName (apvts.state.getType()))
        return;

    auto tree = juce::ValueTree::fromXml (*xml);

    // Migration: v1 (pre-sample) states have no sample* params — JUCE applies their v2
    // defaults (sampleEnable off) so an old patch sounds identical.
    const int stateVersion = (int) tree.getProperty ("stateVersion", kStateVersion);
    juce::ignoreUnused (stateVersion);

    const auto restoredSampleName = tree.getProperty ("currentSampleName", juce::String()).toString();

    apvts.replaceState (tree);

    // PHASE 2.7b — resolve + decode + publish the referenced sample (message thread).
    // Missing file -> layer silent, name retained, no crash.
    loadSampleByName (restoredSampleName);
}

void KICKRAudioProcessor::retireUnreferenced()
{
    // Message thread only. A voice captures its buffer pointer at noteOn and holds it for
    // its life — but such a pointer is always the current or pending buffer (we only ever
    // move the *previous* owned slot into `retiredSamples`). So once no voice is active,
    // nothing can be pointing at a retired buffer and it is safe to free.
    if (engine.anyVoiceActive())
        return;

    const auto* cur = currentSample.load (std::memory_order_relaxed);
    const auto* pen = pendingSample.load (std::memory_order_relaxed);

    retiredSamples.erase (
        std::remove_if (retiredSamples.begin(), retiredSamples.end(),
                        [cur, pen] (const std::unique_ptr<kickr::SampleBuffer>& b)
                        {
                            return b.get() != cur && b.get() != pen;
                        }),
        retiredSamples.end());
}

void KICKRAudioProcessor::loadSampleByName (const juce::String& bareName)
{
    // Message thread only (setStateInformation / prepareToPlay / Stage-3 UI).
    currentSampleName = bareName;

    if (bareName.isEmpty())
    {
        pendingSample.store (nullptr, std::memory_order_release);
        retireUnreferenced();
        return;
    }

    auto decoded = sampleLibrary.load (bareName);

    if (decoded == nullptr)
    {
        // TODO Stage 3: non-modal "sample not found" notice. Name is retained; layer silent.
        pendingSample.store (nullptr, std::memory_order_release);
        retireUnreferenced();
        return;
    }

    // Write into the slot that is NOT currently live; retire whatever was there.
    const int freeSlot = (liveSampleSlot == 0) ? 1 : 0;
    auto& slot = (freeSlot == 0) ? ownedSampleA : ownedSampleB;

    if (slot != nullptr)
        retiredSamples.push_back (std::move (slot));

    slot           = std::move (decoded);
    liveSampleSlot = freeSlot;

    pendingSample.store (slot.get(), std::memory_order_release);
    retireUnreferenced();
}

// Plugin factory entry point.
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new KICKRAudioProcessor();
}
