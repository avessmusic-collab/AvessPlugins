#pragma once

#include <juce_core/juce_core.h>
#include <array>
#include <cmath>

//==============================================================================
// Stage 2 Phase 3.8: custom 9-shape Lfo class (x4 instances owned by
// CORRUPTRAudioProcessor) — architecture.md component #8 ("Modulation Matrix
// + 4 LFOs"), parameter-spec.md's lfo1Rate/lfo1Shape/lfo1Sync .. lfo4Rate/
// lfo4Shape/lfo4Sync parameters (12 total, 3 per LFO x 4 LFOs).
//
// architecture.md explicitly recommends implementing ALL 9 shapes in ONE
// custom class (rather than mixing juce::dsp::Oscillator for the
// analytically-simple shapes with hand-rolled logic for the random-family
// ones) "for consistency" — followed here.
//
// CONTROL-RATE RESOLUTION (FLAGGED DESIGN RESOLUTION — same category of
// decision, and same reasoning shape, as RhythmicSequencer's per-block step
// resolution; see PluginProcessor.h's Phase 3.7 doc comment for that
// original precedent): CORRUPTRAudioProcessor calls advanceAndGetValue()
// exactly ONCE per processBlock(), not once per sample.
//   1. The entire Mod Matrix / Macro pipeline this class feeds already
//      resolves at control rate — ModulationAccumulator::accumulate() is
//      called once per block per destination for every destination this
//      phase wires up (drive/mix/filterCutoff/bitDepth/sampleRateReduction/
//      glitchProbability/fold/feedbackAmount/gate), the exact same
//      granularity every existing Sequencer-lane call site already uses.
//      Resolving this class's phase at audio rate would compute per-sample
//      precision immediately discarded by that once-per-block consumer.
//   2. Every one of those destinations already ramps through its own
//      pre-existing per-destination juce::SmoothedValue (driveGainSmoothed,
//      etc.), which absorbs the "steppiness" of a control-rate-updated
//      target exactly the way it already absorbs ordinary host automation.
//      At 20Hz (this class's fastest supported free-running rate) and a
//      typical block size, a block-rate LFO update is well below the point
//      where stepping becomes audible zipper noise.
//   3. Real-time-safety/cost: resolving 4 LFOs x 8 Mod Matrix slots x 12
//      destinations per SAMPLE (rather than per block) would multiply the
//      cost of this whole subsystem by the block's sample count for no
//      audible benefit given points 1/2 above.
//
// TEMPO-SYNCED RATE MAPPING (FLAGGED DESIGN RESOLUTION — parameter-spec.md's
// lfoNSync is a plain bool with NO separate note-division choice parameter,
// unlike sequencerRate's dedicated 7-choice parameter): when sync is
// enabled, the SAME continuous lfoNRate value (0.01-20Hz) is NOT read as raw
// Hz. Instead it is linearly re-mapped (see mapToSyncedHz() below) to select
// one of a fixed table of musical note divisions — slow-to-fast, using the
// identical fraction/triplet convention RhythmicSequencer.h's
// kSequencerRateBeats already established for sequencerRate — then
// converted to Hz via the current host BPM. This is a common, defensible
// pattern (the SAME rate control governs speed in both free and synced
// modes, several commercial synths/effects with a single Rate knob + Sync
// toggle work this way) chosen because no dedicated division parameter
// exists in the locked parameter-spec.md for this MVP.
//
// DETERMINISM (task requirement — must NOT perturb the audio-thread
// glitchRandom draw sequence, Phase 3.5's invariant): every random draw
// this class needs (S&H / Random / Smooth Random / Random Walk shapes) is
// made through the caller-supplied `juce::Random&` parameter, NEVER an
// internal instance — CORRUPTRAudioProcessor passes its own dedicated
// `modMatrixRandom` member (see PluginProcessor.h's Phase 3.8 doc comment
// for why that RNG is deliberately separate from BOTH glitchRandom and
// RhythmicSequencer's editRandom, and why it is NOT persisted/reseeded from
// a saved state the way glitchRandom is).
//==============================================================================
class Lfo
{
public:
    enum Shape
    {
        shapeSine = 0,
        shapeTriangle,
        shapeSaw,
        shapeReverseSaw,
        shapeSquare,
        shapeSampleHold,
        shapeRandom,
        shapeSmoothRandom,
        shapeRandomWalk,
        kNumShapes
    };

