// CHOPLINE — PatternModel implementation. See header for the design.

#include "PatternModel.h"

#include "../engine/DetHash.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace chopline::state
{

const juce::Identifier PatternModel::patternsTag { "PATTERNS" };
const juce::Identifier PatternModel::patternTag { "PATTERN" };
const juce::Identifier PatternModel::eventTag { "EV" };

namespace
{
    const juce::Identifier idProp { "id" };
    const juce::Identifier startProp { "start" };
    const juce::Identifier lenProp { "len" };
    const juce::Identifier chopProp { "chop" };
    const juce::Identifier velProp { "vel" };
    const juce::Identifier pitchProp { "pitch" };
    const juce::Identifier probProp { "prob" };
    const juce::Identifier ratProp { "rat" };
    const juce::Identifier revProp { "rev" };      // Phase 5: generator reverse flip
    const juce::Identifier noteLenProp { "noteLen" };
    const juce::Identifier barsProp { "bars" };
    const juce::Identifier seedProp { "seed" };    // Phase 5: randomize / prob seed
    const juce::Identifier amountProp { "rndAmt" };// Phase 5: randomize amount 0..1
    const juce::Identifier freeBpmProp { "freeBpm" };

    constexpr double maxBars = 4.0;

    int clampBars (int bars) noexcept { return bars >= 4 ? 4 : (bars >= 2 ? 2 : 1); }

    engine::NoteLength clampNoteLength (int index) noexcept
    {
        return static_cast<engine::NoteLength> (
            juce::jlimit (0, engine::numNoteLengths - 1, index));
    }
} // namespace

PatternModel::PatternModel()
{
    resetToDefaults();
    tree.addListener (this);
}

PatternModel::~PatternModel()
{
    tree.removeListener (this);
}

void PatternModel::resetToDefaults()
{
    tree.removeAllChildren (nullptr);
    tree.removeAllProperties (nullptr);
    tree.setProperty (freeBpmProp, 120.0, nullptr);
    for (int i = 0; i < numSlots; ++i)
    {
        juce::ValueTree p (patternTag);
        p.setProperty (noteLenProp, static_cast<int> (engine::NoteLength::sixteenth), nullptr);
        p.setProperty (barsProp, 1, nullptr);
        p.setProperty (seedProp, static_cast<juce::int64> (1), nullptr);
        p.setProperty (amountProp, 0.5, nullptr);
        tree.appendChild (p, nullptr);
    }
    nextEventId = 1;
}

void PatternModel::setActiveSlot (int slot)
{
    const auto v = juce::jlimit (0, numSlots - 1, slot);
    if (v == activeSlot)
        return;
    activeSlot = v;
    notifyChanged();
}

// ---- tree access -------------------------------------------------------------------

juce::ValueTree PatternModel::patternTree (int slot) const
{
    return tree.getChild (juce::jlimit (0, numSlots - 1, slot));
}

juce::ValueTree PatternModel::eventTree (int slot, int id) const
{
    const auto p = patternTree (slot);
    for (const auto& child : p)
        if (child.hasType (eventTag) && static_cast<int> (child.getProperty (idProp, -1)) == id)
            return child;
    return {};
}

// ---- settings ------------------------------------------------------------------------

engine::NoteLength PatternModel::getNoteLength (int slot) const
{
    const juce::ScopedLock sl (lock);
    return clampNoteLength (static_cast<int> (
        patternTree (slot).getProperty (noteLenProp,
                                        static_cast<int> (engine::NoteLength::sixteenth))));
}

void PatternModel::setNoteLength (int slot, engine::NoteLength newLength)
{
    const juce::ScopedLock sl (lock);
    auto p = patternTree (slot);
    const auto v = static_cast<int> (clampNoteLength (static_cast<int> (newLength)));
    if (static_cast<int> (p.getProperty (noteLenProp, -1)) == v)
        return;
    undo.beginNewTransaction ("Note length");
    p.setProperty (noteLenProp, v, &undo);
}

int PatternModel::getPatternBars (int slot) const
{
    const juce::ScopedLock sl (lock);
    return clampBars (static_cast<int> (patternTree (slot).getProperty (barsProp, 1)));
}

void PatternModel::setPatternBars (int slot, int bars)
{
    const juce::ScopedLock sl (lock);
    auto p = patternTree (slot);
    const auto v = clampBars (bars);
    if (static_cast<int> (p.getProperty (barsProp, 1)) == v)
        return;
    undo.beginNewTransaction ("Pattern length");
    p.setProperty (barsProp, v, &undo);
}

// ---- events -----------------------------------------------------------------------------

namespace
{
    PatternModel::EventView viewOf (const juce::ValueTree& ev)
    {
        PatternModel::EventView v;
        v.id = static_cast<int> (ev.getProperty ("id", -1));
        v.startBar = static_cast<double> (ev.getProperty ("start", 0.0));
        v.lenBars = static_cast<double> (ev.getProperty ("len", 0.0));
        v.chop = static_cast<int> (ev.getProperty ("chop", 0));
        v.velocity = static_cast<int> (ev.getProperty ("vel", PatternModel::defaultVelocity));
        v.pitch = static_cast<float> (static_cast<double> (ev.getProperty ("pitch", 0.0)));
        v.probability = static_cast<int> (ev.getProperty ("prob", 100));
        v.ratchet = static_cast<int> (ev.getProperty ("rat", 1));
        v.reverseFlip = static_cast<bool> (ev.getProperty ("rev", false));
        return v;
    }
} // namespace

int PatternModel::getNumEvents (int slot) const
{
    const juce::ScopedLock sl (lock);
    int n = 0;
    for (const auto& child : patternTree (slot))
        if (child.hasType (eventTag))
            ++n;
    return n;
}

PatternModel::EventView PatternModel::getEvent (int slot, int index) const
{
    const juce::ScopedLock sl (lock);
    int n = 0;
    for (const auto& child : patternTree (slot))
        if (child.hasType (eventTag) && n++ == index)
            return viewOf (child);
    return {};
}

PatternModel::EventView PatternModel::getEventById (int slot, int id) const
{
    const juce::ScopedLock sl (lock);
    const auto ev = eventTree (slot, id);
    return ev.isValid() ? viewOf (ev) : EventView {};
}

int PatternModel::findEventAt (int slot, double barPos) const
{
    const juce::ScopedLock sl (lock);
    for (const auto& child : patternTree (slot))
    {
        if (! child.hasType (eventTag))
            continue;
        const auto start = static_cast<double> (child.getProperty (startProp, 0.0));
        const auto len = static_cast<double> (child.getProperty (lenProp, 0.0));
        if (barPos >= start && barPos < start + len)
            return static_cast<int> (child.getProperty (idProp, -1));
    }
    return -1;
}

int PatternModel::findEventStartingIn (int slot, double fromBar, double toBar) const
{
    const juce::ScopedLock sl (lock);
    for (const auto& child : patternTree (slot))
    {
        if (! child.hasType (eventTag))
            continue;
        const auto start = static_cast<double> (child.getProperty (startProp, 0.0));
        if (start >= fromBar && start < toBar)
            return static_cast<int> (child.getProperty (idProp, -1));
    }
    return -1;
}

int PatternModel::addEvent (int slot, double startBar, double lenBars, int chop, int velocity)
{
    const juce::ScopedLock sl (lock);
    auto p = patternTree (slot);
    if (getNumEvents (slot) >= maxEventsPerPattern)
        return -1;

    const auto id = nextEventId++;
    juce::ValueTree ev (eventTag);
    ev.setProperty (idProp, id, nullptr);
    ev.setProperty (startProp, juce::jlimit (0.0, maxBars, startBar), nullptr);
    ev.setProperty (lenProp, juce::jmax (0.0, lenBars), nullptr);
    ev.setProperty (chopProp, juce::jmax (0, chop), nullptr);
    ev.setProperty (velProp, juce::jlimit (0, 127, velocity), nullptr);
    ev.setProperty (pitchProp, 0.0, nullptr);
    ev.setProperty (probProp, 100, nullptr);
    ev.setProperty (ratProp, 1, nullptr);
    ev.setProperty (revProp, false, nullptr);
    p.appendChild (ev, &undo);
    return id;
}

bool PatternModel::removeEvent (int slot, int id)
{
    const juce::ScopedLock sl (lock);
    auto ev = eventTree (slot, id);
    if (! ev.isValid())
        return false;
    auto p = patternTree (slot);
    p.removeChild (ev, &undo);
    return true;
}

bool PatternModel::moveEvent (int slot, int id, double newStartBar)
{
    const juce::ScopedLock sl (lock);
    auto ev = eventTree (slot, id);
    if (! ev.isValid())
        return false;
    ev.setProperty (startProp, juce::jlimit (0.0, maxBars, newStartBar), &undo);
    return true;
}

bool PatternModel::setEventLength (int slot, int id, double lenBars)
{
    const juce::ScopedLock sl (lock);
    auto ev = eventTree (slot, id);
    if (! ev.isValid())
        return false;
    ev.setProperty (lenProp, juce::jmax (0.0, lenBars), &undo);
    return true;
}

bool PatternModel::setEventChop (int slot, int id, int chop)
{
    const juce::ScopedLock sl (lock);
    auto ev = eventTree (slot, id);
    if (! ev.isValid())
        return false;
    ev.setProperty (chopProp, juce::jmax (0, chop), &undo);
    return true;
}

int PatternModel::duplicateEvent (int slot, int id, double newStartBar)
{
    const juce::ScopedLock sl (lock);
    const auto src = getEventById (slot, id);
    if (src.id < 0)
        return -1;
    const auto newId = addEvent (slot, newStartBar, src.lenBars, src.chop, src.velocity);
    if (newId >= 0)
    {
        auto ev = eventTree (slot, newId);
        ev.setProperty (pitchProp, src.pitch, &undo);
        ev.setProperty (probProp, src.probability, &undo);
        ev.setProperty (ratProp, src.ratchet, &undo);
        ev.setProperty (revProp, src.reverseFlip, &undo);
    }
    return newId;
}

// ---- per-step lane edits (Phase 5) -----------------------------------------------------

bool PatternModel::setEventProperty (int slot, int id, const juce::Identifier& prop,
                                     const juce::var& value)
{
    const juce::ScopedLock sl (lock);
    auto ev = eventTree (slot, id);
    if (! ev.isValid())
        return false;
    ev.setProperty (prop, value, &undo);
    return true;
}

bool PatternModel::setEventVelocity (int slot, int id, int velocity)
{
    return setEventProperty (slot, id, velProp, juce::jlimit (0, 127, velocity));
}

bool PatternModel::setEventPitch (int slot, int id, float semitones)
{
    return setEventProperty (slot, id, pitchProp,
                             static_cast<double> (juce::jlimit (-24, 24,
                                 juce::roundToInt (semitones))));
}

bool PatternModel::setEventProbability (int slot, int id, int percent)
{
    return setEventProperty (slot, id, probProp, juce::jlimit (0, 100, percent));
}

bool PatternModel::setEventRatchet (int slot, int id, int ratchet)
{
    return setEventProperty (slot, id, ratProp,
                             engine::clampRatchet (juce::jlimit (1, 8, ratchet)));
}

bool PatternModel::setEventReverseFlip (int slot, int id, bool flip)
{
    return setEventProperty (slot, id, revProp, flip);
}

// ---- generators (spec §5) ---------------------------------------------------------------

juce::int64 PatternModel::getSeed (int slot) const
{
    const juce::ScopedLock sl (lock);
    return static_cast<juce::int64> (patternTree (slot).getProperty (seedProp,
                                         static_cast<juce::int64> (1)));
}

void PatternModel::setSeed (int slot, juce::int64 seed)
{
    const juce::ScopedLock sl (lock);
    patternTree (slot).setProperty (seedProp, seed, nullptr);
}

float PatternModel::getRandomizeAmount (int slot) const
{
    const juce::ScopedLock sl (lock);
    return juce::jlimit (0.0f, 1.0f, static_cast<float> (static_cast<double> (
        patternTree (slot).getProperty (amountProp, 0.5))));
}

void PatternModel::setRandomizeAmount (int slot, float amount01)
{
    const juce::ScopedLock sl (lock);
    patternTree (slot).setProperty (amountProp,
                                    static_cast<double> (juce::jlimit (0.0f, 1.0f, amount01)),
                                    nullptr);
}

void PatternModel::randomize (int slot, int numChops)
{
    const juce::ScopedLock sl (lock);
    auto p = patternTree (slot);

    std::vector<juce::ValueTree> evs;
    for (const auto& child : p)
        if (child.hasType (eventTag))
            evs.push_back (child);
    if (evs.empty())
        return;

    const auto amount = getRandomizeAmount (slot);
    engine::DetRng rng;
    rng.seed (engine::detHash (static_cast<std::uint64_t> (getSeed (slot)), 0xA11CEull,
                               static_cast<std::uint64_t> (slot)));

    undo.beginNewTransaction ("Randomize");

    // 1. Chop order: a seeded subset (probability = amount) of events gets its
    //    chop assignments permuted among themselves (Fisher-Yates).
    std::vector<size_t> marked;
    for (size_t i = 0; i < evs.size(); ++i)
        if (rng.next01() < amount)
            marked.push_back (i);
    if (marked.size() >= 2)
    {
        std::vector<int> chops;
        chops.reserve (marked.size());
        for (auto i : marked)
            chops.push_back (static_cast<int> (evs[i].getProperty (chopProp, 0)));
        for (size_t i = chops.size() - 1; i > 0; --i)
        {
            const auto j = static_cast<size_t> (rng.next01() * static_cast<float> (i + 1));
            std::swap (chops[i], chops[juce::jmin (j, i)]);
        }
        for (size_t k = 0; k < marked.size(); ++k)
        {
            auto c = chops[k];
            if (numChops > 0)
                c = juce::jlimit (0, numChops - 1, c);
            evs[marked[k]].setProperty (chopProp, c, &undo);
        }
    }

    // 2–4. Pitch / reverse / ratchet, rewritten per event as a deterministic
    //      function of the RNG stream (fixed draw order).
    static constexpr int musicalPitches[8] = { -12, -7, -5, -3, 3, 5, 7, 12 };
    static constexpr int ratchetWeighted[8] = { 2, 3, 4, 2, 3, 6, 8, 4 };
    for (auto& ev : evs)
    {
        const bool hitPitch = rng.next01() < amount * 0.6f;
        const auto pitchPick = juce::jmin (7, static_cast<int> (rng.next01() * 8.0f));
        const bool hitReverse = rng.next01() < amount * 0.3f;
        const bool hitRatchet = rng.next01() < amount * 0.35f;
        const auto ratchetPick = juce::jmin (7, static_cast<int> (rng.next01() * 8.0f));

        const auto pitch = hitPitch
            ? juce::roundToInt (static_cast<float> (musicalPitches[pitchPick]) * amount)
            : 0;
        ev.setProperty (pitchProp, static_cast<double> (pitch), &undo);
        ev.setProperty (revProp, hitReverse, &undo);
        ev.setProperty (ratProp, hitRatchet ? ratchetWeighted[ratchetPick] : 1, &undo);
    }
}

void PatternModel::shiftPattern (int slot, int deltaSteps)
{
    const juce::ScopedLock sl (lock);
    auto p = patternTree (slot);
    const auto step = engine::noteLengthBars (getNoteLength (slot));
    const auto patBars = static_cast<double> (getPatternBars (slot));
    const auto delta = step * static_cast<double> (deltaSteps);

    undo.beginNewTransaction (deltaSteps >= 0 ? "Shift right" : "Shift left");
    for (auto child : p)
    {
        if (! child.hasType (eventTag))
            continue;
        auto s = std::fmod (static_cast<double> (child.getProperty (startProp, 0.0)) + delta,
                            patBars);
        if (s < 0.0)
            s += patBars;
        child.setProperty (startProp, s, &undo);
    }
}

void PatternModel::reversePattern (int slot)
{
    const juce::ScopedLock sl (lock);
    auto p = patternTree (slot);
    const auto patBars = static_cast<double> (getPatternBars (slot));

    undo.beginNewTransaction ("Reverse pattern");
    for (auto child : p)
    {
        if (! child.hasType (eventTag))
            continue;
        const auto start = static_cast<double> (child.getProperty (startProp, 0.0));
        const auto len = static_cast<double> (child.getProperty (lenProp, 0.0));
        auto s = std::fmod (patBars - start - len, patBars);
        if (s < 0.0)
            s += patBars;
        child.setProperty (startProp, s, &undo);
    }
}

void PatternModel::replacePatternContents (int slot, const juce::ValueTree& src)
{
    auto dst = patternTree (slot);
    for (int i = dst.getNumChildren() - 1; i >= 0; --i)
        if (dst.getChild (i).hasType (eventTag))
            dst.removeChild (i, &undo);

    dst.setProperty (noteLenProp,
                     static_cast<int> (clampNoteLength (static_cast<int> (src.getProperty (
                         noteLenProp, static_cast<int> (engine::NoteLength::sixteenth))))),
                     &undo);
    dst.setProperty (barsProp, clampBars (static_cast<int> (src.getProperty (barsProp, 1))),
                     &undo);
    dst.setProperty (seedProp,
                     static_cast<juce::int64> (src.getProperty (seedProp,
                                                   static_cast<juce::int64> (1))),
                     &undo);
    dst.setProperty (amountProp, static_cast<double> (src.getProperty (amountProp, 0.5)),
                     &undo);

    for (const auto& ev : src)
    {
        if (! ev.hasType (eventTag))
            continue;
        auto copy = ev.createCopy();
        copy.setProperty (idProp, nextEventId++, nullptr);
        dst.appendChild (copy, &undo);
    }
}

void PatternModel::copyToClipboard (int slot)
{
    const juce::ScopedLock sl (lock);
    clipboard = patternTree (slot).createCopy();
}

void PatternModel::pasteFromClipboard (int slot)
{
    const juce::ScopedLock sl (lock);
    if (! clipboard.isValid())
        return;
    undo.beginNewTransaction ("Paste pattern");
    replacePatternContents (slot, clipboard);
}

void PatternModel::copyPatternToSlot (int fromSlot, int toSlot)
{
    const juce::ScopedLock sl (lock);
    fromSlot = juce::jlimit (0, numSlots - 1, fromSlot);
    toSlot = juce::jlimit (0, numSlots - 1, toSlot);
    if (fromSlot == toSlot)
        return;
    undo.beginNewTransaction ("Copy pattern to slot");
    replacePatternContents (toSlot, patternTree (fromSlot).createCopy());
}

// ---- free-run BPM ---------------------------------------------------------------------

double PatternModel::getFreeBpm() const
{
    const juce::ScopedLock sl (lock);
    return juce::jlimit (20.0, 999.0,
                         static_cast<double> (tree.getProperty (freeBpmProp, 120.0)));
}

void PatternModel::setFreeBpm (double bpm)
{
    const juce::ScopedLock sl (lock);
    tree.setProperty (freeBpmProp, juce::jlimit (20.0, 999.0, bpm), nullptr);
}

// ---- snapshot ---------------------------------------------------------------------------

engine::PatternSnapshot PatternModel::buildSnapshot() const
{
    const juce::ScopedLock sl (lock);
    engine::PatternSnapshot snap;

    for (int s = 0; s < numSlots; ++s)
    {
        auto& dst = snap.slots[static_cast<size_t> (s)];
        dst.noteLength = getNoteLength (s);
        dst.bars = getPatternBars (s);
        dst.seed = getSeed (s);

        for (const auto& child : patternTree (s))
        {
            if (! child.hasType (eventTag)
                || dst.numEvents >= engine::PatternSnapshot::maxEvents)
                continue;
            auto& ev = dst.events[static_cast<size_t> (dst.numEvents++)];
            ev.startBar = static_cast<double> (child.getProperty (startProp, 0.0));
            ev.lenBars = static_cast<double> (child.getProperty (lenProp, 0.0));
            ev.chop = static_cast<int> (child.getProperty (chopProp, 0));
            ev.velocity = static_cast<float> (
                juce::jlimit (0, 127,
                              static_cast<int> (child.getProperty (velProp, defaultVelocity))))
                / 127.0f;
            ev.pitch = static_cast<float> (juce::jlimit (-24.0, 24.0,
                static_cast<double> (child.getProperty (pitchProp, 0.0))));
            ev.probability = juce::jlimit (0, 100,
                static_cast<int> (child.getProperty (probProp, 100)));
            ev.ratchet = engine::clampRatchet (juce::jlimit (1, 8,
                static_cast<int> (child.getProperty (ratProp, 1))));
            ev.reverseFlip = static_cast<bool> (child.getProperty (revProp, false));
        }

        std::sort (dst.events.begin(), dst.events.begin() + dst.numEvents,
                   [] (const auto& a, const auto& b) { return a.startBar < b.startBar; });
    }
    return snap;
}

// ---- state ---------------------------------------------------------------------------------

void PatternModel::serializeTo (juce::ValueTree& root) const
{
    const juce::ScopedLock sl (lock);
    auto copy = tree.createCopy();
    jassert (copy.hasType (patternsTag));
    root.addChild (copy, -1, nullptr);
}

void PatternModel::restoreFrom (const juce::ValueTree& root)
{
    {
        const juce::ScopedLock sl (lock);
        const auto src = root.getChildWithName (patternsTag);

        tree.removeListener (this);
        resetToDefaults();

        if (src.isValid())
        {
            tree.setProperty (freeBpmProp,
                              juce::jlimit (20.0, 999.0,
                                            static_cast<double> (src.getProperty (freeBpmProp, 120.0))),
                              nullptr);
            int slot = 0;
            int maxId = 0;
            for (const auto& p : src)
            {
                if (! p.hasType (patternTag) || slot >= numSlots)
                    continue;
                auto dst = tree.getChild (slot++);
                dst.setProperty (noteLenProp,
                                 static_cast<int> (clampNoteLength (static_cast<int> (
                                     p.getProperty (noteLenProp,
                                                    static_cast<int> (engine::NoteLength::sixteenth))))),
                                 nullptr);
                dst.setProperty (barsProp,
                                 clampBars (static_cast<int> (p.getProperty (barsProp, 1))),
                                 nullptr);
                dst.setProperty (seedProp,
                                 static_cast<juce::int64> (p.getProperty (seedProp,
                                                               static_cast<juce::int64> (1))),
                                 nullptr);
                dst.setProperty (amountProp,
                                 juce::jlimit (0.0, 1.0,
                                               static_cast<double> (p.getProperty (amountProp, 0.5))),
                                 nullptr);
                for (const auto& ev : p)
                {
                    if (! ev.hasType (eventTag))
                        continue;
                    auto copy = ev.createCopy();
                    const auto id = static_cast<int> (copy.getProperty (idProp, 0));
                    maxId = juce::jmax (maxId, id);
                    dst.appendChild (copy, nullptr);
                }
            }
            nextEventId = maxId + 1;
        }

        undo.clearUndoHistory();
        tree.addListener (this);
    }
    notifyChanged();
}

// ---- change plumbing -------------------------------------------------------------------------

void PatternModel::notifyChanged()
{
    if (onChanged != nullptr)
        onChanged();
}

void PatternModel::valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&)
{
    notifyChanged();
}
void PatternModel::valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) { notifyChanged(); }
void PatternModel::valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) { notifyChanged(); }
void PatternModel::valueTreeChildOrderChanged (juce::ValueTree&, int, int) { notifyChanged(); }
void PatternModel::valueTreeParentChanged (juce::ValueTree&) {}

} // namespace chopline::state
