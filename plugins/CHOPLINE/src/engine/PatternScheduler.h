// CHOPLINE — PatternScheduler (spec §5, §8)
// Grid → sample-accurate triggers, derived from the host PPQ every block
// (never a drifting internal counter).
//
// Model: pattern events are TIME-BASED — a musical position plus a length,
// both in fractions of a bar (locked v17 `events_are_time_based`). The audio
// thread sees an immutable PatternSnapshot (published via SnapshotExchange)
// carrying ALL 8 pattern slots (Phase 5: the APVTS `pattern_slot` parameter
// picks the playing slot; switches quantise to the next bar start while the
// transport runs and are immediate when stopped). This class holds only the
// small amount of audio-thread transport state (previous-block prediction,
// free-run position, the playing slot).
//
// Per block (audio thread, no allocation):
//   1. Decide whether the pattern runs: host playing, or free-running when no
//      host transport exists (standalone — Phase 5 gates this on the Space
//      transport-preview). Host present but stopped → the pattern is silent
//      (MIDI chop triggers are handled by the Engine).
//   2. Position in bars = hostPpq / quartersPerBar (time signature from the
//      host). No PPQ → an internal accumulator advanced by the current BPM
//      (host BPM if given, else the manual free-run BPM).
//   3. A PPQ discontinuity (loop wrap, jump, scrub) larger than the tolerance
//      releases ringing pattern voices (short fade) and simply continues from
//      the new position — the correct step follows from the PPQ itself.
//   4. Every event occurrence (pattern cycles overlapping the block) whose
//      swung position falls inside the block becomes a trigger at its exact
//      sample offset. Phase 5 per-step lanes apply here:
//        PROBABILITY — one roll per occurrence, keyed by a deterministic hash
//          of (pattern seed, slot, cycle index, event position), so an offline
//          bounce fires exactly like the live pass (spec §5 "repeatable").
//        RATCHET — the event span subdivides into N equal retriggers (N in
//          {1,2,3,4,6,8}); each retrigger's velocity decays by a fixed 0.8×
//          (≈ −1.9 dB per hit — the musical fixed decay, documented).
//        PITCH / REVERSE — passed through to the voice as trigger mods.
//
// Swing (spec §5): even steps — the 2nd, 4th, … box, i.e. odd 0-based step
// indices of the CURRENT note-length grid — trigger late by swing% of one
// step (0–75%, so order is always preserved). Swing shifts trigger TIME only;
// events that are off the current grid are not swung. Ratchet retriggers ride
// the swung onset.

#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include "DetHash.h"

#include <array>
#include <atomic>
#include <cmath>

namespace chopline::engine
{

// ---- note lengths (locked v17 note_length_model; NO 1/32) -----------------------

enum class NoteLength : int
{
    quarter = 0, quarterTriplet, quarterDotted,
    eighth,      eighthTriplet,  eighthDotted,
    sixteenth,   sixteenthTriplet, sixteenthDotted
};

inline constexpr int numNoteLengths = 9;

/// Step length in fractions of a (4/4) bar.
constexpr double noteLengthBars (NoteLength n) noexcept
{
    constexpr double table[numNoteLengths] = {
        1.0 / 4.0,  1.0 / 6.0,  3.0 / 8.0,   // 1/4  1/4T  1/4D
        1.0 / 8.0,  1.0 / 12.0, 3.0 / 16.0,  // 1/8  1/8T  1/8D
        1.0 / 16.0, 1.0 / 24.0, 3.0 / 32.0,  // 1/16 1/16T 1/16D
    };
    const auto i = static_cast<int> (n);
    return table[i < 0 ? 0 : (i >= numNoteLengths ? numNoteLengths - 1 : i)];
}

/// Boxes per bar (v17: 4/8/16, 6/12/24, 3/6/11 — the last dotted box is
/// fractionally wide; it is simply the remainder of the bar).
constexpr int noteLengthStepsPerBar (NoteLength n) noexcept
{
    constexpr int table[numNoteLengths] = { 4, 6, 3, 8, 12, 6, 16, 24, 11 };
    const auto i = static_cast<int> (n);
    return table[i < 0 ? 0 : (i >= numNoteLengths ? numNoteLengths - 1 : i)];
}

inline const char* noteLengthLabel (NoteLength n) noexcept
{
    static constexpr const char* table[numNoteLengths] = {
        "1/4", "1/4T", "1/4D", "1/8", "1/8T", "1/8D", "1/16", "1/16T", "1/16D",
    };
    const auto i = static_cast<int> (n);
    return table[i < 0 ? 0 : (i >= numNoteLengths ? numNoteLengths - 1 : i)];
}

/// The ratchet choices (spec §5): repeats per step.
inline constexpr int ratchetChoices[6] = { 1, 2, 3, 4, 6, 8 };

/// Clamps an arbitrary stored ratchet to the nearest legal choice (≤ value).
constexpr int clampRatchet (int r) noexcept
{
    int best = 1;
    for (auto c : ratchetChoices)
        if (c <= r)
            best = c;
    return best;
}

/// Fixed per-ratchet velocity decay (documented: 0.8× ≈ −1.9 dB per repeat).
inline constexpr float ratchetDecay = 0.8f;

// ---- immutable audio-thread view ---------------------------------------------------

struct PatternSnapshot
{
    static constexpr int maxEvents = 256;
    static constexpr int numSlots = 8;

