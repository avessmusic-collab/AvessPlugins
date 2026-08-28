#pragma once

#include <array>
#include <atomic>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>

#include "DSP/OversamplingProcessor.h"
#include "DSP/KickVoice.h"
#include "DSP/TransientShaper.h"
#include "Utilities/DSPUtils.h"

namespace kickr
{
    /**
        Synthesis orchestration (architecture.md -> KickEngine).

        PHASE 2.1 scope:
          - Establishes the oversampling region as the substrate (AD-10):
            processSamplesUp(zero block) -> whole-voice render @ fsOversampled ->
            processSamplesDown -> base-rate DC blocker + analyzer-tap hook.
          - `oversampling` present but pinned to 1x (fsOversampled == fs).
          - Reads the Phase-2.1 params once per block, iterates the MidiBuffer with a
            sample-accurate sub-block split at each note-on, resolves pitch
            (MIDI-Pitch / Fixed-Frequency + tune/fineTune + AD-6 global offset),
            applies velocity -> level, renders a single monophonic KickVoice.

        PHASE 2.2: pitch envelope live — `pitchStart` / `pitchTime` / `pitchCurve` snapshot
        per block, velocity-scaled effective `pitchStart` fed to `KickVoice::setPitchParams`.

        PHASE 2.3: owns `std::array<KickVoice, 2>` + `activeVoice`. A note-on while the
        active voice is still ringing triggers the OTHER voice and starts a
        `kRetriggerFadeMs = 3 ms` equal-power crossfade (both voices render, phase-
        continuous; the old voice is `reset()` only once theta reaches 1). A third note
        mid-fade snaps the fade to done first, so never more than 2 voices are live.
        `TransientShaper` runs on the summed mono signal right after the voice mix,
        still inside the OS region (AD-10). Params live: `transientAttack`, `transientSustain`.

        PHASE 2.4: `clickLevel` / `clickTone` / `clickTime` / `clickPitch` snapshot per
        block and forwarded to every voice's `ClickGenerator` via `setClickParams`. The
        moderate click-velocity factor (`lerp(1, v01, velSensitivity·0.6)`) is passed into
        `KickVoice::noteOn`. The click sums with the body inside the voice (before the
        voice mix / TransientShaper), so no engine-side routing change.

        PHASE 2.5: `subLevel` / `subFreq` / `subDecay` snapshot per block and forwarded to
        every voice's `SubOscillator` via `setSubParams`. The sub is an independent mono
        sine at a fixed `subFreq` (pitch-envelope independent), phase-0 on trigger, own exp
        AD envelope; it sums with body + click inside the voice (before the voice mix /
        TransientShaper) and carries `subLevel` internally, so no engine-side routing
        change. Not velocity-scaled in v1.

        Later phases add: tail/noise (2.6-2.7), sample player (2.7b), distortion
        (2.8), tone/stereo/limiter (2.9), real OS switching (2.10), smoothing/macros
        (2.11), analyzer.
    */
    class KickEngine
    {
    public:
        explicit KickEngine (juce::AudioProcessorValueTreeState& stateToUse) noexcept
            : apvts (stateToUse) {}

        void prepare (double sampleRate, int maximumBlockSize);
        void reset();

        void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi);

        /** round(activeOs->getLatencyInSamples()); 0 at 1x (Phase 2.1). */
        int getLatencySamples() const noexcept { return oversampling.getLatencySamples(); }

    private:
        static constexpr double kRetriggerFadeMs = 3.0;   // equal-power crossfade length

        void renderSegment (juce::AudioBuffer<float>& buffer, int startSample, int numSamples);
        void handleNoteOn (const juce::MidiMessage& message);
        void triggerVoice (KickVoice& v, float freqHz, int note,
                           float velLevelGain, float velClickGain, float v01) noexcept;
        float resolvePitchHz (int noteNumber) const noexcept;

        /** architecture Parameter Mapping row 2 — velocity (subtle) scaling of `pitchStart`. */
        float effectivePitchStartRatio (float v01) const noexcept;

        juce::AudioProcessorValueTreeState& apvts;

        double baseSampleRate { 44100.0 };
        double fsOversampled  { 44100.0 };
        int    maxBlockSize   { 512 };

        OversamplingProcessor oversampling;

        // PHASE 2.3: two voices + click-free equal-power retrigger crossfade.
        std::array<KickVoice, 2> voices;
        int    activeVoice       { 0 };
        int    incomingVoice     { 1 };
        bool   fadeActive        { false };
        double theta             { 0.0 };   // crossfade position 0 -> 1
        double retriggerThetaInc { 0.0 };   // 1 / (kRetriggerFadeMs * fsOversampled)

        TransientShaper transientShaper;

        juce::AudioBuffer<float>             scratch;        // stereo, base-rate, pre-sized
        std::array<juce::AudioBuffer<float>, 2> voiceScratch; // mono, oversampled-rate, pre-sized
        std::array<dsputils::DCBlocker, 2>   dcBlockers;

        // Cached raw APVTS pointers (atomic reads on the audio thread).
        std::atomic<float>* pFundamental    { nullptr };
        std::atomic<float>* pPitchStart     { nullptr };
        std::atomic<float>* pPitchTime      { nullptr };
        std::atomic<float>* pPitchCurve     { nullptr };
        std::atomic<float>* pBodyLevel      { nullptr };
        std::atomic<float>* pBodyDecay      { nullptr };
        std::atomic<float>* pTuneMode       { nullptr };
        std::atomic<float>* pTune           { nullptr };
        std::atomic<float>* pFineTune       { nullptr };
        std::atomic<float>* pVelSensitivity { nullptr };
        std::atomic<float>* pOversampling   { nullptr };
        std::atomic<float>* pTransientAttack  { nullptr };
        std::atomic<float>* pTransientSustain { nullptr };
        std::atomic<float>* pClickLevel { nullptr };   // PHASE 2.4
        std::atomic<float>* pClickTone  { nullptr };
        std::atomic<float>* pClickTime  { nullptr };
        std::atomic<float>* pClickPitch { nullptr };
        std::atomic<float>* pSubLevel { nullptr };     // PHASE 2.5
        std::atomic<float>* pSubFreq  { nullptr };
        std::atomic<float>* pSubDecay { nullptr };

        // Per-block parameter snapshot.
        struct Snapshot
        {
            float fundamental     { 55.0f };
            float pitchStartRatio { 4.0f };
            float pitchTimeMs     { 50.0f };
            float pitchCurve      { 0.7f };
            float bodyLevel       { 1.0f };
            float bodyDecayMs     { 400.0f };
            float tune            { 0.0f };
            float fineTune        { 0.0f };
            float velSens         { 0.5f };
            float transientAttack { 0.0f };  // bipolar -1..+1
            float transientSustain { 0.0f }; // bipolar -1..+1
            float clickLevel   { 0.4f };     // PHASE 2.4
            float clickToneHz  { 4000.0f };
            float clickTimeMs  { 3.0f };
            float clickPitchHz { 5000.0f };
            float subLevel     { 0.5f };     // PHASE 2.5
            float subFreqHz    { 40.0f };
            float subDecayMs   { 300.0f };
            int   tuneMode        { 0 };     // 0 = MIDI Pitch, 1 = Fixed Frequency
        };
        Snapshot snap;

        // Velocity of the most recent trigger — folded into the effective `pitchStart`
        // each block so pitchStart/pitchTime/pitchCurve automation stays live mid-voice.
        float lastVel01 { 1.0f };

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (KickEngine)
    };
}