    // `sampleRateIn` is accepted for API symmetry with this codebase's other
    // prepare(sampleRate)-style methods (juce::dsp components,
    // RhythmicSequencer callers, etc.) but is not itself needed internally
    // -- this class's phase accumulator advances by elapsed wall-clock time
    // (`deltaSeconds`, passed directly into advanceAndGetValue() each call)
    // rather than by a sample-count derived from a stored sample rate.
    void prepare(double sampleRateIn) noexcept
    {
        juce::ignoreUnused(sampleRateIn);
        reset();
    }

    void reset() noexcept
    {
        phase = 0.0;
        shHeldValue = 0.0f;
        randomPreviousValue = 0.0f;
        randomTargetValue = 0.0f;
        randomWalkValue = 0.0f;
        smoothRandomCurrent = 0.0f;
        smoothRandomTarget = 0.0f;
    }

    void setShape(int shapeIndex) noexcept { shape = juce::jlimit(0, (int) kNumShapes - 1, shapeIndex); }

    // Maps parameter-spec.md's continuous lfoNRate (0.01-20Hz) to a
    // tempo-synced Hz value — see the class doc comment's "TEMPO-SYNCED
    // RATE MAPPING" section above for the full rationale.
    static float mapToSyncedHz(float rawRateHz, float hostBpm) noexcept
    {
        // Slow -> fast, includes the same 2/3-ratio triplet convention
        // RhythmicSequencer.h's kSequencerRateBeats already established
        // (1/4T = 2/3 of a straight 1/4, etc.): 2 bars, 1 bar, 1/2, 1/4,
        // 1/4T, 1/8, 1/8T, 1/16, 1/16T, 1/32 (all expressed in quarter-note
        // beats).
        static constexpr std::array<double, 10> kSyncDivisionsBeats {
            8.0, 4.0, 2.0, 1.0, (2.0 / 3.0), 0.5, (1.0 / 3.0), 0.25, (1.0 / 6.0), 0.125
        };

        const float norm = juce::jlimit(0.0f, 1.0f, (rawRateHz - 0.01f) / (20.0f - 0.01f));
        const int idx = juce::jlimit(0, (int) kSyncDivisionsBeats.size() - 1,
                                      (int) std::round(norm * (float) (kSyncDivisionsBeats.size() - 1)));
        const double divisionBeats = kSyncDivisionsBeats[(size_t) idx];
        const double bpm = hostBpm > 0.0f ? (double) hostBpm : 120.0;
        return (float) ((bpm / 60.0) / divisionBeats);
    }

