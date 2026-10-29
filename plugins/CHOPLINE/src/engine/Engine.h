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

#include "BeatRepeat.h"
#include "MasterFX.h"
#include "PatternScheduler.h"
#include "SampleStore.h"
#include "SliceMap.h"
#include "VoicePool.h"

#include "../state/PatternModel.h"

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
        BlockContext() = default;
        BlockContext (int baseNote, float swing = 0.0f) // tests: { 36 } etc.
            : midiBaseNote (baseNote), swingPct (swing) {}

        int midiBaseNote = 36;  // chop N triggers from base + N
        float swingPct = 0.0f;  // APVTS swing, 0–75 (trigger time only)

        // Host transport view (filled from AudioPlayHead by the processor;
        // tests drive the scheduler by filling these directly).
        bool hasHostPosition = false; // a playhead exists
        bool hostPlaying = false;
        bool hasPpq = false;
        double ppqPosition = 0.0;
        bool hasBpm = false;
        double bpm = 120.0;
        int timeSigNumerator = 4;
        int timeSigDenominator = 4;

        // Phase 4 (filled from the APVTS by the processor each block; the
        // defaults keep Beat Repeat off and the master chain bypassed, so
        // engine tests of earlier phases stay bit-identical unless they opt in).
        BeatRepeat::Params br;
        MasterParams master;
        /// Beat Repeat MIDI performance base note (spec §7: base = 1/4 grid,
        /// base+1 = 1/8, … base+7 = 1/32T). −1 disables interception — notes
        /// then fall through to the chop trigger range as before.
        int brPerfBase = -1;

        // Phase 5.
        /// APVTS pattern slot (A–H). Switches quantise to the next bar start
        /// while the transport runs; immediate when stopped.
        int patternSlot = 0;
        /// Gates the no-playhead free-run (Standalone Space preview). Hosts —
        /// and earlier-phase tests — pass true (the default).
        bool freeRunGate = true;
        /// Eco quality: caps granular density at 2 per trigger (the stretcher
        /// preset + master oversampling are handled elsewhere).
        bool eco = false;
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

    // ---- per-chop settings (Phase 3; v17 scope per_chop — model, NOT APVTS) ------
    ChopSettings getChopSettings (int chopIndex) const;
    /// Clamps, republishes the slice snapshot and notifies. Ringing voices
    /// keep the settings they were triggered with (copied by value).
    void setChopSettings (int chopIndex, const ChopSettings& settings);

    // ---- audition (message thread → audio thread, lock-free) --------------------
    /// startNorm 0..1 auditions from that point of the chop (overlay editor).
    void audition (int chopIndex, float velocity, double startNorm = 0.0);

    // ---- pattern (message thread edits; snapshots republish automatically) --------
    state::PatternModel& getPatternModel() noexcept { return patternModel; }
    /// UI playhead: position within the pattern in bars, −1 while stopped.
    float getUiPatternPositionBars() const noexcept { return scheduler.getUiPositionBars(); }

    // ---- state (spec §10) --------------------------------------------------------
    bool getEmbedAudioInState() const noexcept { return embedAudioInState; }
    void setEmbedAudioInState (bool b) noexcept { embedAudioInState = b; }

    /// Appends SAMPLE + SLICES children to the given root. Thread-safe.
    void serializeTo (juce::ValueTree& root) const;
    /// Message thread only (the processor hops if needed; an offline-render
    /// engine may call it synchronously from its own thread). `synchronous`
    /// loads the embedded audio inline (tests / tools / offline render)
    /// instead of on the background thread. A KIT-ONLY preset (no embedded
    /// audio, no valid path) restores onto the CURRENT sample: stored marker
    /// times are clamped into the sample (markers beyond its end drop) and
    /// per-chop settings ride their markers; pattern events pointing at chops
    /// beyond the count stay silent (documented Phase 5 mapping rule).
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

    // ---- Phase 4: Beat Repeat / master chain UI + host plumbing -------------------
    /// H key momentary hold (message thread → audio thread).
    void setMomentaryHold (bool held) noexcept
    {
        uiMomentaryHold.store (held, std::memory_order_relaxed);
    }
    /// True while a repeat is sounding or a hold is engaged (red accent rules).
    bool isBeatRepeatActive() const noexcept
    {
        return beatRepeat.isActiveUi() || uiMomentaryHold.load (std::memory_order_relaxed);
    }
    /// Effective BPM of the last processed block (host BPM or free-run BPM).
    double getUiBpm() const noexcept { return uiBpm.load (std::memory_order_relaxed); }
    float getMasterGainReductionDb() const noexcept { return masterFx.getGainReductionDb(); }
    float getMasterOutPeak() const noexcept { return masterFx.getOutPeak(); }
    /// Constant total latency (reported by the processor via setLatencySamples).
    int getLatencySamples() const noexcept { return masterFx.getLatencySamples(); }
    /// Tests only: deterministic Beat Repeat chance.
    BeatRepeat& getBeatRepeat() noexcept { return beatRepeat; }

    // ---- Phase 5 ------------------------------------------------------------------
    /// Space transport-preview (Standalone): the pattern free-runs only while
    /// held (message thread → audio thread via the processor's BlockContext).
    void setPreviewHeld (bool held) noexcept
    {
        previewHeld.store (held, std::memory_order_relaxed);
    }
    bool isPreviewHeld() const noexcept
    {
        return previewHeld.load (std::memory_order_relaxed);
    }

    /// Eco/HQ stretcher preset (Signalsmith presetCheaper in Eco). MESSAGE
    /// thread only, with audio processing SUSPENDED (the processor wraps this
    /// in suspendProcessing) — it re-warms the stretcher pool and resets the
    /// voices (documented: a quality toggle briefly silences ringing voices).
    void setQualityEco (bool eco);
    bool isQualityEco() const noexcept { return stretchEco; }

    /// The UI-edited pattern slot (APVTS pattern_slot; message thread).
    void setUiActiveSlot (int slot) { patternModel.setActiveSlot (slot); }

    /// Frees retired snapshots (call periodically from the editor / after edits).
    void collectGarbage();

private:
    void rebuildAndPublishSlices();   // callers hold modelLock
    void handleLoadFinished (juce::String error);
    void publishPattern();            // message thread

    SampleStore sampleStore;
    SliceModel model;                                 // guarded by modelLock
    std::shared_ptr<const SampleData> stateSample;    // message-thread cache, guarded
    SnapshotExchange<SliceMapSnapshot> sliceExchange;
    VoicePool pool;

    state::PatternModel patternModel;                 // message thread (own lock)
    SnapshotExchange<PatternSnapshot> patternExchange;
    PatternScheduler scheduler;                       // audio-thread transport state
    std::atomic<double> freeBpmCache { 120.0 };       // model → audio thread

    // Phase 4: Beat Repeat → master chain (spec §7; user-decided order).
    BeatRepeat beatRepeat;
    MasterFX masterFx;
    juce::AudioBuffer<float> delayBus, verbBus;       // per-chop send buses
    double brFreeBars = 0.0;                          // BR free-run timeline (audio thread)
    int perfHeldMask = 0;                             // held BR performance notes
    int perfLastGrid = -1;
    std::atomic<bool> uiMomentaryHold { false };
    std::atomic<double> uiBpm { 120.0 };
    std::atomic<bool> previewHeld { false }; // Space transport preview (Phase 5)
    bool stretchEco = false;                 // Eco/HQ stretcher preset (Phase 5)

    struct AuditionEvent { int chopIndex = 0; float velocity = 1.0f; float startNorm = 0.0f; };
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
