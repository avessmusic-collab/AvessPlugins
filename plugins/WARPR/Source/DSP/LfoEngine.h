#pragma once
/*
  LfoEngine - the LFO runtime (architecture C11 / section I "Phase generation"; phase D8).

  One LfoInstance is the state of one LFO (progress in cycles + the two output filters).  advanceLfo() moves one instance
  by one chunk (<= 32 base samples) and returns its value at the END of the chunk; consumers interpolate linearly between
  the previous end value (= chunk start) and the new one.  A bank (advanceLfoBank) advances the LFOs it owns in index order.

  MODES (parameter-spec only says "phase reset behaviour"; the semantics below are the architecture's A1 assumptions,
  verified by Tests/PhaseD8Tests.cpp)
    FREE    one GLOBAL instance, never retriggered.  Sync: progress = ppq / beatsPerCycle (grid-locked, so host jumps, loops
            and stop/start relock); Hz: free-running accumulator.
    RETRIG  one instance PER VOICE, reset to the phase offset at note-on (and at a Mono retrigger).  Sync = tempo-scaled
            speed (not grid-locked).
    ENV     per voice: plays the shape ONCE from the phase offset to the end of the shape (rate = duration of the whole
            shape), then holds the last value; release does not restart it.  Ping-Pong plays as Forward.
    MONO    one GLOBAL instance, reset when a note starts while no other key is held (a new phrase), else continues.
  DIRECTION  Forward p, Reverse 1 - p, Ping-Pong alternates forward / backward per cycle (period = 2 cycles).
  RATE     Hz: 0.01 * 5000^rate.   Sync: index round(rate * 16) into the 17 divisions
           [8 bar, 4 bar, 2 bar, 1 bar, 1/2, 1/4., 1/4, 1/4T, 1/8., 1/8, 1/8T, 1/16., 1/16, 1/16T, 1/32, 1/32T, 1/64];
           the "bar" entries scale with the host bar length (4 beats in 4/4).
  OUTPUT   table value -> fixed 2 ms one-pole slew (de-clicks edits and steps) -> SMOOTH one-pole with
           tau = smooth^2 * min(1 s, period / 2).  mod = 2 v - 1.

  LFO-on-LFO: lfoNRate / lfoNPhase / lfoNSmooth can be modulated.  Within a bank LFOs are evaluated in index order; a
  source with a LOWER index contributes its value of the same chunk, any other source (higher or equal index, or an LFO the
  bank does not own and which is not yet known) its value of the previous chunk - one chunk of delay, no algebraic loops.
*/