    struct Event
    {
        double startBar = 0.0;  // musical position within the pattern, in bars
        double lenBars = 0.0;   // span length in bars
        int chop = 0;
        float velocity = 1.0f;  // 0..1 (model stores 0–127)
        float pitch = 0.0f;     // per-step pitch offset, semitones (lane)
        int probability = 100;  // % (lane)
        int ratchet = 1;        // 1,2,3,4,6,8 repeats (lane)
        bool reverseFlip = false; // per-step reverse override (XOR, generators only)
    };

    struct Slot
    {
        int numEvents = 0;                 // sorted by startBar
        std::array<Event, maxEvents> events {};
        NoteLength noteLength = NoteLength::sixteenth;
        int bars = 1;                      // 1, 2 or 4
        juce::int64 seed = 0;              // probability / randomize seed
    };

    std::array<Slot, numSlots> slots {};
};

// ---- scheduler ------------------------------------------------------------------------

class PatternScheduler final
{
public:
    struct HostView
    {
        bool hasPosition = false; // a playhead exists (false in the Standalone)
        bool playing = false;
        bool hasPpq = false;
        double ppq = 0.0;
        bool hasBpm = false;
        double bpm = 120.0;
        int timeSigNumerator = 4;
        int timeSigDenominator = 4;
    };

    struct Trigger
    {
        int offset = 0; // sample offset within the block
        int chop = 0;
        float velocity = 1.0f;
        juce::int64 lengthSamples = 0; // event span (Gate/Loop gate, Fit target)
        float pitchOffset = 0.0f;      // per-step pitch lane (adds to chop pitch)
        bool reverseFlip = false;      // per-step reverse override
    };

    static constexpr int maxTriggers = 128; // ratchets multiply triggers per block

    void prepare (double newSampleRate) noexcept
    {
        sampleRate = newSampleRate > 0.0 ? newSampleRate : 48000.0;
        wasRunning = false;
        hasPrediction = false;
        freeRunBars = 0.0;
        activeSlot = 0;
        uiPosBars.store (-1.0f, std::memory_order_relaxed);
    }

