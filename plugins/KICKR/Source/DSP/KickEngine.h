#pragma once

#include <array>
#include <atomic>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>

#include "DSP/OversamplingProcessor.h"
#include "DSP/KickVoice.h"
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

        Later phases add: 2-voice retrigger crossfade + transient
        shaper (2.3), click/sub/tail/noise (2.4-2.7), sample player (2.7b), distortion
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
        void renderSegment (juce::AudioBuffer<float>& buffer, int startSample, int numSamples);
        void handleNoteOn (const juce::MidiMessage& message);
        float resolvePitchHz (int noteNumber) const noexcept;

        /** architecture Parameter Mapping row 2 — velocity (subtle) scaling of `pitchStart`. */
        float effectivePitchStartRatio (float v01) const noexcept;

        juce::AudioProcessorValueTreeState& apvts;

        double baseSampleRate { 44100.0 };
        double fsOversampled  { 44100.0 };
        int    maxBlockSize   { 512 };

        OversamplingProcessor oversampling;
        KickVoice             voice;

        juce::AudioBuffer<float>             scratch;        // stereo, base-rate, pre-sized
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
            int   tuneMode        { 0 };     // 0 = MIDI Pitch, 1 = Fixed Frequency
        };
        Snapshot snap;

        // Velocity of the most recent trigger — folded into the effective `pitchStart`
        // each block so pitchStart/pitchTime/pitchCurve automation stays live mid-voice.
        float lastVel01 { 1.0f };

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (KickEngine)
    };
}