#include "DSP/LfoTable.h"
#include "DSP/ModMatrix.h"
#include "Parameters/ParamSnapshot.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace warpr
{
    inline constexpr int kNumLfos = 10;

    enum class LfoMode : int { Free = 0, Retrig = 1, Env = 2, Mono = 3 };
    enum class LfoDir  : int { Forward = 0, Reverse = 1, PingPong = 2 };

    /** An LFO's (unmodulated) parameters for the current block. */
    struct LfoBlockParams
    {
        int mode = 1, dir = 0, sync = 1;
        float rate = 0.375f, phaseDeg = 0.0f, smooth = 0.0f;
    };

    inline constexpr double kLfoBeatsPerCycle[17] = { 32.0, 16.0, 8.0, 4.0, 2.0, 1.5, 1.0, 2.0 / 3.0, 0.75, 0.5, 1.0 / 3.0,
                                                      0.375, 0.25, 1.0 / 6.0, 0.125, 1.0 / 12.0, 0.0625 };

    inline int lfoDivisionIndex (float rate) noexcept
    {
        const int i = (int) std::floor (rate * 16.0f + 0.5f);
        return i < 0 ? 0 : (i > 16 ? 16 : i);
    }

    /** Beats per LFO cycle for a division index; entries 0..3 are 8 / 4 / 2 / 1 BARS (barBeats = 4 in 4/4). */
    inline double lfoBeatsPerCycle (int index, double barBeats) noexcept
    {
        index = index < 0 ? 0 : (index > 16 ? 16 : index);

        if (index < 4)
        {
            constexpr double bars[4] = { 8.0, 4.0, 2.0, 1.0 };
            return bars[index] * barBeats;
        }

        return kLfoBeatsPerCycle[index];
    }

    inline double lfoRateHz (float rate) noexcept { return 0.01 * std::pow (5000.0, (double) rate); }

    inline int lfoParamIndex (int lfo, int slot) noexcept { return (int) toIndex (ParamId::lfo1Mode) + 8 * lfo + slot; }

    inline LfoBlockParams readLfoParams (const ParamBlock& pb, int lfo) noexcept
    {
        LfoBlockParams p;
        p.mode     = std::max (0, std::min (3, juce::roundToInt (pb.values[(size_t) lfoParamIndex (lfo, 0)])));
        p.dir      = std::max (0, std::min (2, juce::roundToInt (pb.values[(size_t) lfoParamIndex (lfo, 1)])));
        p.sync     = std::max (0, std::min (1, juce::roundToInt (pb.values[(size_t) lfoParamIndex (lfo, 2)])));
        p.rate     = pb.values[(size_t) lfoParamIndex (lfo, 3)];
        p.phaseDeg = pb.values[(size_t) lfoParamIndex (lfo, 4)];
        p.smooth   = pb.values[(size_t) lfoParamIndex (lfo, 5)];
        return p;
    }

    //==========================================================================
    struct LfoInstance
    {
        double progress = 0.0;               // cycles since the last reset (FREE + Sync: absolute ppq / beatsPerCycle)
        float  slewed = 0.5f, out = 0.5f;    // after the 2 ms slew / after SMOOTH
        float  phase = 0.0f;                 // direction-mapped shape phase (0..1) of the last chunk end: the UI marker
        bool   envDone = false;
        bool   fresh = true;                 // the next chunk snaps the output filters to the table value (no ramp from 0.5)

        /** New phrase / note: progress back to the phase offset; the output filters keep their state (no click). */
        void restart() noexcept { progress = 0.0; envDone = false; }
    };

    /** Timing of one chunk. */
    struct LfoChunkTiming
    {
        int    n = 32;
        double fs = 44100.0;
        double ppq0 = 0.0, ppq1 = 0.0;       // host quarter-note position at the start / end of the chunk
        double bpm = 120.0;
        double barBeats = 4.0;
    };

    /** Direction-mapped shape phase for `offset` (cycles) + `progress` (cycles).  ENV saturates at the end of the shape. */
    double lfoShapePhase (int mode, int dir, double offset, double progress, bool& envDone) noexcept;

    /** The shape value at progress 0 (what a freshly started RETRIG / ENV LFO outputs).  0.5 without a table. */
    float lfoStartValue (const LfoBlockParams& p, const LfoShapeTable* table) noexcept;

    /** Advances one LFO by one chunk; returns its output value (0..1) at the end of the chunk. */
    float advanceLfo (LfoInstance& s, const LfoBlockParams& p, const LfoShapeTable* table, const LfoChunkTiming& t) noexcept;

    //==========================================================================
    /** Inputs of one bank pass.  `ownMask` bit i = this bank advances LFO i; the other LFOs take extStart / extEnd. */
    struct LfoBankIO
    {
        const LfoBlockParams* params = nullptr;                    // [10]
        const LfoShapeTable* const* tables = nullptr;              // [10] (entries may be nullptr)
        const ModMatrixSnapshot* mm = nullptr;                     // may be nullptr
        int lfoCount = 0;
        LfoChunkTiming timing;
        std::uint32_t ownMask = 0;
        const float* extStart = nullptr;                           // [10] values (0..1) of the LFOs this bank does not own
        const float* extEnd = nullptr;
    };

    /** inst / prevOut: this bank's state (10 entries).  start / end: OUT, 10 values each (0..1) for every LFO - owned ones
        advanced, the others copied from ext, LFOs >= lfoCount = 0.5 (mod 0). */
    void advanceLfoBank (const LfoBankIO& io, LfoInstance* inst, float* prevOut, float* start, float* end) noexcept;

    //==========================================================================
    /** What WarprSynth hands every voice for one chunk. */
    struct ModContext
    {
        const ModMatrixSnapshot* mm = nullptr;                     // nullptr / empty = no modulation
        const LfoBlockParams* params = nullptr;                    // [10]
        const LfoShapeTable* const* tables = nullptr;              // [10]
        int lfoCount = 0;
        LfoChunkTiming timing;
        std::uint32_t voiceOwnMask = 0;                            // LFOs the voices advance (RETRIG / ENV, within lfoCount)
        const float* gStart = nullptr;                             // [10] values of the global LFOs at the chunk start
        const float* gEnd = nullptr;                               //      ... and at the chunk end
    };
}
