#pragma once

#include <juce_core/juce_core.h>
#include <cmath>

//==============================================================================
// Stage 2 Phase 3.2: Unified Modulation Accumulator (Isolated)
//
// See architecture.md's "Unified Modulation Accumulator (Sequencer + Mod
// Matrix + Macros + Performance Triggers)" (Implementation Risks section)
// and plan.md's "Phase 3.2: Unified Modulation Accumulator (Isolated)".
// This is the architectural pattern that will eventually resolve EVERY
// modulatable parameter's per-block value (step 2 of the Sequential DSP
// chain in architecture.md's "Processing Order Requirements", which must
// run BEFORE any DSP component reads a parameter's "current" value).
//
// Combination rule (architecture.md's recommended default, used verbatim,
// per plan.md Phase 3.2's "Combination rule" instruction):
//   1. Performance Triggers are hard overrides -- they win outright over
//      every other contribution while active.
//   2. Sequencer + Mod Matrix + Macro contributions combine ADDITIVELY with
//      the base (raw APVTS) parameter value.
//   3. The final result (whichever branch produced it) is clamped to the
//      destination parameter's valid range.
//
// This class is deliberately generic/stateless (no destination-specific
// logic baked in, no APVTS coupling, no knowledge of `drive` specifically)
// so later phases (3.7 Sequencer, 3.8 Mod Matrix/Macros, 3.9 Performance
// Triggers) can attach it to every other modulatable destination without
// rewriting the combine math -- only the PER-DESTINATION contribution
// VALUES and RANGE change at the call site, not this class.
//
// Phase 3.2 scope: prove this math in isolation on ONE destination (`drive`,
// 0-40dB) using synthetic/stub contributor values, per plan.md's recommended
// build order ("get one destination working end-to-end... before
// generalizing to other destinations"). Real Sequencer/Mod Matrix data
// sources do not exist yet (built in Phase 3.7/3.8) -- see
// PluginProcessor.cpp's Phase 3.2 block for how this class is exercised
// with synthetic stand-ins for `drive` specifically, and runSelfTest()
// below for the isolated math validation.
//==============================================================================
class ModulationAccumulator
{
public:
    //=========================================================================
    // Combine base + up to 3 additive contributors, OR a hard Performance
    // Trigger override, then clamp to [rangeMin, rangeMax]. Real-time-safe
    // (no allocation, no locks, bounded execution, pure arithmetic) -- safe
    // to call once per modulatable parameter, per block (or per sample, if
    // a future phase needs sample-accurate modulation), from the audio
    // thread once this is wired into the real chain.
    //
    //   performanceTriggerActive == true:
    //       result = clamp(performanceTriggerOverrideValue)
    //   performanceTriggerActive == false:
    //       result = clamp(baseValue + sequencerContribution
    //                                 + modMatrixContribution
    //                                 + macroContribution)
    //
    // `rangeMin`/`rangeMax` are the DESTINATION PARAMETER's valid range
    // (e.g. drive: 0.0f/40.0f) -- NOT any individual contributor's range.
    // Every code path funnels through the same juce::jlimit() call so an
    // out-of-range value on ANY term (including a deliberately-corrupt
    // override value) can never escape this function.
    //=========================================================================
    static float accumulate(float baseValue,
                             float sequencerContribution,
                             float modMatrixContribution,
                             float macroContribution,
                             bool performanceTriggerActive,
                             float performanceTriggerOverrideValue,
                             float rangeMin,
                             float rangeMax) noexcept
    {
        jassert(rangeMin <= rangeMax);

        if (performanceTriggerActive)
            return juce::jlimit(rangeMin, rangeMax, performanceTriggerOverrideValue);

        const float additiveSum = baseValue + sequencerContribution + modMatrixContribution + macroContribution;
        return juce::jlimit(rangeMin, rangeMax, additiveSum);
    }

