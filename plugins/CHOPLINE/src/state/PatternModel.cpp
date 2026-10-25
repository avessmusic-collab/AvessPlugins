// CHOPLINE — PatternModel implementation. See header for the design.

#include "PatternModel.h"

#include <algorithm>

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
    const juce::Identifier noteLenProp { "noteLen" };
    const juce::Identifier barsProp { "bars" };
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
        tree.appendChild (p, nullptr);
    }
    nextEventId = 1;
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
    }
    return newId;
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
    const auto slot = getActiveSlot();
    snap.noteLength = getNoteLength (slot);
    snap.bars = getPatternBars (slot);

    for (const auto& child : patternTree (slot))
    {
        if (! child.hasType (eventTag) || snap.numEvents >= engine::PatternSnapshot::maxEvents)
            continue;
        auto& ev = snap.events[static_cast<size_t> (snap.numEvents++)];
        ev.startBar = static_cast<double> (child.getProperty (startProp, 0.0));
        ev.lenBars = static_cast<double> (child.getProperty (lenProp, 0.0));
        ev.chop = static_cast<int> (child.getProperty (chopProp, 0));
        ev.velocity = static_cast<float> (
            juce::jlimit (0, 127, static_cast<int> (child.getProperty (velProp, defaultVelocity))))
            / 127.0f;
    }

    std::sort (snap.events.begin(), snap.events.begin() + snap.numEvents,
               [] (const auto& a, const auto& b) { return a.startBar < b.startBar; });
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
