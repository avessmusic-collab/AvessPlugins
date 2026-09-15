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
*/
#include "PluginProcessor.h"
#include "DSP/MusicalClock.h"
#include "DSP/TransitionEngine.h"
#include "DSP/DropStateMachine.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
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
    std::printf ("\n[Phase 3.1] PluginProcessor - audio pass-through + finite output across an active build\n");
    {
        DROPAudioProcessor p;
        p.setRateAndBufferSizeDetails (48000.0, 256);
        p.prepareToPlay (48000.0, 256);

        if (auto* prm = p.parameters.getParameter ("trigger"))
            prm->setValueNotifyingHost (1.0f);

        bool allFinite = true;
        bool bitIdentical = true;

        for (int block = 0; block < 50; ++block)
        {
            juce::AudioBuffer<float> buf (2, 256);
            for (int ch = 0; ch < 2; ++ch)
            {
                auto* data = buf.getWritePointer (ch);
                for (int i = 0; i < 256; ++i)
                    data[i] = std::sin ((float) (block * 256 + i) * 0.01f) * 0.5f;
            }

            juce::AudioBuffer<float> reference;
            reference.makeCopyOf (buf);

            juce::MidiBuffer midi;
            p.processBlock (buf, midi);

            if (block == 0)
                if (auto* prm = p.parameters.getParameter ("trigger"))
                    prm->setValueNotifyingHost (0.0f); // release the momentary pulse after the first block

            for (int ch = 0; ch < 2; ++ch)
            {
                const float* out = buf.getReadPointer (ch);
                const float* ref = reference.getReadPointer (ch);
                for (int i = 0; i < 256; ++i)
                {
                    if (! std::isfinite (out[i])) allFinite = false;
                    if (std::abs (out[i] - ref[i]) > 1.0e-7f) bitIdentical = false;
                }
            }
        }

        check (allFinite, "output stays finite (no NaN/Inf) across an active Idle->Arming->Building sequence");
        check (bitIdentical, "audio remains an EXACT pass-through through Idle/Arming/Building (no audible DSP yet - Phase 3.1 scope)");
        p.releaseResources();
    }

    std::printf ("\n%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
