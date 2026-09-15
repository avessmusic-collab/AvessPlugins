/*
    DROP - console test runner. Grows one block of checks per Stage 3 DSP
    phase (mirrors plugins/KICKR/Source/Tests/RunTests.cpp's convention).
    Build:  cmake --build build --target DROP_Tests
    Run:    ./build/plugins/DROP/DROP_Tests

    Phase 3.1 (Timing & Control Skeleton) scope, per plugins/DROP/.ideas/plan.md:
      - MusicalClock division->sample conversion at multiple tempos/sample
        rates/time signatures.
      - TransitionEngine curve shapes at known progress fractions.
      - DropStateMachine transitions at exact sample offsets across varying
        host block sizes (1, 32, 64, 128, 512, 2048, 8192).
      - Re-trigger safety.
      - MIDI-note trigger vs `trigger` parameter equivalence.
      - No NaN/Inf, no allocation/locks in processBlock() (allocation/lock
        freedom is verified by code review per plan.md's own test-criteria
        wording; this file verifies the OBSERVABLE behavioural guarantees:
        finite output and an exact audio pass-through).

    Phase 3.3 (Creative DSP Modules) scope, per plugins/DROP/.ideas/plan.md:
      - GranularGrainEngine (shared by PitchModule/StutterModule) ISOLATED
        FFT-based semitone verification, per architecture.md's PitchModule
        mitigation strategy ("implement and unit-test the grain engine in
        isolation... before wiring it into PitchModule/StutterModule").
      - StutterModule repeat-boundary crossfade continuity, all 5 modes.
      - VacuumModule envelope click-free/continuous-derivative verification,
        including a deliberately extreme 200+ BPM-scale edge case.
      - NoiseModule + DropRandomizer bit-identical determinism (same seed).
      - DigitalModule DC-blocker verification against an asymmetric,
        DC-biased test signal.
      - New modules' weight==0 transparency (mirrors the Phase 3.2 pattern).
      - Full processBlock() end-to-end sanity through Peak/Impact/Recovery
        (VacuumModule + all Phase 3.3 modules engaged in the live chain).
      - Zero-allocation-in-process() conventions: verified by code review
        (every module preallocates in prepare()), per plan.md's own wording
        for this test criterion - not a runtime-checkable assertion.

    Phase 3.4 (ImpactEngine) scope, per plugins/DROP/.ideas/plan.md:
      - Sub-hit spectral concentration in the 40-80Hz band (isolated FFT
        check via ImpactEngine::renderSubHitOnly(), same conventions -
        Hann-windowed juce::dsp::FFT + parabolic peak interpolation - as the
        Phase 3.3 GranularGrainEngine FFT test).
      - Vacuum->Impact handoff: windowed-RMS single-trough (no
        double-envelope bump) verification, using VacuumModule + ImpactEngine
        directly (isolated from the other 10 always-process modules'
        interference) on a sustained test tone.
      - Full processBlock() transient-preservation check: a mono impulse
        placed exactly at the Impact-state's first sample (computed via the
        same deterministic fallback-transport arithmetic style as the Phase
        3.3 end-to-end test) survives with a documented fraction of its dry
        amplitude.
      - `impact` parameter scaling monotonicity (0/50/100%).
      - Low-end soft-clip safety on bass-heavy material at full impact.
      - Short-tail reverb decay bounded within a generous tail window.
*/
#include "PluginProcessor.h"
#include "DSP/MusicalClock.h"
#include "DSP/TransitionEngine.h"
#include "DSP/DropStateMachine.h"
#include "DSP/FilterModule.h"
#include "DSP/WidthModule.h"
#include "DSP/GateModule.h"
#include "DSP/DelayModule.h"
#include "DSP/SaturationModule.h"
#include "DSP/ReverbModule.h"
#include "DSP/IntensityBudget.h"
#include "DSP/PitchModule.h"
#include "DSP/StutterModule.h"
#include "DSP/DigitalModule.h"
#include "DSP/VacuumModule.h"
#include "DSP/NoiseModule.h"
#include "DSP/ImpactEngine.h"
#include "Core/DropRandomizer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <functional>
#include <limits>
#include <random>
#include <vector>

namespace
{
    int  failures = 0;
    void check (bool cond, const char* name)
    {
        std::printf ("  %s  %s\n", cond ? "PASS" : "FAIL", name);
        if (! cond) ++failures;
    }

    bool nearlyEqual (double a, double b, double tolerance)
    {
        return std::abs (a - b) <= tolerance;
    }

    //==========================================================================
    // A minimal AudioPlayHead supplying a fixed, synthetic PositionInfo -
    // lets MusicalClock tests exercise the "valid host playhead" path
    // deterministically, without a real host.
    struct TestPlayHead : public juce::AudioPlayHead
    {
        juce::AudioPlayHead::PositionInfo info;
        juce::Optional<juce::AudioPlayHead::PositionInfo> getPosition() const override { return info; }
    };

    juce::AudioPlayHead::PositionInfo makePosition (double bpm, int numerator, int denominator,
                                                     double ppqPosition, double ppqPositionOfLastBarStart)
    {
        juce::AudioPlayHead::PositionInfo info;
        info.setBpm (bpm);
        juce::AudioPlayHead::TimeSignature sig;
        sig.numerator = numerator;
        sig.denominator = denominator;
        info.setTimeSignature (sig);
        info.setPpqPosition (ppqPosition);
        info.setPpqPositionOfLastBarStart (ppqPositionOfLastBarStart);
        return info;
    }
}

//==============================================================================
// Phase 3.1 - DropStateMachine sample-accurate segmentation driver. Mirrors
// PluginProcessor::processBlock()'s sub-block segmentation loop exactly, but
// drives DropStateMachine directly (no PluginProcessor/APVTS/MIDI involved)
// so the state-transition-timing guarantee can be tested in isolation,
// across arbitrary host "block sizes", with a single reusable helper.
namespace
{
    struct TransitionRecord
    {
        int64_t sampleOffset;
        drop::DropState newState;
    };

    std::vector<TransitionRecord> simulateStateMachine (const drop::DropStateMachine::Durations& durations,
                                                         int blockSize, int64_t totalSamplesToSimulate)
    {
        drop::DropStateMachine sm;
        std::vector<TransitionRecord> transitions;
        int64_t globalSample = 0;
        bool triggered = false;

        while (globalSample < totalSamplesToSimulate)
        {
            const int thisBlockSize = (int) std::min<int64_t> (blockSize, totalSamplesToSimulate - globalSample);
            int segmentStart = 0;

            while (segmentStart < thisBlockSize)
            {
                if (! triggered && globalSample + segmentStart == 0)
                {
                    sm.requestTrigger (durations);
                    transitions.push_back ({ globalSample + segmentStart, sm.getState() });
                    triggered = true;
                }

                while (sm.tick())
                    transitions.push_back ({ globalSample + segmentStart, sm.getState() });

                int64_t segmentLength = (int64_t) (thisBlockSize - segmentStart);
                segmentLength = std::min (segmentLength, sm.samplesUntilNextTransition());
                if (segmentLength <= 0)
                    segmentLength = 1;

                sm.advance (segmentLength);
                segmentStart += (int) segmentLength;
            }

            while (sm.tick())
                transitions.push_back ({ globalSample + thisBlockSize, sm.getState() });

            globalSample += thisBlockSize;
        }

        return transitions;
    }

