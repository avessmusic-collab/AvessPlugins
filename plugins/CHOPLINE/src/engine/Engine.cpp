// CHOPLINE — Engine implementation. See header for the thread model.

#include "Engine.h"

namespace chopline::engine
{

const juce::Identifier Engine::sampleTag { "SAMPLE" };
const juce::Identifier Engine::slicesTag { "SLICES" };
const juce::Identifier Engine::markerTag { "M" };

Engine::Engine()
{
    sampleStore.onLoadFinished = [this] (juce::String error) { handleLoadFinished (std::move (error)); };

    patternModel.onChanged = [this]
    {
        freeBpmCache.store (patternModel.getFreeBpm(), std::memory_order_relaxed);
        publishPattern();
        sendChangeMessage();
    };
    publishPattern(); // audio thread always sees a valid (empty) pattern
}

void Engine::publishPattern()
{
    patternExchange.publish (std::make_shared<PatternSnapshot> (patternModel.buildSnapshot()));
}

void Engine::prepare (double sampleRate, int maxBlockSize, int numOutputChannels)
{
    currentSampleRate = sampleRate;
    currentMaxBlockSize = maxBlockSize;
    currentNumChannels = numOutputChannels;

    pool.prepare (sampleRate);
    pool.reset();
    scheduler.prepare (sampleRate);

    // Re-resample the loaded sample at the new session rate (async; markers are
    // stored in seconds, so the slice map survives the rate change).
    sampleStore.setSessionRate (sampleRate);
}

void Engine::release()
{
    pool.reset();
}

// ---- audio thread --------------------------------------------------------------

void Engine::process (juce::AudioBuffer<float>& buffer, const juce::MidiBuffer& midi,
                      const BlockContext& context) noexcept
{
    buffer.clear();
    const auto numSamples = buffer.getNumSamples();
    if (numSamples <= 0 || buffer.getNumChannels() == 0)
        return;

    const auto* sampleNode = sampleStore.acquire();
    const auto* sliceNode = sliceExchange.acquire();
    const auto* patternNode = patternExchange.acquire();
    const SampleData* sample = sampleNode != nullptr ? sampleNode->object.get() : nullptr;
    const SliceMapSnapshot* slices = sliceNode != nullptr ? sliceNode->object.get() : nullptr;
    const PatternSnapshot* pattern = patternNode != nullptr ? patternNode->object.get() : nullptr;

    struct Event { int offset = 0; int chop = 0; float gain = 1.0f; int tag = ChopVoice::tagMidi; };
    static constexpr int maxEvents = 128;
    Event events[maxEvents];
    int numEvents = 0;

    // Pattern triggers from the host position (spec §8: derived from PPQ every
    // block; loop/jump releases ringing pattern voices with a short fade).
    {
        PatternScheduler::HostView host;
        host.hasPosition = context.hasHostPosition;
        host.playing = context.hostPlaying;
        host.hasPpq = context.hasPpq;
        host.ppq = context.ppqPosition;
        host.hasBpm = context.hasBpm;
        host.bpm = context.bpm;
        host.timeSigNumerator = context.timeSigNumerator;
        host.timeSigDenominator = context.timeSigDenominator;

        PatternScheduler::Trigger triggers[PatternScheduler::maxTriggers];
        bool releasePatternVoices = false;
        const auto numTriggers = scheduler.process (
            pattern, host, context.swingPct,
            freeBpmCache.load (std::memory_order_relaxed),
            numSamples, triggers, releasePatternVoices);

        if (releasePatternVoices)
            pool.releaseTagged (ChopVoice::tagPattern);

        for (int i = 0; i < numTriggers && numEvents < maxEvents; ++i)
            events[numEvents++] = { triggers[i].offset, triggers[i].chop,
                                    triggers[i].velocity, ChopVoice::tagPattern };
    }

    // Audition triggers (lock-free SPSC FIFO; UI is the producer).
    {
        const auto ready = auditionFifo.getNumReady();
        int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
        auditionFifo.prepareToRead (ready, start1, size1, start2, size2);
        for (int pass = 0; pass < 2; ++pass)
        {
            const auto start = pass == 0 ? start1 : start2;
            const auto size = pass == 0 ? size1 : size2;
            for (int i = 0; i < size && numEvents < maxEvents; ++i)
            {
                const auto& slot = auditionSlots[static_cast<size_t> (start + i)];
                events[numEvents++] = { 0, slot.chopIndex, slot.velocity, ChopVoice::tagMidi };
            }
        }
        auditionFifo.finishedRead (size1 + size2);
    }

    // MIDI note-ons → chop triggers, sample-accurate within the block.
    for (const auto metadata : midi)
    {
        const auto msg = metadata.getMessage();
        if (msg.isNoteOn())
        {
            midiFlash.fetch_add (1, std::memory_order_relaxed);
            const auto chop = msg.getNoteNumber() - context.midiBaseNote;
            if (chop >= 0 && chop < SliceMapSnapshot::maxChops && numEvents < maxEvents)
                events[numEvents++] = { juce::jlimit (0, numSamples - 1, metadata.samplePosition),
                                        chop, msg.getFloatVelocity(), ChopVoice::tagMidi };
        }
        else if (msg.isAllNotesOff() || msg.isAllSoundOff() || msg.isResetAllControllers())
        {
            pool.releaseAll();
        }
    }

    // Sort by offset (tiny fixed array; insertion sort, no allocation).
    for (int i = 1; i < numEvents; ++i)
    {
        const auto e = events[i];
        int j = i - 1;
        while (j >= 0 && events[j].offset > e.offset)
        {
            events[j + 1] = events[j];
            --j;
        }
        events[j + 1] = e;
    }

    // Segment render: voices always render (they may still hold an older sample
    // snapshot, which the GC keeps alive for them); triggers need live snapshots.
    int cursor = 0;
    for (int i = 0; i < numEvents; ++i)
    {
        pool.renderAdd (buffer, cursor, events[i].offset - cursor);
        cursor = events[i].offset;
        if (sample != nullptr && slices != nullptr && events[i].chop < slices->numChops)
            pool.trigger (sample, slices->chops[static_cast<size_t> (events[i].chop)],
                          events[i].chop, events[i].gain, events[i].tag);
    }
    pool.renderAdd (buffer, cursor, numSamples - cursor);

    // Block completed on these snapshots.
    sampleStore.ack (sampleNode);
    sliceExchange.ack (sliceNode);
    patternExchange.ack (patternNode);
}

// ---- loading ----------------------------------------------------------------------

void Engine::loadFile (const juce::File& file)
{
    {
        const juce::ScopedLock sl (modelLock);
        restorePendingMarkers = false;
    }
    sampleStore.loadFileAsync (file, currentSampleRate > 0.0 ? currentSampleRate : 48000.0);
    sendChangeMessage(); // spinner
}

juce::String Engine::loadFileSync (const juce::File& file)
{
    return sampleStore.loadFileSync (file, currentSampleRate > 0.0 ? currentSampleRate : 48000.0);
}

juce::String Engine::loadBufferSync (const juce::AudioBuffer<float>& source, double sourceRate,
                                     const juce::String& name)
{
    return sampleStore.loadBufferSync (source, sourceRate,
                                       currentSampleRate > 0.0 ? currentSampleRate : 48000.0, name);
}

void Engine::handleLoadFinished (juce::String error)
{
    {
        const juce::ScopedLock sl (modelLock);
        stateSample = sampleStore.current();

        if (error.isEmpty() && stateSample != nullptr)
        {
            if (restorePendingMarkers)
            {
                model.setMarkersFromState (std::move (pendingMarkers));
                pendingMarkers.clear();
                model.sampleBpm = pendingBpm > 1.0 ? pendingBpm : stateSample->detectedBpm;
                restorePendingMarkers = false;
            }
            else
            {
                model.sampleBpm = stateSample->detectedBpm;
                model.regenerate (stateSample.get());
            }
            rebuildAndPublishSlices();
        }
    }
    juce::ignoreUnused (error);
    sendChangeMessage();
}

// ---- slicing ------------------------------------------------------------------------

void Engine::rebuildAndPublishSlices()
{
    auto snapshot = std::make_shared<SliceMapSnapshot> (model.buildSnapshot (stateSample.get()));
    sliceExchange.publish (std::move (snapshot));
    sampleStore.collectGarbage ([this] (const SampleData* s) { return pool.usesSample (s); });
}

void Engine::setSliceMode (SliceMode newMode)
{
    {
        const juce::ScopedLock sl (modelLock);
        if (model.mode == newMode)
            return;
        model.mode = newMode;
        model.regenerate (stateSample.get());
        rebuildAndPublishSlices();
    }
    sendChangeMessage();
}

void Engine::setSensitivity (float sensitivity01)
{
    {
        const juce::ScopedLock sl (modelLock);
        model.sensitivity = juce::jlimit (0.0f, 1.0f, sensitivity01);
        if (model.mode != SliceMode::manual)
        {
            model.regenerate (stateSample.get());
            rebuildAndPublishSlices();
        }
    }
    sendChangeMessage();
}

void Engine::setMinSliceLengthMs (float ms)
{
    {
        const juce::ScopedLock sl (modelLock);
        model.minSliceLengthMs = juce::jlimit (20.0f, 500.0f, ms);
        if (model.mode != SliceMode::manual)
        {
            model.regenerate (stateSample.get());
            rebuildAndPublishSlices();
        }
    }
    sendChangeMessage();
}

void Engine::setZeroCrossSnap (bool shouldSnap)
{
    {
        const juce::ScopedLock sl (modelLock);
        model.zeroCrossSnap = shouldSnap;
        if (model.mode != SliceMode::manual)
        {
            model.regenerate (stateSample.get());
            rebuildAndPublishSlices();
        }
    }
    sendChangeMessage();
}

void Engine::setSampleBpm (double bpm)
{
    {
        const juce::ScopedLock sl (modelLock);
        model.sampleBpm = juce::jlimit (20.0, 999.0, bpm);
        if (model.mode == SliceMode::grid)
        {
            model.regenerate (stateSample.get());
            rebuildAndPublishSlices();
        }
    }
    sendChangeMessage();
}

bool Engine::moveMarker (int id, double newTimeSec)
{
    bool changed = false;
    {
        const juce::ScopedLock sl (modelLock);
        changed = model.moveMarker (id, newTimeSec, stateSample.get());
        if (changed)
            rebuildAndPublishSlices();
    }
    if (changed)
        sendChangeMessage();
    return changed;
}

int Engine::addMarker (double timeSec)
{
    int id = -1;
    {
        const juce::ScopedLock sl (modelLock);
        id = model.addMarker (timeSec, stateSample.get());
        if (id >= 0)
            rebuildAndPublishSlices();
    }
    if (id >= 0)
        sendChangeMessage();
    return id;
}

bool Engine::removeMarker (int id)
{
    bool removed = false;
    {
        const juce::ScopedLock sl (modelLock);
        removed = model.removeMarker (id);
        if (removed)
            rebuildAndPublishSlices();
    }
    if (removed)
        sendChangeMessage();
    return removed;
}

// ---- audition -----------------------------------------------------------------------

void Engine::audition (int chopIndex, float velocity)
{
    int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
    auditionFifo.prepareToWrite (1, start1, size1, start2, size2);
    if (size1 > 0)
        auditionSlots[static_cast<size_t> (start1)] = { chopIndex,
                                                        juce::jlimit (0.0f, 1.0f, velocity) };
    auditionFifo.finishedWrite (size1);
}

// ---- state ----------------------------------------------------------------------------

void Engine::serializeTo (juce::ValueTree& root) const
{
    const juce::ScopedLock sl (modelLock);

    juce::ValueTree sampleTree (sampleTag);
    if (stateSample != nullptr)
    {
        sampleTree.setProperty ("path", stateSample->filePath, nullptr);
        sampleTree.setProperty ("hash", stateSample->fileHash, nullptr);
        sampleTree.setProperty ("bpm", model.sampleBpm, nullptr);
        sampleTree.setProperty ("embed", embedAudioInState, nullptr);
        if (embedAudioInState && stateSample->flacData.getSize() > 0)
            sampleTree.setProperty ("flac", juce::var (stateSample->flacData), nullptr);
    }
    else
    {
        sampleTree.setProperty ("embed", embedAudioInState, nullptr);
    }

    juce::ValueTree slicesTree (slicesTag);
    slicesTree.setProperty ("mode", static_cast<int> (model.mode), nullptr);
    slicesTree.setProperty ("sens", model.sensitivity, nullptr);
    slicesTree.setProperty ("minLen", model.minSliceLengthMs, nullptr);
    slicesTree.setProperty ("zeroSnap", model.zeroCrossSnap, nullptr);
    for (const auto& m : model.getMarkers())
    {
        juce::ValueTree marker (markerTag);
        marker.setProperty ("t", m.timeSec, nullptr);
        marker.setProperty ("fi", m.fadeInMs, nullptr);
        marker.setProperty ("fo", m.fadeOutMs, nullptr);
        slicesTree.appendChild (marker, nullptr);
    }

    root.addChild (sampleTree, -1, nullptr);
    root.addChild (slicesTree, -1, nullptr);

    patternModel.serializeTo (root); // PATTERNS child, all 8 slots (own lock)
}

void Engine::restoreFrom (const juce::ValueTree& root, bool synchronous)
{
    JUCE_ASSERT_MESSAGE_THREAD

    // Patterns: a missing PATTERNS child (Phase 1 state) resets to defaults.
    patternModel.restoreFrom (root);

    const auto sampleTree = root.getChildWithName (sampleTag);
    const auto slicesTree = root.getChildWithName (slicesTag);

    juce::MemoryBlock flacBlob;
    juce::String path, hash;
    double storedBpm = 0.0;

    {
        const juce::ScopedLock sl (modelLock);

        if (slicesTree.isValid())
        {
            model.mode = static_cast<SliceMode> (
                juce::jlimit (0, 3, static_cast<int> (slicesTree.getProperty ("mode", 0))));
            model.sensitivity = juce::jlimit (0.0f, 1.0f,
                static_cast<float> (static_cast<double> (slicesTree.getProperty ("sens", 0.55))));
            model.minSliceLengthMs = juce::jlimit (20.0f, 500.0f,
                static_cast<float> (static_cast<double> (slicesTree.getProperty ("minLen", 80.0))));
            model.zeroCrossSnap = static_cast<bool> (slicesTree.getProperty ("zeroSnap", true));

            pendingMarkers.clear();
            for (const auto& child : slicesTree)
            {
                if (! child.hasType (markerTag))
                    continue;
                SliceMarker m;
                m.timeSec = static_cast<double> (child.getProperty ("t", 0.0));
                m.fadeInMs = static_cast<float> (static_cast<double> (child.getProperty ("fi", 2.0)));
                m.fadeOutMs = static_cast<float> (static_cast<double> (child.getProperty ("fo", 2.0)));
                pendingMarkers.push_back (m);
            }
        }

        if (sampleTree.isValid())
        {
            embedAudioInState = static_cast<bool> (sampleTree.getProperty ("embed", true));
            path = sampleTree.getProperty ("path", juce::String()).toString();
            hash = sampleTree.getProperty ("hash", juce::String()).toString();
            storedBpm = static_cast<double> (sampleTree.getProperty ("bpm", 0.0));
            const auto flacVar = sampleTree.getProperty ("flac"); // keep the var alive
            if (const auto* block = flacVar.getBinaryData())
                flacBlob = *block;
        }

        restorePendingMarkers = ! pendingMarkers.empty();
        pendingBpm = storedBpm;
    }

    const auto rate = currentSampleRate > 0.0 ? currentSampleRate : 48000.0;

    // Prefer the embedded audio (always self-consistent); fall back to the path.
    // A full relink dialog is spec §10 / a later phase — path + hash are stored now.
    if (flacBlob.getSize() > 0)
    {
        if (synchronous)
            sampleStore.loadMemorySync (std::move (flacBlob), path, hash, rate);
        else
            sampleStore.loadMemoryAsync (std::move (flacBlob), path, hash, rate);
    }
    else if (path.isNotEmpty() && juce::File (path).existsAsFile())
    {
        if (synchronous)
            sampleStore.loadFileSync (juce::File (path), rate);
        else
            sampleStore.loadFileAsync (juce::File (path), rate);
    }
    else
    {
        sendChangeMessage(); // nothing to load (empty state or missing file)
    }
}

// ---- UI feedback ------------------------------------------------------------------------

int Engine::getVoiceViews (VoiceView* dest, int maxCount) const noexcept
{
    int n = 0;
    for (int i = 0; i < VoicePool::numVoices && n < maxCount; ++i)
    {
        const auto& v = pool.getVoice (i);
        const auto chop = v.getUiChopIndex();
        if (chop >= 0)
        {
            dest[n].chopIndex = chop;
            dest[n].normalisedPos = v.getUiNormalisedPos();
            ++n;
        }
    }
    return n;
}

void Engine::collectGarbage()
{
    const juce::ScopedLock sl (modelLock);
    sampleStore.collectGarbage ([this] (const SampleData* s) { return pool.usesSample (s); });
    sliceExchange.collectGarbage();
    patternExchange.collectGarbage();
}

} // namespace chopline::engine