    /// Audio thread. Collects this block's pattern triggers (≤ maxTriggers,
    /// unsorted). `desiredSlot` is the APVTS pattern-slot choice: a change
    /// quantises to the next bar start while running, applies immediately when
    /// stopped. `freeRunGate` gates the no-playhead free-run (Space preview in
    /// the Standalone; hosts always pass true). Sets `releasePatternVoices`
    /// when ringing pattern voices should fast-fade (transport stop or PPQ
    /// discontinuity).
    int process (const PatternSnapshot* snap, const HostView& host, float swingPct,
                 double freeRunBpm, int numSamples, int desiredSlot, bool freeRunGate,
                 Trigger* out, bool& releasePatternVoices) noexcept
    {
        releasePatternVoices = false;
        const auto desired = juce::jlimit (0, PatternSnapshot::numSlots - 1, desiredSlot);

        const auto quartersPerBar = host.timeSigDenominator > 0 && host.timeSigNumerator > 0
            ? static_cast<double> (host.timeSigNumerator) * 4.0
                  / static_cast<double> (host.timeSigDenominator)
            : 4.0;

        auto bpm = host.hasBpm && host.bpm > 1.0 ? host.bpm
                                                 : (freeRunBpm > 1.0 ? freeRunBpm : 120.0);
        const auto blockQuarters = static_cast<double> (numSamples) / sampleRate * bpm / 60.0;

        // Host present but transport stopped → the pattern is silent. No host
        // playhead (Standalone) → free-run only while the preview gate is open.
        const bool running = host.hasPosition ? host.playing : freeRunGate;
        if (! running || numSamples <= 0)
        {
            if (wasRunning)
                releasePatternVoices = true;
            wasRunning = false;
            hasPrediction = false;
            activeSlot = desired; // slot switches are immediate while stopped
            uiPosBars.store (-1.0f, std::memory_order_relaxed);
            return 0;
        }

        double startBars = 0.0;
        if (host.hasPosition && host.hasPpq)
        {
            // Loop / jump / scrub: the PPQ no longer matches the prediction.
            const auto tolerance = juce::jmax (0.01, blockQuarters * 0.25);
            if (hasPrediction && std::abs (host.ppq - predictedPpq) > tolerance)
                releasePatternVoices = true;

            startBars = host.ppq / quartersPerBar;
            predictedPpq = host.ppq + blockQuarters;
            hasPrediction = true;
        }
        else
        {
            // Free-run (standalone, or a host that gives no PPQ): internal
            // accumulator — restarted whenever the pattern (re)starts.
            if (! wasRunning)
                freeRunBars = 0.0;
            startBars = freeRunBars;
            freeRunBars += blockQuarters / quartersPerBar;
            hasPrediction = false;
        }
        if (! wasRunning)
            activeSlot = desired; // a (re)start applies the pending slot at once
        wasRunning = true;

        if (snap == nullptr)
        {
            uiPosBars.store (-1.0f, std::memory_order_relaxed);
            return 0;
        }

        const auto samplesPerBar = sampleRate * 60.0 / bpm * quartersPerBar;
        const auto swing = static_cast<double> (juce::jlimit (0.0f, 75.0f, swingPct)) / 100.0;

        int n = 0;

        // Pending slot switch: quantise to the next bar start (spec §5
        // "at bar end"). Triggers before the boundary come from the old slot,
        // from the boundary on from the new slot.
        if (desired != activeSlot)
        {
            const auto halfSampleBars = 0.5 / samplesPerBar;
            const auto boundaryBars = std::ceil (startBars - halfSampleBars);
            const auto boundaryOffset = static_cast<juce::int64> (std::floor (
                (boundaryBars - startBars) * samplesPerBar + 0.5));
            if (boundaryOffset < static_cast<juce::int64> (numSamples))
            {
                const auto b = static_cast<int> (juce::jmax<juce::int64> (0, boundaryOffset));
                n = collectTriggers (snap->slots[static_cast<size_t> (activeSlot)], activeSlot,
                                     startBars, samplesPerBar, swing, 0, b, out, n);
                activeSlot = desired;
                n = collectTriggers (snap->slots[static_cast<size_t> (activeSlot)], activeSlot,
                                     startBars, samplesPerBar, swing, b, numSamples, out, n);
            }
            else
            {
                n = collectTriggers (snap->slots[static_cast<size_t> (activeSlot)], activeSlot,
                                     startBars, samplesPerBar, swing, 0, numSamples, out, n);
            }
        }
        else
        {
            n = collectTriggers (snap->slots[static_cast<size_t> (activeSlot)], activeSlot,
                                 startBars, samplesPerBar, swing, 0, numSamples, out, n);
        }

        const auto& slot = snap->slots[static_cast<size_t> (activeSlot)];
        if (slot.bars > 0)
            uiPosBars.store (static_cast<float> (std::fmod (startBars,
                                 static_cast<double> (slot.bars))),
                             std::memory_order_relaxed);
        else
            uiPosBars.store (-1.0f, std::memory_order_relaxed);

        return n;
    }

