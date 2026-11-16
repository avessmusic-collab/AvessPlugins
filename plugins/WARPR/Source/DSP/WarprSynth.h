#pragma once
/*
  WarprSynth - voice allocation, MIDI handling and block rendering (architecture C12).

  DEVIATION FROM THE ARCHITECTURE TEXT (documented): this is NOT a juce::Synthesiser subclass.
  juce::Synthesiser takes an internal CriticalSection inside renderNextBlock / noteOn, and its voice
  bookkeeping would be bypassed anyway (Poly / Mono / Legato note stack, ghost slots, budget).  The one
  thing the base class provided - splitting a block at MIDI event positions so note starts are
  sample-accurate - is re-implemented in renderBlock() (a dozen lines).  MIDI is parsed from the raw
  bytes of the buffer, so no MidiMessage (which can allocate for long messages) is ever constructed.

  Voices
    34 slots = 32 voices + 2 "ghost" slots.  A stolen / superseded voice is not cut: it keeps
    playing in its slot for a 3 ms linear fade (a ghost; ghosts do not count against polyphony or
    the oscillator budget) while the new note starts in a free slot.
  Poly     : up to `polyphony` voices (live reduction steals with the same policy).  Steal order:
             (1) the same note, (2) the quietest voice in release, (3) the oldest sounding voice.
  Mono     : one voice, last-note priority with a note stack.  A new note while sounding retargets the
             pitch (glide) and RETRIGGERS THE ENVELOPE FROM THE CURRENT LEVEL on the same voice - no
             playhead restart, so it is click-free.  Exception: One-Shot loop mode restarts the sample
             through a ghost fade (a drum hit must re-trigger).
  One-Shot TRIGGER: loopMode One-Shot + Classic playback (latched per voice at note-on) ignores key-up and sustain-pedal
             release - one tap plays the whole region; the voice auto-releases at the region end (WarprVoice, 5 ms fast
             release).  Mono / Legato key-up of such a voice removes the note from the stack but neither releases nor retargets
             it.  Granular One-Shot is unchanged (gated: grains have no natural end).  Steal / ghost / CC120 / CC123 still end it.
  SLICE MODE (D11, SliceMap.h): while the published slice map is ON, a note in [baseNote, baseNote + N) starts a SLICE voice
             (region [e_k, e_k+1) latched at note-on, unpitched, one-shot trigger, Classic playback) and every other note plays
             nothing.  The map is read wait-free once per block (ModEngine::beginBlock); a replacement never touches sounding
             voices.  In Mono / Legato a slice note always starts a new voice through a ghost fade (no retarget / glide).
  Legato   : as Mono but overlapping notes do not retrigger the envelope (glide only).
  Pedal    : CC64 >= 64 holds released notes; releasing it releases them (Poly: per voice flag,
             Mono / Legato: through the note stack).  CC123 releases everything, CC120 fades it in 3 ms.
  Pitch bend: wheel -> [-1, 1] * bendRange (2 / 12 / 24 st); the voices slew it over 3 ms.
  Velocity : fixed law 0.25 + 0.75 * v (no parameter exists, ambiguity A2).

  Oscillator-budget governor (default 256 OS-weighted units; 0 disables): a new note first
  reduces ITS OWN unison count (never below 1), then steals other voices.  It never changes the
  unison / OS of a sounding voice.  Cost of an oscillator = os x c_mode (architecture "Voice budget governor"):
  c_classic = 1 for Classic playback, c_granular = 1 + 0.25 x grains for a granular note, where grains = the
  scheduler's simultaneous grains per channel (Classic 2, Loop 2, Cloud = grainDensity).  Costs are kept in
  QUARTER units (an integer: os x (4 + grains)) so the arithmetic is exact; the budget is multiplied by 4.  The cost
  is latched at note-on from playMode / grainMode / grainDensity / OS: a later playMode or density edit never
  changes what a sounding voice is charged.  This is also the "unison cap in Granular": a Cloud of 20 grains at
  OS 2 costs 12 units per oscillator, so a 256-unit budget fits 21 of them and new notes shrink their own unison.

  CPU guard (D10, opt-in; CpuGuard.h): PluginProcessor feeds the measured block load to a CpuGuard and publishes its
  level (0..3) here once per block (setGuardLevel).  startNewVoice caps the oversampling factor LATCHED for the NEW note
  to CpuGuard::osCap (level) = 8 / 4 / 2 / 1.  Sounding voices (unison, OS, anything) are never touched, nothing is stolen
  or refused because of the guard, and the guard overrides the warp / shaper minOS floors on purpose (an aliasing note beats
  a dropout).  The cheaper latched OS also lowers the note's budget cost (cost = os x c_mode), so the ordinary budget
  governor above then lets it keep more unison oscillators.  Level 0 (the default, and whenever the setting is off) is the
  untouched path, bit-exact.

  Interpolation quality (D10, Settings): 0 = High (8-tap Kaiser sinc, the default), 1 = Eco (4-point Hermite).  The value is
  read once per block into VoiceParams::interpQuality and acts on EVERY sample read of sounding and new voices alike
  (Classic reads, warp-shifted reads, the Sample-source modulator and the grain reads); see SampleReader.h for the numbers.

  Sample references: each voice owns one explicit reference to the SampleData it plays and returns it
  through the RetireList (deferRelease) when it ends, so the final delete is never on the audio thread.
*/

