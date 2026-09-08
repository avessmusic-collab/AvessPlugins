#pragma once

#include <juce_core/juce_core.h>
#include <array>

//==============================================================================
// Stage 2 Phase 3.8: 8-slot Modulation Matrix — architecture.md component #8
// ("Modulation Matrix + 4 LFOs"), parameter-spec.md's modSlot1..8
// {Source,Destination,Amount,Enable} parameters (32 total, APVTS-backed per
// architecture.md's Architecture Decisions "Mod Matrix Slots as APVTS
// Parameters, Not Custom State").
//
// This class is deliberately generic/stateless — mirrors
// dsp/ModulationAccumulator.h's own design philosophy (see that file's top
// doc comment). It has NO knowledge of what a source value physically
// represents (Hz, dB, %) or which destination maps to which real DSP
// parameter — it only resolves "for each of the 8 slots, if enabled, look
// up that slot's already-computed source value, scale by its signed Amount
// (and any macro-driven depth multiplier), and sum into that slot's
// selected destination's running total." The CALLER
// (CORRUPTRAudioProcessor::resolveModMatrixAndMacroContributions(), see
// PluginProcessor.cpp) is responsible for (a) computing each of the 10
// SOURCE values for this block (4 LFOs, Envelope, Audio Level, Sequencer,
// Random, MIDI CC, Macro) and (b) converting each DESTINATION's raw
// (roughly -1..+1-per-contributing-slot, unbounded-sum-across-slots)
// contribution into that destination's own physical units/range via
// ModulationAccumulator::accumulate() at that destination's existing
// per-block parameter-read call site — the exact same two-step split
// RhythmicSequencer.h/PluginProcessor.cpp already established for the
// Sequencer's 10 fixed lanes (raw step value there, unit conversion +
// accumulate() at the call site here).
//==============================================================================
class ModMatrix
{
public:
    // Matches parameter-spec.md's modSlotNSource StringArray order exactly
    // ("LFO 1, LFO 2, LFO 3, LFO 4, Envelope, Audio Level, Sequencer,
    // Random, MIDI CC, Macro").
    enum Source
    {
        sourceLfo1 = 0,
        sourceLfo2,
        sourceLfo3,
        sourceLfo4,
        sourceEnvelope,
        sourceAudioLevel,
        sourceSequencer,
        sourceRandom,
        sourceMidiCc,
        sourceMacro,
        kNumSources
    };

    // Matches parameter-spec.md's modSlotNDestination StringArray order
    // exactly ("Drive, Mix, Filter Cutoff, Bit Depth, Sample Rate, Fold,
    // Feedback, Glitch Probability, Glitch Size, Pitch, Pan, Width").
    enum Destination
    {
        destDrive = 0,
        destMix,
        destFilterCutoff,
        destBitDepth,
        destSampleRate,
        destFold,
        destFeedback,
        destGlitchProbability,
        destGlitchSize,
        destPitch,
        destPan,
        destWidth,
        kNumDestinations
    };

    // Pre-resolved source values for this block — each entry's natural
    // range is documented at its computation site in
    // resolveModMatrixAndMacroContributions() (LFOs bipolar [-1,+1];
    // Envelope/Audio Level/MIDI CC/Macro-average unipolar [0,1]; Sequencer
    // bipolar [-1,+1]; Random bipolar [-1,+1]).
    struct SourceValues
    {
        std::array<float, kNumSources> value {};
    };

    // One Mod Matrix slot's current APVTS-derived state (read fresh each
    // block by the caller from the 4 cached raw parameter pointers per
    // slot — see PluginProcessor.h's Phase 3.8 doc comment for the caching
    // rationale).
    struct Slot
    {
        int sourceIndex = sourceLfo1;
        int destinationIndex = destDrive;
        float amountPct = 0.0f; // -100..+100, parameter-spec.md's modSlotNAmount
        bool enabled = false;
    };

    // Resolves all 8 slots into a per-destination raw total:
    // sourceValue * (amountPct/100) * extraDepthMultiplier, where
    // extraDepthMultiplier is `lfoDepthMultiplier` for any of the 4 LFO
    // sources (macroMovement's "LFO depth multiplier across all 4 LFOs'
    // Mod Matrix contributions", architecture.md component #9) or
    // `randomDepthMultiplier` for the Random source (macroChaos's "Mod
    // Matrix Random source depth", same component) — 1.0 (no scaling) for
    // every other source. Real-time-safe: a fixed 8-iteration loop, no
    // allocation, no recursion, bounded arithmetic only.
    static std::array<float, kNumDestinations> resolve(const std::array<Slot, 8>& slots,
                                                         const SourceValues& sources,
                                                         float lfoDepthMultiplier,
                                                         float randomDepthMultiplier) noexcept
    {
        std::array<float, kNumDestinations> totals {};
        totals.fill(0.0f);

        for (const auto& slot : slots)
        {
            if (! slot.enabled)
                continue;

            const int srcIdx = juce::jlimit(0, (int) kNumSources - 1, slot.sourceIndex);
            const int dstIdx = juce::jlimit(0, (int) kNumDestinations - 1, slot.destinationIndex);

            float depthMultiplier = 1.0f;
            if (srcIdx == sourceLfo1 || srcIdx == sourceLfo2 || srcIdx == sourceLfo3 || srcIdx == sourceLfo4)
                depthMultiplier = lfoDepthMultiplier;
            else if (srcIdx == sourceRandom)
                depthMultiplier = randomDepthMultiplier;

            const float amountNorm = juce::jlimit(-1.0f, 1.0f, slot.amountPct / 100.0f);
            totals[(size_t) dstIdx] += sources.value[(size_t) srcIdx] * amountNorm * depthMultiplier;
        }

        return totals;
    }
};
