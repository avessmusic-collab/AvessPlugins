#pragma once

#include <array>
#include <atomic>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>

#include "DSP/OversamplingProcessor.h"
#include "DSP/KickVoice.h"
#include "DSP/SamplePlayer.h"
#include "DSP/TransientShaper.h"
#include "DSP/Waveshaper.h"
#include "DSP/OutputStage.h"
#include "Sampling/SampleBuffer.h"
#include "DSP/MasterFilter.h"
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
        active voice is still ringing triggers the OTHER voice immediately at full level
        (no fade-in — new note takes priority instantly) while the old voice is force-
        silenced via a `kDeclickFadeMs = 0.75 ms` fade-out, then unconditionally
        `reset()` the instant that fade completes — regardless of whether any of its
        layers (body/click/sub/tail/noise/sample) would otherwise still be ringing. This
        is a hard, strictly-monophonic voice steal: there is never more than ~0.75 ms of
        genuine overlap between two notes, and no layer is ever allowed to linger past
        that (2026-09-01 user request — previously a 3 ms EQUAL-POWER crossfade let the
        outgoing voice's sample tail continue ringing indefinitely in the background as a
        "retiring voice"; see CHANGELOG). A third note mid-fade snaps the fade to done
        first, so never more than 2 voices are live. `TransientShaper` runs on the summed
        mono signal right after the voice mix, still inside the OS region (AD-10). Params
        live: `transientAttack`, `transientSustain`.

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

        PHASE 2.6: `tailLevel` / `tailLength` / `tailTone` / `tailDrive` snapshot per block
        and forwarded to every voice's `TailGenerator` via `setTailParams`. The tail is a
        dedicated LF sine locked to the resolved `fundamentalEff` (the same `freqHz` the
        body gets in `triggerVoice`, before the pitch envelope) — slow ~10 ms attack, exp
        decay, LP + `tanh` drive, all in-region (AD-10). It sums with body + click + sub
        inside the voice (before the voice mix / TransientShaper) and carries `tailLevel`
        internally, so no engine-side routing change. Not velocity-scaled.

        PHASE 2.7: `noiseLevel` / `noiseDecay` / `noiseTone` / `noiseType` snapshot per block
        and forwarded to every voice's `NoiseGenerator` via `setNoiseParams`. Optional
        white/pink/filtered noise layer, DEFAULT OFF (`noiseLevel` default 0.0 => the layer
        no-ops on `noteOn` and adds exact zeros). `noiseType` is an `AudioParameterChoice`
        (index read via `getRawParameterValue`, same as `tuneMode`). It sums with body +
        click + sub + tail inside the voice (before the voice mix / TransientShaper) and
        carries `noiseLevel` internally, so no engine-side routing change. Not velocity-scaled.

        PHASE 2.7b: the 6th layer. The processor decodes the user sample on the message
        thread and publishes a `const SampleBuffer*` via an atomic pointer; it calls
        `setSampleBuffer()` before each `processBlock`. The engine caches the 14 SAMPLE
        APVTS pointers + `velSensitivity`, snapshots them per block, resolves the effective
        sample values (velocity folds into `sampleLevel` via `velFactor` and darkens
        `sampleLP`), and fans `KickVoice::setSampleParams(...)` out to both voices with the
        two 0/1 gate targets (`synthEnable`, `sampleEnable && buffer present`). The buffer
        pointer is passed into `KickVoice::noteOn` and captured there for the voice's life.
        `anyVoiceActive()` lets the processor retire unreferenced buffers on the message
        thread.

        PHASE 2.8: master morphing distortion. `Waveshaper waveshaper;` is an engine-level
        member (on the summed mono bus, like `transientShaper`) — runs right after the
        transient shaper, still monophonic, still inside the OS region (AD-10). `drive` /
        `character` / `driveMix` cached + snapshot per block; `waveshaper.setParams
        (driveEff, characterEff, driveMix)` per block (`driveEff = drive`,
        `characterEff = character` until Phase 2.11 folds in `macroCrush`). In the
        per-sample loop `shaped = waveshaper.processSample (transientShaper.processSample
        (mono))` before the write to both channels. `driveMix` default 1.0 + `drive`
        default 0.3 + `character` default 0.0 (pure tanh) => the distortion is ACTIVE by
        default; `driveMix = 0` is the pre-drive-gain clean / bit-transparent path.

        PHASE 2.9: tone / output / stereo. The voice mix is now STEREO. Per sample, in the
        OS region: `outputStage.processTone(L, R)` (3-band, PRE-distortion) ->
        `transientShaper.processStereo(L, R)` (stereo-linked gain) -> `waveshaperL/R`
        (independent per-channel morph) -> `outputStage.processOutput(L, R)` (LR2 130 Hz
        mono crossover + M/S width + equal-power mix-to-silence + `dbToGain(output)` +
        an in-region ~5 Hz DC blocker just before it, then the zero-latency `tanh` soft-clip
        limiter at -0.5 dBFS as the genuinely last stage). Then `processSamplesDown`; only a
        NaN/Inf guard + the analyzer tap are base-rate. `KickEngine` caches + snapshots `low` /
        `mid` / `high` / `bodyWidth` / `clickWidth` / `outputWidth` / `output` / `mix` /
        `limiter`; `bodyWidth` -> `KickVoice::setBodyWidth`, `clickWidth` ->
        `KickVoice::setClickParams`, the rest -> `outputStage.setParams`.

        Later phases add: real OS switching (2.10), smoothing/macros (2.11 — incl.
        `macroBody` -> `sampleLevel`, `macroCrush` -> `drive`/`character`), analyzer.
    */
    class KickEngine
    {
    public:
        explicit KickEngine (juce::AudioProcessorValueTreeState& stateToUse) noexcept
            : apvts (stateToUse) {}

        void prepare (double sampleRate, int maximumBlockSize);
        void reset();

        void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi);

        /** round(activeOs->getLatencyInSamples()); 0 at 1x. Reflects the ACTIVE factor —
            used by `prepareToPlay`. */
        int getLatencySamples() const noexcept { return oversampling.getLatencySamples(); }

        /** PHASE 2.10 — the latency the PENDING `oversampling` choice would report. Read
            lock-free by the message-thread APVTS listener that calls `setLatencySamples`. */
        int pendingOsLatencySamples() const noexcept { return oversampling.pendingLatencySamples(); }

        /** PHASE 2.7b — the processor publishes the decoded sample here (audio thread, one
            plain-pointer store per block; the buffer is owned + retired by the processor). */
        void setSampleBuffer (const SampleBuffer* b) noexcept { currentSampleBuf = b; }

        /** PHASE 2.7b — message thread asks this before retiring an old sample buffer. */
        bool anyVoiceActive() const noexcept
        {
            return voices[0].isActive() || voices[1].isActive();
        }

    private:
        static constexpr double kDeclickFadeMs     = 0.75;  // outgoing-voice fade-out length (hard steal, no crossfade)
        static constexpr int    kSwitchFadeSamples = 64;     // OS-factor-switch fade (base rate)

        // 2026-08-31 (user request): in MIDI Pitch mode, plain concert pitch (the
        // un-recentred formula) puts MIDI 60 / "C3" — Ableton's middle C, and a common
        // default note for drum-rack patterns — at 261.63 Hz: a treble tone, not a kick.
        // Recentre the whole note->Hz mapping 2 octaves down so C3 lands at a genuinely
        // low, usable kick pitch; every other note shifts the same 2 octaves, preserving
        // relative tracking. Fixed Frequency mode and `fundamental`'s own AD-6 offset are
        // untouched. Test note `a1` was bumped 33->57 to compensate (still => 55 Hz).
        static constexpr float kMidiPitchRecentreSemitones = -24.0f;

        /** PHASE 2.10 — fan `updateOversampledRate` out to every in-region component and
            recompute `declickThetaInc` for the new `fsOversampled`. Coefficient-only. */
        void updateInRegionRate (double newFsOversampled);

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

        // PHASE 2.3, redesigned 2026-09-01: two voices + a hard, strictly-monophonic
        // voice steal. `fadeActive` tracks a short kDeclickFadeMs fade-OUT applied only to
        // the outgoing (`activeVoice`) signal; the incoming voice always plays at full
        // level from sample 0, no fade-in — the new note takes priority immediately.
        std::array<KickVoice, 2> voices;
        int    activeVoice       { 0 };
        int    incomingVoice     { 1 };
        bool   fadeActive        { false };
        double theta             { 0.0 };   // declick-fade position 0 -> 1

        // 2026-09-01 (user request): previously, once the fade finished, the outgoing
        // voice was only reset() if it had already fallen silent on its own — otherwise it
        // was demoted to a "retiring voice" and kept ringing (unscaled) in the background
        // until its own tail/sample finished naturally, sometimes for seconds. That let two
        // notes be audible at once, which is exactly what strict mono voice-stealing must
        // never do. Now the outgoing voice is unconditionally reset() the instant theta
        // reaches 1 — every layer (body/click/sub/tail/noise/sample) is force-silenced,
        // no exceptions. `retiringVoice` no longer exists.
        double declickThetaInc   { 0.0 };   // 1 / (kDeclickFadeMs * fsOversampled)

        // PHASE 2.10 — OS-factor-switch fade. On a pending change: this block's output is
        // ramped to silence over the last kSwitchFadeSamples, the factor is swapped +
        // every in-region coefficient refreshed, then the NEXT `switchFadeInSamples`
        // samples ramp back 0 -> 1.
        int    switchFadeInSamples { 0 };

        TransientShaper transientShaper;         // stereo-linked since Phase 2.9
        MasterFilter    masterFilter;            // 2026-09-02 — master LP/HP after the crusher
        Waveshaper      waveshaperL;             // PHASE 2.8/2.9 — per-channel master morph
        Waveshaper      waveshaperR;
        OutputStage     outputStage;             // PHASE 2.9 — tone / crossover / width / mix / gain / limiter

        juce::AudioBuffer<float>             scratch;        // stereo, base-rate, pre-sized
        std::array<juce::AudioBuffer<float>, 2> voiceScratch; // stereo, oversampled-rate, pre-sized (per voice)

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
        std::atomic<float>* pDrive     { nullptr };   // PHASE 2.8
        std::atomic<float>* pCharacter { nullptr };
        std::atomic<float>* pDriveMix  { nullptr };
        std::atomic<float>* pLow         { nullptr };   // PHASE 2.9 — TONE
        std::atomic<float>* pMid         { nullptr };
        std::atomic<float>* pHigh        { nullptr };
        std::atomic<float>* pBodyWidth   { nullptr };   // PHASE 2.9 — STEREO
        std::atomic<float>* pMorph       { nullptr };   // 2026-09-01 — body waveform morph
        std::atomic<float>* pClickWidth  { nullptr };
        std::atomic<float>* pOutputWidth { nullptr };
        std::atomic<float>* pOutput      { nullptr };   // PHASE 2.9 — OUTPUT
        std::atomic<float>* pMix         { nullptr };
        std::atomic<float>* pLimiter     { nullptr };   // Bool
        std::atomic<float>* pLimLoudness   { nullptr }; // 2026-09-02 — Color Limiter
        std::atomic<float>* pLimCeiling    { nullptr };
        std::atomic<float>* pLimLookahead  { nullptr };
        std::atomic<float>* pLimRelease    { nullptr };
        std::atomic<float>* pLimSaturation { nullptr };
        std::atomic<float>* pLimColor      { nullptr };
        std::atomic<float>* pFilterOn      { nullptr };   // 2026-09-02 — master filter
        std::atomic<float>* pFilterType    { nullptr };
        std::atomic<float>* pFilterFreq    { nullptr };
        std::atomic<float>* pFilterRes     { nullptr };
        std::atomic<float>* pClickLevel { nullptr };   // PHASE 2.4
        std::atomic<float>* pClickTone  { nullptr };
        std::atomic<float>* pClickTime  { nullptr };
        std::atomic<float>* pClickPitch { nullptr };
        std::atomic<float>* pSubLevel { nullptr };     // PHASE 2.5
        std::atomic<float>* pSubFreq  { nullptr };
        std::atomic<float>* pSubDecay { nullptr };
        std::atomic<float>* pTailLevel  { nullptr };   // PHASE 2.6
        std::atomic<float>* pTailLength { nullptr };
        std::atomic<float>* pTailTone   { nullptr };
        std::atomic<float>* pTailDrive  { nullptr };
        std::atomic<float>* pNoiseLevel { nullptr };   // PHASE 2.7
        std::atomic<float>* pNoiseDecay { nullptr };
        std::atomic<float>* pNoiseTone  { nullptr };
        std::atomic<float>* pNoiseType  { nullptr };   // AudioParameterChoice — index

        std::atomic<float>* pSynthEnable     { nullptr };   // PHASE 2.7b (Bool)
        std::atomic<float>* pSampleEnable    { nullptr };   // Bool
        std::atomic<float>* pSampleLevel     { nullptr };
        std::atomic<float>* pSampleStart     { nullptr };
        std::atomic<float>* pSampleEnd       { nullptr };
        std::atomic<float>* pSampleReverse   { nullptr };   // Bool
        std::atomic<float>* pSampleTune      { nullptr };
        std::atomic<float>* pSampleFine      { nullptr };
        std::atomic<float>* pSampleMidiTrack { nullptr };   // Bool
        std::atomic<float>* pSampleAttack    { nullptr };
        std::atomic<float>* pSampleDecay     { nullptr };
        std::atomic<float>* pSampleHP        { nullptr };
        std::atomic<float>* pSampleLP        { nullptr };
        std::atomic<float>* pSampleCrush     { nullptr };

        std::atomic<float>* pMacroPunch { nullptr };   // PHASE 2.11 — 0.5 = neutral
        std::atomic<float>* pMacroBody  { nullptr };
        std::atomic<float>* pMacroCrush { nullptr };
        std::atomic<float>* pMacroTail  { nullptr };

        // PHASE 2.11 — one ~20 ms smoother per macro so a fast sweep is click-free.
        juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> macroPunchSm { 0.5f };
        juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> macroBodySm  { 0.5f };
        juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> macroCrushSm { 0.5f };
        juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> macroTailSm  { 0.5f };

        const SampleBuffer* currentSampleBuf { nullptr };   // set per block by the processor

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
            float drive01     { 0.3f };      // PHASE 2.8
            float character01 { 0.0f };
            float driveMix01  { 1.0f };
            float lowDb        { 0.0f };     // PHASE 2.9 — TONE (bipolar dB)
            float midDb        { 0.0f };
            float highDb       { 0.0f };
            float bodyWidth01  { 0.0f };     // PHASE 2.9 — STEREO
            float morph01      { 0.0f };     // 2026-09-01 — body waveform morph (0 = pure sine)
            float clickWidth01 { 0.3f };
            float outputWidth01 { 0.5f };
            float outputDb     { 0.0f };     // PHASE 2.9 — OUTPUT (bipolar dB, -24..+12)
            float mix01        { 1.0f };
            bool  limiterOn    { true };
            float limLoudnessDb   { 0.0f };    // 2026-09-02 — Color Limiter
            float limCeilingDb    { -0.5f };
            float limLookaheadMs  { 1.5f };
            float limReleaseMs    { 50.0f };
            float limSaturation01 { 0.0f };
            float limColor01      { 0.5f };
            bool  filterOn        { false };   // 2026-09-02 — master filter
            int   filterType      { 0 };
            float filterFreqHz    { 1000.0f };
            float filterRes01     { 0.2f };
            float clickLevel   { 0.4f };     // PHASE 2.4
            float clickToneHz  { 4000.0f };
            float clickTimeMs  { 3.0f };
            float clickPitchHz { 5000.0f };
            float subLevel     { 0.5f };     // PHASE 2.5
            float subFreqHz    { 40.0f };
            float subDecayMs   { 300.0f };
            float tailLevel    { 0.3f };     // PHASE 2.6
            float tailLengthMs { 200.0f };
            float tailTone01   { 0.5f };
            float tailDrive01  { 0.2f };
            float noiseLevel   { 0.0f };     // PHASE 2.7 — default OFF
            float noiseDecayMs { 60.0f };
            float noiseTone01  { 0.5f };
            int   noiseType    { 0 };        // 0 = White, 1 = Pink, 2 = Filtered
            int   tuneMode        { 0 };     // 0 = MIDI Pitch, 1 = Fixed Frequency

            // PHASE 2.7b — SAMPLE group (effective values resolved in processBlock).
            bool  synthEnable     { true };
            bool  sampleEnable    { false };
            float sampleLevel     { 0.7f };
            float sampleStart     { 0.0f };
            float sampleEnd       { 1.0f };
            bool  sampleReverse   { false };
            float sampleTune      { 0.0f };
            float sampleFine      { 0.0f };
            bool  sampleMidiTrack { true };
            float sampleAttackMs  { 0.0f };
            float sampleDecayMs   { 2200.0f };   // 2026-08-31: was 800 — see ParameterLayout.h note
            float sampleHPHz      { 20.0f };
            float sampleLPHz      { 20000.0f };
            float sampleCrush01   { 0.0f };
        };
        Snapshot snap;

        // Velocity of the most recent trigger — folded into the effective `pitchStart`
        // each block so pitchStart/pitchTime/pitchCurve automation stays live mid-voice.
        float lastVel01 { 1.0f };

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (KickEngine)
    };
}