#include "DSP/HalfbandIIR.h"
#include "DSP/LfoEngine.h"
#include "DSP/ModEngine.h"
#include "DSP/SampleStore.h"
#include "DSP/ShaperEngine.h"
#include "DSP/SliceMap.h"
#include "DSP/TransportClock.h"
#include "DSP/WarprVoice.h"
#include "Parameters/ParamSnapshot.h"
#include "Util/LockFreeSwap.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <atomic>
#include <cstdint>

namespace warpr
{
    class WarprSynth
    {
    public:
        static constexpr int maxVoices  = 32;
        static constexpr int ghostSlots = 2;
        static constexpr int totalSlots = maxVoices + ghostSlots;

        struct VoiceInfo
        {
            bool active = false, ghost = false;
            int note = -1, unison = 0, os = 1;
            int costQ = 4;                       // quarter units per oscillator (D7b)
            float level = 0.0f;
        };

        explicit WarprSynth (RetireList& retireListToUse) noexcept : retireList (retireListToUse) {}
        ~WarprSynth() { reset(); }

        WarprSynth (const WarprSynth&) = delete;
        WarprSynth& operator= (const WarprSynth&) = delete;

        //======================================================================
        // NON-AUDIO thread

        /** Sets rates, designs nothing per note (the half-band is designed here), kills every voice. */
        void prepare (double newSampleRate, int maxBlockSize);

        /** The shaper engine whose sets the voices read (D4).  nullptr = no shaper stage.  Set once, before audio. */
        void attachShaper (ShaperEngine* engine) noexcept { shaper = engine; }

        /** The modulation publication point (LFO shape tables, matrix, lfoCount).  nullptr = no LFO system.  Set once, before audio. */
        void attachModEngine (ModEngine* engine) noexcept { modEngine = engine; }

        /** Kills every voice and releases all sample references directly (audio must be stopped). */
        void reset();

        //======================================================================
        // AUDIO thread

        /** The sample new notes will use (already acquired from the SampleStore). */
        void setCurrentSample (SampleData* s) noexcept { currentSample = s; }

        /** Reads the block's parameter values (once per block).  Also adopts pending LFO tables / matrix (ModEngine). */
        void setBlockParams (const ParamBlock& pb) noexcept;

        /** Host transport for the NEXT renderBlock (once per block).  Default = no host: internal clock at 120 bpm. */
        void setTransport (const TransportInfo& info) noexcept { transportInfo = info; }

        /** masterGain (dB) with its modulation (global class G); valid after renderBlock. */
        float modulatedMasterGainDb (float baseDb) const noexcept;

        /** Viz: direction-mapped phase (0..1) of every LFO, -1 = idle (index >= lfoCount, unused by the matrix, or a voice
            LFO without an active voice).  Wait-free snapshot written once per block; `out` takes 10 values. */
        void readLfoPhases (float* out) const noexcept;

        // D8 diagnostics / tests
        float getGlobalLfoPhase (int lfo) const noexcept      { return gInst[(size_t) lfo].phase; }
        float getGlobalLfoValue (int lfo) const noexcept      { return gPrev[(size_t) lfo]; }
        double getGlobalLfoProgress (int lfo) const noexcept  { return gInst[(size_t) lfo].progress; }
        float getVoiceLfoPhase (int slot, int lfo) const noexcept { return voices[(size_t) slot].lfoPhase (lfo); }
        float getVoiceLfoValue (int slot, int lfo) const noexcept { return voices[(size_t) slot].lfoEndValue (lfo); }
        double getVoiceLfoProgress (int slot, int lfo) const noexcept { return voices[(size_t) slot].lfoProgress (lfo); }
        const TransportClock& getClock() const noexcept       { return clock; }