    // Advances this LFO's phase by `deltaSeconds` at `rateHz` and returns
    // its output for the resulting phase. Every shape shares the SAME
    // bipolar [-1, +1] output convention (so ModMatrix.h/callers never need
    // per-shape special-casing). Real-time-safe: bounded arithmetic, the
    // only "allocation-adjacent" call is juce::Random::nextFloat() on the
    // caller-supplied `rng`, which is documented as allocation-free.
    float advanceAndGetValue(float rateHz, double deltaSeconds, juce::Random& rng) noexcept
    {
        rateHz = juce::jmax(0.0f, rateHz);
        deltaSeconds = juce::jmax(0.0, deltaSeconds);

        phase += (double) rateHz * deltaSeconds; // phase kept in CYCLES (0..1), not radians -- denormal-safe (architecture.md Special Considerations: "custom LFO phase accumulators should use phase-wrapping, not decaying amplitude")
        bool wrapped = false;
        if (phase >= 1.0)
        {
            phase = std::fmod(phase, 1.0);
            wrapped = true;
        }

        switch (shape)
        {
            case shapeSine:
                return (float) std::sin(phase * juce::MathConstants<double>::twoPi);

            case shapeTriangle:
                // Closed-form triangle via asin(sin(.)) -- the same
                // technique processDistortionEngine()'s Wavefolder case
                // already uses elsewhere in this codebase, reused here for
                // consistency.
                return (float) ((2.0 / juce::MathConstants<double>::pi)
                                 * std::asin(std::sin(phase * juce::MathConstants<double>::twoPi)));

            case shapeSaw:
                return (float) (2.0 * phase - 1.0);

            case shapeReverseSaw:
                return (float) (1.0 - 2.0 * phase);

            case shapeSquare:
                return phase < 0.5 ? 1.0f : -1.0f;

            case shapeSampleHold:
                // Classic Sample & Hold: value changes ONCE per cycle, at
                // the wrap, then holds perfectly constant for the rest of
                // the cycle (hard-edged staircase). architecture.md's own
                // explicit definition for this shape.
                if (wrapped)
                    shHeldValue = rng.nextFloat() * 2.0f - 1.0f;
                return shHeldValue;

            case shapeRandom:
                // FLAGGED DESIGN RESOLUTION -- architecture.md names this
                // shape without defining it (only S&H, Smooth Random, and
                // Random Walk get explicit definitions). Distinguished from
                // shapeSampleHold above by NOT holding a hard step: a fresh
                // random TARGET is drawn on the same once-per-cycle
                // cadence, but the OUTPUT ramps linearly from the previous
                // target to the new one across the cycle's own phase
                // (completing exactly at the next wrap) -- a phase-tied
                // glide. This differs from shapeSmoothRandom below, whose
                // glide runs on a FIXED wall-clock time constant
                // independent of this LFO's own rate.
                if (wrapped)
                {
                    randomPreviousValue = randomTargetValue;
                    randomTargetValue = rng.nextFloat() * 2.0f - 1.0f;
                }
                return (float) (randomPreviousValue + (randomTargetValue - randomPreviousValue) * phase);

            case shapeSmoothRandom:
            {
                // architecture.md: "smooth-random = S&H target with
                // SmoothedValue ramp between targets." Implemented as a
                // deltaSeconds-aware one-pole exponential glide (rather
                // than juce::SmoothedValue, whose ramp-length API assumes
                // getNextValue() is ticked once per SAMPLE at a known
                // sample rate -- this class is deliberately ticked once per
                // BLOCK instead, see the class doc comment's "CONTROL-RATE
                // RESOLUTION" section, so a plain time-constant-based
                // one-pole is used instead, correct regardless of how often
                // advanceAndGetValue() is called).
                if (wrapped)
                    smoothRandomTarget = rng.nextFloat() * 2.0f - 1.0f;
                const double coeff = kSmoothRandomGlideSeconds > 0.0
                                          ? std::exp(-deltaSeconds / kSmoothRandomGlideSeconds)
                                          : 0.0;
                smoothRandomCurrent = (float) (coeff * smoothRandomCurrent + (1.0 - coeff) * smoothRandomTarget);
                return smoothRandomCurrent;
            }

            case shapeRandomWalk:
                // architecture.md: "random-walk = accumulate small random
                // deltas each phase-wrap, clamped to range."
                if (wrapped)
                    randomWalkValue = juce::jlimit(-1.0f, 1.0f,
                        randomWalkValue + (rng.nextFloat() * 2.0f - 1.0f) * kRandomWalkStepSize);
                return randomWalkValue;

            default:
                return 0.0f;
        }
    }

private:
    static constexpr float kRandomWalkStepSize = 0.15f;         // "small deltas" per architecture.md -- this implementation's own step-size resolution, flagged
    static constexpr double kSmoothRandomGlideSeconds = 0.08;   // fixed wall-clock glide, independent of this LFO's own rate -- see shapeRandom's doc comment for the contrast

    int shape = shapeSine;
    double phase = 0.0;

    float shHeldValue = 0.0f;
    float randomPreviousValue = 0.0f;
    float randomTargetValue = 0.0f;
    float randomWalkValue = 0.0f;
    float smoothRandomCurrent = 0.0f;
    float smoothRandomTarget = 0.0f;
};
