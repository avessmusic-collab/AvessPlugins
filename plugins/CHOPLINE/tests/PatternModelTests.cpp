// CHOPLINE tests — PatternModel (spec §5, §10): time-based events, undoable
// edits through juce::UndoManager, note-length changes keeping musical
// positions, 8 serialized slots, per-step override fields stored untouched.

#include <catch2/catch_test_macros.hpp>

#include "state/PatternModel.h"

#include <juce_events/juce_events.h>

using namespace chopline;
using Model = state::PatternModel;

TEST_CASE ("Defaults: 8 empty slots, 1/16, 1 bar, 120 free BPM", "[pattern][model]")
{
    Model m;
    for (int slot = 0; slot < Model::numSlots; ++slot)
    {
        REQUIRE (m.getNumEvents (slot) == 0);
        REQUIRE (m.getNoteLength (slot) == engine::NoteLength::sixteenth);
        REQUIRE (m.getPatternBars (slot) == 1);
    }
    REQUIRE (m.getActiveSlot() == 0);
    REQUIRE (juce::exactlyEqual (m.getFreeBpm(), 120.0));
}

TEST_CASE ("Event edits and undo/redo restore the exact state", "[pattern][undo]")
{
    Model m;
    int changes = 0;
    m.onChanged = [&changes] { ++changes; };

    m.beginTransaction ("Place");
    const auto id = m.addEvent (0, 1.0 / 16.0, 1.0 / 16.0, 3, 110);
    REQUIRE (id >= 0);
    REQUIRE (m.getNumEvents (0) == 1);
    REQUIRE (changes > 0);

    auto ev = m.getEventById (0, id);
    REQUIRE (ev.chop == 3);
    REQUIRE (ev.velocity == 110);
    REQUIRE (juce::exactlyEqual (ev.startBar, 1.0 / 16.0));
    REQUIRE (juce::exactlyEqual (ev.lenBars, 1.0 / 16.0));
    REQUIRE (ev.probability == 100);
    REQUIRE (ev.ratchet == 1);

    SECTION ("undo removes the event, redo restores it with the same id")
    {
        REQUIRE (m.getUndoManager().undo());
        REQUIRE (m.getNumEvents (0) == 0);
        REQUIRE (m.getUndoManager().redo());
        REQUIRE (m.getNumEvents (0) == 1);
        REQUIRE (m.getEventById (0, id).id == id);
    }

    SECTION ("move, span, chop change, duplicate, delete are all undoable")
    {
        m.beginTransaction ("Move");
        REQUIRE (m.moveEvent (0, id, 4.0 / 16.0));
        m.beginTransaction ("Span");
        REQUIRE (m.setEventLength (0, id, 3.0 / 16.0));
        m.beginTransaction ("Chop");
        REQUIRE (m.setEventChop (0, id, 5));

        ev = m.getEventById (0, id);
        REQUIRE (juce::exactlyEqual (ev.startBar, 4.0 / 16.0));
        REQUIRE (juce::exactlyEqual (ev.lenBars, 3.0 / 16.0));
        REQUIRE (ev.chop == 5);

        m.beginTransaction ("Duplicate");
        const auto dup = m.duplicateEvent (0, id, 8.0 / 16.0);
        REQUIRE (dup >= 0);
        REQUIRE (m.getNumEvents (0) == 2);
        const auto dupView = m.getEventById (0, dup);
        REQUIRE (dupView.chop == 5);
        REQUIRE (juce::exactlyEqual (dupView.lenBars, 3.0 / 16.0));
        REQUIRE (juce::exactlyEqual (dupView.startBar, 8.0 / 16.0));

        m.beginTransaction ("Delete");
        REQUIRE (m.removeEvent (0, id));
        REQUIRE (m.getNumEvents (0) == 1);

        // Unwind everything.
        while (m.getUndoManager().canUndo())
            REQUIRE (m.getUndoManager().undo());
        REQUIRE (m.getNumEvents (0) == 0);
    }
}

TEST_CASE ("A painted run groups into one undo transaction", "[pattern][undo]")
{
    Model m;
    m.beginTransaction ("Paint steps");
    for (int i = 0; i < 4; ++i)
        REQUIRE (m.addEvent (0, i / 16.0, 1.0 / 16.0, 1) >= 0);
    REQUIRE (m.getNumEvents (0) == 4);

    REQUIRE (m.getUndoManager().undo()); // one undo removes the whole run
    REQUIRE (m.getNumEvents (0) == 0);
}