        /** ADDS the synth output to outL / outR (callers clear them first). */
        void renderBlock (float* outL, float* outR, int numSamples, const juce::MidiBuffer& midi) noexcept;

        int getNumActiveVoices() const noexcept;         // includes ghosts
        int getNumNonGhostVoices() const noexcept;
        int getOscillatorUnitsInUse() const noexcept;    // non-ghost voices: sum of unison * cost, in units (rounded up)
        int getOscillatorQuarterUnitsInUse() const noexcept;   // the same in exact quarter units

        /** Budget cost of ONE oscillator in quarter units: os x (4 for Classic playback; 4 + grains when granular). */
        static constexpr int oscillatorCostQ (int os, bool granular, int grains) noexcept
        {
            return os * (granular ? 4 + grains : 4);
        }

        /** Simultaneous grains per channel of a grain scheduler (grainMode 0 Classic, 1 Loop, 2 Cloud). */
        static constexpr int grainsPerChannel (int grainMode, int density) noexcept
        {
            return grainMode == 2 ? (density < 1 ? 1 : (density > 20 ? 20 : density)) : 2;
        }

        //======================================================================
        // UI playheads: wait-free snapshot written by the audio thread once per block (relaxed
        // atomics, fixed storage, no allocation) and read by the editor timer.  The count and the
        // slots may tear against each other by one block, which is harmless for a display.
        /** Fills up to `maxOut` entries (pos 0..1 absolute in the sample, level 0..1); returns the count. */
        int readPlayheads (float* posOut, float* levelOut, int maxOut) const noexcept;

        /** Same wait-free snapshot for the granular voices' sounding grains (D7a): pos 0..1 absolute in the sample,
            level = the grain's window amplitude.  At most maxVizGrains entries across all voices. */
        static constexpr int maxVizGrains = 64;
        int readGrains (float* posOut, float* levelOut, int maxOut) const noexcept;

        //======================================================================
        // Settings (non-parameter, message thread -> audio thread, relaxed atomics)
        void setOscillatorBudget (int units) noexcept     { oscBudget.store (units, std::memory_order_relaxed); }
        void setInterpolationQuality (int quality) noexcept { interpQuality.store (quality < 0 ? 0 : (quality > 1 ? 1 : quality), std::memory_order_relaxed); }
        int  getOscillatorBudget() const noexcept         { return oscBudget.load (std::memory_order_relaxed); }
        int  getInterpolationQuality() const noexcept     { return interpQuality.load (std::memory_order_relaxed); }

        /** CPU guard level 0..3 (CpuGuard), published by the processor once per block.  0 = guard idle / off. */
        void setGuardLevel (int level) noexcept           { guardLevel.store (level < 0 ? 0 : (level > 3 ? 3 : level), std::memory_order_relaxed); }
        int  getGuardLevel() const noexcept               { return guardLevel.load (std::memory_order_relaxed); }

        /** Notes whose latched oversampling was lowered by the guard since prepare (diagnostics / tests). */
        int  getGuardLoweredNotes() const noexcept        { return guardLowered.load (std::memory_order_relaxed); }

        // Diagnostics / tests
        VoiceInfo getVoiceInfo (int slot) const noexcept;
        int  getGuardKills() const noexcept       { return guardKills.load (std::memory_order_relaxed); }
        int  getLeakedRefs() const noexcept       { return leakedRefs.load (std::memory_order_relaxed); }
        int  getBacklogCount() const noexcept     { return backlogCount; }

    private:
        void handleMidi (const juce::MidiMessageMetadata& m) noexcept;
        void noteOn (int note, float velocity) noexcept;
        void noteOff (int note) noexcept;
        void setPedal (bool down) noexcept;
        void allNotesOff() noexcept;
        void allSoundOff() noexcept;

        void startPoly (int note, float velocity) noexcept;
        void startMono (int note, float velocity, bool legato, bool keysWereAlive) noexcept;
        void monoNoteRemoved (int removedNote) noexcept;
        void monoReevaluate() noexcept;

