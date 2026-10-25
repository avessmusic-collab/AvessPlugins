// CHOPLINE — PatternScheduler (spec §5, §8)
// Grid → sample-accurate triggers, derived from the host PPQ every block
// (never a drifting internal counter).
//
// Model: pattern events are TIME-BASED — a musical position plus a length,
// both in fractions of a bar (locked v17 `events_are_time_based`). The audio
// thread sees an immutable PatternSnapshot (published via SnapshotExchange);
// this class holds only the small amount of audio-thread transport state
// (previous-block prediction, free-run position).
//
// Per block (audio thread, no allocation):
//   1. Decide whether the pattern runs: host playing, or free-running when no
//      host transport exists (standalone). Host present but stopped → the
//      pattern is silent (MIDI chop triggers are handled by the Engine).
//   2. Position in bars = hostPpq / quartersPerBar (time signature from the
//      host). No PPQ → an internal accumulator advanced by the current BPM
//      (host BPM if given, else the manual free-run BPM).
//   3. A PPQ discontinuity (loop wrap, jump, scrub) larger than the tolerance
//      releases ringing pattern voices (short fade) and simply continues from
//      the new position — the correct step follows from the PPQ itself.
//   4. Every event occurrence (pattern cycles overlapping the block) whose
//      swung position falls inside [blockStart, blockEnd) becomes a trigger
//      at its exact sample offset.
//
// Swing (spec §5): even steps — the 2nd, 4th, … box, i.e. odd 0-based step
// indices of the CURRENT note-length grid — trigger late by swing% of one
// step (0–75%, so order is always preserved). Swing shifts trigger TIME only;
// events that are off the current grid are not swung.

#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

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

// ---- immutable audio-thread view ---------------------------------------------------

struct PatternSnapshot
{
    static constexpr int maxEvents = 256;

    struct Event
    {
        double startBar = 0.0; // musical position within the pattern, in bars
        double lenBars = 0.0;  // span length in bars (stored + rendered;
                               // playback is one-shot until Phase 3)
        int chop = 0;
        float velocity = 1.0f; // 0..1 (model stores 0–127)
    };

    int numEvents = 0;                      // sorted by startBar
    std::array<Event, maxEvents> events {};
    NoteLength noteLength = NoteLength::sixteenth;
    int bars = 1;                           // 1, 2 or 4
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
    };

    static constexpr int maxTriggers = 64;

    void prepare (double newSampleRate) noexcept
    {
        sampleRate = newSampleRate > 0.0 ? newSampleRate : 48000.0;
        wasRunning = false;
        hasPrediction = false;
        freeRunBars = 0.0;
        uiPosBars.store (-1.0f, std::memory_order_relaxed);
    }

    /// Audio thread. Collects this block's pattern triggers (≤ maxTriggers,
    /// unsorted). Sets `releasePatternVoices` when ringing pattern voices
    /// should fast-fade (transport stop or PPQ discontinuity).
    int process (const PatternSnapshot* snap, const HostView& host, float swingPct,
                 double freeRunBpm, int numSamples, Trigger* out,
                 bool& releasePatternVoices) noexcept
    {
        releasePatternVoices = false;

        const auto quartersPerBar = host.timeSigDenominator > 0 && host.timeSigNumerator > 0
            ? static_cast<double> (host.timeSigNumerator) * 4.0
                  / static_cast<double> (host.timeSigDenominator)
            : 4.0;

        auto bpm = host.hasBpm && host.bpm > 1.0 ? host.bpm
                                                 : (freeRunBpm > 1.0 ? freeRunBpm : 120.0);
        const auto blockQuarters = static_cast<double> (numSamples) / sampleRate * bpm / 60.0;

        // Host present but transport stopped → the pattern is silent.
        const bool running = host.hasPosition ? host.playing : true;
        if (! running || numSamples <= 0)
        {
            if (wasRunning)
                releasePatternVoices = true;
            wasRunning = false;
            hasPrediction = false;
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
        wasRunning = true;

        const auto endBars = startBars + blockQuarters / quartersPerBar;

        if (snap == nullptr || snap->bars <= 0)
        {
            uiPosBars.store (-1.0f, std::memory_order_relaxed);
            return 0;
        }

        const auto patBars = static_cast<double> (snap->bars);
        uiPosBars.store (static_cast<float> (std::fmod (startBars, patBars)),
                         std::memory_order_relaxed);

        if (snap->numEvents <= 0)
            return 0;

        const auto stepLen = noteLengthBars (snap->noteLength);
        const auto swing = static_cast<double> (juce::jlimit (0.0f, 75.0f, swingPct)) / 100.0;
        const auto samplesPerBar = sampleRate * 60.0 / bpm * quartersPerBar;

        int n = 0;
        const auto firstCycle = static_cast<juce::int64> (std::floor (startBars / patBars)) - 1;
        const auto lastCycle = static_cast<juce::int64> (std::floor (endBars / patBars)) + 1;

        for (auto c = firstCycle; c <= lastCycle && n < maxTriggers; ++c)
        {
            const auto base = static_cast<double> (c) * patBars;
            for (int e = 0; e < snap->numEvents && n < maxTriggers; ++e)
            {
                const auto& ev = snap->events[static_cast<size_t> (e)];
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

                // Inclusion test in ROUNDED sample space so a trigger sitting
                // exactly on a block boundary lands once, in the right block
                // (bar-space tests clamp it a sample early at tiny blocks).
                const auto t = base + pos;
                const auto offsetD = (t - startBars) * samplesPerBar;
                const auto offset = static_cast<int> (std::floor (offsetD + 0.5));
                if (offset >= 0 && offset < numSamples)
                    out[n++] = { offset, ev.chop, ev.velocity };
            }
        }
        return n;
    }

    /// Message thread (UI playhead): position within the pattern in bars,
    /// −1 while the pattern is not running.
    float getUiPositionBars() const noexcept
    {
        return uiPosBars.load (std::memory_order_relaxed);
    }

private:
    double sampleRate = 48000.0;
    bool wasRunning = false;
    bool hasPrediction = false;
    double predictedPpq = 0.0;
    double freeRunBars = 0.0;
    std::atomic<float> uiPosBars { -1.0f };
};

} // namespace chopline::engine