TEST_CASE ("Note-length change keeps events at their musical positions (spec §5)",
           "[pattern][resolution]")
{
    Model m;
    const auto id = m.addEvent (0, 1.0 / 16.0, 1.0 / 16.0, 2);
    m.setNoteLength (0, engine::NoteLength::quarterTriplet);

    // The event stays where it was — off the new grid, kept until moved.
    REQUIRE (m.getNoteLength (0) == engine::NoteLength::quarterTriplet);
    REQUIRE (juce::exactlyEqual (m.getEventById (0, id).startBar, 1.0 / 16.0));

    // The snapshot reflects the new grid but the same event time.
    const auto snap = m.buildSnapshot();
    REQUIRE (snap.noteLength == engine::NoteLength::quarterTriplet);
    REQUIRE (snap.numEvents == 1);
    REQUIRE (juce::exactlyEqual (snap.events[0].startBar, 1.0 / 16.0));
}

TEST_CASE ("Pattern length clamps to 1/2/4 and shortening keeps events",
           "[pattern][length]")
{
    Model m;
    m.setPatternBars (0, 4);
    REQUIRE (m.getPatternBars (0) == 4);
    const auto id = m.addEvent (0, 3.5, 1.0 / 16.0, 1); // in bar 4
    m.setPatternBars (0, 3);                            // clamps to 2
    REQUIRE (m.getPatternBars (0) == 2);
    REQUIRE (juce::exactlyEqual (m.getEventById (0, id).startBar, 3.5)); // kept (silent until grown back)
}

TEST_CASE ("Snapshot: active slot only, sorted by time, velocity normalised",
           "[pattern][snapshot]")
{
    Model m;
    m.addEvent (0, 0.5, 1.0 / 16.0, 2, 127);
    m.addEvent (0, 0.25, 1.0 / 16.0, 1, 0);
    m.addEvent (1, 0.0, 1.0 / 16.0, 7); // other slot: not in the snapshot

    const auto snap = m.buildSnapshot();
    REQUIRE (snap.numEvents == 2);
    REQUIRE (juce::exactlyEqual (snap.events[0].startBar, 0.25));
    REQUIRE (juce::exactlyEqual (snap.events[1].startBar, 0.5));
    REQUIRE (juce::exactlyEqual (snap.events[0].velocity, 0.0f));
    REQUIRE (juce::exactlyEqual (snap.events[1].velocity, 1.0f));
    REQUIRE (snap.events[1].chop == 2);
}

TEST_CASE ("Per-step override fields survive restore → serialize untouched",
           "[pattern][state]")
{
    // Hand-built state carrying Phase 5 override values the UI cannot set yet.
    juce::ValueTree root ("CHOPLINE");
    juce::ValueTree patterns (Model::patternsTag);
    patterns.setProperty ("freeBpm", 133.0, nullptr);
    for (int i = 0; i < Model::numSlots; ++i)
    {
        juce::ValueTree p (Model::patternTag);
        p.setProperty ("noteLen", 6, nullptr);
        p.setProperty ("bars", 1, nullptr);
        patterns.appendChild (p, nullptr);
    }
    juce::ValueTree ev (Model::eventTag);
    ev.setProperty ("id", 42, nullptr);
    ev.setProperty ("start", 0.125, nullptr);
    ev.setProperty ("len", 0.0625, nullptr);
    ev.setProperty ("chop", 4, nullptr);
    ev.setProperty ("vel", 77, nullptr);
    ev.setProperty ("pitch", -5.0, nullptr);
    ev.setProperty ("prob", 50, nullptr);
    ev.setProperty ("rat", 3, nullptr);
    patterns.getChild (0).appendChild (ev, nullptr);
    root.addChild (patterns, -1, nullptr);

    Model m;
    m.restoreFrom (root);
    REQUIRE (juce::exactlyEqual (m.getFreeBpm(), 133.0));

    const auto view = m.getEventById (0, 42);
    REQUIRE (view.velocity == 77);
    REQUIRE (juce::exactlyEqual (view.pitch, -5.0f));
    REQUIRE (view.probability == 50);
    REQUIRE (view.ratchet == 3);

    // New events get ids beyond the restored ones.
    REQUIRE (m.addEvent (0, 0.5, 0.0625, 1) > 42);

    juce::ValueTree out ("CHOPLINE");
    m.serializeTo (out);
    const auto outEv = out.getChildWithName (Model::patternsTag).getChild (0).getChild (0);
    REQUIRE (static_cast<int> (outEv.getProperty ("prob")) == 50);
    REQUIRE (static_cast<int> (outEv.getProperty ("rat")) == 3);
    REQUIRE (juce::exactlyEqual (static_cast<double> (outEv.getProperty ("pitch")), -5.0));
}

TEST_CASE ("findEventAt / findEventStartingIn", "[pattern][model]")
{
    Model m;
    const auto id = m.addEvent (0, 0.25, 0.125, 1); // covers [0.25, 0.375)
    REQUIRE (m.findEventAt (0, 0.3) == id);
    REQUIRE (m.findEventAt (0, 0.375) == -1);
    REQUIRE (m.findEventAt (0, 0.1) == -1);
    REQUIRE (m.findEventStartingIn (0, 0.2, 0.3) == id);
    REQUIRE (m.findEventStartingIn (0, 0.26, 0.5) == -1);
}
