#pragma once
/*
  WarprVoice - one note: N unison oscillators -> sum -> ONE decimator -> DC block -> amp ADSR
  (architecture C12, "Per-voice" chain).  Phase D1 has no warp, shaper or filter stage; the
  warp / shaper slot inside UnisonOsc is an explicit identity and the per-voice filter slot
  (between the DC blocker and the envelope) does not exist yet (D5).

  Everything is fixed-size: a voice owns its OS-rate bus (32 base samples x 8x), its oscillators
  and its decimator state.  Nothing is allocated after construction.

  Pitch model (semitones):  p = glide(note) + bend + (transpose + fine/100 - rootNote) + detune_i/100
                            rho_i = 2^(p/12) * fileSampleRate / hostSampleRate   (source frames / host sample)
  Glide is a constant-time linear ramp in the semitone domain; bend (3 ms) and the transpose / fine /
  root offset (5 ms) are one-pole slewed.  The per-OS-sample step ramps linearly across each 32-sample
  chunk, so glides and bends are smooth rather than stair-stepped.
*/

#include "DSP/AmpEnvelope.h"
#include "DSP/HalfbandIIR.h"
#include "DSP/RegionFold.h"
#include "DSP/SampleData.h"
#include "DSP/UnisonOsc.h"

#include <array>
#include <cstdint>

namespace warpr
{
    /** Per-block parameter values the voices read (filled by WarprSynth from the ParamBlock). */
    struct VoiceParams
    {
        float sampleStart = 0.0f, sampleEnd = 1.0f;
        int   rootNote = 60, transpose = 0;
        float fineTuneCents = 0.0f;
        int   loopMode = 0;                  // 0 Forward Loop, 1 Ping-Pong, 2 One-Shot
        int   direction = 0;                 // 0 Forward, 1 Reverse
        float attackMs = 4.0f, decayMs = 180.0f, sustain = 0.7f, releaseMs = 240.0f;
        float unisonDetuneCents = 12.0f, unisonSpread = 0.5f;
        float glideMs = 0.0f;
        float bendSemis = 0.0f;              // wheel position * range (kept current by the synth)
        int   bendRangeSemis = 2;
        int   interpQuality = 0;             // 0 sinc8, 1 Hermite4 (draft)
    };

    class WarprVoice
    {
    public:
        static constexpr int chunkSize  = 32;
        static constexpr int maxOs      = 8;
        static constexpr int maxUnison  = 8;
        static constexpr double maxRho  = 256.0;     // above this the oscillator mutes (no garbage)

        struct StartInfo
        {
            int note = 60;
            float velocity = 1.0f;               // 0..1
            int os = 2;                          // 1 / 2 / 4 / 8, latched for the life of the note
            int unison = 1;                      // 1..8, latched
            double glideFromSemis = 60.0;
            double glideMs = 0.0;
            std::uint64_t age = 0;               // note counter (stealing order)
        };

        //======================================================================
        void prepare (double newSampleRate, const HalfbandCoeffs& coeffs) noexcept;

        /** Forgets all state.  Does NOT touch the sample reference (see takeSample()). */
        void reset() noexcept;

        /** `ownedSample` already carries ONE reference for this voice. */
        void start (const StartInfo& info, const VoiceParams& vp, SampleData* ownedSample) noexcept;

        /** Adds `numSamples` of audio to outL / outR (any length; chunked to 32 internally). */
        void render (const VoiceParams& vp, float* outL, float* outR, int numSamples) noexcept;

        //======================================================================
        // Control (audio thread)
        void release() noexcept;                              // normal note-off
        void beginFade (int samples) noexcept;                // ghost fade-out (steal, mode change, sound-off)
        void retarget (int newNote, double glideMs) noexcept; // mono / legato pitch change, no restart
        void retriggerEnvelope (float attackMs) noexcept;     // attack again from the current level
        void setVelocity (float velocity) noexcept;
        void kill() noexcept;                                 // immediate, silent

        /** Hands back the voice's sample reference (the caller must release it off the audio thread's
            critical path - see WarprSynth::deferRelease).  nullptr if none. */
        SampleData* takeSample() noexcept
        {
            auto* s = sample;
            sample = nullptr;
            return s;
        }

        bool consumeGuardKill() noexcept
        {
            const bool k = killedByGuard;
            killedByGuard = false;
            return k;
        }

        //======================================================================
        bool isActive() const noexcept   { return active; }
        bool isSounding() const noexcept { return active && ! ghost && env.isActive(); }
        AmpEnvelope::State envState() const noexcept { return env.getState(); }
        float envLevel() const noexcept  { return env.getLevel(); }
        int   getUnison() const noexcept { return unison; }
        int   getOs() const noexcept     { return os; }
        double getPitchSemis() const noexcept { return pitchCur; }
        int   fadeSamplesLeft() const noexcept { return fadeLeft; }

        // state owned by the synth's allocation logic
        int  note = -1;
        std::uint64_t age = 0;
        bool keyHeld = false;
        bool pedalHeld = false;
        bool ghost = false;

    private:
        void renderChunk (const VoiceParams& vp, float* outL, float* outR, int n) noexcept;
        void setGlideTarget (double target, double glideMs) noexcept;
        void advanceGlide (int n) noexcept;

        double sampleRate = 44100.0;
        const HalfbandCoeffs* hb = nullptr;
        SampleData* sample = nullptr;                // owns one reference

        bool active = false;
        bool oneShotReleased = false;
        bool killedByGuard = false;
        float velGain = 1.0f;
        int os = 2, unison = 1;

        // pitch
        double pitchCur = 60.0, pitchTarget = 60.0, glideRate = 0.0;
        int glideLeft = 0;
        float bendCur = 0.0f, offsetCur = 0.0f;
        float spreadCur = 0.5f, detuneCur = 12.0f;
        float startCur = 0.0f, endCur = 1.0f;

        // fades
        int fadeLeft = 0, fadeTotal = 0;

        RegionFold region;
        std::array<UnisonOsc, (size_t) maxUnison> osc {};
        std::array<float, (size_t) maxUnison> prevGainL {}, prevGainR {};

        OsDecimator decimator;
        float dcxL = 0.0f, dcxR = 0.0f, dcyL = 0.0f, dcyR = 0.0f;
        float dcCoeff = 0.9993f;

        AmpEnvelope env;

        std::array<float, (size_t) (chunkSize * maxOs)> busL {}, busR {};
        std::array<float, (size_t) chunkSize> envBuf {};
    };
}