    //=========================================================================
    // Self-test harness (Phase 3.2's primary validation vehicle) --
    // exercises accumulate() with synthetic inputs representative of the
    // three Test Criteria in plan.md's "Phase 3.2" section, plus
    // deliberately out-of-range edge cases. Called unconditionally once
    // from CORRUPTRAudioProcessor's constructor (see PluginProcessor.cpp)
    // so it runs on EVERY plugin instantiation -- including under
    // pluginval, which is the closest thing this repo's toolchain has to
    // an executable unit-test harness (no Catch2/JUCE UnitTestRunner
    // infrastructure exists anywhere in this repo, confirmed during Phase
    // 3.1 -- see .continue-here.md's Phase 3.1 validation note, same
    // finding rechecked for Phase 3.2). In debug builds a failing check
    // trips a `jassert` at the exact failing check (hard-stop during
    // development/debug pluginval runs); in release builds the function
    // still runs every check and returns an aggregate pass/fail bool so
    // the caller can record the result in a diagnostic atomic without
    // crashing a shipped binary (same pattern as Phase 3.1's
    // `feedbackCircuitBreakerTripped` diagnostic-atomic convention).
    //
    // Returns true iff every check below passes.
    //=========================================================================
    static bool runSelfTest() noexcept
    {
        bool allPassed = true;
        constexpr float epsilon = 1.0e-4f;

        // Destination under test: `drive`, 0.0-40.0 dB (parameter-spec.md).
        constexpr float driveRangeMin = 0.0f;
        constexpr float driveRangeMax = 40.0f;

        // --- Test Criterion 1: base + synthetic Sequencer + synthetic Mod
        //     Matrix + synthetic Macro combine additively and correctly.
        //     (All four contributors deliberately kept in-range here, and
        //     their sum also in-range, so clamping can't mask an additive-
        //     math bug -- clamping is checked separately below.)
        {
            const float base = 6.0f;       // drive's default (parameter-spec.md)
            const float seq = 3.0f;        // synthetic Sequencer stand-in
            const float mm = 2.0f;         // synthetic Mod Matrix stand-in
            const float macro = 4.0f;      // synthetic Macro stand-in
            const float expected = base + seq + mm + macro; // 15.0
            const float actual = accumulate(base, seq, mm, macro, false, 0.0f, driveRangeMin, driveRangeMax);
            const bool ok = std::abs(actual - expected) < epsilon;
            jassert(ok);
            allPassed = allPassed && ok;
        }

        // --- Test Criterion 1b: negative contributors must subtract
        //     correctly, not just add -- real Mod Matrix/Macro contributions
        //     are bipolar in the locked spec (e.g. modSlotNAmount is
        //     -100/+100), so this guards against a signed-arithmetic bug
        //     that an all-positive test case above wouldn't catch.
        {
            const float base = 20.0f;
            const float seq = -5.0f;
            const float mm = -3.5f;
            const float macro = 1.5f;
            const float expected = base + seq + mm + macro; // 13.0
            const float actual = accumulate(base, seq, mm, macro, false, 0.0f, driveRangeMin, driveRangeMax);
            const bool ok = std::abs(actual - expected) < epsilon;
            jassert(ok);
            allPassed = allPassed && ok;
        }

        // --- Test Criterion 2 (part A): Performance Trigger override wins
        //     outright while active, regardless of what the additive
        //     contributors are (even ones that would sum to something very
        //     different -- 15.0 here -- if the override weren't active).
        {
            const float base = 6.0f;
            const float seq = 3.0f;
            const float mm = 2.0f;
            const float macro = 4.0f; // additive sum would be 15.0 if not overridden
            const float overrideValue = 32.0f; // synthetic override stand-in
            const float actual = accumulate(base, seq, mm, macro, true, overrideValue, driveRangeMin, driveRangeMax);
            const bool ok = std::abs(actual - overrideValue) < epsilon;
            jassert(ok);
            allPassed = allPassed && ok;
        }

        // --- Test Criterion 2 (part B): ...and correctly RELEASES back to
        //     additive combination the instant the trigger goes inactive
        //     (identical contributor values to the override case above,
        //     trigger flag flipped false -- must NOT "stick" at 32.0).
        {
            const float base = 6.0f;
            const float seq = 3.0f;
            const float mm = 2.0f;
            const float macro = 4.0f;
            const float expected = base + seq + mm + macro; // 15.0, NOT the override's 32.0
            const float actual = accumulate(base, seq, mm, macro, false, 32.0f, driveRangeMin, driveRangeMax);
            const bool ok = std::abs(actual - expected) < epsilon;
            jassert(ok);
            allPassed = allPassed && ok;
        }

        // --- Test Criterion 3: result is ALWAYS clamped to [0, 40],
        //     regardless of input combination, including deliberately
        //     out-of-range synthetic inputs on every term (additive sum
        //     overshoot/undershoot, AND a corrupt override value itself).
        {
            // Additive sum deliberately overshoots the top of the range.
            const float actualHigh = accumulate(6.0f, 100.0f, 100.0f, 100.0f, false, 0.0f, driveRangeMin, driveRangeMax);
            const bool okHigh = std::abs(actualHigh - driveRangeMax) < epsilon;
            jassert(okHigh);
            allPassed = allPassed && okHigh;

            // Additive sum deliberately undershoots below zero.
            const float actualLow = accumulate(6.0f, -50.0f, -50.0f, -50.0f, false, 0.0f, driveRangeMin, driveRangeMax);
            const bool okLow = std::abs(actualLow - driveRangeMin) < epsilon;
            jassert(okLow);
            allPassed = allPassed && okLow;

            // Override value itself deliberately out-of-range -- a
            // malformed/future Performance Trigger implementation must
            // still never escape the destination's valid range.
            const float actualOverrideHigh = accumulate(6.0f, 0.0f, 0.0f, 0.0f, true, 999.0f, driveRangeMin, driveRangeMax);
            const bool okOverrideHigh = std::abs(actualOverrideHigh - driveRangeMax) < epsilon;
            jassert(okOverrideHigh);
            allPassed = allPassed && okOverrideHigh;

            const float actualOverrideLow = accumulate(6.0f, 0.0f, 0.0f, 0.0f, true, -999.0f, driveRangeMin, driveRangeMax);
            const bool okOverrideLow = std::abs(actualOverrideLow - driveRangeMin) < epsilon;
            jassert(okOverrideLow);
            allPassed = allPassed && okOverrideLow;
        }

        return allPassed;
    }
};