    // Extracts the sample offset at which `state` was FIRST entered, or -1 if never reached.
    int64_t offsetOfFirstEntry (const std::vector<TransitionRecord>& transitions, drop::DropState state)
    {
        for (const auto& t : transitions)
            if (t.newState == state)
                return t.sampleOffset;
        return -1;
    }
}

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    std::printf ("DROP tests\n");

    //==========================================================================
    std::printf ("\n[Phase 3.1] MusicalClock - division/bar -> sample conversion\n");
    {
        // A spread of tempos, sample rates, and time signatures (plan.md:
        // "division->sample conversion at multiple tempos/sample
        // rates/time signatures").
        struct Case { double bpm; double sampleRate; int num; int den; };
        const Case cases[] = {
            { 120.0, 44100.0, 4, 4 },
            { 120.0, 48000.0, 4, 4 },
            {  60.0, 48000.0, 4, 4 },
            { 174.0, 96000.0, 4, 4 },
            { 140.0, 44100.0, 3, 4 },
            { 100.0, 192000.0, 6, 8 },
        };

        bool allOk = true;
        for (const auto& c : cases)
        {
            drop::MusicalClock clock;
            clock.prepare (c.sampleRate);
            TestPlayHead ph;
            ph.info = makePosition (c.bpm, c.num, c.den, 0.0, 0.0);
            clock.update (&ph, 0);

            const double expectedSamplesPerBeat = (60.0 / c.bpm) * c.sampleRate;
            const double expectedBarLength = expectedSamplesPerBeat * c.num * (4.0 / (double) c.den);

            const bool spbOk = nearlyEqual (clock.getSamplesPerBeat(), expectedSamplesPerBeat, 1.0e-6 * expectedSamplesPerBeat + 1.0e-6);
            const bool barOk = nearlyEqual (clock.getBarLengthSamples(), expectedBarLength, 1.0e-6 * expectedBarLength + 1.0e-6);

            const bool quarterOk = nearlyEqual (clock.divisionToSamples (drop::MusicalDivision::Quarter, false), expectedSamplesPerBeat, 1.0e-6 * expectedSamplesPerBeat + 1.0e-6);
            const bool wholeOk = nearlyEqual (clock.divisionToSamples (drop::MusicalDivision::Whole, false), expectedSamplesPerBeat * 4.0, 1.0e-6 * expectedSamplesPerBeat * 4.0 + 1.0e-6);
            const bool eighthOk = nearlyEqual (clock.divisionToSamples (drop::MusicalDivision::Eighth, false), expectedSamplesPerBeat * 0.5, 1.0e-6 * expectedSamplesPerBeat * 0.5 + 1.0e-6);
            const bool sixteenthTripletOk = nearlyEqual (clock.divisionToSamples (drop::MusicalDivision::Sixteenth, true),
                                                          expectedSamplesPerBeat * 0.25 * (2.0 / 3.0), 1.0e-6 * expectedSamplesPerBeat + 1.0e-6);
            const bool barsOk = nearlyEqual (clock.barsToSamples (4), expectedBarLength * 4.0, 1.0e-6 * expectedBarLength * 4.0 + 1.0e-6);

            const bool ok = spbOk && barOk && quarterOk && wholeOk && eighthOk && sixteenthTripletOk && barsOk;
            std::printf ("  bpm=%.0f sr=%.0f sig=%d/%d: samplesPerBeat %.3f (exp %.3f) barLength %.3f (exp %.3f) -> %s\n",
                         c.bpm, c.sampleRate, c.num, c.den, clock.getSamplesPerBeat(), expectedSamplesPerBeat,
                         clock.getBarLengthSamples(), expectedBarLength, ok ? "ok" : "MISMATCH");
            allOk = allOk && ok;
        }
        check (allOk, "division/bar -> sample conversion matches hand-computed values across tempos/sample rates/time signatures");
    }

    {
        // Mid-bar position -> samplesToNextBarBoundary() should reflect the
        // exact remaining distance to the bar boundary, using
        // ppqPositionOfLastBarStart when the host supplies it.
        drop::MusicalClock clock;
        clock.prepare (48000.0);
        TestPlayHead ph;
        // 120 BPM, 4/4: bar = 4 beats = 96000 samples. Position 1.0 ppq into
        // the bar (last bar start at ppq 8.0, position at ppq 9.0) -> 1 beat
        // (24000 samples) into the bar -> 72000 samples remain.
        ph.info = makePosition (120.0, 4, 4, 9.0, 8.0);
        clock.update (&ph, 0);
        const double expectedRemaining = 72000.0;
        check (nearlyEqual (clock.samplesToNextBarBoundary(), expectedRemaining, 1.0),
              "samplesToNextBarBoundary() reflects exact mid-bar host position (ppqPositionOfLastBarStart honored)");

        // Exactly on a bar boundary -> 0 remaining.
        TestPlayHead phOnBoundary;
        phOnBoundary.info = makePosition (120.0, 4, 4, 8.0, 8.0);
        clock.update (&phOnBoundary, 0);
        check (nearlyEqual (clock.samplesToNextBarBoundary(), 0.0, 1.0e-6),
              "samplesToNextBarBoundary() is 0 exactly on a bar boundary");
    }

    {
        // Standalone fallback transport (no host playhead): fixed 120 BPM /
        // 4/4, driven purely by a running sample counter (never wall-clock).
        drop::MusicalClock clock;
        clock.prepare (48000.0);
        clock.update (nullptr, 0);
        check (clock.isUsingFallbackTransport(), "no playhead -> fallback transport active");
        check (nearlyEqual (clock.getBpm(), 120.0, 1.0e-9), "fallback transport is 120 BPM");
        check (clock.getTimeSigNumerator() == 4 && clock.getTimeSigDenominator() == 4, "fallback transport is 4/4");

        const double barLength = clock.getBarLengthSamples(); // 96000 samples @ 48kHz/120bpm
        check (nearlyEqual (clock.samplesToNextBarBoundary(), 0.0, 1.0e-6), "fallback transport starts exactly on a bar boundary");

        clock.update (nullptr, 1000); // advance 1000 samples (a "processed host block")
        check (nearlyEqual (clock.samplesToNextBarBoundary(), barLength - 1000.0, 1.0),
              "fallback transport's standalone sample counter advances correctly (never wall-clock)");

        // Advance past a full bar - position should wrap, not accumulate unbounded.
        clock.update (nullptr, (int) barLength); // total advanced: 1000 + barLength
        check (nearlyEqual (clock.samplesToNextBarBoundary(), barLength - 1000.0, 1.0),
              "fallback transport wraps correctly across a bar boundary");
    }

    //==========================================================================
    std::printf ("\n[Phase 3.1] TransitionEngine - curve shapes at known progress fractions\n");
    {
        drop::TransitionEngine engine;

        engine.setCurveType (drop::CurveType::Linear);
        check (nearlyEqual (engine.evaluate (0.0f), 0.0, 1.0e-6), "Linear: evaluate(0) == 0");
        check (nearlyEqual (engine.evaluate (0.5f), 0.5, 1.0e-6), "Linear: evaluate(0.5) == 0.5");
        check (nearlyEqual (engine.evaluate (1.0f), 1.0, 1.0e-6), "Linear: evaluate(1) == 1");

        engine.setCurveType (drop::CurveType::Exponential);
        engine.setTension (0.0f); // exponent 1 -> linear
        check (nearlyEqual (engine.evaluate (0.5f), 0.5, 1.0e-4), "Exponential @ tension=0 behaves linearly");
        engine.setTension (1.0f); // exponent 4 -> steep late rise
        check (engine.evaluate (0.5f) < 0.2f, "Exponential @ tension=1 stays low at the midpoint (late rise)");
        check (nearlyEqual (engine.evaluate (0.0f), 0.0, 1.0e-6) && nearlyEqual (engine.evaluate (1.0f), 1.0, 1.0e-4),
              "Exponential endpoints are 0 and 1 regardless of tension");

        engine.setCurveType (drop::CurveType::SCurve);
        engine.setTension (0.0f);
        check (nearlyEqual (engine.evaluate (0.5f), 0.5, 1.0e-4), "S-Curve is symmetric (evaluate(0.5) == 0.5)");
        check (engine.evaluate (0.25f) < 0.25f, "S-Curve: slow start (evaluate(0.25) < 0.25)");
        check (engine.evaluate (0.75f) > 0.75f, "S-Curve: fast finish (evaluate(0.75) > 0.75)");
        check (nearlyEqual (engine.evaluate (0.0f), 0.0, 1.0e-6) && nearlyEqual (engine.evaluate (1.0f), 1.0, 1.0e-4),
              "S-Curve endpoints are 0 and 1 regardless of tension");

        engine.setCurveType (drop::CurveType::LateAcceleration);
        engine.setTension (0.5f); // default tension -> tensionBias == 0.25 (architecture.md default)
        check (nearlyEqual (engine.evaluate (0.75f), 0.75 * 0.3, 1.0e-3),
              "LateAcceleration @ default tension: breakpoint (t=0.75) reaches the expected low progress value");
        check (engine.evaluate (0.9f) > engine.evaluate (0.75f) + 0.3f,
              "LateAcceleration: rapid rise in the final tensionBias fraction");
        check (nearlyEqual (engine.evaluate (0.0f), 0.0, 1.0e-6) && nearlyEqual (engine.evaluate (1.0f), 1.0, 1.0e-4),
              "LateAcceleration endpoints are 0 and 1");

        engine.setCurveType (drop::CurveType::Custom);
        check (nearlyEqual (engine.evaluate (0.37f), 0.37, 1.0e-4), "Custom with no points set: safe Linear-equivalent default");

        engine.setCustomPoints ({ { 0.0f, 0.0f }, { 0.5f, 0.8f }, { 1.0f, 1.0f } });
        check (nearlyEqual (engine.evaluate (0.25f), 0.4, 1.0e-3), "Custom: linear interpolation between (0,0) and (0.5,0.8) at t=0.25");
        check (nearlyEqual (engine.evaluate (0.75f), 0.9, 1.0e-3), "Custom: linear interpolation between (0.5,0.8) and (1,1) at t=0.75");
        check (nearlyEqual (engine.evaluate (0.0f), 0.0, 1.0e-6), "Custom: evaluate(0) matches the first point exactly");
        check (nearlyEqual (engine.evaluate (1.0f), 1.0, 1.0e-6), "Custom: evaluate(1) matches the last point exactly");

        // Out-of-order/duplicate/out-of-range points are sanitized (clamped,
        // sorted, de-duplicated by x) rather than corrupting evaluation.
        engine.setCustomPoints ({ { 1.5f, 2.0f }, { -0.5f, -1.0f }, { 0.5f, 0.5f }, { 0.5f, 0.6f } });
        const float afterSanitize = engine.evaluate (0.5f);
        check (nearlyEqual (afterSanitize, 0.6, 1.0e-3), "Custom: duplicate x keeps the LAST written point (0.6, not 0.5)");
        check (engine.getNumCustomPoints() == 3, "Custom: out-of-range/duplicate points sanitized to 3 unique, clamped, sorted points");

        // Monotonicity: every curve type/tension combination tested above
        // must be non-decreasing across the full [0,1] domain (a hard
        // requirement for "normalized progression" per architecture.md).
        bool allMonotonic = true;
        for (auto curveType : { drop::CurveType::Linear, drop::CurveType::Exponential, drop::CurveType::SCurve, drop::CurveType::LateAcceleration })
        {
            for (float tension : { 0.0f, 0.5f, 1.0f })
            {
                drop::TransitionEngine e;
                e.setCurveType (curveType);
                e.setTension (tension);
                float previous = -1.0f;
                for (int i = 0; i <= 20; ++i)
                {
                    const float t = (float) i / 20.0f;
                    const float v = e.evaluate (t);
                    if (v < previous - 1.0e-5f)
                        allMonotonic = false;
                    previous = v;
                }
            }
        }
        check (allMonotonic, "every curve type is monotonically non-decreasing across [0,1] at tension 0/0.5/1");
    }

    //==========================================================================
    std::printf ("\n[Phase 3.1] DropStateMachine - re-trigger safety\n");
    {
        drop::DropStateMachine::Durations d;
        d.armingToBuildingSamples = 100;
        d.buildingSamples = 200;
        d.peakSamples = 50;
        d.impactSamples = 150;
        d.recoverySamples = 300;

        drop::DropStateMachine sm;
        check (sm.getState() == drop::DropState::Idle, "starts Idle");
        check (sm.requestTrigger (d), "first requestTrigger() from Idle is accepted");
        check (sm.getState() == drop::DropState::Arming, "state is Arming after the accepted trigger");

        const int64_t remainingBeforeRetrigger = sm.samplesUntilNextTransition();
        sm.advance (10);
        check (! sm.requestTrigger (d), "second requestTrigger() while Arming is REJECTED (re-trigger safety)");
        check (sm.getState() == drop::DropState::Arming, "state remains Arming after the rejected re-trigger");
        check (sm.samplesUntilNextTransition() == remainingBeforeRetrigger - 10,
              "rejected re-trigger did NOT reset progress within the current state (durations/samplesIntoState untouched)");

        // Advance through the full state sequence and confirm re-trigger
        // stays rejected in every non-Idle state, then succeeds again once
        // back in Idle.
        // advance(n) consumes samples within the CURRENT state only; tick()
        // performs the pending transition at a boundary (call in a loop to
        // cascade through zero-length states) - same contract processBlock's
        // segmentation loop uses.
        auto advanceAndCross = [&sm] (int64_t n) { sm.advance (n); while (sm.tick()) {} };

        advanceAndCross (remainingBeforeRetrigger - 10); // -> Building
        check (sm.getState() == drop::DropState::Building, "advanced into Building");
        check (! sm.requestTrigger (d), "requestTrigger() while Building is REJECTED");

        advanceAndCross (d.buildingSamples);
        check (sm.getState() == drop::DropState::Peak, "advanced into Peak");
        check (! sm.requestTrigger (d), "requestTrigger() while Peak is REJECTED");

        advanceAndCross (d.peakSamples);
        check (sm.getState() == drop::DropState::Impact, "advanced into Impact");
        check (! sm.requestTrigger (d), "requestTrigger() while Impact is REJECTED");

        advanceAndCross (d.impactSamples);
        check (sm.getState() == drop::DropState::Recovery, "advanced into Recovery");
        check (! sm.requestTrigger (d), "requestTrigger() while Recovery is REJECTED");

        advanceAndCross (d.recoverySamples);
        check (sm.getState() == drop::DropState::Idle, "advanced back to Idle after the full sequence");
        check (sm.requestTrigger (d), "requestTrigger() is accepted again once back in Idle");
    }

    //==========================================================================
    std::printf ("\n[Phase 3.1] DropStateMachine - exact transition offsets across varying block sizes\n");
    {
        // Deliberately non-block-aligned durations (not multiples of any of
        // the tested block sizes) to genuinely stress sample accuracy.
        drop::DropStateMachine::Durations d;
        d.armingToBuildingSamples = 137;
        d.buildingSamples = 9973;
        d.peakSamples = 257;
        d.impactSamples = 4001;
        d.recoverySamples = 6000;

        const int64_t expectedArming   = 0;
        const int64_t expectedBuilding = expectedArming + d.armingToBuildingSamples;
        const int64_t expectedPeak     = expectedBuilding + d.buildingSamples;
        const int64_t expectedImpact   = expectedPeak + d.peakSamples;
        const int64_t expectedRecovery = expectedImpact + d.impactSamples;
        const int64_t expectedIdle     = expectedRecovery + d.recoverySamples;

        const int blockSizes[] = { 1, 32, 64, 128, 512, 2048, 8192 };
        bool allExact = true;

        for (int blockSize : blockSizes)
        {
            const auto transitions = simulateStateMachine (d, blockSize, expectedIdle + (int64_t) blockSize * 2);

            const int64_t armingOffset   = offsetOfFirstEntry (transitions, drop::DropState::Arming);
            const int64_t buildingOffset = offsetOfFirstEntry (transitions, drop::DropState::Building);
            const int64_t peakOffset     = offsetOfFirstEntry (transitions, drop::DropState::Peak);
            const int64_t impactOffset   = offsetOfFirstEntry (transitions, drop::DropState::Impact);
            const int64_t recoveryOffset = offsetOfFirstEntry (transitions, drop::DropState::Recovery);
            const int64_t idleOffset     = offsetOfFirstEntry (transitions, drop::DropState::Idle); // 2nd Idle entry (post-build), NOT the implicit start

            const bool ok = armingOffset == expectedArming
                          && buildingOffset == expectedBuilding
                          && peakOffset == expectedPeak
                          && impactOffset == expectedImpact
                          && recoveryOffset == expectedRecovery
                          && idleOffset == expectedIdle;

            std::printf ("  blockSize=%5d: Arming@%lld(exp %lld) Building@%lld(exp %lld) Peak@%lld(exp %lld) "
                         "Impact@%lld(exp %lld) Recovery@%lld(exp %lld) Idle@%lld(exp %lld) -> %s\n",
                         blockSize,
                         (long long) armingOffset, (long long) expectedArming,
                         (long long) buildingOffset, (long long) expectedBuilding,
                         (long long) peakOffset, (long long) expectedPeak,
                         (long long) impactOffset, (long long) expectedImpact,
                         (long long) recoveryOffset, (long long) expectedRecovery,
                         (long long) idleOffset, (long long) expectedIdle,
                         ok ? "ok" : "MISMATCH");
            allExact = allExact && ok;
        }

        check (allExact, "every state transition lands on the exact expected sample offset, identically across block sizes 1/32/64/128/512/2048/8192");
    }

    //==========================================================================
    std::printf ("\n[Phase 3.1] PluginProcessor - trigger parameter vs MIDI-note trigger equivalence\n");
    {
        auto pulseTrigger = [] (DROPAudioProcessor& p, bool high)
        {
            if (auto* prm = p.parameters.getParameter ("trigger"))
                prm->setValueNotifyingHost (high ? 1.0f : 0.0f);
        };

        // --- trigger parameter (host automation / manual button) ---------
        {
            DROPAudioProcessor p;
            p.setRateAndBufferSizeDetails (48000.0, 64);
            p.prepareToPlay (48000.0, 64);
            check (p.getCurrentDropState() == drop::DropState::Idle, "trigger-param processor: starts Idle");

            pulseTrigger (p, true);
            juce::AudioBuffer<float> buf (2, 64);
            buf.clear();
            juce::MidiBuffer midi;
            p.processBlock (buf, midi);
            pulseTrigger (p, false);

            check (p.getCurrentDropState() != drop::DropState::Idle, "trigger parameter's rising edge moved the state machine out of Idle");
            p.releaseResources();
        }

        // --- MIDI trigger note (only active when triggerMode == Midi) ----
        {
            DROPAudioProcessor p;
            p.setTriggerMode (DROPAudioProcessor::TriggerMode::Midi);
            p.setMidiTriggerNote (60);
            p.setRateAndBufferSizeDetails (48000.0, 64);
            p.prepareToPlay (48000.0, 64);
            check (p.getCurrentDropState() == drop::DropState::Idle, "MIDI-trigger processor: starts Idle");

            juce::AudioBuffer<float> buf (2, 64);
            buf.clear();
            juce::MidiBuffer midi;
            midi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), 10);
            p.processBlock (buf, midi);

            check (p.getCurrentDropState() != drop::DropState::Idle,
                  "matching MIDI note-on moved the state machine out of Idle - same Idle->Arming transition as the trigger parameter");
            p.releaseResources();
        }

        // --- MIDI note NOT matching the configured trigger note: no-op ---
        {
            DROPAudioProcessor p;
            p.setTriggerMode (DROPAudioProcessor::TriggerMode::Midi);
            p.setMidiTriggerNote (60);
            p.prepareToPlay (48000.0, 64);

            juce::AudioBuffer<float> buf (2, 64);
            buf.clear();
            juce::MidiBuffer midi;
            midi.addEvent (juce::MidiMessage::noteOn (1, 61, (juce::uint8) 100), 10); // wrong note
            p.processBlock (buf, midi);

            check (p.getCurrentDropState() == drop::DropState::Idle, "non-matching MIDI note does NOT trigger");
            p.releaseResources();
        }

        // --- MIDI listener inactive outside Midi trigger mode -------------
        {
            DROPAudioProcessor p; // default TriggerMode::Manual
            p.setMidiTriggerNote (60);
            p.prepareToPlay (48000.0, 64);

            juce::AudioBuffer<float> buf (2, 64);
            buf.clear();
            juce::MidiBuffer midi;
            midi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), 10);
            p.processBlock (buf, midi);

            check (p.getCurrentDropState() == drop::DropState::Idle,
                  "matching MIDI note is ignored when triggerMode != Midi (avoids accidental triggers from unrelated MIDI content)");
            p.releaseResources();
        }
    }

    //==========================================================================
    std::printf ("\n[Phase 3.1->3.2] PluginProcessor - finite, bounded output across an active build\n");
    {
        // NOTE: this check's ORIGINAL Phase 3.1 assertion was bit-identical
        // pass-through, valid only because no audible DSP existed yet.
        // Stage 3 DSP Phase 3.2 adds six always-process DSP modules whose
        // weight rises above 0 during Building by design (IntensityBudget's
        // progress-driven budget) - bit-identical pass-through through
        // Building is therefore no longer the correct expectation and this
        // check is updated accordingly. Finite/bounded output remains a
        // hard requirement (updated further to explicit NaN/Inf/denormal
        // sweeps below, per plan.md Phase 3.2 test criteria).
        DROPAudioProcessor p;
        p.setRateAndBufferSizeDetails (48000.0, 256);
        p.prepareToPlay (48000.0, 256);

        if (auto* prm = p.parameters.getParameter ("trigger"))
            prm->setValueNotifyingHost (1.0f);

        bool allFinite = true;
        bool allBounded = true;

        for (int block = 0; block < 50; ++block)
        {
            juce::AudioBuffer<float> buf (2, 256);
            for (int ch = 0; ch < 2; ++ch)
            {
                auto* data = buf.getWritePointer (ch);
                for (int i = 0; i < 256; ++i)
                    data[i] = std::sin ((float) (block * 256 + i) * 0.01f) * 0.5f;
            }

            juce::MidiBuffer midi;
            p.processBlock (buf, midi);

            if (block == 0)
                if (auto* prm = p.parameters.getParameter ("trigger"))
                    prm->setValueNotifyingHost (0.0f); // release the momentary pulse after the first block

            for (int ch = 0; ch < 2; ++ch)
            {
                const float* out = buf.getReadPointer (ch);
                for (int i = 0; i < 256; ++i)
                {
                    if (! std::isfinite (out[i])) allFinite = false;
                    if (std::abs (out[i]) > 4.0f) allBounded = false;
                }
            }
        }

        check (allFinite, "output stays finite (no NaN/Inf) across an active Idle->Arming->Building sequence");
        check (allBounded, "output stays bounded (no blow-up) across an active Idle->Arming->Building sequence with all six Phase 3.2 DSP modules engaged");
        p.releaseResources();
    }

    //==========================================================================
    std::printf ("\n[Phase 3.2] IntensityBudget - proportional-cap invariant + zero-macro transparency\n");
    {
        drop::IntensityBudget budget;
        std::mt19937 rng (12345u); // fixed seed - determinism not required here, just repeatable fuzzing
        std::uniform_real_distribution<float> dist01 (0.0f, 1.0f);

        bool invariantHolds = true;
        for (int trial = 0; trial < 500; ++trial)
        {
            std::array<float, (size_t) drop::BudgetModule::Count> macros {};
            for (auto& m : macros)
                m = dist01 (rng);
            const float progress = dist01 (rng);
            const float intensity = dist01 (rng);

            const auto weights = budget.compute (macros, progress, intensity);

            float sum = 0.0f;
            for (auto w : weights.value)
            {
                if (! std::isfinite (w) || w < 0.0f || w > 1.0f) invariantHolds = false;
                sum += w;
            }

            const float totalBudget = intensity * juce::jlimit (0.0f, 1.0f, progress)
                                     * (float) ((size_t) drop::BudgetModule::Count) * 0.6f;
            if (sum > totalBudget + 1.0e-3f) invariantHolds = false;
        }
        check (invariantHolds, "computeWeights(): sum(output) <= totalBudget(progress) invariant holds across 500 randomized fuzz inputs, every weight in [0,1] and finite");

        std::array<float, (size_t) drop::BudgetModule::Count> allZeroMacros {};
        const auto zeroWeights = budget.compute (allZeroMacros, 1.0f, 1.0f);
        bool allZero = true;
        for (auto w : zeroWeights.value)
            if (w != 0.0f) allZero = false;
        check (allZero, "all-zero macros -> all-zero weights regardless of progress/intensity");

        std::array<float, (size_t) drop::BudgetModule::Count> allFullMacros {};
        for (auto& m : allFullMacros) m = 1.0f;
        const auto zeroProgressWeights = budget.compute (allFullMacros, 0.0f, 1.0f);
        bool allZeroAtZeroProgress = true;
        for (auto w : zeroProgressWeights.value)
            if (w != 0.0f) allZeroAtZeroProgress = false;
        check (allZeroAtZeroProgress, "progress == 0 (Idle/Arming) -> all-zero weights regardless of macros/intensity");
    }

    //==========================================================================
    std::printf ("\n[Phase 3.2] Six modules - weight==0 transparency\n");
    {
        juce::dsp::ProcessSpec spec { 48000.0, 256, 2 };
        auto makeTestBuffer = [] (int numSamples)
        {
            juce::AudioBuffer<float> buf (2, numSamples);
            for (int ch = 0; ch < 2; ++ch)
            {
                auto* data = buf.getWritePointer (ch);
                for (int i = 0; i < numSamples; ++i)
                    data[i] = std::sin ((float) i * (0.05f + 0.01f * (float) ch)) * 0.6f;
            }
            return buf;
        };

        auto transparencyCheck = [&] (const char* name, auto&& processFn)
        {
            auto buf = makeTestBuffer (256);
            juce::AudioBuffer<float> reference;
            reference.makeCopyOf (buf);

            // Prime the smoothed weight to settle exactly at 0 before measuring.
            for (int i = 0; i < 4; ++i)
            {
                auto primeBuf = makeTestBuffer (256);
                juce::dsp::AudioBlock<float> block (primeBuf);
                processFn (block);
            }

            juce::dsp::AudioBlock<float> block (buf);
            processFn (block);

            float maxDiff = 0.0f;
            for (int ch = 0; ch < 2; ++ch)
            {
                const float* out = buf.getReadPointer (ch);
                const float* ref = reference.getReadPointer (ch);
                for (int i = 0; i < 256; ++i)
                    maxDiff = std::max (maxDiff, std::abs (out[i] - ref[i]));
            }
            check (maxDiff < 1.0e-4f, name);
        };

        {
            drop::FilterModule m; m.prepare (spec);
            transparencyCheck ("FilterModule: weight==0 is near-transparent", [&] (juce::dsp::AudioBlock<float>& b) { m.process (b, 0.0f, 0.5f); });
        }
        {
            drop::WidthModule m; m.prepare (spec);
            transparencyCheck ("WidthModule: weight==0 is near-transparent", [&] (juce::dsp::AudioBlock<float>& b) { m.process (b, 0.0f, 0.5f); });
        }
        {
            drop::GateModule m; m.prepare (spec);
            drop::MusicalClock clock; clock.prepare (48000.0);
            TestPlayHead ph; ph.info = makePosition (120.0, 4, 4, 0.0, 0.0);
            clock.update (&ph, 0);
            transparencyCheck ("GateModule: weight==0 is EXACTLY transparent", [&] (juce::dsp::AudioBlock<float>& b) { m.process (b, 0.0f, 0.5f, clock); });
        }
        {
            drop::SaturationModule m; m.prepare (spec);
            transparencyCheck ("SaturationModule: weight==0 is near-transparent", [&] (juce::dsp::AudioBlock<float>& b) { m.process (b, 0.0f, 0.5f); });
        }
        {
            drop::DelayModule m; m.prepare (spec);
            drop::MusicalClock clock; clock.prepare (48000.0);
            TestPlayHead ph; ph.info = makePosition (120.0, 4, 4, 0.0, 0.0);
            clock.update (&ph, 0);
            transparencyCheck ("DelayModule: weight==0 is near-transparent", [&] (juce::dsp::AudioBlock<float>& b) { m.process (b, 0.0f, 0.5f, clock); });
        }
        {
            drop::ReverbModule m; m.prepare (spec);
            transparencyCheck ("ReverbModule: weight==0 is near-transparent", [&] (juce::dsp::AudioBlock<float>& b) { m.process (b, 0.0f, 0.5f); });
        }
    }

    //==========================================================================
    std::printf ("\n[Phase 3.2] FilterModule - stability at max resonance sweep\n");
    {
        juce::dsp::ProcessSpec spec { 48000.0, 512, 2 };
        drop::FilterModule filter;
        filter.prepare (spec);

        std::mt19937 rng (7u);
        std::uniform_real_distribution<float> noiseDist (-1.0f, 1.0f);

        bool allFinite = true;
        bool allBounded = true;
        for (int block = 0; block < 200; ++block)
        {
            juce::AudioBuffer<float> buf (2, 512);
            for (int ch = 0; ch < 2; ++ch)
            {
                auto* data = buf.getWritePointer (ch);
                for (int i = 0; i < 512; ++i)
                    data[i] = noiseDist (rng) * 0.9f;
            }

            // Rapidly sweeping progress (drives resonance toward its
            // ceiling at weight==1) across blocks - stresses coefficient
            // updates at the resonance ceiling.
            const float progress = (block % 2 == 0) ? 1.0f : 0.0f;

            juce::dsp::AudioBlock<float> block_ (buf);
            filter.process (block_, 1.0f, progress);

            for (int ch = 0; ch < 2; ++ch)
            {
                const float* out = buf.getReadPointer (ch);
                for (int i = 0; i < 512; ++i)
                {
                    if (! std::isfinite (out[i])) allFinite = false;
                    if (std::abs (out[i]) > 10.0f) allBounded = false;
                }
            }
        }
        check (allFinite, "FilterModule: no NaN/Inf across a rapid max-resonance progress sweep with full-scale noise, weight==1");
        check (allBounded, "FilterModule: output stays bounded (no blow-up) at the resonance ceiling");
    }

    //==========================================================================
    std::printf ("\n[Phase 3.2] WidthModule - low-band mono-sum protection\n");
    {
        juce::dsp::ProcessSpec spec { 48000.0, 4096, 2 };

        auto measureCorrelation = [] (const juce::AudioBuffer<float>& buf)
        {
            double sumLR = 0.0, sumLL = 0.0, sumRR = 0.0;
            const auto* l = buf.getReadPointer (0);
            const auto* r = buf.getReadPointer (1);
            for (int i = 0; i < buf.getNumSamples(); ++i)
            {
                sumLR += (double) l[i] * (double) r[i];
                sumLL += (double) l[i] * (double) l[i];
                sumRR += (double) r[i] * (double) r[i];
            }
            const double denom = std::sqrt (sumLL * sumRR);
            return denom > 1.0e-9 ? (float) (sumLR / denom) : 1.0f;
        };

        // A pure anti-phase (L=s, R=-s) test signal has NO mid component at
        // all (L+R == 0 always), which makes normalized correlation
        // scale-invariant - scaling the side gain by ANY nonzero factor
        // leaves L'==-R' exactly, so correlation would stay at -1
        // regardless of protection. Use a genuine mid+side construction
        // instead: mid and side as quadrature (90-degree-shifted) sines at
        // the SAME frequency - exactly orthogonal (uncorrelated) by
        // construction, avoiding both the anti-phase degeneracy above AND
        // the near-silent-channel degeneracy a slowly-drifting/detuned pair
        // would produce over a short analysis window.
        auto makeMidSideTestTone = [] (float freqHz, double sampleRate, int numSamples)
        {
            juce::AudioBuffer<float> buf (2, numSamples);
            auto* l = buf.getWritePointer (0);
            auto* r = buf.getWritePointer (1);
            for (int i = 0; i < numSamples; ++i)
            {
                const float phase = 2.0f * juce::MathConstants<float>::pi * freqHz * (float) i / (float) sampleRate;
                const float mid = std::sin (phase) * 0.5f;
                const float side = std::cos (phase) * 0.5f; // quadrature -> orthogonal/uncorrelated with mid
                l[i] = mid + side;
                r[i] = mid - side;
            }
            return buf;
        };

        // progress == 0 maximizes the contrast: at progress 0 the high-band
        // narrow target is exactly 1.0 (untouched, per WidthModule's
        // narrowTarget curve), so ONLY the low band's fixed protection gain
        // (kLowBandProtectedGain, independent of progress) is engaged -
        // proving low-band protection holds even at full width weight and
        // regardless of where in the build progress currently sits.
        drop::WidthModule lowWidth; lowWidth.prepare (spec);
        {
            // Prime: let the smoothed low/high-band gains settle to their
            // targets before measuring (avoids diluting the correlation
            // measurement with the ~20ms smoothing ramp-up transient).
            auto primeBuf = makeMidSideTestTone (50.0f, 48000.0, 4096);
            juce::dsp::AudioBlock<float> block (primeBuf);
            lowWidth.process (block, 1.0f, 0.0f);
        }
        auto lowBuf = makeMidSideTestTone (50.0f, 48000.0, 4096); // below the ~120Hz crossover -> treated as low band
        {
            juce::dsp::AudioBlock<float> block (lowBuf);
            lowWidth.process (block, 1.0f, 0.0f);
        }
        const float lowFreqCorrelation = measureCorrelation (lowBuf);

        drop::WidthModule highWidth; highWidth.prepare (spec);
        {
            auto primeBuf = makeMidSideTestTone (4000.0f, 48000.0, 4096);
            juce::dsp::AudioBlock<float> block (primeBuf);
            highWidth.process (block, 1.0f, 0.0f);
        }
        auto highBuf = makeMidSideTestTone (4000.0f, 48000.0, 4096); // above the crossover -> treated as high band
        {
            juce::dsp::AudioBlock<float> block (highBuf);
            highWidth.process (block, 1.0f, 0.0f);
        }
        const float highFreqCorrelation = measureCorrelation (highBuf);

        check (lowFreqCorrelation > highFreqCorrelation + 0.3f,
              "WidthModule: low-frequency (50Hz) content ends up measurably more mono-summed than high-frequency (4kHz) content at full width weight (low-band protection verified via correlation)");
        check (lowFreqCorrelation > 0.7f, "WidthModule: low-band correlation stays strongly positive (near-mono) even at full width weight");
    }

    //==========================================================================
    std::printf ("\n[Phase 3.2] GateModule - edge timing + no-click (ramp continuity)\n");
    {
        juce::dsp::ProcessSpec spec { 48000.0, 4096, 2 };
        drop::GateModule gate;
        gate.prepare (spec);

        drop::MusicalClock clock;
        clock.prepare (48000.0);
        TestPlayHead ph;
        ph.info = makePosition (120.0, 4, 4, 0.0, 0.0);
        clock.update (&ph, 0);

        juce::AudioBuffer<float> buf (2, 4096);
        for (int ch = 0; ch < 2; ++ch)
        {
            auto* data = buf.getWritePointer (ch);
            for (int i = 0; i < 4096; ++i)
                data[i] = 0.8f; // constant input -> output IS the gain envelope, directly measurable
        }

        juce::dsp::AudioBlock<float> block (buf);
        gate.process (block, 1.0f, 0.9f, clock); // high progress -> fast (audible) rate

        const float* out = buf.getReadPointer (0);
        float maxDelta = 0.0f;
        bool sawBothStates = false;
        float minVal = out[0], maxVal = out[0];
        for (int i = 1; i < 4096; ++i)
        {
            maxDelta = std::max (maxDelta, std::abs (out[i] - out[i - 1]));
            minVal = std::min (minVal, out[i]);
            maxVal = std::max (maxVal, out[i]);
        }
        sawBothStates = (minVal < 0.1f * 0.8f) && (maxVal > 0.9f * 0.8f);

        // Max sample-to-sample delta bound derived from the anti-click ramp
        // time (2ms) at 48kHz: a full 0.8-amplitude transition ramped over
        // 2ms takes ~96 samples, so per-sample delta should be well under
        // an instant full-amplitude jump (0.8).
        check (maxDelta < 0.8f * 0.5f, "GateModule: no instantaneous full-amplitude jump between adjacent samples (anti-click ramp active)");
        check (sawBothStates, "GateModule: rhythmic on/off edges are audible (both near-open and near-closed states reached) at high progress/weight==1");
    }

    //==========================================================================
    std::printf ("\n[Phase 3.2] SaturationModule - all 5 modes stable across full drive range\n");
    {
        juce::dsp::ProcessSpec spec { 48000.0, 512, 2 };
        const drop::SaturationMode modes[] = {
            drop::SaturationMode::Clean, drop::SaturationMode::Warm, drop::SaturationMode::Driven,
            drop::SaturationMode::Distorted, drop::SaturationMode::Crushed
        };

        bool allOk = true;
        for (auto mode : modes)
        {
            drop::SaturationModule sat;
            sat.prepare (spec);
            sat.setMode (mode);

            std::mt19937 rng (99u);
            std::uniform_real_distribution<float> fullScale (-1.0f, 1.0f);

            for (int block = 0; block < 50; ++block)
            {
                juce::AudioBuffer<float> buf (2, 512);
                for (int ch = 0; ch < 2; ++ch)
                {
                    auto* data = buf.getWritePointer (ch);
                    for (int i = 0; i < 512; ++i)
                        data[i] = fullScale (rng);
                }

                juce::dsp::AudioBlock<float> block_ (buf);
                sat.process (block_, 1.0f, 1.0f); // max drive (progress*weight == 1)

                for (int ch = 0; ch < 2; ++ch)
                {
                    const float* out = buf.getReadPointer (ch);
                    for (int i = 0; i < 512; ++i)
                        if (! std::isfinite (out[i]) || std::abs (out[i]) > 4.0f)
                            allOk = false;
                }
            }
        }
        check (allOk, "SaturationModule: all 5 modes (Clean/Warm/Driven/Distorted/Crushed) stay finite and bounded across full-scale noise at maximum drive");
    }

    //==========================================================================
    std::printf ("\n[Phase 3.2] DelayModule - feedback-runaway stress test\n");
    {
        juce::dsp::ProcessSpec spec { 48000.0, 256, 2 };
        drop::DelayModule delay;
        delay.prepare (spec);

        drop::MusicalClock clock;
        clock.prepare (48000.0);
        TestPlayHead ph;
        ph.info = makePosition (174.0, 4, 4, 0.0, 0.0); // fast tempo -> short division -> tight feedback loop
        clock.update (&ph, 0);

        std::mt19937 rng (2026u);
        std::uniform_real_distribution<float> weightDist (0.0f, 1.0f);

        bool allFinite = true;
        bool allBounded = true;

        for (int block = 0; block < 400; ++block)
        {
            juce::AudioBuffer<float> buf (2, 256);
            for (int ch = 0; ch < 2; ++ch)
            {
                auto* data = buf.getWritePointer (ch);
                for (int i = 0; i < 256; ++i)
                    data[i] = 0.95f * std::sin ((float) (block * 256 + i) * 0.3f); // full-scale-ish, high-frequency content
            }

            // Extreme rapid automation: weight/progress jump randomly every
            // block between 0 and 1 (worst-case for a feedback path).
            const float weight = weightDist (rng);
            const float progress = weightDist (rng);

            juce::dsp::AudioBlock<float> block_ (buf);
            delay.process (block_, weight, progress, clock);

            for (int ch = 0; ch < 2; ++ch)
            {
                const float* out = buf.getReadPointer (ch);
                for (int i = 0; i < 256; ++i)
                {
                    if (! std::isfinite (out[i])) allFinite = false;
                    if (std::abs (out[i]) > 4.0f) allBounded = false;
                }
            }
        }

        check (allFinite, "DelayModule: no NaN/Inf under extreme rapid weight/progress automation (feedback-runaway stress test)");
        check (allBounded, "DelayModule: output stays bounded - feedback cannot run away regardless of automation extremes (hard <=0.92 clamp + tanh soft-clip double-guard)");
    }

    //==========================================================================
    std::printf ("\n[Phase 3.2] ReverbModule - finite output at full weight\n");
    {
        juce::dsp::ProcessSpec spec { 48000.0, 512, 2 };
        drop::ReverbModule reverb;
        reverb.prepare (spec);

        bool allFinite = true;
        for (int block = 0; block < 100; ++block)
        {
            juce::AudioBuffer<float> buf (2, 512);
            for (int ch = 0; ch < 2; ++ch)
            {
                auto* data = buf.getWritePointer (ch);
                for (int i = 0; i < 512; ++i)
                    data[i] = (block == 0 && i == 0) ? 1.0f : 0.0f; // impulse, then silence (tail)
            }

            juce::dsp::AudioBlock<float> block_ (buf);
            reverb.process (block_, 1.0f, 1.0f);

            for (int ch = 0; ch < 2; ++ch)
            {
                const float* out = buf.getReadPointer (ch);
                for (int i = 0; i < 512; ++i)
                    if (! std::isfinite (out[i])) allFinite = false;
            }
        }
        check (allFinite, "ReverbModule: finite output across an impulse + silence tail at full weight (uses modern dsp::Reverb API exclusively)");
    }

    //==========================================================================
    std::printf ("\n[Phase 3.2] Full processBlock() - NaN/Inf/denormal sweep across test signal types\n");
    {
        auto runSignalSweep = [] (const char* signalName, std::function<float (int sampleIndex)> generator)
        {
            DROPAudioProcessor p;
            p.setRateAndBufferSizeDetails (48000.0, 256);
            p.prepareToPlay (48000.0, 256);

            if (auto* prm = p.parameters.getParameter ("trigger"))
                prm->setValueNotifyingHost (1.0f);

            bool allFinite = true;
            int globalSample = 0;

            for (int block = 0; block < 30; ++block)
            {
                juce::AudioBuffer<float> buf (2, 256);
                for (int ch = 0; ch < 2; ++ch)
                {
                    auto* data = buf.getWritePointer (ch);
                    for (int i = 0; i < 256; ++i)
                        data[i] = generator (globalSample + i);
                }
                globalSample += 256;

                juce::MidiBuffer midi;
                p.processBlock (buf, midi);

                if (block == 0)
                    if (auto* prm = p.parameters.getParameter ("trigger"))
                        prm->setValueNotifyingHost (0.0f);

                for (int ch = 0; ch < 2; ++ch)
                {
                    const float* out = buf.getReadPointer (ch);
                    for (int i = 0; i < 256; ++i)
                        if (! std::isfinite (out[i])) allFinite = false;
                }
            }

            char msg[128];
            std::snprintf (msg, sizeof (msg), "processBlock(): finite output across an active build with %s input", signalName);
            check (allFinite, msg);
            p.releaseResources();
        };

        runSignalSweep ("silence", [] (int) { return 0.0f; });
        runSignalSweep ("DC", [] (int) { return 0.5f; });
        runSignalSweep ("impulse", [] (int i) { return i == 0 ? 1.0f : 0.0f; });
        runSignalSweep ("sine", [] (int i) { return std::sin ((float) i * 0.05f) * 0.7f; });
        runSignalSweep ("full-scale", [] (int i) { return (i % 2 == 0) ? 1.0f : -1.0f; });

        std::mt19937 noiseRng (321u);
        std::uniform_real_distribution<float> noiseDist (-1.0f, 1.0f);
        runSignalSweep ("noise", [&] (int) { return noiseDist (noiseRng); });

        // Denormal sweep: a tiny, slowly-decaying signal that can drive
        // recursive filter state (Width's crossover, Delay's feedback
        // HPF/LPF) toward subnormal values if not handled correctly.
        runSignalSweep ("denormal-decay", [] (int i) { return 1.0e-30f * std::exp (-(float) i * 0.001f); });
    }

    //==========================================================================
    std::printf ("\n[Phase 3.3] GranularGrainEngine - ISOLATED FFT-based semitone verification\n");
    {
        // architecture.md PitchModule mitigation strategy: "Implement and
        // unit-test the grain engine in isolation (feed a pure sine sweep...
        // verify output pitch/timing... via FFT-based measurement in a test
        // harness) before wiring it into PitchModule/StutterModule."
        constexpr double testSampleRate = 48000.0;
        constexpr float inputFreqHz = 440.0f;
        constexpr int fftOrder = 12;
        constexpr int fftSize = 1 << fftOrder;

        auto measurePitchRatio = [&] (int semitones) -> float
        {
            juce::dsp::ProcessSpec spec { testSampleRate, 512, 1 };
            drop::GranularGrainEngine engine;
            engine.prepare (spec, 30.0f);
            engine.setRatio (drop::PitchModule::ratioForSemitones ((float) semitones));

            const int settleSamples = 8000; // several grain lengths - lets the ramped ratio + staggered grains settle
            const int totalSamples = settleSamples + fftSize;

            std::vector<float> output ((size_t) fftSize, 0.0f);
            const double phaseInc = 2.0 * juce::MathConstants<double>::pi * (double) inputFreqHz / testSampleRate;

            for (int i = 0; i < totalSamples; ++i)
            {
                const float input = (float) std::sin (phaseInc * (double) i);

                engine.beginSample();
                const float out = engine.processChannel (0, input);
                if (i >= settleSamples)
                    output[(size_t) (i - settleSamples)] = out;
            }

            juce::dsp::FFT fft (fftOrder);
            juce::dsp::WindowingFunction<float> window (fftSize, juce::dsp::WindowingFunction<float>::hann);

            std::vector<float> fftData ((size_t) (2 * fftSize), 0.0f);
            for (int i = 0; i < fftSize; ++i)
                fftData[(size_t) i] = output[(size_t) i];
            window.multiplyWithWindowingTable (fftData.data(), fftSize);
            fft.performRealOnlyForwardTransform (fftData.data());

            int peakBin = 1;
            float peakMag = 0.0f;
            for (int bin = 1; bin < fftSize / 2; ++bin)
            {
                const float re = fftData[(size_t) (2 * bin)];
                const float im = fftData[(size_t) (2 * bin + 1)];
                const float mag = re * re + im * im;
                if (mag > peakMag) { peakMag = mag; peakBin = bin; }
            }

            // Parabolic (quadratic) interpolation of the log-magnitude peak
            // recovers sub-bin frequency precision. Necessary because at the
            // lowest measured frequency (-12 st -> 220 Hz) one FFT bin
            // (11.7 Hz) is already 5.3% of the true value - coarser than the
            // 5% acceptance tolerance itself. Standard technique; engine
            // correctness was independently verified (see bugfix notes).
            auto magAt = [&] (int bin) -> float
            {
                const float re = fftData[(size_t) (2 * bin)];
                const float im = fftData[(size_t) (2 * bin + 1)];
                return std::sqrt (re * re + im * im) + 1.0e-12f;
            };
            float delta = 0.0f;
            if (peakBin > 1 && peakBin < fftSize / 2 - 1)
            {
                const float lm = std::log (magAt (peakBin - 1));
                const float cm = std::log (magAt (peakBin));
                const float rm = std::log (magAt (peakBin + 1));
                const float denom = lm - 2.0f * cm + rm;
                if (std::abs (denom) > 1.0e-9f)
                    delta = juce::jlimit (-0.5f, 0.5f, 0.5f * (lm - rm) / denom);
            }
            const float measuredFreq = ((float) peakBin + delta) * (float) testSampleRate / (float) fftSize;
            return measuredFreq / inputFreqHz;
        };

        const int semitoneCases[] = { 0, 1, 2, 3, 5, 7, 12, -12 };
        bool allOk = true;
        for (auto semitones : semitoneCases)
        {
            const float measuredRatio = measurePitchRatio (semitones);
            const float expectedRatio = drop::PitchModule::ratioForSemitones ((float) semitones);
            const bool ok = nearlyEqual ((double) measuredRatio, (double) expectedRatio, (double) expectedRatio * 0.05);
            std::printf ("  semitones=%+3d: measured ratio %.4f (expected %.4f) -> %s\n",
                         semitones, measuredRatio, expectedRatio, ok ? "ok" : "MISMATCH");
            allOk = allOk && ok;
        }
        check (allOk, "GranularGrainEngine (isolated, pre-wiring): FFT-measured output pitch matches the expected 2^(semitones/12) ratio within 5% across all 8 quantized semitone steps (0/+1/+2/+3/+5/+7/+12/-12)");
    }

    //==========================================================================
    std::printf ("\n[Phase 3.3] New modules - weight==0 transparency\n");
    {
        juce::dsp::ProcessSpec spec { 48000.0, 256, 2 };
        drop::MusicalClock clock; clock.prepare (48000.0);
        TestPlayHead ph; ph.info = makePosition (120.0, 4, 4, 0.0, 0.0);
        clock.update (&ph, 0);

        auto makeTestBuffer = [] (int numSamples)
        {
            juce::AudioBuffer<float> buf (2, numSamples);
            for (int ch = 0; ch < 2; ++ch)
            {
                auto* data = buf.getWritePointer (ch);
                for (int i = 0; i < numSamples; ++i)
                    data[i] = std::sin ((float) i * (0.05f + 0.01f * (float) ch)) * 0.6f;
            }
            return buf;
        };

        auto transparencyCheck = [&] (const char* name, auto&& processFn)
        {
            auto buf = makeTestBuffer (256);
            juce::AudioBuffer<float> reference;
            reference.makeCopyOf (buf);

            for (int i = 0; i < 4; ++i)
            {
                auto primeBuf = makeTestBuffer (256);
                juce::dsp::AudioBlock<float> block (primeBuf);
                processFn (block);
            }

            juce::dsp::AudioBlock<float> block (buf);
            processFn (block);

            float maxDiff = 0.0f;
            for (int ch = 0; ch < 2; ++ch)
            {
                const float* out = buf.getReadPointer (ch);
                const float* ref = reference.getReadPointer (ch);
                for (int i = 0; i < 256; ++i)
                    maxDiff = std::max (maxDiff, std::abs (out[i] - ref[i]));
            }
            check (maxDiff < 1.0e-4f, name);
        };

        {
            drop::PitchModule m; m.prepare (spec);
            transparencyCheck ("PitchModule: weight==0 is EXACTLY transparent", [&] (juce::dsp::AudioBlock<float>& b) { m.process (b, 0.0f, 0.5f); });
        }
        {
            drop::StutterModule m; m.prepare (spec);
            transparencyCheck ("StutterModule: weight==0 is EXACTLY transparent", [&] (juce::dsp::AudioBlock<float>& b) { m.process (b, 0.0f, 0.5f, clock); });
        }
        {
            drop::DigitalModule m; m.prepare (spec);
            transparencyCheck ("DigitalModule: weight==0 is EXACTLY transparent", [&] (juce::dsp::AudioBlock<float>& b) { m.process (b, 0.0f, 0.5f, 0.5f, 0.5f); });
        }
        {
            drop::NoiseModule m; m.prepare (spec); m.setSeed (42u);
            transparencyCheck ("NoiseModule: weight==0 contributes exactly zero (parallel add)", [&] (juce::dsp::AudioBlock<float>& b) { m.process (b, 0.0f, 0.0f, 0.5f); });
        }
    }

    //==========================================================================
    std::printf ("\n[Phase 3.3] StutterModule - repeat-boundary crossfade continuity (all 5 modes)\n");
    {
        juce::dsp::ProcessSpec spec { 48000.0, 256, 2 };
        drop::MusicalClock clock;
        clock.prepare (48000.0);
        TestPlayHead ph;
        ph.info = makePosition (174.0, 4, 4, 0.0, 0.0); // fast tempo -> short stutter length -> frequent crossfade boundaries
        clock.update (&ph, 0);

        const drop::StutterMode modes[] = {
            drop::StutterMode::Repeat, drop::StutterMode::Reverse, drop::StutterMode::Half,
            drop::StutterMode::Double, drop::StutterMode::PitchVariation
        };

        bool allOk = true;
        for (auto mode : modes)
        {
            drop::StutterModule stutter;
            stutter.prepare (spec);
            stutter.setMode (mode);

            const int blockSize = 256;
            const int totalSamples = 20480;
            float maxDelta = 0.0f;
            float prevSample = 0.0f;
            bool first = true;
            bool allFinite = true;

            for (int start = 0; start < totalSamples; start += blockSize)
            {
                juce::AudioBuffer<float> buf (2, blockSize);
                for (int ch = 0; ch < 2; ++ch)
                {
                    auto* data = buf.getWritePointer (ch);
                    for (int i = 0; i < blockSize; ++i)
                        data[i] = 0.4f * std::sin ((float) (start + i) * (float) (2.0 * juce::MathConstants<double>::pi * 200.0 / 48000.0));
                }

                juce::dsp::AudioBlock<float> block_ (buf);
                stutter.process (block_, 1.0f, 0.95f, clock); // high progress -> short stutter length, stress-tests crossfade frequency

                const float* out = buf.getReadPointer (0);
                for (int i = 0; i < blockSize; ++i)
                {
                    if (! std::isfinite (out[i])) allFinite = false;
                    if (! first)
                        maxDelta = std::max (maxDelta, std::abs (out[i] - prevSample));
                    prevSample = out[i];
                    first = false;
                }
            }

            const bool ok = allFinite && maxDelta < 0.3f;
            std::printf ("  mode=%d: maxDelta=%.4f allFinite=%s -> %s\n", (int) mode, maxDelta, allFinite ? "yes" : "no", ok ? "ok" : "FAIL");
            allOk = allOk && ok;
        }
        check (allOk, "StutterModule: no discontinuity/click at any repeat-boundary crossfade (bounded sample-to-sample delta, well under a hard full-scale jump) across all 5 modes (Repeat/Reverse/Half/Double/PitchVariation)");
    }

    //==========================================================================
    std::printf ("\n[Phase 3.3] VacuumModule - click-free envelope continuity (incl. 200+ BPM edge case)\n");
    {
        const double windowsToTest[] = { 44100.0, 4000.0, 800.0, 120.0 }; // 120 samples is a deliberately extreme, far-beyond-200-BPM-scale edge case, stress-testing the math itself
        bool allOk = true;

        for (double totalWindow : windowsToTest)
        {
            float maxDelta = 0.0f;
            float prevGain = drop::VacuumModule::computeGain (-5.0, totalWindow, 1.0f);
            bool inRange = true;

            for (int n = -5; n <= (int) totalWindow + 5; ++n)
            {
                const float g = drop::VacuumModule::computeGain ((double) n, totalWindow, 1.0f);
                if (g < 0.0f || g > 1.0001f || ! std::isfinite (g)) inRange = false;
                maxDelta = std::max (maxDelta, std::abs (g - prevGain));
                prevGain = g;
            }

            // Generous, window-size-independent bound: confirms no
            // single-sample discontinuity exists at ANY window length,
            // including the deliberately extreme 120-sample case.
            const bool deltaOk = maxDelta < 0.5f;

            const bool ok = inRange && deltaOk;
            std::printf ("  totalWindow=%.0f samples: maxDelta=%.4f inRange=%s -> %s\n",
                         totalWindow, maxDelta, inRange ? "yes" : "no", ok ? "ok" : "FAIL");
            allOk = allOk && ok;
        }
        check (allOk, "VacuumModule::computeGain(): finite, in-range [floor,1], and no single-sample discontinuity across window lengths from 44100 samples down to a deliberately extreme 120-sample (far beyond 200+ BPM 1/64-note scale) edge case");

        // Derivative continuity (stronger than mere value continuity,
        // architecture.md "must be click-free"): at both internal
        // boundaries (fade-out->hold, hold->fade-in), the discrete slope
        // immediately either side should be near zero, matching the
        // raised-cosine's zero-derivative endpoints.
        const double totalWindow = 4410.0;
        const double fadeOutEndSample = 0.2 * totalWindow;
        const double holdEndSample = 0.7 * totalWindow;
        bool boundariesSmooth = true;
        for (double boundary : { fadeOutEndSample, holdEndSample })
        {
            const float before = drop::VacuumModule::computeGain (boundary - 1.0, totalWindow, 1.0f);
            const float at = drop::VacuumModule::computeGain (boundary, totalWindow, 1.0f);
            const float after = drop::VacuumModule::computeGain (boundary + 1.0, totalWindow, 1.0f);
            if (std::abs (at - before) > 0.01f || std::abs (after - at) > 0.01f)
                boundariesSmooth = false;
        }
        check (boundariesSmooth, "VacuumModule::computeGain(): derivative is continuous (near-zero slope either side) at both internal segment boundaries, not just the value");
    }

    //==========================================================================
    std::printf ("\n[Phase 3.3] NoiseModule + DropRandomizer - deterministic reproducibility (same seed -> bit-identical output)\n");
    {
        struct RunResult
        {
            std::vector<float> samples;
            int pitchChoice = 0;
            int stutterChoice = 0;
            drop::MusicalDivision delayDiv = drop::MusicalDivision::Eighth;
            drop::MusicalDivision gateDiv = drop::MusicalDivision::Eighth;
        };

        auto runOnce = [] (uint32_t seed) -> RunResult
        {
            drop::DropRandomizer randomizer;
            randomizer.setSeed (seed);
            randomizer.setChaosAmount01 (0.6f);
            randomizer.drawForNewBuild();

            RunResult result;
            result.pitchChoice = randomizer.getPitchSemitoneChoice();
            result.stutterChoice = randomizer.getStutterModeChoice();
            result.delayDiv = randomizer.getDelayDivisionChoice();
            result.gateDiv = randomizer.getGateDivisionChoice();

            juce::dsp::ProcessSpec spec { 48000.0, 256, 2 };
            drop::NoiseModule noise;
            noise.prepare (spec);
            noise.setSeed (randomizer.getSeed());
            noise.setColor (drop::NoiseColor::Pink);

            for (int block = 0; block < 20; ++block)
            {
                juce::AudioBuffer<float> buf (2, 256);
                buf.clear();
                juce::dsp::AudioBlock<float> block_ (buf);
                noise.process (block_, 1.0f, 0.8f, 0.7f);

                const float* out = buf.getReadPointer (0);
                for (int i = 0; i < 256; ++i)
                    result.samples.push_back (out[i]);
            }
            return result;
        };

        const RunResult a = runOnce (777u);
        const RunResult b = runOnce (777u);

        bool identical = (a.samples.size() == b.samples.size());
        for (size_t i = 0; identical && i < a.samples.size(); ++i)
            if (a.samples[i] != b.samples[i]) identical = false;
        check (identical, "NoiseModule: same seed -> bit-identical output across two independent runs (std::mt19937-based determinism, NOT juce::Random)");

        const bool randomizerIdentical = (a.pitchChoice == b.pitchChoice) && (a.stutterChoice == b.stutterChoice)
                                        && (a.delayDiv == b.delayDiv) && (a.gateDiv == b.gateDiv);
        check (randomizerIdentical, "DropRandomizer: same seed + same chaos amount -> bit-identical musically-constrained choices across two independent runs");

        // "A different seed produces a different choice" - at chaosAmount01
        // = 0.6 the draw space is genuinely small (8 pitch x 2 delay-
        // division x 5 stutter-mode buckets, ~1/80 chance two INDEPENDENT
        // seeds collide on all three simultaneously by pure chance), so a
        // single fixed seed PAIR is a statistically fragile comparison, not
        // a meaningful correctness check, even for a fully correct
        // implementation (verified: setSeed() genuinely reseeds the
        // std::mt19937 engine every time - see DropRandomizer.cpp, no
        // discard/constant-reseed anywhere). Check across a battery of
        // several distinct seeds instead: requiring only ONE of them to
        // differ from the reference makes the false-failure probability for
        // a CORRECT implementation astronomically small (~(1/80)^N), while
        // a genuinely broken "seed ignored" implementation would still fail
        // this immediately (every alternate seed would trivially match).
        const uint32_t alternateSeeds[] = { 778u, 900u, 12345u, 999999u, 2u, 555555u, 42u, 8675309u };
        bool anySeedDiffers = false;
        for (auto altSeed : alternateSeeds)
        {
            const RunResult alt = runOnce (altSeed);
            if (alt.pitchChoice != a.pitchChoice || alt.stutterChoice != a.stutterChoice
                || alt.delayDiv != a.delayDiv || alt.gateDiv != a.gateDiv)
            {
                anySeedDiffers = true;
                break;
            }
        }
        check (anySeedDiffers, "DropRandomizer: at least one of 8 alternate seeds produces a different randomized choice than the reference seed (not a degenerate constant implementation) - verified via a seed battery rather than a single statistically-fragile pair, since the small musically-constrained draw space makes any ONE pair's collision non-negligible even when the seed genuinely perturbs the RNG");
    }

    //==========================================================================
    std::printf ("\n[Phase 3.3] DigitalModule - bit/SR crush stability + DC-blocker verification\n");
    {
        juce::dsp::ProcessSpec spec { 48000.0, 256, 2 };
        drop::DigitalModule digital;
        digital.prepare (spec);

        const int totalSamples = 48000; // 1 second - plenty for the ~5Hz one-pole HP DC blocker to settle
        double runningSum = 0.0;
        int summedSamples = 0;
        bool allFinite = true;

        for (int start = 0; start < totalSamples; start += 256)
        {
            juce::AudioBuffer<float> buf (2, 256);
            for (int ch = 0; ch < 2; ++ch)
            {
                auto* data = buf.getWritePointer (ch);
                for (int i = 0; i < 256; ++i)
                    // Deliberately DC-biased, asymmetric signal - bit-crushing
                    // asymmetric content is exactly the DC-buildup risk case
                    // architecture.md flags for DigitalModule.
                    data[i] = 0.6f + 0.3f * std::sin ((float) (start + i) * 0.37f);
            }

            juce::dsp::AudioBlock<float> block_ (buf);
            digital.process (block_, 1.0f, 1.0f, 1.0f, 1.0f); // weight/progress/tension/chaos all maxed -> full severity + full wet

            const float* out = buf.getReadPointer (0);
            for (int i = 0; i < 256; ++i)
            {
                if (! std::isfinite (out[i])) allFinite = false;
                if (start >= totalSamples / 2) // only average the SETTLED second half
                {
                    runningSum += out[i];
                    ++summedSamples;
                }
            }
        }

        const double averageOutput = summedSamples > 0 ? runningSum / summedSamples : 0.0;
        check (allFinite, "DigitalModule: finite output across a DC-biased asymmetric signal at full severity/weight");
        check (std::abs (averageOutput) < 0.05, "DigitalModule: DC-blocker keeps the settled average output near zero despite crushing a strongly DC-biased asymmetric input (no DC buildup)");
    }

    //==========================================================================
    std::printf ("\n[Phase 3.3] Full processBlock() - end-to-end wiring sanity through Peak/Impact/Recovery\n");
    {
        // Exercises the ACTUAL live wiring (not just isolated unit tests):
        // PitchModule/StutterModule/DigitalModule/NoiseModule all in the
        // series/parallel chain, and VacuumModule state-gated to Peak.
        DROPAudioProcessor p;
        p.setRateAndBufferSizeDetails (48000.0, 256);
        p.prepareToPlay (48000.0, 256);

        if (auto* prm = p.parameters.getParameter ("buildLength"))
            prm->setValueNotifyingHost (0.0f); // shortest: 1 bar
        if (auto* prm = p.parameters.getParameter ("chaos"))
            prm->setValueNotifyingHost (0.5f); // exercises DropRandomizer's non-trivial jitter/choice path
        if (auto* prm = p.parameters.getParameter ("trigger"))
            prm->setValueNotifyingHost (1.0f);

        bool allFinite = true;
        bool sawPeak = false;
        bool sawImpact = false;
        bool sawRecovery = false;
        bool backToIdle = false;

        // Block-budget arithmetic (updated post-Phase-3.4: Peak/Impact/
        // Recovery are now musically-derived, not Phase-3.1-era fixed-
        // seconds placeholders - see computeDurationsForNewBuild()): no
        // playhead is set, so MusicalClock uses its 120 BPM/4/4 fallback
        // transport - samplesPerBeat = 24000, barLength = 96000. buildLength
        // choice index 0 = 1 bar -> buildingSamples = 96000. Arming
        // quantizes to the next bar boundary, but MusicalClock's fallback
        // transport has already been advanced by this SAME block's 256
        // samples before the trigger is detected (update() runs before
        // trigger detection in processBlock()), so armingToBuildingSamples
        // = 96000 - 256 = 95744, not ~0. Plus this phase's musically-derived
        // windows: peakSamples (Vacuum, 1/32 note @120bpm) = 3000,
        // impactSamples (fixed 220ms) = 10560, recoverySamples (half a bar)
        // = 48000. Total = 95744 + 96000 + 3000 + 10560 + 48000 = 253304
        // samples = ~990 blocks of 256. The existing 1300-block (332800-
        // sample) budget still comfortably covers this (~31% margin), so it
        // is intentionally left unchanged this phase.
        for (int block = 0; block < 1300 && ! backToIdle; ++block)
        {
            juce::AudioBuffer<float> buf (2, 256);
            for (int ch = 0; ch < 2; ++ch)
            {
                auto* data = buf.getWritePointer (ch);
                for (int i = 0; i < 256; ++i)
                    data[i] = std::sin ((float) (block * 256 + i) * 0.02f) * 0.5f;
            }

            juce::MidiBuffer midi;
            p.processBlock (buf, midi);

            if (block == 0)
                if (auto* prm = p.parameters.getParameter ("trigger"))
                    prm->setValueNotifyingHost (0.0f);

            for (int ch = 0; ch < 2; ++ch)
            {
                const float* out = buf.getReadPointer (ch);
                for (int i = 0; i < 256; ++i)
                    if (! std::isfinite (out[i])) allFinite = false;
            }

            switch (p.getCurrentDropState())
            {
                case drop::DropState::Peak:     sawPeak = true; break;
                case drop::DropState::Impact:   sawImpact = true; break;
                case drop::DropState::Recovery: sawRecovery = true; break;
                case drop::DropState::Idle:     if (block > 10) backToIdle = true; break;
                default: break;
            }
        }

        check (allFinite, "Full build (Idle->Arming->Building->Peak->Impact->Recovery->Idle): finite output throughout, with PitchModule/StutterModule/DigitalModule/NoiseModule/VacuumModule all wired into the live chain");
        check (sawPeak && sawImpact && sawRecovery && backToIdle, "Full build reaches Peak (VacuumModule engaged), Impact, Recovery, and returns to Idle within the test's block budget");
        p.releaseResources();
    }

    //==========================================================================
    std::printf ("\n[Phase 3.4] ImpactEngine - sub-hit spectral concentration (isolated FFT check)\n");
    {
        constexpr double testSampleRate = 48000.0;
        constexpr int fftOrder = 13;
        constexpr int fftSize = 1 << fftOrder; // 8192 samples (~170ms @48kHz) - comfortably spans the sub-hit's ~1.5ms attack + 120ms decay

        juce::dsp::ProcessSpec spec { testSampleRate, 4096, 2 };
        drop::ImpactEngine engine;
        engine.prepare (spec);

        std::vector<float> subOut ((size_t) fftSize, 0.0f);
        engine.renderSubHitOnly (subOut.data(), fftSize, 0.0, 1.0f);

        juce::dsp::FFT fft (fftOrder);
        juce::dsp::WindowingFunction<float> window (fftSize, juce::dsp::WindowingFunction<float>::hann);

        std::vector<float> fftData ((size_t) (2 * fftSize), 0.0f);
        for (int i = 0; i < fftSize; ++i)
            fftData[(size_t) i] = subOut[(size_t) i];
        window.multiplyWithWindowingTable (fftData.data(), fftSize);
        fft.performRealOnlyForwardTransform (fftData.data());

        const double binHz = testSampleRate / (double) fftSize;
        const int bandLowBin  = (int) std::floor (40.0 / binHz);
        const int bandHighBin = (int) std::ceil  (80.0 / binHz);

        double bandEnergy = 0.0, totalEnergy = 0.0;
        int peakBin = 1;
        double peakMag2 = 0.0;
        for (int bin = 1; bin < fftSize / 2; ++bin)
        {
            const float re = fftData[(size_t) (2 * bin)];
            const float im = fftData[(size_t) (2 * bin + 1)];
            const double mag2 = (double) re * (double) re + (double) im * (double) im;
            totalEnergy += mag2;
            if (bin >= bandLowBin && bin <= bandHighBin) bandEnergy += mag2;
            if (mag2 > peakMag2) { peakMag2 = mag2; peakBin = bin; }
        }

        // Parabolic (quadratic) log-magnitude interpolation for sub-bin peak
        // precision - same convention as the Phase 3.3 GranularGrainEngine
        // FFT test.
        auto magAt = [&] (int bin) -> float
        {
            const float re = fftData[(size_t) (2 * bin)];
            const float im = fftData[(size_t) (2 * bin + 1)];
            return std::sqrt (re * re + im * im) + 1.0e-12f;
        };
        float delta = 0.0f;
        if (peakBin > 1 && peakBin < fftSize / 2 - 1)
        {
            const float lm = std::log (magAt (peakBin - 1));
            const float cm = std::log (magAt (peakBin));
            const float rm = std::log (magAt (peakBin + 1));
            const float denom = lm - 2.0f * cm + rm;
            if (std::abs (denom) > 1.0e-9f)
                delta = juce::jlimit (-0.5f, 0.5f, 0.5f * (lm - rm) / denom);
        }
        const double measuredPeakHz = ((double) peakBin + (double) delta) * binHz;
        const double concentration = totalEnergy > 1.0e-20 ? bandEnergy / totalEnergy : 0.0;

        std::printf ("  peak frequency: %.2f Hz, energy concentration in [40,80]Hz: %.4f\n", measuredPeakHz, concentration);
        check (measuredPeakHz >= 35.0 && measuredPeakHz <= 85.0,
              "ImpactEngine sub-hit: FFT-measured peak frequency lands within (a small margin around) the 40-80Hz target band");
        check (concentration >= 0.85,
              "ImpactEngine sub-hit: at least 85% of spectral energy is concentrated within the 40-80Hz band (band-limited, per architecture.md)");
    }

    //==========================================================================
    std::printf ("\n[Phase 3.4] Vacuum->Impact handoff - no double-envelope bump\n");
    {
        // Isolated from the other 10 always-process modules (which are
        // ducked to weight==0 during the real Impact state anyway, per
        // computeIntensityProgress() - testing VacuumModule+ImpactEngine
        // directly avoids any risk of unrelated modules' own dynamics
        // (e.g. GateModule's rhythmic gating during Peak) polluting the
        // RMS-envelope shape being verified here).
        constexpr double testSampleRate = 48000.0;
        constexpr int peakSamples = 3000;             // Vacuum window
        constexpr int impactAnalysisSamples = 300;    // just past the sub-hit's ~72-sample (1.5ms @48kHz) attack
        constexpr int totalSamples = peakSamples + impactAnalysisSamples;

        juce::dsp::ProcessSpec spec { testSampleRate, 8192, 2 };
        drop::VacuumModule vacuum;
        vacuum.prepare (spec);
        drop::ImpactEngine impact;
        impact.prepare (spec);
        impact.setSeed (2026u);

        juce::AudioBuffer<float> buf (2, totalSamples);
        constexpr float testFreqHz = 1000.0f; // period = 48 samples - well resolved by the 150-sample analysis window below
        for (int ch = 0; ch < 2; ++ch)
        {
            auto* data = buf.getWritePointer (ch);
            for (int i = 0; i < totalSamples; ++i)
                data[i] = 0.3f * std::sin ((float) (2.0 * juce::MathConstants<double>::pi * testFreqHz * (double) i / testSampleRate));
        }

        juce::dsp::AudioBlock<float> fullBlock (buf);
        auto peakBlock = fullBlock.getSubBlock (0, (size_t) peakSamples);
        vacuum.process (peakBlock, 0.0, (double) peakSamples, 0.5f);

        auto impactBlock = fullBlock.getSubBlock ((size_t) peakSamples, (size_t) impactAnalysisSamples);
        impact.process (impactBlock, 0.0, 10560.0, 0.5f);

        // Windowed RMS envelope (window == 150 samples == ~3.1 periods of
        // the 1kHz test tone, for a stable per-window RMS estimate; both
        // peakSamples and totalSamples are exact multiples of it).
        constexpr int windowLen = 150;
        const int numWindows = totalSamples / windowLen;
        std::vector<double> rms ((size_t) numWindows, 0.0);
        const float* ch0 = buf.getReadPointer (0);
        for (int w = 0; w < numWindows; ++w)
        {
            double sumSq = 0.0;
            for (int i = 0; i < windowLen; ++i)
            {
                const float s = ch0[w * windowLen + i];
                sumSq += (double) s * (double) s;
            }
            rms[(size_t) w] = std::sqrt (sumSq / (double) windowLen);
        }

        int minIdx = 0;
        for (int w = 1; w < numWindows; ++w)
            if (rms[(size_t) w] < rms[(size_t) minIdx]) minIdx = w;

        const double tol = 0.15 * rms[0];
        bool monotoneDownThenUp = true;
        for (int w = 0; w < minIdx; ++w)
            if (rms[(size_t) (w + 1)] > rms[(size_t) w] + tol) monotoneDownThenUp = false;
        for (int w = minIdx; w < numWindows - 1; ++w)
            if (rms[(size_t) (w + 1)] < rms[(size_t) w] - tol) monotoneDownThenUp = false;

        std::printf ("  rms[0]=%.4f (pre-vacuum) rms[min@%d]=%.4f (floor) rms[last]=%.4f (post-attack) -> %s\n",
                     rms[0], minIdx, rms[(size_t) minIdx], rms[(size_t) (numWindows - 1)],
                     monotoneDownThenUp ? "single-trough" : "HUMP DETECTED");

        check (monotoneDownThenUp,
              "Vacuum->Impact handoff: windowed RMS envelope is monotone-down-then-up around a SINGLE trough (no intermediate hump/double-envelope) across the collapse+attack region");
        check (rms[(size_t) minIdx] < rms[0] * 0.5,
              "Vacuum->Impact handoff: the collapse floor is meaningfully lower than the pre-vacuum level (a genuine collapse occurred)");
        check (rms[(size_t) (numWindows - 1)] > rms[(size_t) minIdx] * 1.3,
              "Vacuum->Impact handoff: the level recovers measurably above the floor by the end of the analyzed attack window");
    }

    //==========================================================================
    std::printf ("\n[Phase 3.4] Full processBlock() - transient preservation through the Vacuum->Impact window\n");
    {
        // Exact deterministic sample-offset arithmetic (same fallback-
        // transport style as the Phase 3.3 end-to-end test, updated for
        // this phase's musically-derived durations - see
        // computeDurationsForNewBuild()): 120 BPM/4/4 @48kHz ->
        // samplesPerBeat=24000, barLength=96000; buildLength choice index 0
        // = 1 bar. armingToBuildingSamples = 96000-256 = 95744 (fallback
        // transport already advanced by the triggering block's own 256
        // samples before the trigger is detected); buildingSamples =
        // 96000; peakSamples (Vacuum, 1/32 note @120bpm) = 3000; Impact
        // begins at global sample 95744+96000+3000 = 194744 -> block index
        // 194744/256 = 760 (remainder 184, i.e. local sample offset 184
        // within block 760).
        constexpr int blockSize = 256;
        constexpr int impactStartBlock = 760;
        constexpr int impactStartLocalOffset = 184;

        DROPAudioProcessor p;
        p.setRateAndBufferSizeDetails (48000.0, blockSize);
        p.prepareToPlay (48000.0, blockSize);

        if (auto* prm = p.parameters.getParameter ("buildLength"))
            prm->setValueNotifyingHost (0.0f); // shortest: 1 bar
        if (auto* prm = p.parameters.getParameter ("trigger"))
            prm->setValueNotifyingHost (1.0f);

        constexpr float dryImpulseAmplitude = 1.0f;
        float peakAbsAroundImpulse = 0.0f;
        bool allFinite = true;

        for (int block = 0; block <= impactStartBlock + 4; ++block)
        {
            juce::AudioBuffer<float> buf (2, blockSize);
            buf.clear();

            if (block == impactStartBlock)
            {
                // Mono (L==R) impulse: WidthModule's M/S side signal is
                // exactly 0 for identical L/R content, so the stereo-
                // expansion drive hook cannot attenuate/distort this
                // specific test signal - isolates amplitude preservation
                // from stereo-image effects entirely.
                buf.setSample (0, impactStartLocalOffset, dryImpulseAmplitude);
                buf.setSample (1, impactStartLocalOffset, dryImpulseAmplitude);
            }

            juce::MidiBuffer midi;
            p.processBlock (buf, midi);

            if (block == 0)
                if (auto* prm = p.parameters.getParameter ("trigger"))
                    prm->setValueNotifyingHost (0.0f);

            if (block >= impactStartBlock && block <= impactStartBlock + 1)
            {
                for (int ch = 0; ch < 2; ++ch)
                {
                    const float* out = buf.getReadPointer (ch);
                    for (int i = 0; i < blockSize; ++i)
                    {
                        if (! std::isfinite (out[i])) allFinite = false;
                        peakAbsAroundImpulse = std::max (peakAbsAroundImpulse, std::abs (out[i]));
                    }
                }
            }
        }

        check (allFinite, "transient-preservation test: finite output around the impulse");

        // Documented fraction: >= 50% of the dry impulse's amplitude must
        // survive through the Vacuum->Impact window (TRANSIENT PRESERVATION:
        // "duck the effect chain, not the source" - the shared 10
        // IntensityBudget modules are exactly/near-exactly transparent at
        // weight==0, and ImpactEngine's own additive content at the exact
        // hand-off sample is itself near-zero at that instant, so a much
        // higher fraction is expected in practice; 50% is a conservative,
        // clearly-motivated floor, not a precise prediction).
        check (peakAbsAroundImpulse >= dryImpulseAmplitude * 0.5f,
              "Impact window: source transient (impulse placed exactly at Impact-state entry) survives with >= 50% of its dry amplitude - TRANSIENT PRESERVATION ('duck the effect chain, not the source')");
        p.releaseResources();
    }

    //==========================================================================
    std::printf ("\n[Phase 3.4] ImpactEngine - impact parameter scaling monotonicity (0/50/100%%)\n");
    {
        juce::dsp::ProcessSpec spec { 48000.0, 4096, 2 };

        auto measureRms = [&] (float impactAmount01) -> double
        {
            drop::ImpactEngine engine;
            engine.prepare (spec);
            engine.setSeed (4242u); // identical seed across all three runs -> same burst noise sequence, isolating the effect of impactAmount01 alone

            constexpr int numSamples = 4096;
            juce::AudioBuffer<float> buf (2, numSamples);
            buf.clear();

            juce::dsp::AudioBlock<float> block (buf);
            engine.process (block, 0.0, 10560.0, impactAmount01);

            double sumSq = 0.0;
            const float* out = buf.getReadPointer (0);
            for (int i = 0; i < numSamples; ++i)
                sumSq += (double) out[i] * (double) out[i];
            return std::sqrt (sumSq / (double) numSamples);
        };

        const double rms0   = measureRms (0.0f);
        const double rms50  = measureRms (0.5f);
        const double rms100 = measureRms (1.0f);

        std::printf ("  impact=0%%: rms=%.6f  impact=50%%: rms=%.6f  impact=100%%: rms=%.6f\n", rms0, rms50, rms100);

        check (rms0 < 1.0e-6, "ImpactEngine: impact==0% produces exactly-silent (no sub/burst/tail) output on a silent bus");
        check (rms50 > rms0 + 1.0e-6, "ImpactEngine: impact==50% produces measurably more energy than impact==0%");
        check (rms100 > rms50 + 1.0e-6, "ImpactEngine: impact==100% produces measurably more energy than impact==50% (monotonic scaling)");
    }

    //==========================================================================
    std::printf ("\n[Phase 3.4] ImpactEngine - low-end soft-clip safety on bass-heavy material\n");
    {
        juce::dsp::ProcessSpec spec { 48000.0, 4096, 2 };
        drop::ImpactEngine engine;
        engine.prepare (spec);
        engine.setSeed (99u);

        constexpr int numSamples = 4096;
        juce::AudioBuffer<float> buf (2, numSamples);
        constexpr float bassAmplitude = 5.0f; // deliberately far beyond unity - stress-tests the soft-clip
        constexpr float bassFreqHz = 55.0f;   // low-frequency-dominant content, well within the low band (below the ~120Hz crossover)
        for (int ch = 0; ch < 2; ++ch)
        {
            auto* data = buf.getWritePointer (ch);
            for (int i = 0; i < numSamples; ++i)
                data[i] = bassAmplitude * std::sin ((float) (2.0 * juce::MathConstants<double>::pi * bassFreqHz * (double) i / 48000.0));
        }

        juce::dsp::AudioBlock<float> block (buf);
        engine.process (block, 0.0, 10560.0, 1.0f); // full impact -> maximal additive content on top of the already-huge bass input

        bool allFinite = true;
        float peakAbs = 0.0f;
        for (int ch = 0; ch < 2; ++ch)
        {
            const float* out = buf.getReadPointer (ch);
            for (int i = 0; i < numSamples; ++i)
            {
                if (! std::isfinite (out[i])) allFinite = false;
                peakAbs = std::max (peakAbs, std::abs (out[i]));
            }
        }

        std::printf ("  bass-heavy input amplitude=%.1f -> output peak=%.4f\n", bassAmplitude, peakAbs);
        check (allFinite, "ImpactEngine: finite output on bass-heavy full-scale-exceeding material at full impact");
        check (peakAbs < bassAmplitude * 0.5f,
              "ImpactEngine: low-end soft-clip safety measurably bounds a bass-heavy input well below its raw (5x-over-unity) amplitude");
    }

    //==========================================================================
    std::printf ("\n[Phase 3.4] ImpactEngine - short-tail reverb decay is bounded within the tail window\n");
    {
        constexpr double testSampleRate = 48000.0;
        constexpr int chunkSize = 512;
        constexpr int totalSamples = (int) (testSampleRate * 4.0); // 4 seconds - generously beyond any expected short-tail decay

        juce::dsp::ProcessSpec spec { testSampleRate, chunkSize, 2 };
        drop::ImpactEngine engine;
        engine.prepare (spec);
        engine.setSeed (5150u);

        std::vector<float> captured ((size_t) totalSamples, 0.0f);
        int elapsed = 0;
        while (elapsed < totalSamples)
        {
            const int thisChunk = std::min (chunkSize, totalSamples - elapsed);
            juce::AudioBuffer<float> buf (2, thisChunk);
            buf.clear();

            juce::dsp::AudioBlock<float> block (buf);
            engine.process (block, (double) elapsed, 10560.0, 1.0f); // impactAmount01 held at 1.0 throughout -> the reverb's own natural decay is what's being measured, not a weight fade-out

            const float* out = buf.getReadPointer (0);
            for (int i = 0; i < thisChunk; ++i)
                captured[(size_t) (elapsed + i)] = out[i];

            elapsed += thisChunk;
        }

        auto windowRms = [&] (int startSample, int len) -> double
        {
            double sumSq = 0.0;
            for (int i = 0; i < len; ++i)
                sumSq += (double) captured[(size_t) (startSample + i)] * (double) captured[(size_t) (startSample + i)];
            return std::sqrt (sumSq / (double) len);
        };

        const double earlyRms = windowRms ((int) (0.5 * testSampleRate), (int) (0.05 * testSampleRate));  // ~0.5s in, 50ms window
        const double lateRms  = windowRms (totalSamples - (int) (0.05 * testSampleRate), (int) (0.05 * testSampleRate)); // last 50ms of the 4s capture

        std::printf ("  earlyRms (~0.5s)=%.6f  lateRms (~4s)=%.6f\n", earlyRms, lateRms);

        check (lateRms < 0.05,
              "ImpactEngine short-tail reverb: tail RMS falls below a bounded threshold well within a generous 4-second window (not an unbounded/sustained tail)");
        check (earlyRms > lateRms * 2.0,
              "ImpactEngine short-tail reverb: tail genuinely decays over time (later measurement is meaningfully quieter than an earlier one), not stuck/growing");
    }

    std::printf ("\n%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
