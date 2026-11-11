#pragma once
/*
  WarprVoice - one note: N unison oscillators -> sum -> ONE decimator -> DC block -> amp ADSR
  (architecture C12, "Per-voice" chain).  Phase D2 adds the warp stage inside UnisonOsc (modes 0-14
  real, 15-59 explicit identity); there is still no shaper (D4) or filter stage (D5): the per-voice
  filter slot (between the DC blocker and the envelope) does not exist yet.

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

        // Warp stage (D2).  Defaults mirror the parameter defaults (Sine Fold = not-yet-implemented
        // identity, amount 0).
        bool  warpEnable = true;
        int   warpMode = 17;
        float warpAmount = 0.0f, warpA = 0.5f, warpB = 0.0f, warpMix = 1.0f;
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

        /** Read position of the first oscillator as a fraction (0..1) of the sample, for the UI
            playhead.  Audio thread (or any thread that owns the voice); no side effects. */
        float playheadNorm() const noexcept
        {
            if (sample == nullptr || sample->numFrames <= 0)
                return 0.0f;
            const double t = osc[0].pos / (double) sample->numFrames;
            return t < 0.0 ? 0.0f : (t > 1.0 ? 1.0f : (float) t);
        }

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

        // warp stage smoothers (per chunk): amount / A / B / mix one-pole 10 ms, enable linear ramp 5 ms,
        // root note slew 5 ms (drives the warp cycle length Lc)
        float warpAmtCur = 0.0f, warpACur = 0.5f, warpBCur = 0.0f, warpMixCur = 1.0f, warpEnCur = 1.0f;
        float rootCur = 60.0f;

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
