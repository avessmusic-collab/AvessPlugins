#pragma once
/*
  WarprVoice - one note: N unison oscillators -> sum -> ONE decimator -> DC block -> amp ADSR
  (architecture C12, "Per-voice" chain).  Phase D2 adds the warp stage inside UnisonOsc (modes 0-43
  real after D3, 44-59 explicit identity); D4 adds the shaper stage right after it.  D5a adds the filter
  stage between the DC blocker and the envelope (types 0-19; see FilterStage.h).

  Everything is fixed-size: a voice owns its OS-rate bus (32 base samples x 8x), its oscillators
  and its decimator state.  Nothing is allocated after construction.

  Pitch model (semitones):  p = glide(note) + bend + (transpose + fine/100 - rootNote) + detune_i/100
                            rho_i = 2^(p/12) * fileSampleRate / hostSampleRate   (source frames / host sample)
  Glide is a constant-time linear ramp in the semitone domain; bend (3 ms) and the transpose / fine /
  root offset (5 ms) are one-pole slewed.  The per-OS-sample step ramps linearly across each 32-sample
  chunk, so glides and bends are smooth rather than stair-stepped.
*/

#include "DSP/AmpEnvelope.h"
#include "DSP/FilterStage.h"
#include "DSP/HalfbandIIR.h"
#include "DSP/LfoEngine.h"
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
        int   modOscSource = 4;              // D6: 0 Sine, 1 Triangle, 2 Saw, 3 Square, 4 Sample (parameter default)
        float modOscRatio = 1.0f;            // D6: modulator frequency / note frequency

        // Shaper stage (D4).  The set pointers are owned by the ShaperEngine (valid for the whole block);
        // `shaperFadeBase` = base samples already elapsed in the running A/B-set crossfade at the START of the
        // sub-range being rendered (WarprSynth keeps it current).
        bool  shaperEnable = false;
        float shaperMorph = 0.0f, shaperMix = 1.0f;
        const ShaperLutSet* shaperCur = nullptr;
        const ShaperLutSet* shaperPrev = nullptr;        // nullptr when no crossfade is running
        int   shaperFadeBase = 0, shaperFadeLen = 480;

        // Filter stage (D5a).  Defaults mirror the parameter defaults (LP 24, cutoff fully open = transparent).
        bool  filterEnable = true;
        int   filterType = 3;
        float filterCutoffHz = 20000.0f, filterRes = 0.0f, filterDriveDb = 0.0f, filterMorph = 0.0f;
        float filterKeyTrack = 0.0f, filterMix = 1.0f;

        // Granular source (D7a).  playMode 1 = Granular; defaults mirror the parameter defaults.
        int   playMode = 0;
        int   grainMode = 0;                 // 0 Classic, 1 Loop, 2 Cloud
        float grainPosition = 0.0f, grainScan = 0.0f, grainScanDist = 0.0f;
        bool  grainScanLoop = true, grainScanKey = false;
        bool  grainPosKey = false;           // POS-KEY mode: notes pick the grain position, unpitched (non-param state)
        float grainSizeMs = 80.0f, grainShape = 0.5f, grainVariation = 0.0f, grainSpread = 0.0f, grainReverse = 0.0f;
        int   grainDensity = 8;
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
            int oscCostQ = 4;                    // budget cost of ONE oscillator in quarter-units (latched, D7b)
            double glideFromSemis = 60.0;
            double glideMs = 0.0;
            std::uint64_t age = 0;               // note counter (stealing order)
            std::uint64_t startStamp = 0;        // synth sample clock at note-on (one-shot choke window)

            // D11 SLICE MODE: a slice voice plays only [sliceLo, sliceHi) (sample fractions, latched for the life of the note),
            // unpitched (always at the root pitch, whatever `note` is), as a one-shot trigger, with Classic playback.
            bool  slice = false;
            float sliceLo = 0.0f, sliceHi = 1.0f;
        };

        //======================================================================
        void prepare (double newSampleRate, const HalfbandCoeffs& coeffs) noexcept;

        /** Forgets all state.  Does NOT touch the sample reference (see takeSample()). */
        void reset() noexcept;

        /** `ownedSample` already carries ONE reference for this voice.  `mc` (D8, optional): the modulation context at the
            note start - the voice-owned LFOs are reset to their start phase and the latched destination (ampAttack) is
            evaluated from it. */
        void start (const StartInfo& info, const VoiceParams& vp, SampleData* ownedSample, const ModContext* mc = nullptr) noexcept;

        /** Adds `numSamples` of audio to outL / outR (any length; chunked to 32 internally).  No modulation. */
        void render (const VoiceParams& vp, float* outL, float* outR, int numSamples) noexcept;

        /** D8: renders ONE chunk (n <= 32 base samples) with the modulation of `mc` applied (nullptr = none).
            `rangeOffset` = base samples between the start of the current render range and this chunk. */
        void renderModulated (const VoiceParams& vp, float* outL, float* outR, int n, int rangeOffset, const ModContext* mc) noexcept
        {
            renderChunk (vp, outL, outR, n, rangeOffset, mc);
        }

        /** D8: a Mono retrigger is a new note for the voice-owned (RETRIG / ENV) LFOs. */
        void restartLfos() noexcept
        {
            for (auto& l : lfoInst)
                l.restart();
        }

        /** D8 diagnostics / viz: direction-mapped shape phase, and the output value (0..1) at the end of the last chunk. */
        float lfoPhase (int lfo) const noexcept      { return lfoInst[(size_t) lfo].phase; }
        float lfoEndValue (int lfo) const noexcept   { return lfoPrev[(size_t) lfo]; }
        double lfoProgress (int lfo) const noexcept  { return lfoInst[(size_t) lfo].progress; }

        //======================================================================
        // Control (audio thread)
        void release() noexcept;                              // normal note-off

        /** Key-up / pedal-up release.  A one-shot TRIGGER voice (loopMode One-Shot + Classic playback, latched at note-on)
            ignores it: one tap plays the whole region and the voice auto-releases when the read head reaches the region end.
            Panic paths (CC123, ghosts, kill) use release() / beginFade() / kill() and are unaffected. */
        void releaseKey() noexcept
        {
            if (! triggerLatched)
                release();
        }

        bool isTriggerLatched() const noexcept { return triggerLatched; }
        std::uint64_t startedAt() const noexcept { return startStamp; }     // synth sample clock at note-on

        /** D11: the voice was started by a slice note (region latched, unpitched, one-shot, Classic).  Always trigger-latched. */
        bool isSliceVoice() const noexcept { return sliceLatched; }

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
        int   getOscCostQ() const noexcept { return oscCostQ; }      // quarter-units per oscillator, charged at note-on
        double getPitchSemis() const noexcept { return pitchCur; }
        int   fadeSamplesLeft() const noexcept { return fadeLeft; }

        /** Read position of the first oscillator as a fraction (0..1) of the sample, for the UI
            playhead.  Audio thread (or any thread that owns the voice); no side effects. */
        float playheadNorm() const noexcept
        {
            if (sample == nullptr || sample->numFrames <= 0)
                return 0.0f;

            double frames = osc[0].pos;

            // Granular: the read position of the most recently spawned grain (left head), architecture "voices[]"
            if (granWCur >= 0.5f && osc[0].grain.isRunning())
                osc[0].grain.newestPosition (frames);

            const double t = frames / (double) sample->numFrames;
            return t < 0.0 ? 0.0f : (t > 1.0 ? 1.0f : (float) t);
        }

        /** Viz feed: position (0..1 of the sample) and window level of every sounding grain of oscillator 0.
            Audio thread; 0 when the voice is not granular. */
        int collectGrains (float* posOut, float* levelOut, int maxOut) const noexcept
        {
            if (sample == nullptr || ! isSounding() || granWCur <= 0.0f || ! osc[0].grain.isRunning())
                return 0;

            return osc[0].grain.collect (posOut, levelOut, maxOut, *sample);
        }

        /** Granular share of the source (playMode crossfade state), 0..1. */
        float granularShare() const noexcept { return granWCur; }

        /** Diagnostics / tests: the grain engine of unison oscillator `index`. */
        const GrainEngine& grainEngine (int index) const noexcept { return osc[static_cast<size_t> (index)].grain; }

        // state owned by the synth's allocation logic
        int  note = -1;
        std::uint64_t age = 0;
        bool keyHeld = false;
        bool pedalHeld = false;
        bool ghost = false;

    private:
        /** `rangeOffset` = base samples between the start of the current render range and this chunk (shaper crossfade). */
        void renderChunk (const VoiceParams& vp, float* outL, float* outR, int n, int rangeOffset, const ModContext* mc) noexcept;
        void setGlideTarget (double target, double glideMs) noexcept;
        RegionFold::Mode regionMode (const VoiceParams& vp) const noexcept;
        void advanceGlide (int n) noexcept;

        // ---- D8 modulation.  mod0 / mod1 apply the matrix delta at the START / END of the chunk in the parameter's own
        //      normalised domain (no-op while modActive is false or the delta is exactly 0).
        void beginModulation (const ModContext* mc) noexcept;
        void startModulation (const ModContext& mc) noexcept;
        float mod0 (ParamId p, float v) const noexcept { return modActive ? applyModulation (p, v, modD0[toIndex (p)]) : v; }
        float mod1 (ParamId p, float v) const noexcept { return modActive ? applyModulation (p, v, modD1[toIndex (p)]) : v; }
        FilterTargets makeFilterTargets (const VoiceParams& vp, double pitchSemis) const noexcept;

        double sampleRate = 44100.0;
        const HalfbandCoeffs* hb = nullptr;
        SampleData* sample = nullptr;                // owns one reference

        bool active = false;
        bool oneShotReleased = false;
        std::uint64_t startStamp = 0;                // synth sample clock at note-on (one-shot choke window)
        float posKeyFrac = -1.0f;                    // POS-KEY: latched note->position (0..1 over C1..C6), -1 = off
        bool triggerLatched = false;                 // one-shot trigger voice (latched at note-on, see releaseKey())
        bool sliceLatched = false;                   // D11 slice voice (implies triggerLatched)
        bool killedByGuard = false;
        float velGain = 1.0f;
        int os = 2, unison = 1;
        int oscCostQ = 4;

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
        float modRatioCur = 1.0f;                    // modOscRatio, log-domain one-pole 15 ms (D6)

        // shaper stage smoothers (per chunk): morph / mix one-pole 10 ms, enable linear ramp 5 ms
        float shaperMorphCur = 0.0f, shaperMixCur = 1.0f, shaperEnCur = 0.0f;

        // granular source (D7a): playMode weight (linear 10 ms ramp), scan / scan-dist one-pole 20 ms per chunk
        float granWCur = 0.0f, scanCur = 0.0f, scanDistCur = 0.0f;
        GrainScratch grainScratch;                   // staging shared by this voice's oscillators (rendered one after another)

        // D8: voice-owned LFO instances (RETRIG / ENV) and this chunk's per-parameter deltas (indexed by ParamId)
        std::array<LfoInstance, (size_t) kNumLfos> lfoInst {};
        std::array<float, (size_t) kNumLfos> lfoPrev {}, lfoStart {}, lfoEnd {};
        std::array<float, kNumParams> modD0 {}, modD1 {};
        bool modActive = false;

        // fades
        int fadeLeft = 0, fadeTotal = 0;

        RegionFold region;
        std::array<UnisonOsc, (size_t) maxUnison> osc {};
        std::array<float, (size_t) maxUnison> prevGainL {}, prevGainR {};

        OsDecimator decimator;
        float dcxL = 0.0f, dcxR = 0.0f, dcyL = 0.0f, dcyR = 0.0f;
        float dcCoeff = 0.9993f;

        FilterStage filter;                          // D5a: after the DC blocker, before the envelope

        AmpEnvelope env;

        std::array<float, (size_t) (chunkSize * maxOs)> busL {}, busR {};
        std::array<float, (size_t) chunkSize> envBuf {};
    };
}