        WarprVoice* startNewVoice (int note, float velocity) noexcept;
        WarprVoice* findIdleSlot() noexcept;
        WarprVoice* reclaimGhost() noexcept;
        WarprVoice* pickVictim() noexcept;
        WarprVoice* findMonoVoice() noexcept;
        void makeGhost (WarprVoice& v, int fadeSamples) noexcept;
        int  ghostFadeSamples() const noexcept;

        void pushNote (int note) noexcept;
        void removeNote (int note) noexcept;
        bool inStack (int note) const noexcept;

        void renderRange (float* outL, float* outR, int n) noexcept;
        void harvestFinishedVoices() noexcept;
        void applyModeAndPolyphonyChanges() noexcept;
        void publishPlayheads() noexcept;

        void deferRelease (SampleData* s) noexcept;
        void flushBacklog() noexcept;
        void syncShaperToVoices() noexcept;

        // ---- D8 modulation
        const ModContext* beginChunkModulation (int m) noexcept;     // advances the global LFOs, returns the voices' context
        void endChunkModulation() noexcept;                          // voice means, master-gain delta
        void publishLfoPhases() noexcept;
        float latchedValue (ParamId p, float base) const noexcept;   // class N (and ampAttack): sampled at note-on
        float attackNow() const noexcept   { return latchedValue (ParamId::ampAttack, vp.attackMs); }
        double glideNow() const noexcept   { return (double) latchedValue (ParamId::glideTime, vp.glideMs); }

        RetireList& retireList;
        ShaperEngine* shaper = nullptr;
        ModEngine* modEngine = nullptr;

        TransportInfo transportInfo;
        TransportClock clock;
        int blockPos = 0;                            // base samples of the current block already rendered
        std::array<LfoBlockParams, (size_t) kNumLfos> lfoParams {};
        std::array<const LfoShapeTable*, (size_t) kNumLfos> tablePtrs {};
        const ModMatrixSnapshot* matrixPtr = nullptr;
        const SliceMap* slicePtr = nullptr;          // D11: this block's published slice map (nullptr = none)
        bool sliceNoteActive = false;                // set by noteOn() around the voice start of a slice note
        float sliceNoteLo = 0.0f, sliceNoteHi = 1.0f;
        int lfoCountBlock = 0;
        std::uint32_t globalOwnMask = 0, voiceOwnMask = 0;
        std::array<LfoInstance, (size_t) kNumLfos> gInst {};
        std::array<float, (size_t) kNumLfos> gPrev {}, gStart {}, gEnd {}, voiceMean {};
        ModContext modCtx;
        bool monoResetPending = false;
        float masterDelta = 0.0f;
        std::array<std::atomic<float>, (size_t) kNumLfos> lfoPhaseOut {};
        double sampleRate = 44100.0;
        const HalfbandCoeffs* halfband = nullptr;

        std::array<WarprVoice, (size_t) totalSlots> voices {};
        VoiceParams vp;
        SampleData* currentSample = nullptr;

        int voiceMode = 0, lastVoiceMode = 0;        // 0 Poly, 1 Mono, 2 Legato
        int polyLimit = 16, unisonLatched = 1, osLatched = 2;
        int lastPolyLimit = -1;                      // live polyphony reduction runs when the parameter changes
        float bendNorm = 0.0f;
        float modWheel = 0.0f;                       // stored; the contract has no destination for it (A2)
        bool pedalDown = false;

        std::array<unsigned char, 128> stack {};     // alive notes in press order (Mono / Legato priority)
        int stackCount = 0;
        std::array<bool, 128> sustained {};

        std::uint64_t noteCounter = 0;
        double lastPlayedPitch = -1.0;

        std::array<SampleData*, 128> backlog {};     // references waiting for retire-list space
        int backlogCount = 0;

        std::array<std::atomic<float>, (size_t) maxVoices> playheadPos {}, playheadLevel {};
        std::atomic<int> playheadCount { 0 };
        std::array<std::atomic<float>, (size_t) maxVizGrains> grainPos {}, grainLevel {};
        std::atomic<int> grainCount { 0 };

        std::atomic<int> oscBudget { 256 };
        std::atomic<int> interpQuality { 0 };
        std::atomic<int> guardLevel { 0 };
        std::atomic<int> guardLowered { 0 };
        std::atomic<int> guardKills { 0 };
        std::atomic<int> leakedRefs { 0 };
    };
}