    /// Message thread (UI playhead): position within the pattern in bars,
    /// −1 while the pattern is not running.
    float getUiPositionBars() const noexcept
    {
        return uiPosBars.load (std::memory_order_relaxed);
    }

private:
    int collectTriggers (const PatternSnapshot::Slot& slot, int slotIndex,
                         double startBars, double samplesPerBar, double swing,
                         int minOffset, int maxOffset, Trigger* out, int n) noexcept
    {
        if (slot.numEvents <= 0 || slot.bars <= 0 || maxOffset <= minOffset)
            return n;

        const auto patBars = static_cast<double> (slot.bars);
        const auto stepLen = noteLengthBars (slot.noteLength);
        const auto endBars = startBars + static_cast<double> (maxOffset) / samplesPerBar;

        const auto firstCycle = static_cast<juce::int64> (std::floor (startBars / patBars)) - 1;
        const auto lastCycle = static_cast<juce::int64> (std::floor (endBars / patBars)) + 1;

        for (auto c = firstCycle; c <= lastCycle && n < maxTriggers; ++c)
        {
            const auto base = static_cast<double> (c) * patBars;
            for (int e = 0; e < slot.numEvents && n < maxTriggers; ++e)
            {
                const auto& ev = slot.events[static_cast<size_t> (e)];
                if (ev.startBar >= patBars) // beyond a shortened pattern: kept, silent
                    continue;

                auto pos = ev.startBar;
                if (swing > 0.0)
                {
                    // Swing even steps (odd 0-based index) of the current grid.
                    const auto idx = static_cast<juce::int64> (std::llround (pos / stepLen));
                    if ((idx & 1) != 0
                        && std::abs (pos - static_cast<double> (idx) * stepLen) < 1.0e-6)
                        pos += swing * stepLen;
                }

                // PROBABILITY (spec §5): one deterministic roll per occurrence,
                // keyed by (seed, slot, cycle, event position + event index) —
                // a bounce fires exactly like the live pass at any block size,
                // and two events layered on the SAME step roll independently.
                const auto prob = juce::jlimit (0, 100, ev.probability);
                if (prob < 100)
                {
                    if (prob <= 0)
                        continue;
                    const auto posKey = static_cast<std::uint64_t> (
                                            std::llround (ev.startBar * 1.0e9))
                                      ^ detMix (static_cast<std::uint64_t> (e) + 1u);
                    const auto key = detHash (static_cast<std::uint64_t> (slot.seed),
                                              static_cast<std::uint64_t> (slotIndex) + 101u,
                                              static_cast<std::uint64_t> (c + (1ll << 32)),
                                              posKey);
                    if (det01 (key) * 100.0f >= static_cast<float> (prob))
                        continue;
                }

                // RATCHET (spec §5): subdivide the event span into N equal
                // retriggers riding the swung onset; fixed 0.8× decay per hit.
                const auto ratchet = ev.lenBars > 0.0 ? clampRatchet (ev.ratchet) : 1;
                const auto subLen = ev.lenBars / static_cast<double> (ratchet);

                auto gain = ev.velocity;
                for (int k = 0; k < ratchet && n < maxTriggers; ++k)
                {
                    // Inclusion test in ROUNDED sample space so a trigger
                    // sitting exactly on a block boundary lands once, in the
                    // right block (exact down to block size 1).
                    const auto t = base + pos + static_cast<double> (k) * subLen;
                    const auto offsetD = (t - startBars) * samplesPerBar;
                    const auto offset = static_cast<int> (std::floor (offsetD + 0.5));
                    if (offset >= minOffset && offset < maxOffset)
                    {
                        const auto spanBars = ratchet > 1 ? subLen : ev.lenBars;
                        const auto len = juce::jmax<juce::int64> (1,
                            static_cast<juce::int64> (std::llround (spanBars * samplesPerBar)));
                        out[n++] = { offset, ev.chop, gain, len, ev.pitch, ev.reverseFlip };
                    }
                    gain *= ratchetDecay;
                }
            }
        }
        return n;
    }

    double sampleRate = 48000.0;
    bool wasRunning = false;
    bool hasPrediction = false;
    double predictedPpq = 0.0;
    double freeRunBars = 0.0;
    int activeSlot = 0; // the PLAYING slot (bar-quantised switches)
    std::atomic<float> uiPosBars { -1.0f };
};

} // namespace chopline::engine
