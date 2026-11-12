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
  Legato   : as Mono but overlapping notes do not retrigger the envelope (glide only).
  Pedal    : CC64 >= 64 holds released notes; releasing it releases them (Poly: per voice flag,
             Mono / Legato: through the note stack).  CC123 releases everything, CC120 fades it in 3 ms.
  Pitch bend: wheel -> [-1, 1] * bendRange (2 / 12 / 24 st); the voices slew it over 3 ms.
  Velocity : fixed law 0.25 + 0.75 * v (no parameter exists, ambiguity A2).

  Oscillator-budget governor (skeleton, default 256 OS-weighted units; 0 disables): a new note first
  reduces ITS OWN unison count (never below 1), then steals other voices.  It never changes the
  unison / OS of a sounding voice.  Cost of an oscillator = os (Classic playback = 1 unit x OS).

  Sample references: each voice owns one explicit reference to the SampleData it plays and returns it
  through the RetireList (deferRelease) when it ends, so the final delete is never on the audio thread.
*/

#include "DSP/HalfbandIIR.h"
#include "DSP/SampleStore.h"
#include "DSP/ShaperEngine.h"
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

        /** Kills every voice and releases all sample references directly (audio must be stopped). */
        void reset();

        //======================================================================
        // AUDIO thread

        /** The sample new notes will use (already acquired from the SampleStore). */
        void setCurrentSample (SampleData* s) noexcept { currentSample = s; }

        /** Reads the block's parameter values (once per block). */
        void setBlockParams (const ParamBlock& pb) noexcept;

        /** ADDS the synth output to outL / outR (callers clear them first). */
        void renderBlock (float* outL, float* outR, int numSamples, const juce::MidiBuffer& midi) noexcept;

        int getNumActiveVoices() const noexcept;         // includes ghosts
        int getNumNonGhostVoices() const noexcept;
        int getOscillatorUnitsInUse() const noexcept;    // non-ghost voices: unison * os

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
        void setInterpolationQuality (int quality) noexcept { interpQuality.store (quality, std::memory_order_relaxed); }
        int  getOscillatorBudget() const noexcept         { return oscBudget.load (std::memory_order_relaxed); }

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

        RetireList& retireList;
        ShaperEngine* shaper = nullptr;
        double sampleRate = 44100.0;
        const HalfbandCoeffs* halfband = nullptr;

        std::array<WarprVoice, (size_t) totalSlots> voices {};
        VoiceParams vp;
        SampleData* currentSample = nullptr;

        int voiceMode = 0, lastVoiceMode = 0;        // 0 Poly, 1 Mono, 2 Legato
        int polyLimit = 16, unisonLatched = 1, osLatched = 2;
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
        std::atomic<int> guardKills { 0 };
        std::atomic<int> leakedRefs { 0 };
    };
}
