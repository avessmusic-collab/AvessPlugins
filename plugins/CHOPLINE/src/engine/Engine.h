// CHOPLINE — Engine (spec §3)
// Owns the DSP graph. Phase 1: SampleStore (background loading + snapshots),
// SliceModel (message-thread slice editing) with a published SliceMapSnapshot,
// the 16-voice pool, MIDI chop triggering and the lock-free audition path.
//
// Threads (spec §3, §8):
//   audio thread    — process() only: acquires snapshots, drains the audition
//                     FIFO, merges MIDI, renders voices. No allocation, locks,
//                     logging or file I/O.
//   message thread  — all edit methods, state (de)serialisation, snapshot
//                     publication and garbage collection.
//   background      — file decode / resample / analysis (inside SampleStore).
//
// The engine builds and runs in unit tests with no GUI module linked.

#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_data_structures/juce_data_structures.h>
#include <juce_events/juce_events.h>

#include "SampleStore.h"
#include "SliceMap.h"
#include "VoicePool.h"

namespace chopline::engine
{

class Engine final : public juce::ChangeBroadcaster
{
public:
    Engine();
    ~Engine() override = default;

    // ---- lifecycle (message thread) ------------------------------------------
    void prepare (double sampleRate, int maxBlockSize, int numOutputChannels);
    void release();

    // ---- audio thread ----------------------------------------------------------
    struct BlockContext
    {
        int midiBaseNote = 36; // chop N triggers from base + N
    };
    void process (juce::AudioBuffer<float>& buffer, const juce::MidiBuffer& midi,
                  const BlockContext& context) noexcept;

    double getSampleRate() const noexcept { return currentSampleRate; }

    // ---- loading (message thread) ----------------------------------------------
    void loadFile (const juce::File& file);
    juce::String loadFileSync (const juce::File& file);                       // tests / tools
    juce::String loadBufferSync (const juce::AudioBuffer<float>& source,
                                 double sourceRate, const juce::String& name = "buffer");
    bool isLoading() const noexcept { return sampleStore.isLoading(); }
    juce::String getLastError() const { return sampleStore.getLastError(); }
    std::shared_ptr<const SampleData> currentSample() const { return sampleStore.current(); }

    // ---- slicing (message thread; every setter republishes + notifies) ---------
    SliceMode getSliceMode() const noexcept { return model.mode; }
    float getSensitivity() const noexcept { return model.sensitivity; }
    float getMinSliceLengthMs() const noexcept { return model.minSliceLengthMs; }
    bool getZeroCrossSnap() const noexcept { return model.zeroCrossSnap; }
    double getSampleBpm() const noexcept { return model.sampleBpm; }

    void setSliceMode (SliceMode);
    void setSensitivity (float sensitivity01);
    void setMinSliceLengthMs (float ms);
    void setZeroCrossSnap (bool shouldSnap);
    void setSampleBpm (double bpm);

    const std::vector<SliceMarker>& getMarkers() const noexcept { return model.getMarkers(); }
    int getNumChops() const noexcept { return model.getNumChops (stateSample.get()); }
    bool moveMarker (int id, double newTimeSec);
    int addMarker (double timeSec);
    bool removeMarker (int id);

    // ---- audition (message thread → audio thread, lock-free) --------------------
    void audition (int chopIndex, float velocity);

    // ---- state (spec §10) --------------------------------------------------------
    bool getEmbedAudioInState() const noexcept { return embedAudioInState; }
    void setEmbedAudioInState (bool b) noexcept { embedAudioInState = b; }

    /// Appends SAMPLE + SLICES children to the given root. Thread-safe.
    void serializeTo (juce::ValueTree& root) const;
    /// Message thread only (the processor hops if needed). `synchronous` loads
    /// the embedded audio inline (tests / tools) instead of on the background
    /// thread.
    void restoreFrom (const juce::ValueTree& root, bool synchronous = false);

    static const juce::Identifier sampleTag, slicesTag, markerTag;

    // ---- UI feedback (message thread, lock-free reads) ----------------------------
    std::uint32_t getMidiFlashCount() const noexcept
    {
        return midiFlash.load (std::memory_order_relaxed);
    }
    struct VoiceView { int chopIndex = -1; float normalisedPos = 0.0f; };
    int getVoiceViews (VoiceView* dest, int maxCount) const noexcept;
    int countActiveVoices() const noexcept { return pool.countActive(); }

    /// Frees retired snapshots (call periodically from the editor / after edits).
    void collectGarbage();

private:
    void rebuildAndPublishSlices();   // callers hold modelLock
    void handleLoadFinished (juce::String error);

    SampleStore sampleStore;
    SliceModel model;                                 // guarded by modelLock
    std::shared_ptr<const SampleData> stateSample;    // message-thread cache, guarded
    SnapshotExchange<SliceMapSnapshot> sliceExchange;
    VoicePool pool;

    struct AuditionEvent { int chopIndex = 0; float velocity = 1.0f; };
    static constexpr int auditionFifoSize = 64;
    juce::AbstractFifo auditionFifo { auditionFifoSize };
    std::array<AuditionEvent, auditionFifoSize> auditionSlots {};

    std::atomic<std::uint32_t> midiFlash { 0 };

    double currentSampleRate = 0.0;
    int currentMaxBlockSize = 0;
    int currentNumChannels = 0;

    bool embedAudioInState = true;
    bool restorePendingMarkers = false;
    std::vector<SliceMarker> pendingMarkers;
    double pendingBpm = 0.0;

    mutable juce::CriticalSection modelLock; // model/stateSample vs. off-thread getState

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Engine)
};

} // namespace chopline::engine
