// CHOPLINE — PatternModel (spec §5, §10)
// Message-thread pattern store: 8 pattern slots (all serialized; Phase 5 adds
// the APVTS `pattern_slot` choice that drives the ACTIVE slot — the UI edits
// it and playback switches to it bar-quantised), each a list of TIME-BASED
// events (start + length in fractions of a bar, v17 events_are_time_based)
// with per-step override fields (velocity, pitch, probability, ratchet — the
// v17 lane set — plus the generator-only reverse flip).
//
// Phase 5 also adds the spec §5 generators: seeded Randomize (chop order,
// pitch, reverse, ratchet) with Amount, shift left/right, reverse pattern and
// copy/paste between slots. Every generator result is a deterministic
// function of (pattern, seed, amount), so results are repeatable.
//
// The model is a ValueTree so every edit goes through juce::UndoManager
// (100 transactions, spec §5). Any change rebuilds an immutable
// PatternSnapshot (ALL slots) for the audio thread (the Engine publishes it
// through the same SnapshotExchange pattern as the slice map).
//
// Thread-safety: edits happen on the message thread; a re-entrant lock makes
// serializeTo() safe from getStateInformation on any thread.

#pragma once

#include <juce_data_structures/juce_data_structures.h>

#include "../engine/PatternScheduler.h"

#include <functional>

namespace chopline::state
{

class PatternModel final : private juce::ValueTree::Listener
{
public:
    static constexpr int numSlots = 8;
    static constexpr int maxEventsPerPattern = engine::PatternSnapshot::maxEvents;
    static constexpr int defaultVelocity = 100; // 0–127

    PatternModel();
    ~PatternModel() override;

    /// Message thread, fired after any pattern change (edit, undo, restore).
    std::function<void()> onChanged;

    juce::UndoManager& getUndoManager() noexcept { return undo; }
    void beginTransaction (const juce::String& name) { undo.beginNewTransaction (name); }

    /// The slot the UI edits (driven by the APVTS `pattern_slot` parameter).
    int getActiveSlot() const noexcept { return activeSlot; }
    void setActiveSlot (int slot); // message thread; notifies (not undoable)

    // ---- pattern settings (undoable) --------------------------------------------
    engine::NoteLength getNoteLength (int slot) const;
    void setNoteLength (int slot, engine::NoteLength newLength);
    int getPatternBars (int slot) const;
    void setPatternBars (int slot, int bars); // 1, 2 or 4

    // ---- events ---------------------------------------------------------------------
    struct EventView
    {
        int id = -1;
        double startBar = 0.0;
        double lenBars = 0.0;
        int chop = 0;
        int velocity = defaultVelocity; // 0–127
        float pitch = 0.0f;             // semitones ±24 (lane)
        int probability = 100;          // % (lane)
        int ratchet = 1;                // repeats 1,2,3,4,6,8 (lane)
        bool reverseFlip = false;       // generator-only reverse override
    };

    int getNumEvents (int slot) const;
    EventView getEvent (int slot, int index) const;
    EventView getEventById (int slot, int id) const; // id −1 when not found

    /// Event whose [start, start+len) contains barPos, else −1.
    int findEventAt (int slot, double barPos) const;
    /// Event whose START lies in [fromBar, toBar), else −1.
    int findEventStartingIn (int slot, double fromBar, double toBar) const;

    // Edits (all undoable; group a gesture with beginTransaction first).
    int addEvent (int slot, double startBar, double lenBars, int chop,
                  int velocity = defaultVelocity); // returns id, −1 when full
    bool removeEvent (int slot, int id);
    bool moveEvent (int slot, int id, double newStartBar);
    bool setEventLength (int slot, int id, double lenBars);
    bool setEventChop (int slot, int id, int chop);
    int duplicateEvent (int slot, int id, double newStartBar);

    // Per-step lane edits (Phase 5; caller groups a paint gesture into one
    // transaction with beginTransaction).
    bool setEventVelocity (int slot, int id, int velocity);     // 0–127
    bool setEventPitch (int slot, int id, float semitones);     // ±24, step 1
    bool setEventProbability (int slot, int id, int percent);   // 0–100
    bool setEventRatchet (int slot, int id, int ratchet);       // 1,2,3,4,6,8
    bool setEventReverseFlip (int slot, int id, bool flip);     // generators only

    // ---- generators (spec §5; each one is ONE undoable transaction) ---------------
    /// Randomize seed / amount are stored per pattern (serialize with it).
    juce::int64 getSeed (int slot) const;
    void setSeed (int slot, juce::int64 seed); // not undoable (like freeBpm)
    float getRandomizeAmount (int slot) const; // 0..1
    void setRandomizeAmount (int slot, float amount01);

    /// Seeded randomize (chop order, pitch, reverse, ratchet). Deterministic:
    /// the same pattern + seed + amount always produces the same result.
    /// `numChops` bounds the chop shuffle (pass the engine's chop count).
    void randomize (int slot, int numChops);
    /// Shift every event by ±1 step of the slot's grid, wrapping in the bar(s).
    void shiftPattern (int slot, int deltaSteps);
    /// Mirror the pattern in time (event starts flip around the pattern length).
    void reversePattern (int slot);

    /// Copy/paste between slots (context menu). Paste replaces the target.
    void copyToClipboard (int slot);
    bool hasClipboard() const noexcept { return clipboard.isValid(); }
    void pasteFromClipboard (int slot);
    void copyPatternToSlot (int fromSlot, int toSlot);

    // ---- standalone free-run BPM (engine state, not undoable) ------------------------
    double getFreeBpm() const;
    void setFreeBpm (double bpm);

    // ---- audio thread view -----------------------------------------------------------
    engine::PatternSnapshot buildSnapshot() const; // ALL slots, events sorted

    // ---- state (spec §10) --------------------------------------------------------------
    /// Appends a PATTERNS child (all 8 slots) to the given root. Thread-safe.
    void serializeTo (juce::ValueTree& root) const;
    /// Replaces the model from the root's PATTERNS child; a missing child
    /// (Phase 1 state) resets to defaults. Clears the undo history.
    void restoreFrom (const juce::ValueTree& root);

    static const juce::Identifier patternsTag, patternTag, eventTag;

private:
    juce::ValueTree patternTree (int slot) const;
    juce::ValueTree eventTree (int slot, int id) const;
    bool setEventProperty (int slot, int id, const juce::Identifier& prop,
                           const juce::var& value);
    void replacePatternContents (int slot, const juce::ValueTree& src); // undoable
    void resetToDefaults();
    void notifyChanged();

    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override;
    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override;
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override;
    void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override;
    void valueTreeParentChanged (juce::ValueTree&) override;

    juce::ValueTree tree { "PATTERNS" }; // the live model (copied on serialize)
    juce::ValueTree clipboard;           // copy/paste between slots (not serialized)
    juce::UndoManager undo { 30000, 100 };    // ≥100 undoable transactions (spec §5)
    int nextEventId = 1;
    int activeSlot = 0; // UI-edited slot (APVTS pattern_slot; not serialized here)

    mutable juce::CriticalSection lock; // re-entrant; guards tree vs. off-thread serialize

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PatternModel)
};

} // namespace chopline::state
