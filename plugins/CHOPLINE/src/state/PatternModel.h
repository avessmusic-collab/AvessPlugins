// CHOPLINE — PatternModel (spec §5, §10)
// Message-thread pattern store: 8 pattern slots (all serialized; the UI
// surfaces only slot A until Phase 5 — locked v17 deferred_pattern_slots),
// each a list of TIME-BASED events (start + length in fractions of a bar,
// v17 events_are_time_based) with per-step override FIELDS (velocity, pitch,
// probability, ratchet — the v17 lane set). Only velocity plays in Phase 2;
// the other overrides are stored untouched for Phase 5.
//
// The model is a ValueTree so every edit goes through juce::UndoManager
// (100 transactions, spec §5). Any change rebuilds an immutable
// PatternSnapshot for the audio thread (the Engine publishes it through the
// same SnapshotExchange pattern as the slice map).
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

    int getActiveSlot() const noexcept { return 0; } // slots A–H surface in Phase 5

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
        float pitch = 0.0f;             // semitones (stored only, Phase 5)
        int probability = 100;          // % (stored only, Phase 5)
        int ratchet = 1;                // repeats (stored only, Phase 5)
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

    // ---- standalone free-run BPM (engine state, not undoable) ------------------------
    double getFreeBpm() const;
    void setFreeBpm (double bpm);

    // ---- audio thread view -----------------------------------------------------------
    engine::PatternSnapshot buildSnapshot() const; // active slot, events sorted

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
    void resetToDefaults();
    void notifyChanged();

    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override;
    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override;
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override;
    void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override;
    void valueTreeParentChanged (juce::ValueTree&) override;

    juce::ValueTree tree { "PATTERNS" }; // the live model (copied on serialize)
    juce::UndoManager undo { 30000, 100 };    // ≥100 undoable transactions (spec §5)
    int nextEventId = 1;

    mutable juce::CriticalSection lock; // re-entrant; guards tree vs. off-thread serialize

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PatternModel)
};

} // namespace chopline::state
