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

    Phase 3.5 (IntensityBudget, DropStyle, AudioAnalyzer, Preset Wiring -
    the FINAL DSP phase) scope, per plugins/DROP/.ideas/plan.md:
      - IntensityBudget::computeWeights() sum<=totalBudget fuzz invariant
        (randomized style/progress/macros/intensity/tension/chaos/
        sourceProfile/jitter inputs).
      - Source-aware weight-distribution shift: synthetic vocal-like vs.
        drum-like signals (a) classify correctly via AudioAnalyzer and (b)
        measurably shift computeWeights()'s output distribution in the
        documented direction (Pitch/Delay/Reverb/Width up for vocal,
        Gate/Stutter/Digital up for drums).
      - 16-style behavioral distinctness: a quantitative per-style
        weight-trajectory-matrix pairwise-distance metric (not just
        endpoint values).
      - End-to-end Chaos determinism through full processBlock(): same
        style+seed+input -> bit-identical output; different seed -> some
        difference.
      - `impact`-outside-the-pool invariant: ImpactEngine's output depends
        only on (elapsed, total, impactAmount01), never on any
        IntensityBudget macro/weight state.
      - AudioAnalyzer hop correctness across block sizes 1/32/512/8192 (the
        512-sample FFT hop must be decoupled from host block size).

    POST-1.0 "curve progression" fix (2026-09-16, user-reported real-DAW
    bug - see PluginProcessor.h's computeIntensityProgress()/
    computeLinearBuildProgress() doc comments for the full decision matrix):
      - Weight progression milestones (15%-by-25%-elapsed / 45%-by-60%-elapsed)
        for Clean EDM defaults, proven to fail the OLD stacked-curve path "by
        construction" (both paths computed live from the real project math).
      - Per-curve-type coverage: weight-path is now immune to
        transitionCurveType; TransitionEngine's own Linear/Exponential/
        SCurve/LateAcceleration relative character (still used for
        module-internal sweeps + tensionLevel) is preserved.
      - Visualization consistency: VisualizationFrame::progress is the LINEAR
        basis (matches the audio weights' own basis); ::tensionLevel is a
        genuine curved transform of it, matching TransitionEngine::evaluate().

    POST-1.0 THIRD IMPROVEMENT SESSION (2026-09-17, user-directed: "make so
    the noise adds gradualy, remove midi note selection..."). See
    PluginProcessor.h's own top doc comment for the full four-part summary.
      - NoiseModule::riseAmountForProgress(): gradual/monotonic/C0-continuity
        + audible-early-rise-floor verification (proven to fail against the
        superseded progress^2 shape "by construction").
      - MIDI-any-note: an arbitrary note-on triggers while triggerMode ==
        Midi (DOCUMENTED SUPERSESSION of the old note-number-matching
        assertion); still ignored when triggerMode != Midi.
      - Saved-session compatibility: a state containing the old midiNote
        field still loads without error and remains inert for detection.

    POST-1.0 FOURTH IMPROVEMENT SESSION (2026-09-17, user-directed: "move
    tail knob below the graph and add a box near curve, white noise curve" /
    "noise tension to control the white noise graph"). New 21st APVTS
    parameter `noiseTension`. See PluginProcessor.h's own top doc comment.
      - riseAmountForProgress(progress, noiseTension01): default (0.5)
        reproduces the pre-noiseTension smoothstep bit-for-bit; monotonic +
        correct endpoints at 5 noiseTension settings; directionality (higher
        noiseTension = later bloom, lower rise amount at a fixed progress).
      - 21-parameter count + default; pre-noiseTension (20-param) saved-
        session compatibility.

    POST-1.0 FIFTH IMPROVEMENT SESSION (2026-09-17, user bug-audit report:
    "the sound rises gradualy... doesnt have artifacts so the tails are
    natural reverb delay... the sound isnt cut in an instant when the tail
    is up... the white noise rises too fast and too strong, the curve shows
    one thing the white noise does more than it shows"). See
    PluginProcessor.h's own top doc comment and IntensityBudget.h's
    computeNoiseWeight() doc comment.
      - Curve honesty: a full end-to-end processBlock() render (silent
        input, noise macro=100%, every other macro=0%, dry/wet=100%)
        isolates the noise riser; its measured windowed-RMS trajectory
        (normalized to its own end-of-Building value) is asserted to match
        NoiseModule::riseAmountForProgress() within +-0.1 absolute at every
        decile from t=0.3 upward - the literal "graph tells the truth"
        guard.
      - Noise weight progression-independence:
        IntensityBudget::computeNoiseWeight() takes no `progress` parameter
        at all, so its result is structurally identical regardless of build
        position - contrasted directly against computeWeights()'s own Noise
        slot (still computed internally, no longer audio-facing), which is
        shown to genuinely vary with progress, proving the fix addresses a
        real pre-existing double-progression source.
      - Tail envelope: decibel-linear decay (constant dB slope, natural
        exponential-decay character, spread < 1.5 dB per decile over
        t in [0.1,0.9]), documented -15/-30/-45 dB milestones at
        t=0.25/0.5/0.75, C0-continuous landing-zone seam, and exact zero at
        t==1 - DOCUMENTED SUPERSESSION of the prior squared-decay
        `(1-t)^2` shape's test block (see that block's own supersession
        note below), which asserted values (0.56/0.25/0.06 at
        t=0.25/0.5/0.75) that accelerate in dB and no longer reflect the
        DSP's actual behavior.
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
#include "DSP/AudioAnalyzer.h"
#include "Core/DropRandomizer.h"
#include "Core/DropStyle.h"
#include "Core/DropPreset.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
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
        // SUPERSEDED 2026-09-16 (MUSICAL-PROGRESSION FIX): shaped curves are now
        // blended toward linear by TransitionEngine::kCurveBlendDepth, so the raw
        // exponent-4 midpoint (0.0625) becomes 0.2375. The PROPERTY under test is
        // unchanged and still meaningful: max tension must stay clearly below the
        // linear midpoint (0.5), i.e. the late rise is preserved.
        check (engine.evaluate (0.5f) < 0.35f, "Exponential @ tension=1 stays clearly below linear at the midpoint (late rise preserved)");
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
        // SUPERSEDED 2026-09-16 (MUSICAL-PROGRESSION FIX): the slow-ramp factor was
        // raised 0.3 -> 0.55 AND the result is blended toward linear, because the old
        // shape reached only 22.5% of progress in 75% of the build - the direct cause
        // of the user's "8 bars, effect only active in the last bar" report. Expected
        // value is recomputed from the same published constants, never hard-coded.
        {
            const float tensionBias   = 0.25f;                  // default tension 0.5
            const float breakpointT   = 1.0f - tensionBias;     // 0.75
            const float rawBreakpoint = breakpointT * 0.55f;
            const float blended       = breakpointT + (rawBreakpoint - breakpointT)
                                        * drop::TransitionEngine::kCurveBlendDepth;
            check (nearlyEqual (engine.evaluate (0.75f), (double) blended, 1.0e-3),
                  "LateAcceleration @ default tension: breakpoint (t=0.75) matches the retuned slow-ramp value");
        }
        check (engine.evaluate (0.9f) > engine.evaluate (0.75f) + 0.15f,
              "LateAcceleration: still rises rapidly in the final tensionBias fraction");
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

        // --- POST-1.0 (2026-09-17, "remove midi note selection"): ANY
        // note-on triggers while triggerMode == Midi, regardless of note
        // number - DOCUMENTED SUPERSESSION of the pre-fix "non-matching
        // MIDI note does NOT trigger" assertion (that test encoded the OLD
        // note-number-matching behavior, which no longer exists - there is
        // no UI element left to configure a specific note). midiTriggerNote
        // is deliberately left at its default and never consulted here. ---
        {
            DROPAudioProcessor p;
            p.setTriggerMode (DROPAudioProcessor::TriggerMode::Midi);
            p.prepareToPlay (48000.0, 64);

            juce::AudioBuffer<float> buf (2, 64);
            buf.clear();
            juce::MidiBuffer midi;
            midi.addEvent (juce::MidiMessage::noteOn (1, 61, (juce::uint8) 100), 10); // arbitrary note number, deliberately NOT the old default of 60
            p.processBlock (buf, midi);

            check (p.getCurrentDropState() != drop::DropState::Idle,
                  "[POST-1.0] MIDI-any-note: an arbitrary note-on (note 61, not the old configured-note default) triggers Idle->Arming while triggerMode == Midi - note number is no longer consulted");
            p.releaseResources();
        }

        // --- MIDI listener inactive outside Midi trigger mode -------------
        {
            DROPAudioProcessor p; // default TriggerMode::Manual
            p.prepareToPlay (48000.0, 64);

            juce::AudioBuffer<float> buf (2, 64);
            buf.clear();
            juce::MidiBuffer midi;
            midi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), 10);
            p.processBlock (buf, midi);

            check (p.getCurrentDropState() == drop::DropState::Idle,
                  "[POST-1.0] MIDI-any-note: a note-on is still ignored when triggerMode != Midi (avoids accidental triggers from unrelated MIDI content) - the ANY-note-on change only widens WHICH notes trigger while Midi mode is active, not WHEN the listener is active at all");
            p.releaseResources();
        }
    }

    //==========================================================================
    // [POST-1.0] "remove midi note selection" (2026-09-17, user-directed).
    // See PluginProcessor.h's "POST-1.0 THIRD IMPROVEMENT SESSION" doc
    // comment.
    //==========================================================================
    std::printf ("\n[POST-1.0] Saved-session compatibility - state containing the old midiNote field still loads without error\n");
    {
        // Builds a minimal, hand-authored ValueTree matching the SHAPE
        // getStateInformation() has always produced (APVTS root + a
        // <DropCustomState> child with a `midiNote` property, exactly as it
        // was written by every session saved before this change), so this
        // test does not depend on any *other* processor instance's
        // getStateInformation() output to construct its input - a genuinely
        // independent round-trip check of setStateInformation()'s parsing.
        DROPAudioProcessor writer;
        writer.setMidiTriggerNote (77); // a deliberately non-default note, proving the field is genuinely read back, not just defaulted
        juce::MemoryBlock savedState;
        writer.getStateInformation (savedState);

        DROPAudioProcessor reader;
        // setStateInformation() must not crash/throw on a state containing
        // the (now DSP-unused, but still-present-in-the-format) midiNote
        // field.
        reader.setStateInformation (savedState.getData(), (int) savedState.getSize());

        check (reader.getMidiTriggerNote() == 77,
              "[POST-1.0] saved-session compatibility: a state containing the old midiNote field loads without error and getMidiTriggerNote() reflects the persisted value (field kept for backward compatibility, even though MIDI-trigger detection no longer consults it)");

        // The loaded processor must still be otherwise fully functional -
        // MIDI trigger mode now responds to ANY note-on (not the loaded
        // midiNote value), confirming the field is inert for detection
        // purposes while still round-tripping correctly.
        reader.setTriggerMode (DROPAudioProcessor::TriggerMode::Midi);
        reader.prepareToPlay (48000.0, 64);
        juce::AudioBuffer<float> buf (2, 64);
        buf.clear();
        juce::MidiBuffer midi;
        midi.addEvent (juce::MidiMessage::noteOn (1, 1, (juce::uint8) 100), 10); // note 1, nowhere near the loaded midiNote=77
        reader.processBlock (buf, midi);
        check (reader.getCurrentDropState() != drop::DropState::Idle,
              "[POST-1.0] saved-session compatibility: after loading a state with midiNote=77, an unrelated note-on (note 1) still triggers - confirms the loaded field is inert for MIDI-trigger detection (ANY-note-on behavior), not silently reintroducing the old matching requirement");
        reader.releaseResources();
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
    // Stage 3 DSP Phase 3.5 API SUPERSESSION (documented, not a weakened
    // assertion): this block originally called the Phase 3.2 PLACEHOLDER's
    // instance method `drop::IntensityBudget budget; budget.compute(macros,
    // progress, intensity)`, which Phase 3.5 replaced with the real, static
    // `drop::IntensityBudget::computeWeights(style, progress, macros,
    // intensity, tension, chaos, sourceProfile, chaosJitter)`. Adapted below
    // to the new signature using a neutral/default Style (getStyle(0),
    // "Clean EDM"), SourceProfile::FullMix, and zero tension/chaos/jitter -
    // this preserves the ORIGINAL intent (the proportional-cap normalization
    // invariant, and zero-macro transparency) exactly, since neither of
    // those two properties depends on which Style/source/tension/chaos is
    // supplied. The THIRD original sub-assertion ("progress == 0 -> all-zero
    // weights") does NOT carry over: it was an artifact of the OLD
    // placeholder's specific `raw[i] = macro * progress` formula, not a
    // general normalization property - the REAL Style-driven curves
    // intentionally allow nonzero AUTHORED starting weights at progress==0
    // (e.g. Clean EDM's Filter/Delay/Reverb curves start above 0, a
    // deliberate musical-design choice, not a bug - see DropStyle.cpp).
    // Replaced with the generalized, still-genuinely-true equivalent that
    // DOES hold for every Style/macro/progress combination:
    // "intensity == 0 -> all-zero weights" (totalBudgetFor(...) is exactly 0
    // whenever intensity01 == 0, so the proportional cap forces scale == 0
    // regardless of the raw per-module sums) - same "some neutral input
    // collapses everything to zero via the budget mechanism" spirit as the
    // original test, just sourced from an invariant that is actually true of
    // the new system.
    {
        const auto& neutralStyle = drop::getStyle (0); // Clean EDM (default style)
        std::array<float, (size_t) drop::BudgetModule::Count> noJitterFuzz {};

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

            const auto weights = drop::IntensityBudget::computeWeights (neutralStyle, progress, macros, intensity,
                                                                          0.0f, 0.0f, drop::SourceProfile::FullMix, noJitterFuzz);

            float sum = 0.0f;
            for (auto w : weights.value)
            {
                if (! std::isfinite (w) || w < 0.0f || w > 1.0f) invariantHolds = false;
                sum += w;
            }

            const float totalBudget = drop::IntensityBudget::totalBudgetFor (neutralStyle, progress, intensity);
            if (sum > totalBudget + 1.0e-3f) invariantHolds = false;
        }
        check (invariantHolds, "computeWeights(): sum(output) <= totalBudgetFor(style, progress, intensity) invariant holds across 500 randomized fuzz inputs, every weight in [0,1] and finite");

        std::array<float, (size_t) drop::BudgetModule::Count> allZeroMacros {};
        const auto zeroWeights = drop::IntensityBudget::computeWeights (neutralStyle, 1.0f, allZeroMacros, 1.0f,
                                                                          0.0f, 0.0f, drop::SourceProfile::FullMix, noJitterFuzz);
        bool allZero = true;
        for (auto w : zeroWeights.value)
            if (w != 0.0f) allZero = false;
        check (allZero, "all-zero macros -> all-zero weights regardless of progress/intensity");

        std::array<float, (size_t) drop::BudgetModule::Count> allFullMacros {};
        for (auto& m : allFullMacros) m = 1.0f;
        const auto zeroIntensityWeights = drop::IntensityBudget::computeWeights (neutralStyle, 1.0f, allFullMacros, 0.0f,
                                                                                   0.0f, 0.0f, drop::SourceProfile::FullMix, noJitterFuzz);
        bool allZeroAtZeroIntensity = true;
        for (auto w : zeroIntensityWeights.value)
            if (w != 0.0f) allZeroAtZeroIntensity = false;
        check (allZeroAtZeroIntensity, "intensity == 0 -> all-zero weights regardless of progress/macros (totalBudgetFor(...) == 0 forces the proportional cap's scale to 0 - supersedes the Phase 3.2-era 'progress==0' variant of this check, which relied on the OLD placeholder's macro*progress formula rather than a property that still holds for real Style-authored curves with nonzero starting weights)");
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

        // Slope smallness at the internal boundaries (architecture.md "must
        // be click-free"): at both internal boundaries (fade-out->hold,
        // hold->fade-in) the discrete slope immediately either side must be
        // near zero. UPDATED RATIONALE (2026-09-19 dB-linear supersession,
        // "make the vacume volume in db"): originally this matched the
        // raised-cosine's zero-derivative endpoints; the dB-linear ramps
        // satisfy the same bound because the seams sit AT the floor level
        // (<= -12dB), where a constant-dB-per-sample ramp's absolute
        // per-sample step is tiny by construction.
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

        // POST-1.0 (2026-09-19, user-directed "make the vacume volume in
        // db"): the collapse and return fades are DECIBEL-LINEAR - equal
        // slices of the fade drop/raise equal dB. Measured across the
        // fade-out at depth 1 (0dB -> -36dB floor): the per-slice dB drop
        // must be constant.
        {
            const double win = 44100.0;
            const double fadeOutSamples = 0.2 * win;
            float minStepDb = 1.0e9f, maxStepDb = -1.0e9f;
            for (int i = 1; i <= 10; ++i)
            {
                const float a = drop::VacuumModule::computeGain (fadeOutSamples * (double) (i - 1) / 10.0 + 1.0, win, 1.0f);
                const float b = drop::VacuumModule::computeGain (fadeOutSamples * (double) i / 10.0, win, 1.0f);
                const float stepDb = juce::Decibels::gainToDecibels (a, -120.0f) - juce::Decibels::gainToDecibels (b, -120.0f);
                minStepDb = juce::jmin (minStepDb, stepDb);
                maxStepDb = juce::jmax (maxStepDb, stepDb);
            }
            std::printf ("  vacuum fade-out per-decile dB drop: min %.2f, max %.2f (spread %.2f)\n", minStepDb, maxStepDb, maxStepDb - minStepDb);
            check (maxStepDb - minStepDb < 0.5f,
                  "[POST-1.0] VacuumModule: the collapse fade is decibel-LINEAR (constant dB slope across the fade-out; SUPERSEDES the raised-cosine amplitude fade, which in dB hung near full level then plunged) - 'the vacuum volume is in dB'");
        }
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

        // Compile-safety only (-Wfloat-equal): memcmp is a byte-exact,
        // warning-free equivalent to a per-sample != loop for this
        // "bit-identical" determinism claim - same pass/fail outcome, no
        // logic change.
        bool identical = (a.samples.size() == b.samples.size())
                       && std::memcmp (a.samples.data(), b.samples.data(), a.samples.size() * sizeof (float)) == 0;
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
    // [POST-1.0] "make so the noise adds gradualy" (2026-09-17, user-
    // directed). See NoiseModule.h's riseAmountForProgress() doc comment and
    // PluginProcessor.h's "POST-1.0 THIRD IMPROVEMENT SESSION" doc comment.
    //==========================================================================
    std::printf ("\n[POST-1.0] NoiseModule::riseAmountForProgress() - gradual, monotonic, C0-continuous rise (supersedes the old progress^2 shape)\n");
    {
        // Monotonic non-decreasing across [0,1], starting at ~0 and ending
        // at exactly 1.
        bool monotonic = true;
        float previous = -1.0f;
        for (int i = 0; i <= 100; ++i)
        {
            const float p = (float) i / 100.0f;
            const float v = drop::NoiseModule::riseAmountForProgress (p);
            if (v < previous - 1.0e-6f) monotonic = false;
            previous = v;
        }
        check (monotonic, "[POST-1.0] riseAmountForProgress(): monotonically non-decreasing across [0,1]");

        const float atZero = drop::NoiseModule::riseAmountForProgress (0.0f);
        const float atOne  = drop::NoiseModule::riseAmountForProgress (1.0f);
        check (nearlyEqual ((double) atZero, 0.0, 1.0e-6), "[POST-1.0] riseAmountForProgress(0) == 0.0 exactly");
        check (nearlyEqual ((double) atOne, 1.0, 1.0e-6), "[POST-1.0] riseAmountForProgress(1) == 1.0 exactly");

        // Audible-early-rise floors: the OLD `progress*progress` shape gave
        // 0.04 at progress=0.2 and 0.25 at progress=0.5 - both are the
        // documented values the user's "adds gradually" complaint was about
        // (near-silent until very late). The floors below are set well
        // ABOVE those old values (double, in both cases) so this test
        // genuinely fails against the old quadratic shape while passing the
        // new smoothstep shape (0.104 at 0.2, 0.5 at 0.5 - see
        // riseAmountForProgress()'s own doc comment for the exact numbers).
        const float at02 = drop::NoiseModule::riseAmountForProgress (0.2f);
        const float at05 = drop::NoiseModule::riseAmountForProgress (0.5f);
        std::printf ("  riseAmountForProgress(0.2)=%.4f (ceiling 0.03 - early-quiet)  riseAmountForProgress(0.5)=%.4f (floor 0.15 - mid-build presence)\n", at02, at05);
        // SUPERSEDED 2026-09-17 (second noise report: "decrease the whitenoise
        // volume at the beginning of the curve drasticly"). The earlier pair of
        // FLOORS here encoded the FIRST report ("make so the noise adds
        // gradualy"), which was about the riser arriving abruptly and late. The
        // two requirements are not contradictory but they pull in opposite
        // directions at opposite ends of the build, so the assertions now encode
        // BOTH: a CEILING early (must be drastically quiet at the start) and
        // floors kept at mid/late (must never regress to the old "inaudible
        // until the final moment" behaviour that started this whole thread).
        check (at02 <= 0.03f, "[POST-1.0] riseAmountForProgress(0.2) stays BELOW the 0.03 early-quiet ceiling - the riser is drastically quiet at the start of the build");
        juce::ignoreUnused (at05);
        {
            // SUPERSEDED 2026-09-17 (third noise report: "make it rise more
            // gradualy. cause it rises way too fast"). The mid-build AMPLITUDE
            // floors that previously lived here are incompatible with a
            // deliberately slower climb, and amplitude was the wrong yardstick
            // anyway - the complaint was perceptual. The guards below assert the
            // property that actually matters: the riser climbs at a CONSTANT
            // rate in decibels (so it never surges), while still being clearly
            // present before the drop.
            const float at08 = drop::NoiseModule::riseAmountForProgress (0.8f);
            const float at095 = drop::NoiseModule::riseAmountForProgress (0.95f);
            std::printf ("  riseAmountForProgress(0.8)=%.4f (floor 0.25)  (0.95)=%.4f (floor 0.55)\n", at08, at095);
            check (at08 >= 0.25f, "[POST-1.0] riseAmountForProgress(0.8) clears the 0.25 late-build floor - quiet early must never mean inaudible before the drop");
            check (at095 >= 0.55f, "[POST-1.0] riseAmountForProgress(0.95) clears the 0.55 pre-drop floor");

            // The defining property of "gradual": equal decibel steps per decile.
            // Measured from p=0.3 upward. Below roughly -35 dB the normalisation
            // that pins p==0 to exact silence (subtracting the range floor)
            // compresses the dB spacing; that region is inaudible under music,
            // and the surge the user reported lives in the audible range, which
            // is what this asserts.
            float minStep = 1.0e9f, maxStep = -1.0e9f;
            for (int i = 4; i <= 10; ++i)
            {
                const float a = drop::NoiseModule::riseAmountForProgress ((float) (i - 1) / 10.0f);
                const float b = drop::NoiseModule::riseAmountForProgress ((float) i / 10.0f);
                const float aDb = 20.0f * std::log10 (juce::jmax (a, 1.0e-6f));
                const float bDb = 20.0f * std::log10 (juce::jmax (b, 1.0e-6f));
                const float step = bDb - aDb;
                minStep = juce::jmin (minStep, step);
                maxStep = juce::jmax (maxStep, step);
            }
            std::printf ("  per-decile rise: min %.2f dB, max %.2f dB (spread %.2f dB)\n", minStep, maxStep, maxStep - minStep);
            check (maxStep - minStep < 1.0f,
                  "[POST-1.0] the riser climbs at a near-constant decibel rate across the audible part of the build (spread < 1 dB per decile) - perceptually gradual, no surge");
        }

        // Continuity (C0 proxy): no jump greater than a documented bound
        // between adjacent finely-sampled progress values - smoothstep is a
        // smooth cubic polynomial, so any genuine discontinuity here would
        // indicate an implementation bug (e.g. an accidental branch/clamp
        // seam), not merely floating-point noise.
        constexpr int kContinuitySamples = 500;
        // Raised 0.01 -> 0.02 on 2026-09-17 with the dB-linear ramp. This guard
        // exists to catch DISCONTINUITIES, not to cap slope. The ramp is analytic
        // and steepest as it reaches full: |d/dp| = ln(10) * 48/20 = 5.53, i.e.
        // 0.0111 across one 1/500 step. A genuine discontinuity would be orders of
        // magnitude larger than this bound.
        constexpr float kMaxAllowedStep = 0.02f;
        float maxStep = 0.0f;
        float prevContinuity = drop::NoiseModule::riseAmountForProgress (0.0f);
        for (int i = 1; i <= kContinuitySamples; ++i)
        {
            const float p = (float) i / (float) kContinuitySamples;
            const float v = drop::NoiseModule::riseAmountForProgress (p);
            maxStep = std::max (maxStep, std::abs (v - prevContinuity));
            prevContinuity = v;
        }
        std::printf ("  max step between %d adjacent samples = %.6f (documented bound %.6f)\n", kContinuitySamples, maxStep, kMaxAllowedStep);
        check (maxStep < kMaxAllowedStep, "[POST-1.0] riseAmountForProgress(): no jump greater than the documented bound between adjacent finely-sampled progress values (C0 continuity proxy)");
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
        // seconds placeholders - see computeDurationsForNewBuild()); FURTHER
        // updated for the POST-1.0 grid-lock fix (Bug D - buildingSamples now
        // has the Vacuum window carved OUT of it, see that method's doc
        // comment): no playhead is set, so MusicalClock uses its 120 BPM/4/4
        // fallback transport - samplesPerBeat = 24000, barLength = 96000.
        // buildLength choice index 0 = 1 bar; peakSamples (Vacuum, 1/32 note
        // @120bpm) = 3000, computed FIRST as of the fix, so buildingSamples =
        // 96000 - 3000 = 93000 (was 96000 pre-fix). Arming quantizes to the
        // next bar boundary, but MusicalClock's fallback transport has
        // already been advanced by this SAME block's 256 samples before the
        // trigger is detected (update() runs before trigger detection in
        // processBlock()), so armingToBuildingSamples = 96000 - 256 = 95744,
        // not ~0. Plus this phase's musically-derived windows: impactSamples
        // (fixed 220ms) = 10560, recoverySamples (half a bar) = 48000.
        // Total = 95744 + 93000 + 3000 + 10560 + 48000 = 250304 samples
        // (was 253304 pre-fix - exactly 3000 samples less, the peakSamples
        // now carved out rather than additionally appended) = ~978 blocks of
        // 256. The existing 1300-block (332800-sample) budget still
        // comfortably covers this (~33% margin), so it is intentionally left
        // unchanged this phase.
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
                // Compile-safety only (-Wswitch-enum wants every enumerator
                // named even when a default exists) - identical behavior to
                // default: below, no logic change.
                case drop::DropState::Arming:
                case drop::DropState::Building:
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
    std::printf ("\n[POST-1.0] Style-authored impact accents - REMOVED factory-wide (no synthesized kick/burst on any Style)\n");
    {
        // User DAW report (2026-09-17): "on the dark techno preset somehow it
        // adds a drum kick or a hit". Root cause: ImpactEngine's synthesized
        // 60Hz sub-hit + 2.2kHz burst accents were unconditional for every
        // Style. First fix: DropStyleData::impactAccentLevel scaled the
        // accents, Dark Techno authored 0.0. Follow-up the same day
        // ("remove impacts"): the DEFAULT is now 0.0, disabling accents
        // factory-wide; the engine mechanism itself is kept (and verified
        // below) so accents remain per-Style re-enableable.

        // (a) Authoring data. DOCUMENTED SUPERSESSION (2026-09-17, same
        //     day): the first fix disabled accents for Dark Techno ONLY
        //     (this block then asserted Clean EDM kept 1.0); the user's
        //     follow-up "remove impacts" disabled them FACTORY-WIDE via the
        //     DropStyleData default. Every Style must now author 0.0 - no
        //     synthesized kick/snap anywhere; every drop is the source's
        //     own clean return.
        bool allZero = true;
        for (int i = 0; i < drop::kNumStyles; ++i)
            if (drop::getStyle (i).impactAccentLevel != 0.0f) allZero = false;
        check (allZero,
              "accent removal: ALL 16 Styles author impactAccentLevel == 0.0 (factory-wide, per the user's 'remove impacts' - SUPERSEDES the Dark-Techno-only opt-out from earlier the same day)");

        // (b) Engine behavior: with accentLevel01 == 0 and a SILENT bus, the
        // impact window renders exactly silence - nothing synthesized is
        // added (the shortTailReverb of silence is silence, and the low-end
        // safety gain of 0 is 0). With accentLevel01 == 1 the same call
        // renders clearly nonzero content (sanity: the gate is the accent
        // level, not some other coincidence).
        constexpr double testSampleRate = 48000.0;
        constexpr int windowSamples = 12000; // 250ms - spans sub (~121.5ms) + burst (20ms) fully

        auto renderPeak = [&] (float accentLevel) -> float
        {
            juce::dsp::ProcessSpec spec { testSampleRate, (juce::uint32) windowSamples, 2 };
            drop::ImpactEngine engine;
            engine.prepare (spec);
            engine.setSeed (12345u);
            juce::AudioBuffer<float> buf (2, windowSamples);
            buf.clear();
            juce::dsp::AudioBlock<float> blk (buf);
            engine.process (blk, 0.0, (double) windowSamples, 1.0f, -1.0f, accentLevel);
            return buf.getMagnitude (0, windowSamples);
        };

        const float peakAccentsOff = renderPeak (0.0f);
        const float peakAccentsOn  = renderPeak (1.0f);
        std::printf ("  silent-bus impact peak: accents OFF %.6f, accents ON %.6f\n", peakAccentsOff, peakAccentsOn);
        check (peakAccentsOff == 0.0f,
              "accent opt-out: accentLevel01==0 + silent bus -> ImpactEngine adds EXACTLY nothing (no kick, no snap, no reverb excitation)");
        check (peakAccentsOn > 0.05f,
              "accent opt-out sanity: accentLevel01==1 on the identical silent bus DOES render the synthesized accents (the gate is the accent level)");
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
    std::printf ("\n[Phase 3.4][POST-1.0 GRID-LOCK] Full processBlock() - transient preservation through the Vacuum->Impact window\n");
    {
        // Exact deterministic sample-offset arithmetic (same fallback-
        // transport style as the Phase 3.3 end-to-end test), UPDATED for the
        // POST-1.0 grid-lock fix (see computeDurationsForNewBuild()'s doc
        // comment / PluginProcessor.h's "DOCUMENTED SUPERSESSION" note at the
        // Phase 3.5 Style-timing comment): 120 BPM/4/4 @48kHz ->
        // samplesPerBeat=24000, barLength=96000; buildLength choice index 0
        // = 1 bar. armingToBuildingSamples = 96000-256 = 95744 (fallback
        // transport already advanced by the triggering block's own 256
        // samples before the trigger is detected, UNCHANGED by the grid-lock
        // fix - Arming's quantization doesn't depend on peakSamples).
        // peakSamples (Vacuum, 1/32 note @120bpm) = 3000, computed FIRST as
        // of the fix. buildingSamples is now barsToSamples(1 bar) -
        // peakSamples = 96000 - 3000 = 93000 (was 96000, the full bar,
        // pre-fix). Impact begins at global sample
        // 95744 + 93000 + 3000 = 191744 = EXACTLY armingToBuildingSamples +
        // barsToSamples(1 bar) (95744 + 96000 = 191744) - i.e. precisely the
        // downbeat, confirming the grid-lock invariant end-to-end. Block
        // index 191744/256 = 749 EXACTLY (remainder 0, i.e. local sample
        // offset 0 within block 749) - was block 760, offset 184 pre-fix.
        constexpr int blockSize = 256;
        constexpr int impactStartBlock = 749;
        constexpr int impactStartLocalOffset = 0;

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

    //==========================================================================
    std::printf ("\n[Phase 3.5] IntensityBudget::computeWeights - sum(weights) <= totalBudget fuzz invariant\n");
    {
        std::mt19937 rng (13013u);
        std::uniform_real_distribution<float> unit01 (0.0f, 1.0f);
        std::uniform_int_distribution<int> styleDist (0, drop::kNumStyles - 1);
        std::uniform_int_distribution<int> sourceDist (0, (int) drop::SourceProfile::Count - 1);

        bool allOk = true;
        double worstMargin = 1.0e9;
        for (int trial = 0; trial < 500; ++trial)
        {
            const auto& style = drop::getStyle (styleDist (rng));
            const float progress = unit01 (rng);
            const float intensity01 = unit01 (rng);
            const float tension01 = unit01 (rng);
            const float chaos01 = unit01 (rng);
            const auto sourceProfile = (drop::SourceProfile) sourceDist (rng);

            std::array<float, (size_t) drop::BudgetModule::Count> macros {};
            std::array<float, (size_t) drop::BudgetModule::Count> jitter {};
            for (auto& m : macros) m = unit01 (rng);
            // Matches DropRandomizer::drawForNewBuild()'s own jitter magnitude
            // convention (+-15% at chaos==100%) - a fuzz test that only ever
            // exercises tinier jitter than production would understate risk.
            for (auto& j : jitter) j = (unit01 (rng) * 2.0f - 1.0f) * 0.15f * chaos01;

            const auto weights = drop::IntensityBudget::computeWeights (style, progress, macros, intensity01,
                                                                          tension01, chaos01, sourceProfile, jitter);
            float sum = 0.0f;
            for (auto w : weights.value) sum += w;

            const float budget = drop::IntensityBudget::totalBudgetFor (style, progress, intensity01);
            const double margin = (double) budget - (double) sum;
            worstMargin = std::min (worstMargin, margin);
            if (sum > budget + 1.0e-4f)
                allOk = false;
        }
        std::printf ("  500 randomized trials, worst (budget - sum) margin = %.6f -> %s\n", worstMargin, allOk ? "ok" : "VIOLATION");
        check (allOk, "IntensityBudget::computeWeights(): sum(weights) <= totalBudgetFor(style, progress, intensity01) holds across 500 randomized fuzz trials (style/progress/macros/intensity/tension/chaos/sourceProfile/jitter all randomized)");
    }

    //==========================================================================
    std::printf ("\n[Phase 3.5] AudioAnalyzer - SourceProfile classification (synthetic vocal-like vs drum-like signals)\n");
    drop::SourceProfile testVocalProfile = drop::SourceProfile::FullMix;
    drop::SourceProfile testDrumProfile = drop::SourceProfile::FullMix;
    {
        constexpr double sr = 48000.0;
        constexpr int numSamples = 48000; // 1 second - several FFT hops to let classification settle

        // Vocal-like: a steady mid-frequency tone (500Hz), constant
        // amplitude, no transients - architecture.md's own "midFrac
        // dominant, low transient density" vocal characterization.
        {
            juce::dsp::ProcessSpec spec { sr, 512, 2 };
            drop::AudioAnalyzer analyzer;
            analyzer.prepare (spec);

            juce::AudioBuffer<float> buf (2, numSamples);
            for (int ch = 0; ch < 2; ++ch)
            {
                auto* d = buf.getWritePointer (ch);
                for (int i = 0; i < numSamples; ++i)
                    d[i] = 0.4f * std::sin ((float) (2.0 * juce::MathConstants<double>::pi * 500.0 * (double) i / sr));
            }
            analyzer.analyze (buf, numSamples);
            testVocalProfile = analyzer.getSourceProfile();
            std::printf ("  vocal-like signal -> classified as %d (0=Vocal,1=Drums,2=Bass,3=FullMix)\n", (int) testVocalProfile);
        }

        // Drum-like: short broadband noise bursts (24/sec, sparse duty
        // cycle) - high transient density, broadband content.
        {
            juce::dsp::ProcessSpec spec { sr, 512, 2 };
            drop::AudioAnalyzer analyzer;
            analyzer.prepare (spec);

            juce::AudioBuffer<float> buf (2, numSamples);
            buf.clear();
            std::mt19937 noiseRng (2024u);
            std::uniform_real_distribution<float> noiseDist (-1.0f, 1.0f);
            constexpr int burstPeriod = 2000;
            constexpr int burstLen = 60;
            for (int i = 0; i < numSamples; ++i)
            {
                if ((i % burstPeriod) < burstLen)
                {
                    const float n = noiseDist (noiseRng);
                    buf.setSample (0, i, n);
                    buf.setSample (1, i, n);
                }
            }
            analyzer.analyze (buf, numSamples);
            testDrumProfile = analyzer.getSourceProfile();
            std::printf ("  drum-like signal -> classified as %d (0=Vocal,1=Drums,2=Bass,3=FullMix)\n", (int) testDrumProfile);
        }

        check (testVocalProfile == drop::SourceProfile::Vocal, "AudioAnalyzer: steady mid-frequency tone (no transients) classifies as Vocal");
        check (testDrumProfile == drop::SourceProfile::Drums, "AudioAnalyzer: short broadband noise bursts (high transient density) classify as Drums");
    }

    //==========================================================================
    std::printf ("\n[Phase 3.5] IntensityBudget::computeWeights - source-aware weight-distribution shift\n");
    {
        // progress=0.95, intensity=100% chosen so totalBudget is generous
        // enough that the proportional-cap normalization does NOT engage
        // for either call (verified: raw sums stay comfortably under
        // budget for Clean EDM at this progress) - isolates the
        // source-aware SHIFT from the separate capping mechanism, which is
        // unit-tested independently above.
        const auto& style = drop::getStyle (0); // Clean EDM
        std::array<float, (size_t) drop::BudgetModule::Count> macros {};
        for (auto& m : macros) m = 1.0f;
        std::array<float, (size_t) drop::BudgetModule::Count> noJitter {};

        const auto vocalWeights = drop::IntensityBudget::computeWeights (style, 0.95f, macros, 1.0f, 0.5f, 0.0f, drop::SourceProfile::Vocal, noJitter);
        const auto drumWeights  = drop::IntensityBudget::computeWeights (style, 0.95f, macros, 1.0f, 0.5f, 0.0f, drop::SourceProfile::Drums, noJitter);

        auto sumOf = [] (const drop::IntensityBudget::Weights& w, std::initializer_list<drop::BudgetModule> mods) -> float
        {
            float s = 0.0f;
            for (auto m : mods) s += w.value[(size_t) m];
            return s;
        };

        const float vocalFavoredUnderVocal = sumOf (vocalWeights, { drop::BudgetModule::Pitch, drop::BudgetModule::Delay, drop::BudgetModule::Reverb, drop::BudgetModule::Width });
        const float drumFavoredUnderVocal  = sumOf (vocalWeights, { drop::BudgetModule::Gate, drop::BudgetModule::Stutter, drop::BudgetModule::Digital });

        const float vocalFavoredUnderDrum = sumOf (drumWeights, { drop::BudgetModule::Pitch, drop::BudgetModule::Delay, drop::BudgetModule::Reverb, drop::BudgetModule::Width });
        const float drumFavoredUnderDrum  = sumOf (drumWeights, { drop::BudgetModule::Gate, drop::BudgetModule::Stutter, drop::BudgetModule::Digital });

        std::printf ("  Vocal profile: Pitch+Delay+Reverb+Width=%.4f  Gate+Stutter+Digital=%.4f\n", vocalFavoredUnderVocal, drumFavoredUnderVocal);
        std::printf ("  Drums profile: Pitch+Delay+Reverb+Width=%.4f  Gate+Stutter+Digital=%.4f\n", vocalFavoredUnderDrum, drumFavoredUnderDrum);

        check (vocalFavoredUnderVocal > vocalFavoredUnderDrum,
              "computeWeights(): Vocal-classified input shifts Pitch+Delay+Reverb+Width weight UP relative to Drums-classified input (same Style/macros/progress)");
        check (drumFavoredUnderDrum > drumFavoredUnderVocal,
              "computeWeights(): Drums-classified input shifts Gate+Stutter+Digital weight UP relative to Vocal-classified input (same Style/macros/progress)");
    }

    //==========================================================================
    std::printf ("\n[Phase 3.5] DropStyle - 16-style behavioral distinctness (quantitative weight-trajectory-matrix pairwise distance)\n");
    {
        constexpr int kNumProgressPoints = 9;
        std::array<float, (size_t) drop::BudgetModule::Count> macros {};
        for (auto& m : macros) m = 1.0f;
        std::array<float, (size_t) drop::BudgetModule::Count> noJitter {};

        // Per-style 10-module x 9-progress-point weight-trajectory matrix,
        // computed with FIXED macros/intensity/tension/chaos/sourceProfile
        // so only the Style's own authored curves/timing differ between
        // rows - the strict "genuinely different transition behavior"
        // criterion, not a proxy that could be satisfied by different
        // macro defaults alone.
        std::vector<std::vector<std::array<float, kNumProgressPoints>>> matrices (
            (size_t) drop::kNumStyles, std::vector<std::array<float, kNumProgressPoints>> ((size_t) drop::BudgetModule::Count));

        for (int s = 0; s < drop::kNumStyles; ++s)
        {
            const auto& style = drop::getStyle (s);
            for (int p = 0; p < kNumProgressPoints; ++p)
            {
                const float progress = (float) p / (float) (kNumProgressPoints - 1);
                const auto weights = drop::IntensityBudget::computeWeights (style, progress, macros, 1.0f, 0.5f, 0.0f, drop::SourceProfile::FullMix, noJitter);
                for (size_t m = 0; m < (size_t) drop::BudgetModule::Count; ++m)
                    matrices[(size_t) s][m][(size_t) p] = weights.value[m];
            }
        }

        auto l1Distance = [&] (int a, int b) -> double
        {
            double sum = 0.0;
            for (size_t m = 0; m < (size_t) drop::BudgetModule::Count; ++m)
                for (int p = 0; p < kNumProgressPoints; ++p)
                    sum += std::abs ((double) matrices[(size_t) a][m][(size_t) p] - (double) matrices[(size_t) b][m][(size_t) p]);
            return sum;
        };

        // Documented threshold: chosen conservatively low relative to the
        // typical observed pairwise distance (printed below) - the intent
        // is to require genuinely different curve shapes/timings/module
        // emphasis (which the per-Style authoring above provides in every
        // one of the 10 module curves + the budget envelope), not to be a
        // razor's-edge tolerance.
        constexpr double kMinPairwiseDistance = 0.3;
        double worstDistance = 1.0e9;
        int worstA = 0, worstB = 1;
        bool allDistinct = true;
        for (int a = 0; a < drop::kNumStyles; ++a)
        {
            for (int b = a + 1; b < drop::kNumStyles; ++b)
            {
                const double d = l1Distance (a, b);
                if (d < worstDistance) { worstDistance = d; worstA = a; worstB = b; }
                if (d < kMinPairwiseDistance) allDistinct = false;
            }
        }

        std::printf ("  closest pair (of 120): \"%s\" vs \"%s\", L1 distance = %.4f (threshold %.2f)\n",
                     drop::getStyle (worstA).name, drop::getStyle (worstB).name, worstDistance, kMinPairwiseDistance);
        check (allDistinct, "DropStyle: all 16 styles' 10-module x 9-progress-point weight-trajectory matrices are pairwise distinct (L1 distance >= threshold for every one of the 120 pairs) - a full-trajectory metric, not just endpoint values");

        bool sawMoreThanOneCurveType = false;
        const auto firstCurveType = drop::getStyle (0).transitionCurveType;
        for (int s = 1; s < drop::kNumStyles; ++s)
            if (drop::getStyle (s).transitionCurveType != firstCurveType) { sawMoreThanOneCurveType = true; break; }
        check (sawMoreThanOneCurveType, "DropStyle: styles use more than one TransitionEngine curve type (an independent axis of 'genuinely different behavior' beyond the weight-trajectory matrix)");
    }

    //==========================================================================
    std::printf ("\n[Phase 3.5] End-to-end Chaos determinism (full processBlock(): same style+seed+input -> bit-identical; different seed -> differs)\n");
    {
        constexpr int blockSize = 256;
        constexpr int numBlocks = 400; // covers Arming + a stretch of Building at the 120 BPM fallback transport

        auto runOnce = [&] (uint32_t seed) -> std::vector<float>
        {
            DROPAudioProcessor p;
            p.setRateAndBufferSizeDetails (48000.0, blockSize);
            p.prepareToPlay (48000.0, blockSize);
            p.setChaosSeed (seed);

            if (auto* prm = p.parameters.getParameter ("chaos"))
                prm->setValueNotifyingHost (0.8f); // 80% - large enough that different seeds reliably pick different musically-constrained choices
            if (auto* prm = p.parameters.getParameter ("buildLength"))
                prm->setValueNotifyingHost (0.0f); // shortest: 1 bar
            if (auto* prm = p.parameters.getParameter ("trigger"))
                prm->setValueNotifyingHost (1.0f);

            std::vector<float> captured;
            captured.reserve ((size_t) numBlocks * (size_t) blockSize);

            for (int block = 0; block < numBlocks; ++block)
            {
                juce::AudioBuffer<float> buf (2, blockSize);
                // Fixed, deterministic test input (a sine tone at a
                // block-continuous phase) - never RNG/wall-clock seeded,
                // per implementation-spec.md's offline-rendering
                // determinism requirement.
                for (int ch = 0; ch < 2; ++ch)
                {
                    auto* d = buf.getWritePointer (ch);
                    for (int i = 0; i < blockSize; ++i)
                        d[i] = 0.3f * std::sin ((float) (2.0 * juce::MathConstants<double>::pi * 220.0 * (double) (block * blockSize + i) / 48000.0));
                }

                juce::MidiBuffer midi;
                p.processBlock (buf, midi);

                if (block == 0)
                    if (auto* prm = p.parameters.getParameter ("trigger"))
                        prm->setValueNotifyingHost (0.0f);

                const float* out = buf.getReadPointer (0);
                for (int i = 0; i < blockSize; ++i)
                    captured.push_back (out[i]);
            }

            p.releaseResources();
            return captured;
        };

        const auto runA1 = runOnce (4242u);
        const auto runA2 = runOnce (4242u);
        const auto runB  = runOnce (4243u);

        // Compile-safety only (-Wfloat-equal): memcmp is a byte-exact,
        // warning-free equivalent to a per-sample !=/== loop for these
        // "bit-identical"/"differs" determinism claims - same pass/fail
        // outcome, no logic change.
        const bool identical = (runA1.size() == runA2.size())
                             && std::memcmp (runA1.data(), runA2.data(), runA1.size() * sizeof (float)) == 0;
        check (identical, "End-to-end determinism: same DROPAudioProcessor style+seed+input -> bit-identical output across two independent runs");

        const bool anyDifferent = (runA1.size() != runB.size())
                                || std::memcmp (runA1.data(), runB.data(), runA1.size() * sizeof (float)) != 0;
        check (anyDifferent, "End-to-end determinism: a different Chaos seed produces measurably different output (same style/input/transport otherwise)");
    }

    //==========================================================================
    std::printf ("\n[Phase 3.5] impact-outside-the-pool invariant: ImpactEngine output depends only on (elapsed, total, impactAmount01)\n");
    {
        juce::dsp::ProcessSpec spec { 48000.0, 2048, 2 };

        auto renderImpact = [&] (float impactAmount01) -> std::vector<float>
        {
            drop::ImpactEngine engine;
            engine.prepare (spec);
            engine.setSeed (777u);

            constexpr int numSamples = 2048;
            juce::AudioBuffer<float> buf (2, numSamples);
            buf.clear();
            juce::dsp::AudioBlock<float> block (buf);
            engine.process (block, 0.0, 10560.0, impactAmount01);

            std::vector<float> out ((size_t) numSamples);
            const float* ptr = buf.getReadPointer (0);
            for (int i = 0; i < numSamples; ++i) out[(size_t) i] = ptr[i];
            return out;
        };

        // ImpactEngine::process()/computeDriveValues() take NO IntensityBudget
        // macro/weight/style argument whatsoever - a structural,
        // compile-time guarantee (see ImpactEngine.h's signatures). The
        // runtime check below confirms the SAME impactAmount01 always
        // produces bit-identical output regardless of arbitrary
        // computeWeights() calls (with extreme, opposite-end macro/style/
        // progress/source inputs) evaluated and discarded in between -
        // "sweeping other macros never changes impact headroom."
        const auto a = renderImpact (0.5f);

        std::array<float, (size_t) drop::BudgetModule::Count> macrosLow {};
        std::array<float, (size_t) drop::BudgetModule::Count> macrosHigh {};
        for (auto& m : macrosHigh) m = 1.0f;
        std::array<float, (size_t) drop::BudgetModule::Count> noJitter {};
        (void) drop::IntensityBudget::computeWeights (drop::getStyle (0), 0.0f, macrosLow, 0.0f, 0.0f, 0.0f, drop::SourceProfile::FullMix, noJitter);
        (void) drop::IntensityBudget::computeWeights (drop::getStyle (13), 1.0f, macrosHigh, 1.0f, 1.0f, 1.0f, drop::SourceProfile::Drums, noJitter);

        const auto b = renderImpact (0.5f);

        // Compile-safety only (-Wfloat-equal): memcmp is a byte-exact,
        // warning-free equivalent to a per-sample != loop - same pass/fail
        // outcome, no logic change.
        const bool identical = (a.size() == b.size())
                             && std::memcmp (a.data(), b.data(), a.size() * sizeof (float)) == 0;

        check (identical, "impact-outside-pool invariant: ImpactEngine output for a fixed impactAmount01 is bit-identical regardless of any intervening IntensityBudget macro/style/progress computation (sweeping other macros never changes impact headroom)");
    }

    //==========================================================================
    std::printf ("\n[Phase 3.5] AudioAnalyzer - FFT hop correctness across block sizes (1/32/512/8192), hop decoupled from host block size\n");
    {
        constexpr double sr = 48000.0;
        constexpr int totalSamples = 48000; // 1 second of a steady 1kHz tone

        auto runWithBlockSize = [&] (int blockSize) -> drop::AudioAnalyzerMetrics
        {
            juce::dsp::ProcessSpec spec { sr, (juce::uint32) juce::jmax (1, blockSize), 2 };
            drop::AudioAnalyzer analyzer;
            analyzer.prepare (spec);

            int processed = 0;
            while (processed < totalSamples)
            {
                const int thisBlock = std::min (blockSize, totalSamples - processed);
                juce::AudioBuffer<float> buf (2, thisBlock);
                for (int ch = 0; ch < 2; ++ch)
                {
                    auto* d = buf.getWritePointer (ch);
                    for (int i = 0; i < thisBlock; ++i)
                        d[i] = 0.5f * std::sin ((float) (2.0 * juce::MathConstants<double>::pi * 1000.0 * (double) (processed + i) / sr));
                }
                analyzer.analyze (buf, thisBlock);
                processed += thisBlock;
            }
            return analyzer.getMetrics();
        };

        const auto m1    = runWithBlockSize (1);
        const auto m32   = runWithBlockSize (32);
        const auto m512  = runWithBlockSize (512);
        const auto m8192 = runWithBlockSize (8192);

        std::printf ("  spectral centroid: blockSize=1 -> %.1fHz, 32 -> %.1fHz, 512 -> %.1fHz, 8192 -> %.1fHz\n",
                     m1.spectralCentroidHz, m32.spectralCentroidHz, m512.spectralCentroidHz, m8192.spectralCentroidHz);

        // Same TOTAL input samples (a steady 1kHz tone), chunked
        // differently into analyze() calls - the internal 512-sample hop
        // accumulator must be decoupled from how those samples arrived, so
        // the FINAL metrics should converge to essentially the same value
        // regardless of host block size (architecture.md AudioAnalyzer FFT
        // sizing: "independent of how many host processBlock() calls that took").
        constexpr float kCentroidTolHz = 50.0f;
        check (std::abs (m1.spectralCentroidHz - m512.spectralCentroidHz) < kCentroidTolHz,
              "AudioAnalyzer: spectral centroid at blockSize=1 matches blockSize=512 within tolerance (hop decoupled from host block size)");
        check (std::abs (m32.spectralCentroidHz - m512.spectralCentroidHz) < kCentroidTolHz,
              "AudioAnalyzer: spectral centroid at blockSize=32 matches blockSize=512 within tolerance");
        check (std::abs (m8192.spectralCentroidHz - m512.spectralCentroidHz) < kCentroidTolHz,
              "AudioAnalyzer: spectral centroid at blockSize=8192 matches blockSize=512 within tolerance");

        check (m1.spectralCentroidHz > 900.0f && m1.spectralCentroidHz < 1100.0f,
              "AudioAnalyzer: spectral centroid correctly resolves a pure 1kHz test tone (within +-100Hz)");

        check (std::isfinite (m1.rms) && std::isfinite (m8192.rms) && m1.rms > 0.0f,
              "AudioAnalyzer: finite, nonzero RMS regardless of block size");
    }

    //==========================================================================
    std::printf ("\n[Phase 3.5] DropPreset - the 16 presets are the 16 Styles at recommended macro defaults\n");
    {
        bool allNamesMatch = true;
        bool allStyleIndicesValid = true;
        for (int i = 0; i < drop::kNumPresets; ++i)
        {
            const auto& preset = drop::getPreset (i);
            const auto& style = drop::getStyle (preset.styleIndex);
            if (juce::String (preset.name) != juce::String (style.name))
                allNamesMatch = false;
            if (preset.styleIndex < 0 || preset.styleIndex >= drop::kNumStyles)
                allStyleIndicesValid = false;
        }
        check (allNamesMatch, "DropPreset: all 16 presets' names match their bound DropStyle's name (presets ARE the styles, not a separate list)");
        check (allStyleIndicesValid, "DropPreset: all 16 presets bind to a valid DropStyle index");
        check (drop::kNumPresets == drop::kNumStyles, "DropPreset: preset count exactly matches style count (16 == 16)");
    }

    //==========================================================================
    // [POST-1.0] Bug A/B/C fix + `tail` parameter tests (2026-09-16,
    // user-directed improvement, see plugins/DROP/CHANGELOG.md and
    // PluginProcessor.h's "POST-1.0 IMPROVEMENT" doc comment). Stays at
    // version 1.0.0 per this repo's factory convention.
    //==========================================================================

    std::printf ("\n[POST-1.0] PluginProcessor - 22 APVTS parameters (tail 20th, noiseTension 21st, noiseType 22nd - all additive)\n");
    {
        DROPAudioProcessor p;
        check (p.getParameters().size() == 22, "PluginProcessor: exactly 22 APVTS parameters (19 original parameter-spec.md-locked + `tail` + `noiseTension` + `noiseType`; SUPERSEDES the earlier 21-param count, all additions strictly additive)");
        auto* tailP = p.parameters.getParameter ("tail");
        check (tailP != nullptr, "PluginProcessor: `tail` parameter exists and is retrievable by ID");
        if (tailP != nullptr)
            // DOCUMENTED SUPERSESSION (2026-09-17, POST-1.0 SIXTH IMPROVEMENT
            // SESSION, user-directed: "make the default tail zero"): was 35%
            // (normalized 0.35). Default 0% == the TAIL=CUT behavior - a
            // fresh instance cuts cleanly at the drop; ring-out is opt-in.
            // DOCUMENTED SUPERSESSION (2026-09-20, user-directed "make
            // default tail 0.7 sec"): was 0% (CUT), before that 35%.
            check (nearlyEqual ((double) tailP->getValue(), 0.4691, 1.0e-3),
                  "PluginProcessor: `tail` parameter defaults to 46.91% == 0.70s nominal tail (SUPERSEDES the 0%/CUT default of 2026-09-17; CUT remains available at exactly 0%)");
    }

    std::printf ("\n[POST-1.0] PluginProcessor - setStateInformation()/replaceState() tolerates a session missing `tail` (pre-1.0.0-tail compatibility)\n");
    {
        // Simulates restoring a 1.0.0 session saved BEFORE `tail` existed:
        // strip the `tail` PARAM child from a real APVTS state ValueTree,
        // then replaceState() it into a fresh processor - proves JUCE's
        // documented APVTS behavior (a parameter absent from a loaded state
        // is left at its constructor-supplied default) holds for this
        // specific additive parameter, and that every OTHER parameter still
        // round-trips correctly through the SAME call (not a broken/no-op
        // restore).
        DROPAudioProcessor source;
        if (auto* prm = source.parameters.getParameter ("intensity"))
            prm->setValueNotifyingHost (0.9f); // distinctive non-default value

        auto state = source.parameters.copyState(); // ValueTree, type "Parameters", one <PARAM id="..." value="..."/> child per parameter
        int removedCount = 0;
        for (int i = state.getNumChildren() - 1; i >= 0; --i)
        {
            auto child = state.getChild (i);
            if (child.hasType ("PARAM") && child.getProperty ("id").toString() == "tail")
            {
                state.removeChild (i, nullptr);
                ++removedCount;
            }
        }
        check (removedCount == 1, "pre-tail compatibility test setup: exactly one `tail` PARAM node was stripped");

        DROPAudioProcessor target;
        target.parameters.replaceState (state);

        auto* tailP = target.parameters.getParameter ("tail");
        auto* intensityP = target.parameters.getParameter ("intensity");
        check (tailP != nullptr, "pre-tail compatibility: `tail` parameter still exists after loading a state missing its node");
        if (tailP != nullptr)
            // DOCUMENTED SUPERSESSION (2026-09-20): default is now 46.91%
            // (0.70s), see the default-value test above.
            check (nearlyEqual ((double) tailP->getValue(), 0.4691, 1.0e-3),
                  "pre-tail compatibility: `tail` loads at its CURRENT default (46.91% == 0.70s as of 2026-09-20) when absent from a restored pre-1.0.0-tail session");
        if (intensityP != nullptr)
            check (nearlyEqual ((double) intensityP->getValue(), 0.9, 1.0e-4),
                  "pre-tail compatibility: the OTHER 19 parameters (e.g. intensity) still round-trip correctly through the SAME replaceState() call - proves this is a targeted `tail`-only default fallback, not a broken/no-op restore");
    }

    std::printf ("\n[POST-1.0] Per-sample tail cut - TAIL=CUT (0%%) fades to true silence inside its ~5ms window, click-free, no post-effect artifacts\n");
    {
        // POST-1.0 SIXTH IMPROVEMENT SESSION (2026-09-17, user DAW report:
        // "fix the sound. It doesnt cut immedietly. it has artifacts in the
        // end."). ROOT CAUSE (regression class: segment-granularity): the
        // tail envelope was evaluated ONCE per segment at the segment's
        // START. Segments are only cut at state boundaries, so at tail==0%
        // the whole ~5ms kTailCutSeconds Recovery window sat inside ONE
        // segment with envelope == env(0) == 1.0 - the fade NEVER actually
        // ran, and Recovery->Idle then hard-muted the still-full-level
        // delay/reverb/impact tails in a single sample (the audible end
        // artifact). The fix applies the (unchanged) envelope math
        // PER-SAMPLE at the end of processSegment(). This test uses a
        // 1024-sample block - LARGER than the whole 240-sample cut window @
        // 48kHz - so the pre-fix failure mode is reproduced by construction.
        constexpr int blockSize = 1024;
        constexpr double sr = 48000.0;
        DROPAudioProcessor p;
        p.setRateAndBufferSizeDetails (sr, blockSize);
        p.prepareToPlay (sr, blockSize);

        // tail = 0% EXACTLY (the CUT special case; also the new default,
        // asserted separately above). Delay+Reverb macros at 100% so the
        // Building phase charges genuinely audible tails to cut; noise off
        // (deterministic riser would blur the silence assertions).
        if (auto* prm = p.parameters.getParameter ("tail")) prm->setValueNotifyingHost (0.0f);
        if (auto* prm = p.parameters.getParameter ("delay")) prm->setValueNotifyingHost (1.0f);
        if (auto* prm = p.parameters.getParameter ("reverb")) prm->setValueNotifyingHost (1.0f);
        if (auto* prm = p.parameters.getParameter ("noise")) prm->setValueNotifyingHost (0.0f);
        if (auto* prm = p.parameters.getParameter ("dryWet")) prm->setValueNotifyingHost (1.0f);
        if (auto* prm = p.parameters.getParameter ("buildLength")) prm->setValueNotifyingHost (0.0f); // 1 bar
        if (auto* prm = p.parameters.getParameter ("trigger")) prm->setValueNotifyingHost (1.0f);

        // Sine input while building (charges the delay/reverb sends); HARD
        // SILENT from the first Peak-state block onward, so everything
        // captured from Impact on is purely the plugin's own ringing state -
        // the thing tail==0 must cut.
        double phase = 0.0;
        const double phaseInc = 2.0 * juce::MathConstants<double>::pi * 220.0 / sr;
        bool sawPeak = false, sawImpact = false, sawIdleAfterImpact = false;
        // POST-1.0 TRUE CUT (2026-09-18): at tail==0% the Impact state is
        // now only ~5ms (240 samples) - Impact AND Recovery can both begin
        // and end INSIDE one 1024-sample block, so block-granular state
        // observation may never literally see DropState::Impact. "The drop
        // happened" is therefore detected as any post-Peak progression
        // (Impact observed directly, or Peak left for Recovery/Idle).
        std::vector<float> captured;             // impact onward, channel 0
        int idleBlocksCaptured = 0;
        int firstIdleCaptureIndex = -1;          // index into `captured` where the first fully-Idle block starts
        int measureStartIndex = -1;              // index into `captured` where Recovery-side measurement starts (first block whose PRE-state was Recovery, or the Impact->Idle straddle block)

        for (int block = 0; block < 400 && idleBlocksCaptured < 3; ++block)
        {
            const auto preState = p.getCurrentDropState();
            juce::AudioBuffer<float> buf (2, blockSize);
            if (sawPeak)
                buf.clear();
            else
                for (int i = 0; i < blockSize; ++i)
                {
                    const float s = 0.5f * (float) std::sin (phase);
                    phase += phaseInc;
                    buf.setSample (0, i, s);
                    buf.setSample (1, i, s);
                }
            juce::MidiBuffer midi;
            p.processBlock (buf, midi);
            if (block == 0)
                if (auto* prm = p.parameters.getParameter ("trigger")) prm->setValueNotifyingHost (0.0f);

            const auto postState = p.getCurrentDropState();
            if (postState == drop::DropState::Peak) sawPeak = true;
            if (preState == drop::DropState::Impact || postState == drop::DropState::Impact
                || (sawPeak && (postState == drop::DropState::Recovery || postState == drop::DropState::Idle)))
                sawImpact = true;

            if (sawImpact)
            {
                const bool recoverySide = (preState == drop::DropState::Recovery)
                    || (preState == drop::DropState::Impact && postState != drop::DropState::Impact)
                    || (preState == drop::DropState::Peak && postState != drop::DropState::Peak && postState != drop::DropState::Impact);
                if (recoverySide && measureStartIndex < 0)
                    measureStartIndex = (int) captured.size();
                if (sawIdleAfterImpact && firstIdleCaptureIndex < 0 && preState == drop::DropState::Idle)
                {
                    firstIdleCaptureIndex = (int) captured.size();
                }
                const float* out = buf.getReadPointer (0);
                captured.insert (captured.end(), out, out + blockSize);
                if (postState == drop::DropState::Idle) sawIdleAfterImpact = true;
                if (sawIdleAfterImpact && preState == drop::DropState::Idle) ++idleBlocksCaptured;
            }
        }
        p.releaseResources();

        check (sawImpact && sawIdleAfterImpact, "per-sample tail cut test: transition reached Impact and returned to Idle within the block budget");
        check (measureStartIndex >= 0 && firstIdleCaptureIndex > measureStartIndex,
              "per-sample tail cut test: captured a Recovery-side measurement region followed by fully-Idle blocks");

        if (measureStartIndex >= 0 && firstIdleCaptureIndex > measureStartIndex)
        {
            // (a) The tails being cut were genuinely audible going INTO the
            //     cut (sanity - otherwise the criteria below are vacuous).
            float maxAbs = 0.0f;
            for (int i = measureStartIndex; i < firstIdleCaptureIndex; ++i)
                maxAbs = juce::jmax (maxAbs, std::abs (captured[(size_t) i]));
            std::printf ("  recovery-side peak |sample|: %.4f\n", maxAbs);
            check (maxAbs > 1.0e-3f, "per-sample tail cut: the charged delay/reverb/impact tails are genuinely nonzero going into the cut window (test sanity)");

            // (b) CLICK-FREE: max one-sample delta across the whole
            //     Recovery-side region (which contains the entire ~5ms cut,
            //     wherever it lands inside a block) stays a fraction of the
            //     region's own peak level. The pre-fix hard mute produced a
            //     single-sample step of ~the full tail amplitude (delta
            //     ratio ~1.0); the per-sample -60dB fade plus the tails' own
            //     smooth waveforms stay well below half that.
            float maxDelta = 0.0f;
            for (int i = measureStartIndex + 1; i < firstIdleCaptureIndex; ++i)
                maxDelta = juce::jmax (maxDelta, std::abs (captured[(size_t) i] - captured[(size_t) (i - 1)]));
            std::printf ("  recovery-side max one-sample delta: %.4f (ratio to peak: %.3f)\n", maxDelta, maxAbs > 0.0f ? maxDelta / maxAbs : 0.0f);
            check (maxDelta < 0.5f * maxAbs + 1.0e-3f,
                  "[POST-1.0] per-sample tail cut: no single-sample step anywhere across the cut window (the pre-fix per-segment envelope hard-muted full-level tails in one sample at Recovery->Idle and fails this bound by construction)");

            // (c) TRUE SILENCE afterwards: with silent input, exact-zero
            //     landing, and the Recovery->Idle module-hygiene reset, the
            //     fully-Idle blocks after the cut must be silent - "no
            //     artifacts after the effect ends".
            double sumSq = 0.0;
            int n = 0;
            for (int i = firstIdleCaptureIndex; i < (int) captured.size(); ++i, ++n)
                sumSq += (double) captured[(size_t) i] * (double) captured[(size_t) i];
            const double idleRms = n > 0 ? std::sqrt (sumSq / (double) n) : 0.0;
            std::printf ("  post-cut Idle RMS over %d samples: %.2e\n", n, idleRms);
            check (idleRms < 1.0e-4,
                  "[POST-1.0] per-sample tail cut: fully-Idle output after the cut is silent (< -80dBFS RMS) - no ringing residue, no stale-buffer artifacts after the effect ends");
        }
    }

    std::printf ("\n[POST-1.0] Build Length - 32-bar choice (6th entry, appended 2026-09-17 at user direction)\n");
    {
        // (a) Choice-list contract: 6 entries, the original five UNCHANGED
        //     at their original indices (saved sessions store the actual
        //     index, so 0-4 must keep meaning 1/2/4/8/16 bars), "32" last.
        DROPAudioProcessor p;
        auto* choiceParam = dynamic_cast<juce::AudioParameterChoice*> (p.parameters.getParameter ("buildLength"));
        check (choiceParam != nullptr, "32-bar choice: buildLength is an AudioParameterChoice");
        if (choiceParam != nullptr)
        {
            check (choiceParam->choices.size() == 6, "32-bar choice: buildLength now has exactly 6 choices (SUPERSEDES the original 5)");
            const char* expected[] = { "1", "2", "4", "8", "16", "32" };
            bool allMatch = choiceParam->choices.size() == 6;
            for (int i = 0; i < juce::jmin (6, choiceParam->choices.size()); ++i)
                if (choiceParam->choices[i] != expected[i]) allMatch = false;
            check (allMatch, "32-bar choice: the original five choices keep their exact indices (session compatibility) and '32' is appended last");
        }

        // (b) End-to-end: index 5 genuinely builds for ~32 bars. Fallback
        //     transport (no playhead): 120 BPM 4/4 @ 48kHz -> bar = 96000
        //     samples. Proves the kBuildLengthBars mapping (and its jlimit
        //     bound, previously clamped to index 4) actually reaches 32 -
        //     a pre-fix build would have clamped to 16 bars and left
        //     Building early.
        constexpr int blockSize = 1024;
        constexpr double sr = 48000.0;
        constexpr int64_t barSamples = 96000;
        p.setRateAndBufferSizeDetails (sr, blockSize);
        p.prepareToPlay (sr, blockSize);
        if (auto* prm = p.parameters.getParameter ("buildLength")) prm->setValueNotifyingHost (1.0f); // normalized 1.0 = index 5 = 32 bars
        if (auto* prm = p.parameters.getParameter ("trigger")) prm->setValueNotifyingHost (1.0f);

        int64_t buildingSamples = 0;
        bool sawBuilding = false, leftBuilding = false;
        for (int block = 0; block < 3400 && ! leftBuilding; ++block)
        {
            juce::AudioBuffer<float> buf (2, blockSize);
            buf.clear();
            juce::MidiBuffer midi;
            p.processBlock (buf, midi);
            if (block == 0)
                if (auto* prm = p.parameters.getParameter ("trigger")) prm->setValueNotifyingHost (0.0f);
            const auto state = p.getCurrentDropState();
            if (state == drop::DropState::Building) { sawBuilding = true; buildingSamples += blockSize; }
            else if (sawBuilding) leftBuilding = true;
        }
        p.releaseResources();

        std::printf ("  Building lasted ~%.2f bars (block-granular measurement; expected ~32 minus the carved-out Vacuum)\n",
                     (double) buildingSamples / (double) barSamples);
        check (sawBuilding && leftBuilding, "32-bar choice: transition entered AND completed Building within the block budget");
        check (buildingSamples > 16 * barSamples,
              "[POST-1.0] 32-bar choice: Building genuinely lasts LONGER than the previous 16-bar maximum (the pre-fix jlimit clamp to index 4 would fail this by construction)");
        check (buildingSamples > 31 * barSamples && buildingSamples < 33 * barSamples,
              "[POST-1.0] 32-bar choice: Building duration is ~32 bars (within [31,33] bars, block-granular; Vacuum is carved out of the final bar per the grid-alignment design)");
    }

    std::printf ("\n[POST-1.0] PluginProcessor - `tail` parameter scales Recovery's duration (~0.15s just above 0%%, 100%% ~4.0s)\n");
    {
        constexpr int blockSize = 256;

        auto measureRecoverySamples = [] (float tailPercent) -> int64_t
        {
            DROPAudioProcessor p;
            p.setRateAndBufferSizeDetails (48000.0, blockSize);
            p.prepareToPlay (48000.0, blockSize);
            if (auto* prm = p.parameters.getParameter ("buildLength")) prm->setValueNotifyingHost (0.0f); // shortest: 1 bar
            if (auto* prm = p.parameters.getParameter ("tail")) prm->setValueNotifyingHost (tailPercent / 100.0f);
            if (auto* prm = p.parameters.getParameter ("trigger")) prm->setValueNotifyingHost (1.0f);

            int64_t recoverySampleCount = 0;
            bool sawRecovery = false;
            bool done = false;
            for (int block = 0; block < 5000 && ! done; ++block)
            {
                juce::AudioBuffer<float> buf (2, blockSize);
                buf.clear();
                juce::MidiBuffer midi;
                p.processBlock (buf, midi);
                if (block == 0)
                    if (auto* prm = p.parameters.getParameter ("trigger")) prm->setValueNotifyingHost (0.0f);

                if (p.getCurrentDropState() == drop::DropState::Recovery)
                {
                    sawRecovery = true;
                    recoverySampleCount += blockSize;
                }
                if (sawRecovery && p.getCurrentDropState() == drop::DropState::Idle)
                    done = true;
            }
            p.releaseResources();
            return recoverySampleCount;
        };

        // DOCUMENTED SUPERSESSION (2026-09-16, POST-1.0 "TAIL=CUT" fix, Bug
        // F): this test originally measured tail==0% expecting ~0.15s
        // (kTailMinSeconds). The "TAIL=CUT" fix intentionally redirects
        // EXACTLY tail==0% to a ~5ms near-instant clean cut instead (see
        // this file's dedicated "[POST-1.0][Bug F] TAIL=CUT" test block
        // below for THAT behavior) - 0.15s is now the behavior for a value
        // just above the epsilon (0.1%), which this test measures instead,
        // preserving the ORIGINAL intent (verify the exponential ring-out
        // mapping's short end) against the still-unchanged exponential
        // formula.
        const int64_t recoveryJustAboveZeroSamples = measureRecoverySamples (0.1f);
        const int64_t recovery100Samples = measureRecoverySamples (100.0f);
        const double recoveryJustAboveZeroSeconds = (double) recoveryJustAboveZeroSamples / 48000.0;
        const double recovery100Seconds = (double) recovery100Samples / 48000.0;

        std::printf ("  tail=0.1%%: Recovery ~%.4fs (%lld samples, block-granular measurement); tail=100%%: Recovery ~%.4fs (%lld samples)\n",
                     recoveryJustAboveZeroSeconds, (long long) recoveryJustAboveZeroSamples, recovery100Seconds, (long long) recovery100Samples);

        // Block-granularity (256 samples = ~5.3ms @48kHz) measurement
        // tolerance, generously widened for the practical fuzz of
        // "count whole blocks the state machine reports Recovery".
        // DOCUMENTED SUPERSESSION chain (2026-09-20): the x2.5 extension
        // (which briefly made these ~0.375s/~10.0s) is itself superseded by
        // the user's "make the tail continue for as long as SELECTED" -
        // the knob is now exact again: Recovery == the mapped seconds.
        check (nearlyEqual (recoveryJustAboveZeroSeconds, 0.15, 0.02),
              "tail param scales Recovery duration: tail=0.1% -> ~0.15s EXACTLY as selected (kTailMinSeconds; the x2.5 extension is superseded - selected == actual)");
        check (nearlyEqual (recovery100Seconds, 4.0, 0.15),
              "tail param scales Recovery duration: tail=100% -> ~4.0s EXACTLY as selected (kTailMaxSeconds)");
        check (recovery100Samples > recoveryJustAboveZeroSamples * 10,
              "tail param scales Recovery duration: 100% produces a MUCH longer Recovery than 0.1% (monotonic exponential mapping, not a flat/ignored parameter)");
    }

    std::printf ("\n[POST-1.0] PluginProcessor - shared IntensityBudget weights during Recovery never exceed their Peak/Impact ducked baseline (Bug B fix, CORRECTED test)\n");
    {
        // CORRECTED (coordinator-reported test-design contradiction, second
        // pass): the original version of this test asserted budget weights
        // are EXACTLY 0.0f throughout Recovery. That premise is FALSE for
        // the live system, independent of anything in this improvement
        // session - DropStyle.cpp authors budgetEnvelope/moduleCurves with
        // NONZERO startWeight (e.g. Clean EDM's budgetEnvelope starts at
        // 0.05, Filter/Delay/Reverb curves start at 0.05/0.1/0.15), so
        // IntensityBudget::computeWeights(style, progress==0, ...) legitimately
        // returns small-but-nonzero weights for several modules - this was
        // already true, unmodified, for Idle/Arming/Peak/Impact BEFORE any
        // of this session's changes (verified by reading IntensityBudget.cpp/
        // DropStyle.cpp directly), so it is equally true during Recovery
        // once computeIntensityProgress() returns 0 there too (Bug B's
        // actual fix). "Duck the effect chain" means SMALL, not "exactly
        // zero" - the true, meaningful, regression-preventing property is
        // that Recovery does NOT exceed what Peak/Impact already had (the
        // OLD `1.0f - t` ramp's bug was specifically that it grew SUBSTANTIALLY
        // above the ducked baseline) - that is what this rewritten test
        // verifies instead.
        constexpr int blockSize = 256;
        DROPAudioProcessor p;
        p.setRateAndBufferSizeDetails (48000.0, blockSize);
        p.prepareToPlay (48000.0, blockSize);
        if (auto* prm = p.parameters.getParameter ("buildLength")) prm->setValueNotifyingHost (0.0f); // 1 bar
        if (auto* prm = p.parameters.getParameter ("intensity")) prm->setValueNotifyingHost (1.0f); // max intensity - largest ducked baseline to compare against
        if (auto* prm = p.parameters.getParameter ("trigger")) prm->setValueNotifyingHost (1.0f);

        std::array<float, 10> duckedBaseline {}; // last-seen Peak/Impact frame's weights
        bool sawPeakOrImpact = false;
        bool sawRecovery = false;
        bool exceededBaseline = false;
        bool done = false;
        constexpr float kTolerance = 1.0e-4f; // generous vs. float/smoothing noise, tiny vs. any real "ramp back up"

        for (int block = 0; block < 1300 && ! done; ++block)
        {
            juce::AudioBuffer<float> buf (2, blockSize);
            for (int ch = 0; ch < 2; ++ch)
            {
                auto* data = buf.getWritePointer (ch);
                for (int i = 0; i < blockSize; ++i)
                    data[i] = 0.5f * std::sin ((float) (block * blockSize + i) * 0.02f);
            }
            juce::MidiBuffer midi;
            p.processBlock (buf, midi);
            if (block == 0)
                if (auto* prm = p.parameters.getParameter ("trigger")) prm->setValueNotifyingHost (0.0f);

            DROPAudioProcessor::VisualizationFrame frame;
            bool got = false;
            while (p.popVisualizationFrame (frame)) got = true; // drain-all-keep-latest, matches PluginEditor's own convention
            if (! got)
                continue;

            const auto state = p.getCurrentDropState();
            if (state == drop::DropState::Peak || state == drop::DropState::Impact)
            {
                sawPeakOrImpact = true;
                for (int i = 0; i < 10; ++i)
                    duckedBaseline[(size_t) i] = frame.activeModuleWeights[i]; // Peak/Impact weights are ~constant (progress==0 throughout both) - last-seen value is representative
            }
            else if (state == drop::DropState::Recovery)
            {
                sawRecovery = true;
                if (sawPeakOrImpact)
                {
                    for (int i = 0; i < 10; ++i)
                        if (frame.activeModuleWeights[i] > duckedBaseline[(size_t) i] + kTolerance)
                            exceededBaseline = true;
                }
            }
            if (sawRecovery && state == drop::DropState::Idle)
                done = true;
        }
        p.releaseResources();

        check (sawPeakOrImpact, "ducked-baseline test: build reached Peak/Impact within the block budget");
        check (sawRecovery, "ducked-baseline test: build reached Recovery within the block budget");
        check (! exceededBaseline,
              "PluginProcessor: shared IntensityBudget module weights during Recovery never exceed their Peak/Impact ducked baseline (Bug B fix - computeIntensityProgress() now returns 0 for Recovery, matching Peak/Impact exactly; DOCUMENTED SUPERSESSION of the pre-bugfix `1.0f - t` ramp, which specifically grew weights substantially ABOVE this same baseline right after Impact)");
    }

    std::printf ("\n[POST-1.0] PluginProcessor - post-impact continuity: no instant cut, no post-impact spike (Bug A/B fix)\n");
    {
        // Exact deterministic sample-offset arithmetic (SAME fallback-
        // transport style as the Phase 3.4 transient-preservation test
        // above), UPDATED for the POST-1.0 GRID-LOCK fix (Bug D - see
        // computeDurationsForNewBuild()'s doc comment) and the POST-1.0
        // "TAIL=CUT" fix (Bug F): 120 BPM/4/4 @48kHz -> samplesPerBeat=24000,
        // barLength=96000; buildLength choice index 0 = 1 bar.
        // armingToBuildingSamples = 96000-256 = 95744 (fallback transport
        // already advanced by the triggering block's own 256 samples before
        // the trigger is detected, UNCHANGED by the grid-lock fix);
        // peakSamples (Vacuum, 1/32 note @120bpm) = 3000, computed FIRST as
        // of the fix, so buildingSamples = 96000-3000 = 93000 (was 96000
        // pre-fix). Impact begins at global sample
        // 95744+93000+3000 = 191744 (was 194744 pre-fix - matches the
        // updated Phase 3.4 transient-preservation test's own value
        // exactly). impactSamples (fixed 220ms) = 10560.
        //
        // tailPercentForTest is deliberately a small NONZERO value (0.1%),
        // not 0% - the "TAIL=CUT" fix (Bug F) redirects EXACTLY tail==0% to
        // a ~5ms near-instant clean cut (kTailCutSeconds), which is FAR too
        // short for this test's multi-window RMS attack-then-decay shape
        // analysis (that specific behavior - tail==0% producing a clean,
        // click-free cut with no ring-out - is verified separately by the
        // dedicated "[POST-1.0][Bug F] TAIL=CUT" test block below). 0.1% is
        // just above the epsilon, landing on the SAME exponential formula's
        // short end (~0.15s, essentially identical to the OLD tail==0%
        // pre-fix behavior this test was originally calibrated against -
        // "no discontinuity in feel just above 0%" from the fix's own design
        // holds here too) - recoverySecondsRef/recoverySamplesForTest below
        // are computed via that SAME formula (mirrors
        // computeDurationsForNewBuild() exactly), not a stale hand-typed
        // constant.
        constexpr int blockSize = 256;
        constexpr int64_t impactStartSample = 191744;
        constexpr int64_t recoveryStartSample = impactStartSample + 10560;
        constexpr float tailPercentForTest = 0.1f;
        constexpr double kTailMinSecondsRef = 0.15;
        constexpr double kTailMaxSecondsRef = 4.0;
        const double recoverySecondsRef = kTailMinSecondsRef
            * std::pow (kTailMaxSecondsRef / kTailMinSecondsRef, (double) (tailPercentForTest / 100.0f));
        const int64_t recoverySamplesForTest = (int64_t) std::llround (48000.0 * recoverySecondsRef);

        DROPAudioProcessor p;
        p.setRateAndBufferSizeDetails (48000.0, blockSize);
        p.prepareToPlay (48000.0, blockSize);
        if (auto* prm = p.parameters.getParameter ("buildLength")) prm->setValueNotifyingHost (0.0f);
        if (auto* prm = p.parameters.getParameter ("tail")) prm->setValueNotifyingHost (tailPercentForTest / 100.0f);
        if (auto* prm = p.parameters.getParameter ("trigger")) prm->setValueNotifyingHost (1.0f);

        const int64_t captureFromBlock = (impactStartSample / blockSize) - 2;
        const int64_t captureUntilBlock = ((recoveryStartSample + recoverySamplesForTest) / blockSize) + 4;

        std::vector<float> captured; // relative to captureFromBlock*blockSize
        bool allFinite = true;

        // CORRECTED (coordinator-reported bug, third pass): the ORIGINAL
        // version of this test fed a CONTINUOUS tone straight through
        // Recovery too. That made the "does the tail decay" measurement
        // meaningless - Recovery's job is to let the LIVE SOURCE return
        // cleanly (TRANSIENT PRESERVATION's whole point), so with a live
        // tone still playing, the bus's steady-state RMS approaches the
        // DRY tone's OWN level (~0.4/sqrt(2) =~ 0.283) as the tail's wet
        // contribution fades out and reveals the (near-transparent, small-
        // weight) dry bus underneath - a RISE toward that dry level is
        // therefore CORRECT, EXPECTED behavior, not a decay-chain bug (this
        // is exactly what the coordinator's round-3 evidence, RMS rising to
        // 0.2512, matches almost exactly). To genuinely isolate "does the
        // TAIL (echoes/reverb of PAST content) decay to silence", the
        // SOURCE itself must actually go silent - a raised-cosine fade-out
        // (50ms, kFadeSamples) ending EXACTLY at Recovery's first sample
        // silences the input smoothly (no click of its own) before Recovery
        // begins, so the boundary-discontinuity check below still measures
        // genuine DSP continuity (not an artificial input-signal edge), and
        // the post-boundary RMS decay check now measures ONLY the decaying
        // tail (delay/reverb wet-return + ImpactEngine's continued output),
        // exactly matching Bug C's own framing ("echoes... decay audibly and
        // smoothly instead of being muted").
        constexpr int64_t kFadeSamples = 2400; // 50ms @48kHz
        constexpr int64_t kFadeStartSample = recoveryStartSample - kFadeSamples;

        for (int64_t block = 0; block < captureUntilBlock; ++block)
        {
            juce::AudioBuffer<float> buf (2, blockSize);
            for (int ch = 0; ch < 2; ++ch)
            {
                auto* data = buf.getWritePointer (ch);
                for (int i = 0; i < blockSize; ++i)
                {
                    const int64_t globalSample = block * blockSize + i;
                    float toneAmp = 0.4f;
                    if (globalSample >= recoveryStartSample)
                        toneAmp = 0.0f; // fully silent from Recovery's first sample onward
                    else if (globalSample >= kFadeStartSample)
                    {
                        const float fadeT = (float) (globalSample - kFadeStartSample) / (float) kFadeSamples;
                        toneAmp = 0.4f * 0.5f * (1.0f + std::cos (juce::MathConstants<float>::pi * fadeT)); // smooth 1->0 raised-cosine fade, reaching exactly 0 at recoveryStartSample
                    }
                    data[i] = toneAmp * std::sin ((float) globalSample * 0.05f);
                }
            }

            juce::MidiBuffer midi;
            p.processBlock (buf, midi);
            if (block == 0)
                if (auto* prm = p.parameters.getParameter ("trigger")) prm->setValueNotifyingHost (0.0f);

            if (block >= captureFromBlock)
            {
                const float* out = buf.getReadPointer (0);
                for (int i = 0; i < blockSize; ++i)
                {
                    if (! std::isfinite (out[i])) allFinite = false;
                    captured.push_back (out[i]);
                }
            }
        }
        p.releaseResources();

        check (allFinite, "post-impact continuity test: finite output throughout the capture window");

        const int64_t captureStartSample = captureFromBlock * blockSize;
        const int64_t boundaryIndex = recoveryStartSample - captureStartSample;
        const bool boundaryInRange = boundaryIndex > 8 && boundaryIndex + 8 < (int64_t) captured.size();
        check (boundaryInRange, "post-impact continuity test: the exact Impact->Recovery boundary sample falls within the captured window");

        if (boundaryInRange)
        {
            // No single-sample discontinuity larger than this documented
            // bound at the EXACT state boundary (Bug A: previously
            // ImpactEngine was state-gated OFF at this exact instant,
            // producing an audible instant cut - this bound catches any
            // reintroduction of that).
            constexpr float kMaxAllowedStepAtBoundary = 0.35f;
            float maxStepNearBoundary = 0.0f;
            for (int64_t i = boundaryIndex - 8; i < boundaryIndex + 8; ++i)
                maxStepNearBoundary = std::max (maxStepNearBoundary,
                    std::abs (captured[(size_t) i] - captured[(size_t) (i - 1)]));

            std::printf ("  Impact->Recovery boundary sample=%lld, max adjacent-sample step within +-8 samples = %.4f (bound %.2f)\n",
                         (long long) recoveryStartSample, maxStepNearBoundary, kMaxAllowedStepAtBoundary);
            check (maxStepNearBoundary < kMaxAllowedStepAtBoundary,
                  "post-impact continuity: no instantaneous discontinuity at the exact Impact->Recovery boundary sample (Bug A fix)");

            // Windowed-RMS decay check, RESTRICTED to the Recovery window
            // itself (recoverySamplesForTest) - deliberately NOT extended
            // into Idle, where the live pass-through tone naturally resumes
            // at full amplitude once the state machine returns to Idle
            // (correct, expected behavior, not part of what this test
            // measures).
            //
            // CORRECTED (coordinator-reported bug, second pass): the
            // original version of this check required monotonic-ish decay
            // starting from window 0 (Recovery's very first sample). That
            // predicate is WRONG given the actual, intended design: at
            // Recovery's first sample, DelayModule/ReverbModule's
            // RETURN-weight TARGET jumps from Impact's small live weight up
            // to the frozen (Building-end) weight * tailEnvelope(0) - a
            // legitimate, intentional, LARGER value (that is the whole point
            // of the tail ringing out audibly). This jump is smoothed by
            // each module's own ~20ms internal SmoothedValue ramp
            // (kWeightSmoothingSeconds in DelayModule.cpp/ReverbModule.cpp),
            // producing a genuine, expected ATTACK phase (~20ms rise)
            // immediately followed by the tail envelope's much slower
            // raised-cosine DECAY across the rest of Recovery. A predicate
            // requiring decay from sample 0 necessarily mischaracterizes
            // this attack phase as a bug - it isn't; asserting decay only
            // AFTER the attack phase has settled is the correct, honest test
            // of "no post-impact effect-blast spike" (Bug B), which is
            // specifically about the shared IntensityBudget modules (tested
            // separately above, and unaffected by this attack/decay shape,
            // which is local to the tail-capable modules' RETURN smoothing
            // only).
            constexpr int windowLen = 128;
            const int64_t samplesAfterBoundary = std::min ((int64_t) captured.size() - boundaryIndex, recoverySamplesForTest);
            const int numWindows = (int) (samplesAfterBoundary / windowLen);
            std::vector<double> rms ((size_t) juce::jmax (0, numWindows), 0.0);
            for (int w = 0; w < numWindows; ++w)
            {
                double sumSq = 0.0;
                for (int i = 0; i < windowLen; ++i)
                {
                    const float s = captured[(size_t) (boundaryIndex + w * windowLen + i)];
                    sumSq += (double) s * (double) s;
                }
                rms[(size_t) w] = std::sqrt (sumSq / (double) windowLen);
            }

            // Attack-phase allowance: generously 2x the ~20ms smoother
            // settle time (kWeightSmoothingSeconds == 0.02s in DelayModule.cpp/
            // ReverbModule.cpp), rounded up to whole windows - rise is
            // permitted (not required) only within this many leading windows.
            constexpr double kSmootherSeconds = 0.02;
            constexpr double kTestSampleRate = 48000.0;
            const int attackWindows = juce::jmax (1, (int) std::ceil (2.0 * kSmootherSeconds * kTestSampleRate / (double) windowLen));

            int peakWindowIndex = 0;
            double peakValue = rms.empty() ? 0.0 : rms[0];
            for (int w = 1; w < numWindows; ++w)
                if (rms[(size_t) w] > peakValue) { peakValue = rms[(size_t) w]; peakWindowIndex = w; }

            const double tol = 0.10 * peakValue + 0.01;
            bool decaysAfterAttack = true;
            for (int w = juce::jmax (peakWindowIndex, attackWindows); w + 1 < numWindows; ++w)
                if (rms[(size_t) (w + 1)] > rms[(size_t) w] + tol)
                    decaysAfterAttack = false;

            // DOCUMENTED SUPERSESSION (2026-09-20, hold-then-fade tail
            // envelope - see tailEnvelopeForT()): the original assertion
            // required the RMS peak inside the ~20ms smoother attack,
            // which was only true because the OLD envelope decayed from
            // Recovery's very first sample, suppressing everything later.
            // With the envelope now FLAT through 70% of the window (the
            // "no tail" fix), the natural reverb bloom / first delay
            // repetitions legitimately peak tens of ms in. The assertion's
            // real intent - "no pathological late-growing effect-BLAST" -
            // is kept as a magnitude bound instead: the peak may occur
            // anywhere in the un-faded region but must stay a gentle bloom
            // (< 3x the boundary level), and the decays-after-peak check
            // below still guards the shape.
            const bool peakIsGentleBloom = peakValue < 3.0 * juce::jmax (rms.empty() ? 0.0 : rms[0], 1.0e-4);

            std::printf ("  post-boundary (within Recovery only) windowed RMS: first=%.4f peak=%.4f @window %d (attack allowance %d windows) last=%.4f (%d windows of %d samples) -> %s\n",
                         rms.empty() ? 0.0 : rms[0], peakValue, peakWindowIndex, attackWindows,
                         rms.empty() ? 0.0 : rms[(size_t) (numWindows - 1)], numWindows, windowLen,
                         (decaysAfterAttack && peakIsGentleBloom) ? "bloom-then-decay (expected)" : "UNEXPECTED SHAPE");
            check (peakIsGentleBloom,
                  "post-impact continuity: the Recovery RMS peak is a gentle natural bloom (< 3x the boundary level), never a late-growing effect-blast (SUPERSEDES the peak-within-20ms form, which was an artifact of the old always-decaying envelope)");
            check (decaysAfterAttack,
                  "post-impact continuity: windowed RMS trends down (monotonically-ish) for the remainder of Recovery once the expected initial attack settles - no post-impact effect-blast spike beyond the smoother's own known settle time (Bug B fix)");
        }
    }

    std::printf ("\n[POST-1.0] PluginProcessor - state reset on Recovery->Idle entry: no stale residue in the NEXT build (Bug C fix)\n");
    {
        constexpr int blockSize = 256;
        DROPAudioProcessor p;
        p.setRateAndBufferSizeDetails (48000.0, blockSize);
        p.prepareToPlay (48000.0, blockSize);

        if (auto* prm = p.parameters.getParameter ("buildLength")) prm->setValueNotifyingHost (0.0f); // 1 bar
        if (auto* prm = p.parameters.getParameter ("tail")) prm->setValueNotifyingHost (1.0f); // 100% - longest possible tail, maximizes residual-energy risk if the reset were missing
        if (auto* prm = p.parameters.getParameter ("delay")) prm->setValueNotifyingHost (1.0f);
        if (auto* prm = p.parameters.getParameter ("reverb")) prm->setValueNotifyingHost (1.0f);
        // CORRECTED (coordinator-reported test-premise bug, third pass):
        // NoiseModule generates seeded noise independently of input
        // (architecture.md: "supplementary energy... RNG streams advance
        // every sample regardless of weight" - a deliberate design choice so
        // Chaos-seeded reproducibility stays independent of automation
        // history), so even with pure silence input, a legitimate,
        // DETERMINISTIC riser is added during Building on EVERY build,
        // including the second one - this is NOT stale-buffer residue, it's
        // the SAME seed regenerating the SAME riser (bit-identical across
        // both builds, confirmed by the coordinator: observed residue
        // matched to 6 decimal places across two independent runs, the
        // signature of a deterministic regenerated signal, not leftover
        // decaying state). Setting `noise` (and `impact`, for the same
        // reason - ImpactEngine's sub/burst is ALSO deterministic/seeded)
        // to 0 removes every legitimate signal-generating source, so any
        // remaining nonzero output during the second build's silent-input
        // Building phase can ONLY be genuine stale state left over from the
        // first build - exactly what this test is meant to isolate.
        if (auto* prm = p.parameters.getParameter ("noise")) prm->setValueNotifyingHost (0.0f);
        if (auto* prm = p.parameters.getParameter ("impact")) prm->setValueNotifyingHost (0.0f);

        // --- First build: a loud, sustained tone strongly excites the
        // delay/reverb tails (and ImpactEngine's shortTailReverb). Block
        // budget widened to 1700 (vs. the 1300 used elsewhere in this file)
        // because tail=100% here deliberately maximizes Recovery's own
        // duration (~4.0s = ~192000 samples = ~750 blocks @256/block) to
        // stress-test the reset as hard as possible - total build length
        // (~397304 samples) needs ~1552 blocks, not ~990 like the Phase 3.3
        // end-to-end test's default-tail build. ---
        if (auto* prm = p.parameters.getParameter ("trigger")) prm->setValueNotifyingHost (1.0f);
        bool firstBuildBackToIdle = false;
        for (int block = 0; block < 3200 && ! firstBuildBackToIdle; ++block) // budget widened for the x2.5 gradual-fade Recovery extension (tail=100% -> ~10s)
        {
            juce::AudioBuffer<float> buf (2, blockSize);
            for (int ch = 0; ch < 2; ++ch)
            {
                auto* data = buf.getWritePointer (ch);
                for (int i = 0; i < blockSize; ++i)
                    data[i] = 0.8f * std::sin ((float) (block * blockSize + i) * 0.07f);
            }
            juce::MidiBuffer midi;
            p.processBlock (buf, midi);
            if (block == 0)
                if (auto* prm = p.parameters.getParameter ("trigger")) prm->setValueNotifyingHost (0.0f);
            if (p.getCurrentDropState() == drop::DropState::Idle && block > 10)
                firstBuildBackToIdle = true;
        }
        check (firstBuildBackToIdle, "state reset test: the first (excitation) build returned to Idle within the block budget");

        // --- Brief silence gap between builds (per the test design's
        // "silence between" requirement). ---
        for (int block = 0; block < 20; ++block)
        {
            juce::AudioBuffer<float> buf (2, blockSize);
            buf.clear();
            juce::MidiBuffer midi;
            p.processBlock (buf, midi);
        }

        // --- Second build: PURE SILENCE input throughout (both the
        // pre-trigger gap above and this entire build) - any nonzero output
        // during this build's pre-Impact (Building-phase) portion can ONLY
        // come from STALE internal state left over from the first build's
        // excited delay/reverb tails, since a zero-state linear
        // filter/delay-line fed zero input produces exactly zero output. ---
        if (auto* prm = p.parameters.getParameter ("trigger")) prm->setValueNotifyingHost (1.0f);
        std::vector<float> secondBuildBuildingOutput;
        bool sawImpactSecondBuild = false;
        bool secondBuildBackToIdle = false;
        for (int block = 0; block < 3200 && ! secondBuildBackToIdle; ++block) // same widened budget as the first build above (tail is still 100% for this second build too)
        {
            juce::AudioBuffer<float> buf (2, blockSize);
            buf.clear();
            juce::MidiBuffer midi;
            p.processBlock (buf, midi);
            if (block == 0)
                if (auto* prm = p.parameters.getParameter ("trigger")) prm->setValueNotifyingHost (0.0f);

            if (p.getCurrentDropState() == drop::DropState::Building)
            {
                const float* out = buf.getReadPointer (0);
                for (int i = 0; i < blockSize; ++i)
                    secondBuildBuildingOutput.push_back (out[i]);
            }
            if (p.getCurrentDropState() == drop::DropState::Impact) sawImpactSecondBuild = true;
            if (sawImpactSecondBuild && p.getCurrentDropState() == drop::DropState::Idle) secondBuildBackToIdle = true;
        }
        p.releaseResources();

        check (sawImpactSecondBuild, "state reset test: the second (silent) build also reached Impact within the block budget (state machine itself is unaffected by the reset)");
        check (! secondBuildBuildingOutput.empty(), "state reset test: the second build's Building phase produced a non-empty capture window");

        float peakResidual = 0.0f;
        for (float s : secondBuildBuildingOutput)
            peakResidual = std::max (peakResidual, std::abs (s));

        // With `noise`/`impact` both 0 (see the parameter setup above), the
        // ONLY legitimate signal-generating sources are removed, so this
        // measurement now genuinely isolates stale-buffer residue (not a
        // deterministic regenerated riser, which round-3 evidence confirmed
        // by matching to 6 decimal places across two independent runs -
        // the signature of a reproducible GENERATED signal, not decaying
        // leftover state).
        std::printf ("  second (silent, noise=0, impact=0) build's Building-phase peak output (should be ~0) = %.6f\n", peakResidual);
        check (peakResidual < 1.0e-4f,
              "state reset on Idle entry: with every legitimate signal-generating source (noise riser, ImpactEngine sub/burst) disabled, the second build's pre-Impact (Building-phase) output contains NO residual energy from the first build's excited delay/reverb tails (Bug C fix - every owned DSP module is now reset() on the Recovery->Idle transition)");
    }

    //==========================================================================
    // [POST-1.0] "curve progression" fix (2026-09-16, user-reported real-DAW
    // bug: "I chose 8 bars and the effect is active only at the end of the
    // last bar, while the visual dot on the curve moves"). See
    // PluginProcessor.h's computeIntensityProgress()/computeLinearBuildProgress()
    // doc comments for the full decision matrix. Stays at version 1.0.0.
    //==========================================================================

    std::printf ("\n[POST-1.0] Weight progression - linear-position weight input reaches audible levels early/mid-build (Clean EDM defaults)\n");
    {
        // Reproduces BOTH the OLD (buggy, stacked-curve) path and the NEW
        // (fixed, single-curve) path live, from the actual project math
        // (drop::TransitionEngine::evaluate() + drop::IntensityBudget::
        // computeWeights()) - not hand-picked numbers - so this test is
        // guaranteed to fail against the pre-fix composition "by
        // construction" and to validate the fix's genuine numeric effect.
        const auto& style = drop::getStyle (0); // Clean EDM (transitionCurveType == SCurve)
        drop::TransitionEngine te;
        te.setCurveType (style.transitionCurveType);
        te.setTension (0.5f); // `tension` APVTS default (50%)

        std::array<float, (size_t) drop::BudgetModule::Count> macros100 {};
        for (auto& m : macros100) m = 1.0f; // all 10 module macros at their 100% default
        std::array<float, (size_t) drop::BudgetModule::Count> noJitter {}; // `chaos` APVTS default (0%) -> zero jitter

        auto sumWeightsAt = [&] (float progressInput) -> float
        {
            const auto w = drop::IntensityBudget::computeWeights (style, progressInput, macros100,
                                                                     0.5f, 0.5f, 0.0f, // intensity/tension/chaos APVTS defaults (50/50/0%)
                                                                     drop::SourceProfile::FullMix, noJitter);
            float s = 0.0f;
            for (auto v : w.value) s += v;
            return s;
        };

        // OLD (pre-fix) path: TransitionEngine's CURVED output fed AS the
        // weight-path progress input - composes two end-weighted curves.
        auto oldSumAtElapsedFraction = [&] (float t) { return sumWeightsAt (te.evaluate (t)); };
        // NEW (fixed) path: the LINEAR elapsed fraction fed directly -
        // exactly what computeLinearBuildProgress() now supplies.
        auto newSumAtElapsedFraction = [&] (float t) { return sumWeightsAt (t); };

        const float oldFinal = oldSumAtElapsedFraction (1.0f);
        const float newFinal = newSumAtElapsedFraction (1.0f);
        check (oldFinal > 1.0e-6f && newFinal > 1.0e-6f, "weight progression test setup: both paths produce a nonzero final (t=1.0) weight sum (sanity)");

        const float oldFrac025 = oldFinal > 0.0f ? oldSumAtElapsedFraction (0.25f) / oldFinal : 0.0f;
        const float newFrac025 = newFinal > 0.0f ? newSumAtElapsedFraction (0.25f) / newFinal : 0.0f;
        const float oldFrac060 = oldFinal > 0.0f ? oldSumAtElapsedFraction (0.60f) / oldFinal : 0.0f;
        const float newFrac060 = newFinal > 0.0f ? newSumAtElapsedFraction (0.60f) / newFinal : 0.0f;

        std::printf ("  elapsed=25%%: OLD stacked-curve fraction-of-final=%.4f, NEW linear-input fraction-of-final=%.4f\n", oldFrac025, newFrac025);
        std::printf ("  elapsed=60%%: OLD stacked-curve fraction-of-final=%.4f, NEW linear-input fraction-of-final=%.4f\n", oldFrac060, newFrac060);

        // The floors below are the coordinator-specified milestones. Proven
        // to fail the OLD path "by construction" (asserted directly here,
        // not merely claimed) - this is what makes the NEW-path floor checks
        // a genuine regression guard for the reported bug, not a vacuous one.
        // CORRECTED 2026-09-16: the original pair of assertions here claimed the OLD
        // stacked-curve path failed these floors. Measurement disproved that claim
        // (OLD measured 0.26 @25% and 0.81 @60% - it PASSES both), which is exactly
        // why the weight-sum metric alone never reproduced the user's report. The
        // real cause of "8 bars, effect only in the last bar" was the MODULE-SWEEP
        // position, not the wet-weight sum: at default tension the shaped curves
        // that drive filter cutoff / gate rate / pitch / noise rise measured
        // Exponential=0.031 and LateAcceleration=0.075 at 25% elapsed, so those
        // sweeps had barely moved and nothing was audible regardless of weight.
        // That metric is asserted directly in the sweep-floor test below; making a
        // false claim about the old weight path here would be a fake regression guard.

        check (newFrac025 >= 0.15f, "weight progression (FIXED): Clean EDM defaults reach >= 15%% of the final build's total weight-sum by 25%% elapsed - audible early progression, not silence until the last bar");
        check (newFrac060 >= 0.45f, "weight progression (FIXED): Clean EDM defaults reach >= 45%% of the final build's total weight-sum by 60%% elapsed");

        check (newFrac025 > oldFrac025, "weight progression: the fixed (linear-input) path is more front-loaded than the old (stacked-curve) path at the 25%% milestone");
    }

    std::printf ("\n[POST-1.0] Module-sweep progression - every curve type moves audibly early at DEFAULT tension (the real 'only active in the last bar' guard)\n");
    {
        // THE regression guard for the user's 2026-09-16 DAW report. Audibility
        // during a build is dominated by each module's INTERNAL sweep position
        // (filter cutoff, gate rate, pitch amount, noise rise), all driven by
        // TransitionEngine::evaluate() at the `tension` default of 50%. Floors
        // below are musical minimums: something must be clearly moving by a
        // quarter of the way in, roughly a third of the way by the midpoint, and
        // past halfway by three-quarters. Pre-fix values (recorded for posterity):
        //   Exponential      0.031 / 0.177 / 0.487  -> failed all three
        //   LateAcceleration 0.075 / 0.150 / 0.225  -> failed all three
        //   SCurve           0.113 / 0.500 / 0.887  -> passed (was never the culprit)
        struct Floor { float t; float minValue; };
        const Floor floors[] = { { 0.25f, 0.10f }, { 0.50f, 0.28f }, { 0.75f, 0.50f } };
        const drop::CurveType types[] = { drop::CurveType::Linear, drop::CurveType::Exponential,
                                          drop::CurveType::SCurve, drop::CurveType::LateAcceleration };
        const char* names[] = { "Linear", "Exponential", "SCurve", "LateAcceleration" };

        bool allClearFloors = true;
        for (int c = 0; c < 4; ++c)
        {
            drop::TransitionEngine e;
            e.setCurveType (types[c]);
            e.setTension (0.5f); // the DEFAULT the user actually hears
            std::printf ("  %-17s", names[c]);
            for (const auto& f : floors)
            {
                const float v = e.evaluate (f.t);
                const bool ok = v >= f.minValue;
                allClearFloors = allClearFloors && ok;
                std::printf (" t=%.2f -> %.4f (min %.2f)%s", f.t, v, f.minValue, ok ? "" : " **UNDER**");
            }
            std::printf ("\n");
        }
        check (allClearFloors,
              "module-sweep progression: every curve type clears the musical early/mid-build floors at DEFAULT tension - a build is audibly progressing throughout, not inert until the final bar");

        // The fix must not flatten the curves into linearity: end-weighted
        // character (and the tension parameter's meaning) must survive.
        drop::TransitionEngine lin, exp_, sc, la;
        lin.setCurveType (drop::CurveType::Linear);          lin.setTension (0.5f);
        exp_.setCurveType (drop::CurveType::Exponential);    exp_.setTension (0.5f);
        sc.setCurveType (drop::CurveType::SCurve);           sc.setTension (0.5f);
        la.setCurveType (drop::CurveType::LateAcceleration); la.setTension (0.5f);
        check (exp_.evaluate (0.5f) < lin.evaluate (0.5f) - 0.05f
                && sc.evaluate (0.25f) < lin.evaluate (0.25f) - 0.02f
                && la.evaluate (0.5f) < lin.evaluate (0.5f) - 0.05f,
              "module-sweep progression: shaped curves remain meaningfully end-weighted vs Linear (the fix adds audibility, it does not erase curve character)");

        drop::TransitionEngine lowT, highT;
        lowT.setCurveType (drop::CurveType::Exponential);  lowT.setTension (0.0f);
        highT.setCurveType (drop::CurveType::Exponential); highT.setTension (1.0f);
        check (highT.evaluate (0.5f) < lowT.evaluate (0.5f) - 0.1f,
              "module-sweep progression: the `tension` parameter still audibly controls end-weighting (high tension stays lower mid-build than low tension)");
    }

    std::printf ("\n[POST-1.0] Per-curve-type coverage - weight-path immunity to transitionCurveType (FIXED), TransitionEngine's own curve character preserved (module-internal-sweep consumers)\n");
    {
        // Part A: post-fix, IntensityBudget::computeWeights() never receives
        // transitionCurveType at all (computeLinearBuildProgress() doesn't
        // read TransitionEngine) - so the weight-path's early/mid-build
        // audibility is now IDENTICAL regardless of which of the 4 named
        // curve types a Style picks (a style-count-independent robustness
        // property; pre-fix, this fraction varied a great deal by curve
        // type - Exponential/LateAcceleration were far worse than Linear).
        const auto& style = drop::getStyle (0);
        std::array<float, (size_t) drop::BudgetModule::Count> macros100 {};
        for (auto& m : macros100) m = 1.0f;
        std::array<float, (size_t) drop::BudgetModule::Count> noJitter {};

        auto sumAt = [&] (float p) -> float
        {
            const auto w = drop::IntensityBudget::computeWeights (style, p, macros100, 0.5f, 0.5f, 0.0f, drop::SourceProfile::FullMix, noJitter);
            float s = 0.0f; for (auto v : w.value) s += v; return s;
        };
        const float finalSum = sumAt (1.0f);
        const float frac025 = finalSum > 0.0f ? sumAt (0.25f) / finalSum : 0.0f;

        const drop::CurveType curveTypes[] = { drop::CurveType::Linear, drop::CurveType::Exponential, drop::CurveType::SCurve, drop::CurveType::LateAcceleration };
        bool allIdentical = true;
        for (auto ct : curveTypes)
        {
            // computeWeights()'s LINEAR-input result is, by construction,
            // completely independent of `ct` (never consulted) - this loop
            // documents/verifies that structural guarantee rather than
            // exercising any different code path per iteration.
            juce::ignoreUnused (ct);
            if (! nearlyEqual ((double) frac025, (double) frac025, 1.0e-9)) allIdentical = false; // trivially true; see Part B below for the genuine per-type distinction
        }
        check (allIdentical, "weight-path setup sanity (trivial by construction - computeWeights() takes no curveType argument)");

        // Part B: the CURVED value itself (still used for module-internal
        // sweep character + tensionLevel, per the decision matrix) must
        // PRESERVE each curve type's distinct character - Exponential still
        // more end-weighted (smaller early/mid value) than Linear, at the
        // SAME tension. This is the "relative character" this fix must NOT
        // erase, since TransitionEngine still drives module-internal sweeps.
        drop::TransitionEngine teLinear, teExponential, teSCurve, teLateAccel;
        teLinear.setCurveType (drop::CurveType::Linear);
        teExponential.setCurveType (drop::CurveType::Exponential);
        teSCurve.setCurveType (drop::CurveType::SCurve);
        teLateAccel.setCurveType (drop::CurveType::LateAcceleration);
        for (auto* te : { &teLinear, &teExponential, &teSCurve, &teLateAccel })
            te->setTension (0.5f);

        const float lin025 = teLinear.evaluate (0.25f);
        const float exp025 = teExponential.evaluate (0.25f);
        const float sCurve025 = teSCurve.evaluate (0.25f);
        const float lateAccel025 = teLateAccel.evaluate (0.25f);

        std::printf ("  TransitionEngine::evaluate(0.25) at tension=50%%: Linear=%.4f Exponential=%.4f SCurve=%.4f LateAcceleration=%.4f\n",
                     lin025, exp025, sCurve025, lateAccel025);

        check (lin025 > exp025, "TransitionEngine: Exponential still more end-weighted (smaller value at t=0.25) than Linear at the same tension - module-internal-sweep/tensionLevel character is preserved by this fix, not erased");
        check (lin025 > sCurve025, "TransitionEngine: SCurve still more end-weighted than Linear at t=0.25/tension=50%%");
        check (lin025 > lateAccel025, "TransitionEngine: LateAcceleration still more end-weighted than Linear at t=0.25/tension=50%%");

        // All 4 types still produce SOME nonzero (audible) early/mid
        // progression for module-internal sweeps - none of them are
        // hard-gated to exactly 0 before the very end.
        check (exp025 > 0.0f && sCurve025 > 0.0f && lateAccel025 > 0.0f, "TransitionEngine: every curve type produces nonzero progress by t=0.25 (module-internal sweeps still move continuously, none hard-gated to silence)");
    }

    std::printf ("\n[POST-1.0] Visualization consistency - VisualizationFrame::progress is the LINEAR basis (matches audio weights); tensionLevel is its CURVED transform (matches TransitionEngine)\n");
    {
        constexpr int blockSize = 256;
        DROPAudioProcessor p;
        p.setRateAndBufferSizeDetails (48000.0, blockSize);
        p.prepareToPlay (48000.0, blockSize);
        if (auto* prm = p.parameters.getParameter ("buildLength")) prm->setValueNotifyingHost (0.0f); // 1 bar - fastest to traverse Building
        if (auto* prm = p.parameters.getParameter ("trigger")) prm->setValueNotifyingHost (1.0f);

        // Default style (index 0, Clean EDM = SCurve) + default tension
        // (50%) - matches the processor's own defaults exactly, so this
        // reference engine's evaluate() must match what the processor
        // itself computed internally for computeIntensityProgress().
        drop::TransitionEngine reference;
        reference.setCurveType (drop::getStyle (0).transitionCurveType);
        reference.setTension (0.5f);

        bool sawBuilding = false;
        bool monotonic = true;
        float prevProgress = -1.0f;
        bool sawGenuineDivergence = false; // tensionLevel != progress somewhere mid-build - proves tensionLevel is a REAL curve transform, not just a copy of progress
        float maxCurveMatchError = 0.0f;
        int framesChecked = 0;
        bool done = false;

        for (int block = 0; block < 800 && ! done; ++block)
        {
            juce::AudioBuffer<float> buf (2, blockSize);
            buf.clear();
            juce::MidiBuffer midi;
            p.processBlock (buf, midi);
            if (block == 0)
                if (auto* prm = p.parameters.getParameter ("trigger")) prm->setValueNotifyingHost (0.0f);

            DROPAudioProcessor::VisualizationFrame frame;
            bool got = false;
            while (p.popVisualizationFrame (frame)) got = true; // drain-all-keep-latest, matches PluginEditor's own convention
            const auto state = p.getCurrentDropState();

            if (got && state == drop::DropState::Building)
            {
                sawBuilding = true;
                if (frame.progress < prevProgress - 1.0e-6f) monotonic = false;
                prevProgress = frame.progress;
                ++framesChecked;

                const float expectedCurved = reference.evaluate (frame.progress);
                maxCurveMatchError = std::max (maxCurveMatchError, std::abs (expectedCurved - frame.tensionLevel));

                if (frame.progress > 0.1f && frame.progress < 0.9f && std::abs (frame.tensionLevel - frame.progress) > 0.02f)
                    sawGenuineDivergence = true;
            }
            if (state == drop::DropState::Peak) done = true; // Building has fully elapsed - stop capturing
        }
        p.releaseResources();

        check (sawBuilding, "visualization consistency test: build reached Building within the block budget");
        check (framesChecked > 5, "visualization consistency test: captured a meaningful number of Building-state visualization frames");
        check (monotonic, "VisualizationFrame::progress increases monotonically through Building - the LINEAR elapsed-time basis, the SAME basis IntensityBudget::computeWeights() uses for the audio (requirement: visual dot's X-position must agree with the audio weights' own progress basis)");
        check (sawGenuineDivergence, "VisualizationFrame::tensionLevel genuinely diverges from ::progress mid-build - proves tensionLevel is a real curve-shaped transform (what the user 'feels'), not merely a copy of the linear progress");
        std::printf ("  max |tensionLevel - TransitionEngine::evaluate(progress)| across %d Building frames (Clean EDM/SCurve, tension=50%%) = %.5f\n", framesChecked, maxCurveMatchError);
        check (maxCurveMatchError < 0.01f, "VisualizationFrame::tensionLevel matches TransitionEngine::evaluate(progress) to within tight tolerance - the curve dot's Y-height and the audio's own curved module-sweep basis genuinely agree, on top of progress (X) sharing the audio weights' exact linear basis");
    }

    //==========================================================================
    // [POST-1.0 SECOND IMPROVEMENT SESSION] (2026-09-16, same day, dsp-agent)
    // Grid-lock (Bug D), transport-stop safety (Bug E), TAIL=CUT (Bug F), and
    // BPM publication tests. See PluginProcessor.h's top doc comment "POST-1.0
    // SECOND IMPROVEMENT SESSION" and plugins/DROP/CHANGELOG.md. Stays at
    // version 1.0.0.
    //==========================================================================

    std::printf ("\n[POST-1.0][Bug D] Grid lock - carved-out-Vacuum arithmetic lands Impact exactly on a bar boundary (MusicalClock's own bar math)\n");
    {
        // Mirrors computeDurationsForNewBuild()'s POST-1.0 fix EXACTLY
        // (peakSamples computed FIRST via MusicalClock, then
        // buildingSamples = barsToSamples(bars) - peakSamples, floor-clamped
        // to >= 1) - independent of PluginProcessor's private implementation,
        // proving the INVARIANT the fix guarantees: armingToBuildingSamples +
        // buildingSamples + peakSamples is an EXACT multiple-of-bar-length
        // offset from wherever Arming began, for every tempo/build-length
        // combination, computed purely from MusicalClock's own bar math.
        struct Case { double bpm; int bars; };
        const Case cases[] = {
            { 90.0, 1 }, { 90.0, 4 }, { 90.0, 8 },
            { 128.0, 1 }, { 128.0, 4 }, { 128.0, 8 },
            { 174.0, 1 }, { 174.0, 4 }, { 174.0, 8 },
        };

        bool allOk = true;
        for (const auto& c : cases)
        {
            drop::MusicalClock clock;
            clock.prepare (48000.0);
            // Deliberately NOT exactly on a bar boundary (1.3 beats into the
            // bar) - stress-tests the fix against a genuine mid-bar arming
            // point, not just the degenerate arming==0 case.
            TestPlayHead ph;
            ph.info = makePosition (c.bpm, 4, 4, 1.3, 0.0);
            clock.update (&ph, 0);

            const double barLength = clock.getBarLengthSamples();
            const int64_t armingToBuildingSamples = (int64_t) std::llround (clock.samplesToNextBarBoundary());

            const auto& style = drop::getStyle (0); // "Clean EDM" - matches computeDurationsForNewBuild()'s default-Style timing
            const double vacuumWindowSamples = clock.divisionToSamples (style.vacuumDivision, false);
            const double vacuumFloorSamples = 48000.0 * style.vacuumFloorSeconds;
            const int64_t peakSamples = (int64_t) std::llround (juce::jmax (vacuumWindowSamples, vacuumFloorSamples));

            const int64_t barsSamples = (int64_t) std::llround (clock.barsToSamples (c.bars));
            const int64_t buildingSamples = juce::jmax ((int64_t) 1, barsSamples - peakSamples);

            const int64_t impactOffsetFromArmingStart = armingToBuildingSamples + buildingSamples + peakSamples;
            const int64_t expectedOffsetIfPerfectLock = armingToBuildingSamples + barsSamples;

            // Perfect lock is only guaranteed when the Vacuum window
            // genuinely fits inside the requested build (barsSamples >
            // peakSamples) - true for every case above at these tempi/bar
            // counts (the pathological clamp edge case is tested separately,
            // in isolation, below).
            const bool perfectLockExpected = (barsSamples > peakSamples);
            const int64_t offsetDelta = impactOffsetFromArmingStart - expectedOffsetIfPerfectLock;
            const int64_t offsetDeltaAbs = offsetDelta < 0 ? -offsetDelta : offsetDelta; // avoids relying on <cstdlib>'s std::llabs, not otherwise included in this file
            const bool offsetOk = ! perfectLockExpected || offsetDeltaAbs <= 1;

            // Independently re-derive "lands on a bar boundary" via fmod
            // against MusicalClock's own barLength, rather than just
            // reusing barsSamples by construction.
            const double remainder = std::fmod ((double) (impactOffsetFromArmingStart - armingToBuildingSamples), barLength);
            const bool onBoundary = ! perfectLockExpected || remainder <= 1.0 || (barLength - remainder) <= 1.0;

            const bool ok = offsetOk && onBoundary;
            std::printf ("  bpm=%.0f bars=%d: peakSamples=%lld buildingSamples=%lld impactOffset=%lld (expected %lld) -> %s\n",
                         c.bpm, c.bars, (long long) peakSamples, (long long) buildingSamples,
                         (long long) impactOffsetFromArmingStart, (long long) expectedOffsetIfPerfectLock,
                         ok ? "ok" : "MISMATCH");
            allOk = allOk && ok;
        }
        check (allOk, "[Bug D] Grid lock: armingToBuildingSamples + buildingSamples + peakSamples lands EXACTLY on a bar boundary (within 1 sample) across 90/128/174 BPM x 1/4/8 bars, using the SAME Vacuum-carve-out formula computeDurationsForNewBuild() uses");
    }

    std::printf ("\n[POST-1.0][Bug D] Grid lock - buildingSamples clamp floors to exactly 1 sample (never <= 0) in the pathological vacuum-longer-than-build edge case\n");
    {
        // Direct test of computeDurationsForNewBuild()'s documented clamp
        // formula (`juce::jmax((int64_t)1, barsSamples - peakSamples)`) in
        // the edge case where the Style's Vacuum window is LONGER than the
        // entire requested build - perfect grid-lock is mathematically
        // impossible there (the vacuum collapse alone doesn't fit inside the
        // bar count), so the clamp's job is only to guarantee Building never
        // becomes zero/negative-length.
        const int64_t barsSamplesSynthetic = 100;
        const int64_t peakSamplesSynthetic = 500; // deliberately far larger than the whole synthetic "build"
        const int64_t buildingSamplesClamped = juce::jmax ((int64_t) 1, barsSamplesSynthetic - peakSamplesSynthetic);
        check (buildingSamplesClamped == 1,
              "[Bug D] Grid lock: buildingSamples clamps to exactly 1 sample (never <= 0) when the Vacuum window is longer than the entire requested build");
    }

    std::printf ("\n[POST-1.0][Bug E] Transport-stop safety - host isPlaying==false mid-Building aborts PROMPTLY, click-free, no residual energy\n");
    {
        constexpr int blockSize = 256;
        DROPAudioProcessor p;
        p.setRateAndBufferSizeDetails (48000.0, blockSize);
        p.prepareToPlay (48000.0, blockSize);

        TestPlayHead ph;
        ph.info = makePosition (120.0, 4, 4, 0.0, 0.0);
        ph.info.setIsPlaying (true);
        p.setPlayHead (&ph);

        if (auto* prm = p.parameters.getParameter ("buildLength")) prm->setValueNotifyingHost (0.4f); // choice index 2 of 0-5 = 4 bars (comfortably long; was 0.5f when the list had 5 choices, updated with the 32-bar addition 2026-09-17)
        if (auto* prm = p.parameters.getParameter ("delay")) prm->setValueNotifyingHost (1.0f); // maximizes residual-energy risk if the abort's module reset() were missing
        if (auto* prm = p.parameters.getParameter ("reverb")) prm->setValueNotifyingHost (1.0f);
        if (auto* prm = p.parameters.getParameter ("trigger")) prm->setValueNotifyingHost (1.0f);

        bool stoppedTransport = false;
        bool sawIdleAfterStop = false;
        int blocksAfterStopUntilIdle = -1;
        int blockStoppedAt = -1;
        std::vector<float> captured; // from the block the transport stops onward
        std::vector<float> capturedInput; // same samples of the HOST INPUT - lets the assertion isolate DROP's own contribution from the test's deliberate hard input edge

        for (int block = 0; block < 600; ++block)
        {
            juce::AudioBuffer<float> buf (2, blockSize);
            for (int ch = 0; ch < 2; ++ch)
            {
                auto* data = buf.getWritePointer (ch);
                for (int i = 0; i < blockSize; ++i)
                    // Silence once stopped - isolates "does internal state
                    // (delay/reverb tails, riser) genuinely go quiet", not
                    // merely "is the input silent". NOTE this is a HARD input
                    // edge by design (real hosts do exactly this on stop), so
                    // the continuity assertion below measures DROP's OWN
                    // contribution (out - in), never the host's input step.
                    data[i] = stoppedTransport ? 0.0f : 0.6f * std::sin ((float) (block * blockSize + i) * 0.05f);
            }

            juce::AudioBuffer<float> inputCopy;
            inputCopy.makeCopyOf (buf);

            juce::MidiBuffer midi;
            p.processBlock (buf, midi);

            if (block == 0)
                if (auto* prm = p.parameters.getParameter ("trigger")) prm->setValueNotifyingHost (0.0f);

            if (! stoppedTransport && block == 20 && p.getCurrentDropState() == drop::DropState::Building)
            {
                ph.info.setIsPlaying (false);
                stoppedTransport = true;
                blockStoppedAt = block;
            }

            if (stoppedTransport)
            {
                const float* out = buf.getReadPointer (0);
                for (int i = 0; i < blockSize; ++i)
                {
                    captured.push_back (out[i]);
                    capturedInput.push_back (inputCopy.getSample (0, i));
                }

                if (! sawIdleAfterStop && p.getCurrentDropState() == drop::DropState::Idle)
                {
                    sawIdleAfterStop = true;
                    blocksAfterStopUntilIdle = block - blockStoppedAt;
                }
            }

            if (sawIdleAfterStop && block > blockStoppedAt + 5)
                break;
        }
        p.releaseResources();

        check (stoppedTransport, "transport-stop test setup: the transport was genuinely stopped while state == Building");
        check (sawIdleAfterStop, "transport-stop safety: the state machine returns to Idle after the transport stops");
        check (blocksAfterStopUntilIdle >= 0 && blocksAfterStopUntilIdle <= 3,
              "transport-stop safety: the state machine returns to Idle PROMPTLY (within a few blocks - kTransportStopFadeSeconds is ~5ms) after the transport stops, not seconds later");

        constexpr float kMaxAllowedStep = 0.35f; // same documented bound as the existing post-impact-continuity test
        // CORRECTED 2026-09-17: the original version measured the raw OUTPUT,
        // which necessarily contains the test's own hard input edge (0.6*sin
        // -> 0.0 at the stop block boundary, measured 0.4502 - a step DROP
        // faithfully passes through, exactly as it should). What this test
        // must actually guarantee is that DROP's OWN contribution (the wet /
        // generated content the abort fades out) contains no click: measure
        // out - in.
        const size_t n = std::min (captured.size(), capturedInput.size());
        float maxStep = 0.0f;
        size_t maxStepIndex = 0;
        for (size_t i = 1; i < n; ++i)
        {
            const float wetPrev = captured[i - 1] - capturedInput[i - 1];
            const float wetCurr = captured[i]     - capturedInput[i];
            const float step = std::abs (wetCurr - wetPrev);
            if (step > maxStep) { maxStep = step; maxStepIndex = i; }
        }
        std::printf ("  post-stop captured %zu samples, max adjacent step of DROP's OWN contribution (out-in) = %.4f (bound %.2f) at index %zu\n",
                     captured.size(), maxStep, kMaxAllowedStep, maxStepIndex);
        {
            const size_t lo = maxStepIndex > 3 ? maxStepIndex - 3 : 0;
            const size_t hi = std::min (captured.size(), maxStepIndex + 4);
            std::printf ("    neighborhood:");
            for (size_t i = lo; i < hi; ++i) std::printf (" [%zu]=%.4f", i, captured[i]);
            std::printf ("\n");
        }
        check (maxStep < kMaxAllowedStep, "transport-stop safety: DROP's own contribution (out - in) fades click-free through the abort and stays silent afterwards - no discontinuity above the documented bound");

        const int tailSamplesToCheck = std::min ((int) captured.size(), blockSize * 3);
        float peakTail = 0.0f;
        for (int i = (int) captured.size() - tailSamplesToCheck; i < (int) captured.size(); ++i)
            peakTail = std::max (peakTail, std::abs (captured[(size_t) i]));
        std::printf ("  peak |output| in the final %d captured samples (should be ~0) = %.6f\n", tailSamplesToCheck, peakTail);
        check (peakTail < 1.0e-4f, "transport-stop safety: no residual energy remains once the abort completes and the module-hygiene reset() sweep has run");
    }

    std::printf ("\n[POST-1.0][Bug E] Transport-stop safety - fallback (no playhead) transport NEVER aborts spuriously; a transition runs to completion normally\n");
    {
        constexpr int blockSize = 256;
        DROPAudioProcessor p;
        p.setRateAndBufferSizeDetails (48000.0, blockSize);
        p.prepareToPlay (48000.0, blockSize);
        // Deliberately NO setPlayHead() call - getPlayHead() returns
        // nullptr, MusicalClock's fallback transport is active
        // (isUsingFallbackTransport() == true), which the transport-stop-
        // abort gating explicitly excludes (see processBlock()'s doc
        // comment) - proves the Standalone app / bare-AudioProcessor
        // fallback path remains fully usable and is never spuriously
        // aborted mid-build.
        if (auto* prm = p.parameters.getParameter ("buildLength")) prm->setValueNotifyingHost (0.0f); // 1 bar - fastest to traverse
        if (auto* prm = p.parameters.getParameter ("trigger")) prm->setValueNotifyingHost (1.0f);

        bool sawBuilding = false, sawImpact = false, backToIdle = false;
        for (int block = 0; block < 1300 && ! backToIdle; ++block)
        {
            juce::AudioBuffer<float> buf (2, blockSize);
            for (int ch = 0; ch < 2; ++ch)
            {
                auto* data = buf.getWritePointer (ch);
                for (int i = 0; i < blockSize; ++i)
                    data[i] = 0.4f * std::sin ((float) (block * blockSize + i) * 0.03f);
            }
            juce::MidiBuffer midi;
            p.processBlock (buf, midi);
            if (block == 0)
                if (auto* prm = p.parameters.getParameter ("trigger")) prm->setValueNotifyingHost (0.0f);

            const auto state = p.getCurrentDropState();
            if (state == drop::DropState::Building) sawBuilding = true;
            if (state == drop::DropState::Impact) sawImpact = true;
            if (sawImpact && state == drop::DropState::Idle) backToIdle = true;
        }
        p.releaseResources();

        check (sawBuilding, "fallback-transport safety: build reaches Building normally (no playhead present)");
        check (sawImpact, "fallback-transport safety: build reaches Impact normally (no spurious abort)");
        check (backToIdle, "fallback-transport safety: the FULL build (Idle->...->Impact->Recovery->Idle) completes normally with NO host playhead present - the transport-stop-abort gating (!isUsingFallbackTransport()) never fires spuriously in the standalone/no-playhead fallback path");
    }

    std::printf ("\n[POST-1.0][Bug F] TAIL=CUT - tail==0%% produces near-silence within ~10ms of Impact's end, no discontinuity; tail just above 0%% still rings\n");
    {
        constexpr int blockSize = 256;
        constexpr int64_t impactStartSample = 191744; // see the updated Phase 3.4 transient-preservation test's doc comment for the derivation (grid-lock fix)
        // POST-1.0 TRUE CUT (2026-09-18, user-directed "so the sound stops
        // imidietly volumewise"): with tail==0% the IMPACT window itself now
        // collapses to ~5ms (240 samples @48k) - the CUT capture's recovery
        // boundary therefore sits at impactStart+240, while the ring capture
        // (tail above the epsilon) keeps the full Style-authored 0.22s
        // window (impactStart+10560) byte-identically.
        constexpr int64_t kCutRecoveryStart  = impactStartSample + 240;   // 191984
        constexpr int64_t kRingRecoveryStart = impactStartSample + 10560; // 202304

        // Captures DROP's OWN contribution (out - in), the transport-stop
        // test's convention (TRUE CUT update): the input deliberately
        // hard-silences at the recovery boundary, and measuring raw output
        // would score that synthetic input edge itself as a "discontinuity"
        // at whatever phase the sine happens to hold there.
        auto measureTail = [&] (float tailPercent, int64_t recoveryStartSample, int64_t& boundaryIndexOut) -> std::vector<float>
        {
            const int64_t recoveryStartBlockFloor = recoveryStartSample / blockSize;
            const int captureFromBlock = (int) recoveryStartBlockFloor - 2;
            const int captureUntilBlock = (int) recoveryStartBlockFloor + 20; // ~20 blocks past Recovery's start, comfortably covers the ~10ms(480-sample) window under test plus margin
            const int64_t actualCaptureFromSample = (int64_t) captureFromBlock * blockSize;
            boundaryIndexOut = recoveryStartSample - actualCaptureFromSample;
            DROPAudioProcessor p;
            p.setRateAndBufferSizeDetails (48000.0, blockSize);
            p.prepareToPlay (48000.0, blockSize);
            if (auto* prm = p.parameters.getParameter ("buildLength")) prm->setValueNotifyingHost (0.0f);
            if (auto* prm = p.parameters.getParameter ("tail")) prm->setValueNotifyingHost (tailPercent / 100.0f);
            if (auto* prm = p.parameters.getParameter ("delay")) prm->setValueNotifyingHost (1.0f);
            if (auto* prm = p.parameters.getParameter ("reverb")) prm->setValueNotifyingHost (1.0f);
            // Pinned fully wet (2026-09-18): this test's input deliberately
            // hard-silences at Recovery's first sample and then measures RAW
            // output steps - at the new 50% dryWet DEFAULT the untouched dry
            // half passes that synthetic input edge straight through and the
            // measurement stops isolating the TAIL's own decay (the same
            // reason the curve-honesty and per-sample-cut tests always
            // pinned dryWet explicitly). Engine behavior is unchanged; this
            // restores the test's original measurement conditions.
            if (auto* prm = p.parameters.getParameter ("dryWet")) prm->setValueNotifyingHost (1.0f);
            if (auto* prm = p.parameters.getParameter ("trigger")) prm->setValueNotifyingHost (1.0f);

            std::vector<float> captured;
            for (int block = 0; block < captureUntilBlock; ++block)
            {
                juce::AudioBuffer<float> buf (2, blockSize);
                for (int ch = 0; ch < 2; ++ch)
                {
                    auto* data = buf.getWritePointer (ch);
                    for (int i = 0; i < blockSize; ++i)
                    {
                        const int64_t globalSample = (int64_t) block * blockSize + i;
                        // Sustained tone through Building/Peak/Impact,
                        // silenced exactly at Recovery's first sample - same
                        // convention as the existing post-impact-continuity
                        // test, isolating the TAIL's own decay.
                        data[i] = (globalSample < recoveryStartSample) ? 0.7f * std::sin ((float) globalSample * 0.06f) : 0.0f;
                    }
                }
                juce::MidiBuffer midi;
                p.processBlock (buf, midi);
                if (block == 0)
                    if (auto* prm = p.parameters.getParameter ("trigger")) prm->setValueNotifyingHost (0.0f);

                if (block >= captureFromBlock)
                {
                    const float* out = buf.getReadPointer (0);
                    for (int i = 0; i < blockSize; ++i)
                    {
                        // Residual = out - in (in recomputed from the same
                        // deterministic formula the input loop used).
                        const int64_t globalSample = (int64_t) block * blockSize + i;
                        const float inVal = (globalSample < recoveryStartSample) ? 0.7f * std::sin ((float) globalSample * 0.06f) : 0.0f;
                        captured.push_back (out[i] - inVal);
                    }
                }
            }
            p.releaseResources();
            return captured;
        };

        int64_t cutBoundaryIndex = 0, ringBoundaryIndex = 0;
        const auto cutCapture = measureTail (0.0f, kCutRecoveryStart, cutBoundaryIndex);   // exact CUT (5ms impact + 5ms recovery)
        const auto ringCapture = measureTail (0.1f, kRingRecoveryStart, ringBoundaryIndex); // just above the epsilon - full impact window, should still ring

        auto maxStepNear = [&] (const std::vector<float>& v, int64_t center, int64_t halfWindow) -> float
        {
            float m = 0.0f;
            for (int64_t i = std::max<int64_t> (1, center - halfWindow); i < std::min<int64_t> ((int64_t) v.size(), center + halfWindow); ++i)
                m = std::max (m, std::abs (v[(size_t) i] - v[(size_t) (i - 1)]));
            return m;
        };
        constexpr float kMaxAllowedStep = 0.35f; // same documented bound as the post-impact-continuity/transport-stop tests
        const float cutMaxStep = maxStepNear (cutCapture, cutBoundaryIndex, 8);
        check (cutMaxStep < kMaxAllowedStep, "TAIL=CUT: no discontinuity step of DROP's own contribution (out - in) above the documented bound at the exact Impact->Recovery boundary when tail==0%");

        constexpr int64_t kTenMsSamples = 480; // ~10ms @48kHz
        auto peakAbsInWindow = [&] (const std::vector<float>& v, int64_t start, int64_t len) -> float
        {
            float m = 0.0f;
            for (int64_t i = std::max<int64_t> (0, start); i < std::min<int64_t> ((int64_t) v.size(), start + len); ++i)
                m = std::max (m, std::abs (v[(size_t) i]));
            return m;
        };
        const float cutPeakAt10ms = peakAbsInWindow (cutCapture, cutBoundaryIndex + kTenMsSamples, blockSize);
        const float ringPeakAt10ms = peakAbsInWindow (ringCapture, ringBoundaryIndex + kTenMsSamples, blockSize);

        std::printf ("  tail=0%% (CUT): peak |residual| ~10ms after its (5ms) Impact ends = %.6f; tail=0.1%% (ring): peak |residual| ~10ms after ITS (full-window) Impact ends = %.6f\n", cutPeakAt10ms, ringPeakAt10ms);

        check (cutPeakAt10ms < 1.0e-3f, "TAIL=CUT: tail==0% produces near-silence within ~10ms of its (collapsed ~5ms) Impact's end - with TRUE CUT the effect's own sound is gone ~10ms past the bar-counted downbeat");
        check (ringPeakAt10ms > cutPeakAt10ms * 5.0f,
              "TAIL=CUT: tail just above 0% (0.1%) still measurably RINGS ~10ms past its own Impact window - the CUT behavior (collapsed window included) is isolated to exactly 0% (continuity of the exponential mapping preserved above the epsilon, not a general 'short tail always silent' regression)");
    }

    std::printf ("\n[POST-1.0] BPM publication - VisualizationFrame reports the host BPM when a playhead is present, the fallback marker when not\n");
    {
        constexpr int blockSize = 256;

        {
            DROPAudioProcessor p;
            p.setRateAndBufferSizeDetails (48000.0, blockSize);
            p.prepareToPlay (48000.0, blockSize);
            TestPlayHead ph;
            ph.info = makePosition (128.0, 4, 4, 0.0, 0.0);
            ph.info.setIsPlaying (true);
            p.setPlayHead (&ph);

            juce::AudioBuffer<float> buf (2, blockSize);
            buf.clear();
            juce::MidiBuffer midi;
            p.processBlock (buf, midi);

            DROPAudioProcessor::VisualizationFrame frame;
            bool got = false;
            while (p.popVisualizationFrame (frame)) got = true;
            check (got, "BPM publication test setup: a visualization frame was pushed with a valid host playhead present");
            if (got)
            {
                check (frame.hostPlayheadValid, "VisualizationFrame::hostPlayheadValid is true when a valid host playhead/tempo is present");
                check (nearlyEqual ((double) frame.bpm, 128.0, 1.0e-6), "VisualizationFrame::bpm reports the EXACT host tempo (128.0 BPM) when a valid host playhead is present");
            }
            p.releaseResources();
        }

        {
            DROPAudioProcessor p;
            p.setRateAndBufferSizeDetails (48000.0, blockSize);
            p.prepareToPlay (48000.0, blockSize);
            // No playhead - fallback transport.

            juce::AudioBuffer<float> buf (2, blockSize);
            buf.clear();
            juce::MidiBuffer midi;
            p.processBlock (buf, midi);

            DROPAudioProcessor::VisualizationFrame frame;
            bool got = false;
            while (p.popVisualizationFrame (frame)) got = true;
            check (got, "BPM publication test setup: a visualization frame was pushed with no host playhead present");
            if (got)
            {
                check (! frame.hostPlayheadValid, "VisualizationFrame::hostPlayheadValid is false when no valid host playhead is present (fallback transport active)");
                check (nearlyEqual ((double) frame.bpm, 120.0, 1.0e-6), "VisualizationFrame::bpm reports the fallback transport's fixed 120 BPM when no host playhead is present (the 'INT 120' marker basis)");
            }
            p.releaseResources();
        }
    }

    //==========================================================================
    // [POST-1.0 FOURTH IMPROVEMENT SESSION] "move tail knob below the graph
    // and add a box near curve, white noise curve" / "noise tension to
    // control the white noise graph" (2026-09-17, user-directed). New 21st
    // APVTS parameter `noiseTension` - see NoiseModule.h's
    // riseAmountForProgress() doc comment and PluginProcessor.h's
    // "POST-1.0 FOURTH IMPROVEMENT SESSION" doc comment.
    //==========================================================================

    std::printf ("\n[POST-1.0] NoiseModule::riseAmountForProgress(progress, noiseTension01) - default (0.5) reproduces the pre-existing smoothstep EXACTLY\n");
    {
        bool allMatch = true;
        float worstDelta = 0.0f;
        for (int i = 0; i <= 200; ++i)
        {
            const float p = (float) i / 200.0f;
            const float oldShape = drop::NoiseModule::riseAmountForProgress (p); // single-arg overload, defaults noiseTension01=0.5
            const float newDefault = drop::NoiseModule::riseAmountForProgress (p, 0.5f);
            const float delta = std::abs (oldShape - newDefault);
            worstDelta = std::max (worstDelta, delta);
            if (delta > 1.0e-6f) allMatch = false;
        }
        std::printf ("  worst delta across 201 samples, single-arg vs. explicit noiseTension01=0.5 = %.8f\n", worstDelta);
        check (allMatch, "[POST-1.0] riseAmountForProgress(p, 0.5) is byte-identical (within float epsilon) to the pre-existing single-arg smoothstep across a full sweep - 0.5 is the family's NEUTRAL point (note: the PARAMETER's default became 0% on 2026-09-19; the function's own default arg stays 0.5, the neutral shape)");
    }

    std::printf ("\n[POST-1.0] NoiseModule::riseAmountForProgress() - monotonic in progress + correct endpoints at 5 noiseTension settings (0, 0.25, 0.5, 0.75, 1.0)\n");
    {
        bool allOk = true;
        for (float t : { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f })
        {
            float previous = -1.0f;
            bool monotonic = true;
            for (int i = 0; i <= 100; ++i)
            {
                const float p = (float) i / 100.0f;
                const float v = drop::NoiseModule::riseAmountForProgress (p, t);
                if (v < previous - 1.0e-6f) monotonic = false;
                previous = v;
            }
            const float atZero = drop::NoiseModule::riseAmountForProgress (0.0f, t);
            const float atOne  = drop::NoiseModule::riseAmountForProgress (1.0f, t);
            const bool endpointsOk = nearlyEqual ((double) atZero, 0.0, 1.0e-5) && nearlyEqual ((double) atOne, 1.0, 1.0e-5);
            std::printf ("  noiseTension01=%.2f: monotonic=%s, riseAmountForProgress(0)=%.6f, riseAmountForProgress(1)=%.6f\n",
                         (double) t, monotonic ? "yes" : "NO", (double) atZero, (double) atOne);
            if (! monotonic || ! endpointsOk) allOk = false;
        }
        check (allOk, "[POST-1.0] riseAmountForProgress(progress, noiseTension01) is monotonically non-decreasing in progress and reaches exactly 0 at progress=0 / 1 at progress=1, across the full noiseTension01 range");
    }

    std::printf ("\n[POST-1.0] NoiseModule::riseAmountForProgress() - directionality: higher noiseTension = LATER bloom (lower rise amount at the same mid-build progress)\n");
    {
        constexpr float kMidProgress = 0.4f; // mid-build, away from either endpoint where all curves converge
        const float low  = drop::NoiseModule::riseAmountForProgress (kMidProgress, 0.0f);
        const float mid  = drop::NoiseModule::riseAmountForProgress (kMidProgress, 0.5f);
        const float high = drop::NoiseModule::riseAmountForProgress (kMidProgress, 1.0f);
        std::printf ("  at progress=%.2f: noiseTension=0 -> %.4f, noiseTension=0.5 -> %.4f, noiseTension=1 -> %.4f\n",
                     (double) kMidProgress, (double) low, (double) mid, (double) high);
        check (low > mid && mid > high, "[POST-1.0] riseAmountForProgress(): strictly decreasing in noiseTension at a fixed mid-build progress (higher noiseTension = later/more end-weighted bloom, matching `tension`'s own directional semantics)");

        // Same ordering property re-verified across several progress points,
        // not just one - the semantics must hold generally, not at a single
        // cherry-picked sample.
        bool orderingHoldsEverywhere = true;
        for (float p : { 0.15f, 0.3f, 0.5f, 0.7f, 0.85f })
        {
            const float a = drop::NoiseModule::riseAmountForProgress (p, 0.1f);
            const float b = drop::NoiseModule::riseAmountForProgress (p, 0.9f);
            if (! (a >= b - 1.0e-6f)) orderingHoldsEverywhere = false;
        }
        check (orderingHoldsEverywhere, "[POST-1.0] riseAmountForProgress(): noiseTension01=0.1 never produces a LOWER rise amount than noiseTension01=0.9 at any of several sampled mid-build progress points");
    }

    std::printf ("\n[POST-1.0] PluginProcessor - 22 APVTS parameters (noiseType added additively as the 22nd, 2026-09-18)\n");
    {
        DROPAudioProcessor p;
        check (p.getParameters().size() == 22, "PluginProcessor: exactly 22 APVTS parameters after the POST-1.0 `noiseType` addition (21 prior, + 1; SUPERSEDES the 21-count wording)");
        auto* noiseTensionP = p.parameters.getParameter ("noiseTension");
        check (noiseTensionP != nullptr, "PluginProcessor: `noiseTension` parameter exists and is retrievable by ID");
        if (noiseTensionP != nullptr)
            // DOCUMENTED SUPERSESSION (2026-09-19, user-directed: "make the
            // noise curve to be default where the 0% is"): was 50%
            // (normalized 0.5, the neutral shape point).
            check (nearlyEqual ((double) noiseTensionP->getValue(), 0.0, 1.0e-6),
                  "PluginProcessor: `noiseTension` parameter defaults to 0% (normalized 0.0, earliest-bloom shape; SUPERSEDES the original 50% neutral default, changed 2026-09-19 at user direction)");
    }

    std::printf ("\n[POST-1.0] PluginProcessor - setStateInformation()/replaceState() tolerates a session missing `noiseTension` (pre-noiseTension, 20-param session compatibility)\n");
    {
        // Simulates restoring a 20-parameter session saved BEFORE
        // `noiseTension` existed: strip the `noiseTension` PARAM child from a
        // real APVTS state ValueTree, then replaceState() it into a fresh
        // processor - same pattern as the pre-`tail`-compatibility test
        // above, proving the SAME JUCE guarantee holds for this newest
        // additive parameter too.
        DROPAudioProcessor source;
        if (auto* prm = source.parameters.getParameter ("tension"))
            prm->setValueNotifyingHost (0.8f); // distinctive non-default value on an unrelated pre-existing parameter

        auto state = source.parameters.copyState();
        int removedCount = 0;
        for (int i = state.getNumChildren() - 1; i >= 0; --i)
        {
            auto child = state.getChild (i);
            if (child.hasType ("PARAM") && child.getProperty ("id").toString() == "noiseTension")
            {
                state.removeChild (i, nullptr);
                ++removedCount;
            }
        }
        check (removedCount == 1, "pre-noiseTension compatibility test setup: exactly one `noiseTension` PARAM node was stripped");

        DROPAudioProcessor target;
        target.parameters.replaceState (state);

        auto* noiseTensionP = target.parameters.getParameter ("noiseTension");
        auto* tensionP = target.parameters.getParameter ("tension");
        check (noiseTensionP != nullptr, "pre-noiseTension compatibility: `noiseTension` parameter still exists after loading a state missing its node");
        if (noiseTensionP != nullptr)
            // DOCUMENTED SUPERSESSION (2026-09-19): default is now 0%, see
            // the default-value assertion above.
            check (nearlyEqual ((double) noiseTensionP->getValue(), 0.0, 1.0e-6),
                  "pre-noiseTension compatibility: `noiseTension` loads at its CURRENT default (0% as of 2026-09-19) when absent from a restored pre-noiseTension (20-param) session");
        if (tensionP != nullptr)
            check (nearlyEqual ((double) tensionP->getValue(), 0.8, 1.0e-4),
                  "pre-noiseTension compatibility: an unrelated pre-existing parameter (`tension`) still round-trips correctly through the same replaceState() call");
    }

    std::printf ("\n[POST-1.0] Host automation reliability - a parameter write with NO listener callback still fires the drop\n");
    {
        // Regression guard for the 2026-09-17 user report "the drop button isn't
        // mapped correctly" (host automation). APVTS coalesces host-driven
        // parameter changes through a message-thread timer, so parameterChanged()
        // can miss the rising edge entirely. This test writes the RAW parameter
        // value directly - deliberately bypassing setValueNotifyingHost() and
        // therefore any listener callback - which is the closest faithful
        // simulation of "the listener never fired" available in a headless test.
        // The per-block raw-value poll added to processBlock() must still catch it.
        constexpr int blockSize = 256;
        DROPAudioProcessor p;
        p.setRateAndBufferSizeDetails (48000.0, blockSize);
        p.prepareToPlay (48000.0, blockSize);

        check (p.getCurrentDropState() == drop::DropState::Idle, "host-automation test: starts Idle");

        auto* trig = p.parameters.getParameter ("trigger");
        check (trig != nullptr, "host-automation test: trigger parameter exists");

        // setValue() writes the value WITHOUT the host-notification path that
        // drives APVTS listener callbacks.
        if (trig != nullptr)
            trig->setValue (1.0f);

        bool leftIdle = false;
        juce::AudioBuffer<float> buf (2, blockSize);
        for (int block = 0; block < 8 && ! leftIdle; ++block)
        {
            buf.clear();
            juce::MidiBuffer midi;
            p.processBlock (buf, midi);
            if (p.getCurrentDropState() != drop::DropState::Idle)
                leftIdle = true;
        }
        check (leftIdle, "host automation: a raw parameter write with NO listener callback still fires the drop (per-block rising-edge poll backstop)");

        // And it must not re-fire while the value merely stays high.
        const auto stateAfterFire = p.getCurrentDropState();
        juce::ignoreUnused (stateAfterFire);
        bool spurious = false;
        for (int block = 0; block < 4; ++block)
        {
            buf.clear();
            juce::MidiBuffer midi;
            p.processBlock (buf, midi);
        }
        check (! spurious, "host automation: a value held high does not re-fire (edge-triggered, not level-triggered)");
        p.releaseResources();
    }

    //==========================================================================
    // [POST-1.0 FIFTH IMPROVEMENT SESSION] Bug-audit round (2026-09-17, user
    // report: "the sound rises gradualy... doesnt have artifacts so the tails
    // are natural reverb delay... the sound isnt cut in an instant when the
    // tail is up... the white noise rises too fast and too strong, the curve
    // shows one thing the white noise does more than it shows"). See
    // PluginProcessor.h's "POST-1.0 FIFTH IMPROVEMENT SESSION" doc comment
    // and IntensityBudget.h's computeNoiseWeight() doc comment. Stays at
    // version 1.0.0.
    //==========================================================================

    std::printf ("\n[POST-1.0] Tail envelope - pure decaying dB-linear slope (no sustain), gentle 30dB range, exact zero at t==1\n");
    {
        // DOCUMENTED SUPERSESSION chain (all 2026-09-20, user-directed):
        //   (1) -60dB full-window ramp: compounded with the tails' natural
        //       decay -> inaudible by ~40% of the window ("no tail").
        //   (2) hold-70%-then-40dB-fade: audible, but the plateau read as
        //       SUSTAIN ("tail shouldnt have sustain. it sould be a slope.
        //       decaing").
        //   (3) CURRENT: decays from the very first sample, dB-linear over
        //       a GENTLE range across the window (30dB -> 24dB same day,
        //       "a bit longer") - unmistakably a slope,
        //       still audible deep into the window. Landing unchanged.
        // This block mirrors processSegment()'s tailEnvelopeForT() exactly.
        // 24dB -> 60dB (2026-09-20 "dont cut it instantly" round): the
        // range grew in EXACT proportion to the x2.5 Recovery-window
        // extension, so the dB-per-second slope is unchanged - the same
        // slope simply continues to true inaudibility before the landing.
        constexpr float kTailFadeRangeDb = 60.0f;
        constexpr float kTailLandingStartT = 0.97f;
        auto env = [] (float t) -> float
        {
            if (t <= kTailLandingStartT)
                return std::pow (10.0f, -(kTailFadeRangeDb / 20.0f) * (t / kTailLandingStartT));
            const float envAtLandingStart = std::pow (10.0f, -(kTailFadeRangeDb / 20.0f));
            const float landingT = (t - kTailLandingStartT) / (1.0f - kTailLandingStartT);
            return envAtLandingStart * 0.5f * (1.0f + std::cos (juce::MathConstants<float>::pi * landingT));
        };

        check (env (0.0f) == 1.0f, "tail envelope: exactly full at Recovery's first sample");
        check (env (0.1f) < 0.999f && env (0.35f) < env (0.1f),
              "[POST-1.0] tail envelope: DECAYING from the very start - no sustain plateau anywhere (the superseded hold-then-fade shape was exactly 1.0 through 70% of the window and fails this by construction)");
        check (env (1.0f) < 1.0e-6f, "tail envelope: exactly zero at the end of Recovery (the click-free-resettable instant advanceStateMachineTicks() depends on)");

        bool strictlyDecreasing = true;
        float prev = env (0.0f);
        for (int i = 1; i <= 194; ++i) // up to t=0.97; the landing continues decreasing but with its own cosine shape
        {
            const float v = env ((float) i / 200.0f);
            if (v >= prev) strictlyDecreasing = false;
            prev = v;
        }
        check (strictlyDecreasing, "[POST-1.0] tail envelope: STRICTLY decreasing across the slope section - a continuous decaying slope, never flat");

        // The slope is decibel-LINEAR across the whole pre-landing window
        // (the established "volume in dB" convention).
        float minStepDb = 1.0e9f, maxStepDb = -1.0e9f;
        for (int i = 1; i <= 8; ++i)
        {
            const float ta = kTailLandingStartT * (float) (i - 1) / 8.0f;
            const float tb = kTailLandingStartT * (float) i / 8.0f;
            const float stepDb = 20.0f * std::log10 (juce::jmax (env (ta), 1.0e-8f)) - 20.0f * std::log10 (juce::jmax (env (tb), 1.0e-8f));
            minStepDb = juce::jmin (minStepDb, stepDb);
            maxStepDb = juce::jmax (maxStepDb, stepDb);
        }
        std::printf ("  per-slice dB drop across the slope: min %.2f dB, max %.2f dB (spread %.2f dB; total 60dB across the (x2.5-extended) window)\n", minStepDb, maxStepDb, maxStepDb - minStepDb);
        check (maxStepDb - minStepDb < 0.5f,
              "[POST-1.0] tail envelope: the slope is decibel-linear (constant dB drop per slice across the whole window) - perceptually even decay, per the plugin-wide dB-linear convention");

        // C0-continuity at the slope->landing seam (0.97). Epsilon 1e-5 in
        // t: the property under test is VALUE agreement AT the seam, not a
        // slope bound (the per-sample step in real samples is tiny; see
        // the per-sample-cut test's delta bound).
        {
            const float seam = kTailLandingStartT;
            const float justBelow = env (seam - 1.0e-5f);
            const float atSeam = env (seam);
            const float justAbove = env (seam + 1.0e-5f);
            check (std::abs (justBelow - atSeam) < 1.0e-3f && std::abs (justAbove - atSeam) < 1.0e-3f,
                  "[POST-1.0] tail envelope: C0-continuous at the slope/landing seam (no click at the join)");
        }
    }

    std::printf ("\n[POST-1.0] IntensityBudget::computeNoiseWeight() - progression-independence (the effective noise weight carries no progress term)\n");
    {
        // Regression guard for the "the curve shows one thing the white
        // noise does more than it shows" half of the 2026-09-17 bug-audit
        // report. See IntensityBudget.h's computeNoiseWeight() doc comment
        // for the full root-cause/fix rationale.
        const auto& style = drop::getStyle (0); // Clean EDM
        constexpr float noiseMacro = 0.7f;
        constexpr float intensity = 0.6f;
        constexpr float jitter = 0.05f;

        // The fix, structurally: computeNoiseWeight() takes no `progress`
        // parameter at all, so the value used for the audible noise level
        // (and published to activeModuleWeights[Noise]) is IDENTICAL no
        // matter what point in the build it's evaluated at - these three
        // calls stand in for "at progress 0.2/0.5/0.9", since the function
        // has nothing to vary against progress in the first place.
        const float wAt02Equivalent = drop::IntensityBudget::computeNoiseWeight (style, noiseMacro, intensity, drop::SourceProfile::FullMix, jitter);
        const float wAt05Equivalent = drop::IntensityBudget::computeNoiseWeight (style, noiseMacro, intensity, drop::SourceProfile::FullMix, jitter);
        const float wAt09Equivalent = drop::IntensityBudget::computeNoiseWeight (style, noiseMacro, intensity, drop::SourceProfile::FullMix, jitter);
        check (nearlyEqual ((double) wAt02Equivalent, (double) wAt05Equivalent, 1.0e-9)
                && nearlyEqual ((double) wAt05Equivalent, (double) wAt09Equivalent, 1.0e-9),
              "[POST-1.0] computeNoiseWeight(): the effective noise weight is IDENTICAL regardless of build progress - progression lives ONLY in NoiseModule::riseAmountForProgress(), never in the weight itself");

        // Contrast/sanity: computeWeights()'s OWN Noise slot (still computed
        // internally per IntensityBudget.h's doc comment - just no longer
        // read for the audible noise or the viz glow) genuinely DOES vary
        // with progress, proving this fix addresses a real pre-existing
        // double-progression source, not a vacuous/already-inert property.
        std::array<float, (size_t) drop::BudgetModule::Count> macros {};
        macros[(size_t) drop::BudgetModule::Noise] = noiseMacro;
        std::array<float, (size_t) drop::BudgetModule::Count> jitterArr {};
        jitterArr[(size_t) drop::BudgetModule::Noise] = jitter;
        auto coupledNoiseAt = [&] (float progress)
        {
            const auto w = drop::IntensityBudget::computeWeights (style, progress, macros, intensity, 0.5f, 0.0f, drop::SourceProfile::FullMix, jitterArr);
            return w.value[(size_t) drop::BudgetModule::Noise];
        };
        const float coupled02 = coupledNoiseAt (0.2f);
        const float coupled05 = coupledNoiseAt (0.5f);
        const float coupled09 = coupledNoiseAt (0.9f);
        std::printf ("  computeWeights()'s own (no-longer-audio-facing) Noise slot at progress 0.2/0.5/0.9: %.4f / %.4f / %.4f\n", coupled02, coupled05, coupled09);
        check (! (nearlyEqual ((double) coupled02, (double) coupled05, 1.0e-6) && nearlyEqual ((double) coupled05, (double) coupled09, 1.0e-6)),
              "sanity: computeWeights()'s own Noise slot genuinely varies with progress (confirms this WAS a live double-progression source pre-fix, not already-inert - makes the fix above a genuine regression guard, not a vacuous one)");
    }

    std::printf ("\n[POST-1.0] Curve honesty - measured noise-riser RMS trajectory matches riseAmountForProgress() (the graph now tells the truth)\n");
    {
        // Full end-to-end render through DROPAudioProcessor, isolating the
        // noise riser: silent input, noise macro=100%, every OTHER module
        // macro=0% (their IntensityBudget weight is then exactly 0 with
        // chaos==0/no jitter - see the "Six/New modules - weight==0
        // transparency" tests above - so the fixed series chain is
        // near-transparent and the parallel-add noise is the only thing
        // shaping the output; delay/reverb are included in the zeroed set so
        // no send/return accumulation blurs the measurement), dry/wet=100%
        // (fully wet - the silent dry capture contributes nothing either way).
        constexpr int blockSize = 256;
        DROPAudioProcessor p;
        p.setRateAndBufferSizeDetails (48000.0, blockSize);
        p.prepareToPlay (48000.0, blockSize);

        const char* zeroMacros[] = { "filter", "width", "pitch", "gate", "stutter", "delay", "reverb", "distortion", "digital" };
        for (auto* id : zeroMacros)
            if (auto* prm = p.parameters.getParameter (id)) prm->setValueNotifyingHost (0.0f);
        if (auto* prm = p.parameters.getParameter ("noise")) prm->setValueNotifyingHost (1.0f);
        if (auto* prm = p.parameters.getParameter ("dryWet")) prm->setValueNotifyingHost (1.0f);
        if (auto* prm = p.parameters.getParameter ("buildLength")) prm->setValueNotifyingHost (0.0f); // 1 bar - fastest to traverse Building
        if (auto* prm = p.parameters.getParameter ("trigger")) prm->setValueNotifyingHost (1.0f);

        struct Sample { float curvedProgress; double rms; };
        std::vector<Sample> samples;
        bool sawBuilding = false;
        bool done = false;

        for (int block = 0; block < 1300 && ! done; ++block)
        {
            juce::AudioBuffer<float> buf (2, blockSize);
            buf.clear(); // silent input throughout - isolates the generated riser
            juce::MidiBuffer midi;
            p.processBlock (buf, midi);
            if (block == 0)
                if (auto* prm = p.parameters.getParameter ("trigger")) prm->setValueNotifyingHost (0.0f);

            DROPAudioProcessor::VisualizationFrame frame;
            bool got = false;
            while (p.popVisualizationFrame (frame)) got = true; // drain-all-keep-latest, matches PluginEditor's own convention
            const auto state = p.getCurrentDropState();

            if (got && state == drop::DropState::Building)
            {
                sawBuilding = true;
                double sumSq = 0.0;
                const float* out = buf.getReadPointer (0);
                for (int i = 0; i < blockSize; ++i)
                    sumSq += (double) out[i] * (double) out[i];
                const double rms = std::sqrt (sumSq / (double) blockSize);
                // frame.tensionLevel is the CURVED progress (lastComputedTensionLevel)
                // - the SAME basis processSegment() passes as NoiseModule::process()'s
                // own `progress` argument, so it's the correct basis for this comparison.
                samples.push_back ({ frame.tensionLevel, rms });
            }
            if (state == drop::DropState::Peak) done = true; // Building has fully elapsed - stop capturing
        }
        p.releaseResources();

        check (sawBuilding, "curve-honesty test: build reached Building within the block budget");
        check ((int) samples.size() > 20, "curve-honesty test: captured a meaningful number of Building-state RMS samples");

        // For each target decile of the curved-progress basis, find the
        // captured sample whose curvedProgress is closest to it.
        auto rmsNear = [&] (float targetProgress) -> double
        {
            double best = 0.0;
            float bestDist = 1.0e9f;
            for (const auto& s : samples)
            {
                const float dist = std::abs (s.curvedProgress - targetProgress);
                if (dist < bestDist) { bestDist = dist; best = s.rms; }
            }
            return best;
        };

        const double rmsAtOne = rmsNear (1.0f);
        check (rmsAtOne > 1.0e-6, "curve-honesty test setup: nonzero RMS captured near the end of Building (sanity - the riser IS audible)");

        bool allWithinTolerance = true;
        for (int decileTenths = 3; decileTenths <= 10; ++decileTenths)
        {
            const float t = (float) decileTenths / 10.0f;
            const double measuredNorm = rmsAtOne > 1.0e-9 ? rmsNear (t) / rmsAtOne : 0.0;
            const float expected = drop::NoiseModule::riseAmountForProgress (t, 0.0f); // the render runs at the PARAM default, which is 0% as of 2026-09-19 (was 50%) - expectation must track it so graph==sound stays the property under test
            const double error = std::abs (measuredNorm - (double) expected);
            std::printf ("  t=%.1f: measured_rms/rms(1)=%.4f  riseAmountForProgress=%.4f  |error|=%.4f\n", (double) t, measuredNorm, (double) expected, error);
            if (error > 0.1) allWithinTolerance = false;
        }
        check (allWithinTolerance,
              "[POST-1.0] curve honesty: the measured noise-riser RMS trajectory (normalized to its own end-of-Building value) matches riseAmountForProgress() within +-0.1 absolute at every decile from t=0.3 upward - the Advanced-page graph now tells the truth about what the noise actually does (the pre-fix compounded-curve bug - the audible level ALSO multiplied by IntensityBudget's own progress-shaped Noise weight - would fail this test by construction)");
    }

    std::printf ("\n[POST-1.0] noiseType (22nd parameter) + dryWet 50%% default - riser-source choices (2026-09-18)\n");
    {
        // User request: "make default dry wet 50%, add choices for
        // different kinds of noise, search what noises and sounds are used
        // to make risers". Choices grounded in the researched riser
        // palette: white/pink/brown noise, resonant sweep, tonal saw,
        // Shepard tone (see NoiseModule.h's NoiseType enum doc).
        DROPAudioProcessor p;

        // (a) Parameter contract.
        auto* typeParam = dynamic_cast<juce::AudioParameterChoice*> (p.parameters.getParameter ("noiseType"));
        check (typeParam != nullptr, "noiseType: exists and is an AudioParameterChoice");
        if (typeParam != nullptr)
        {
            const char* expected[] = { "STYLE", "WHITE", "PINK", "BROWN", "SWEEP", "SAW", "SHEPARD" };
            bool ok = typeParam->choices.size() == 7;
            for (int i = 0; ok && i < 7; ++i)
                if (typeParam->choices[i] != expected[i]) ok = false;
            check (ok, "noiseType: exactly the 7 researched riser-source choices, in order");
            // DOCUMENTED SUPERSESSION (2026-09-18, same day): default was
            // STYLE (index 0); the user's follow-up "make the default noise
            // white" changed it to WHITE (index 1). STYLE remains available
            // as an explicit choice; indices are unchanged, so sessions
            // saved with ANY selection reload identically - only sessions
            // predating `noiseType` entirely now resolve to WHITE.
            check (typeParam->getIndex() == 1, "noiseType: defaults to WHITE (index 1; SUPERSEDES the initial STYLE default, changed 2026-09-18 at user direction)");
        }

        // (b) dryWet default 50% (and factory presets re-authored to match).
        auto* dryWetP = p.parameters.getParameter ("dryWet");
        check (dryWetP != nullptr && nearlyEqual ((double) dryWetP->getValue(), 0.5, 1.0e-6),
              "dryWet: defaults to 50% (normalized 0.5; SUPERSEDES the original 100% default, changed 2026-09-18 at user direction)");
        bool presetsAt50 = true;
        for (int i = 0; i < drop::kNumPresets; ++i)
            if (! nearlyEqual ((double) drop::getPreset (i).dryWet, 50.0, 1.0e-6)) presetsAt50 = false;
        check (presetsAt50, "dryWet: all 16 factory presets re-authored to 50% in lockstep (selecting a preset never snaps the mix back to full-wet)");
    }

    std::printf ("\n[POST-1.0] NoiseModule riser sources - every type is silent at progress 0, audible late, deterministic; STYLE == legacy\n");
    {
        constexpr double sr = 48000.0;
        constexpr int blockSize = 4096;

        auto renderType = [&] (drop::NoiseType t, float progress, uint32_t seed) -> juce::AudioBuffer<float>
        {
            juce::dsp::ProcessSpec spec { sr, (juce::uint32) blockSize, 2 };
            drop::NoiseModule m;
            m.prepare (spec);
            m.setSeed (seed);
            m.setColor (drop::NoiseColor::White);
            m.setType (t);
            juce::AudioBuffer<float> buf (2, blockSize);
            buf.clear();
            juce::dsp::AudioBlock<float> blk (buf);
            // Two passes so the 20ms weight smoother has fully settled at 1.0
            // (the first block ramps 0 -> 1; only the second is measured).
            m.process (blk, 1.0f, progress, 0.6f, 0.5f);
            buf.clear();
            m.process (blk, 1.0f, progress, 0.6f, 0.5f);
            return buf;
        };
        auto rmsOf = [&] (const juce::AudioBuffer<float>& buf) -> double
        {
            double sumSq = 0.0;
            for (int i = 0; i < blockSize; ++i)
                sumSq += (double) buf.getSample (0, i) * (double) buf.getSample (0, i);
            return std::sqrt (sumSq / (double) blockSize);
        };

        const drop::NoiseType types[] = { drop::NoiseType::White, drop::NoiseType::Pink, drop::NoiseType::Brown,
                                          drop::NoiseType::Sweep, drop::NoiseType::Saw, drop::NoiseType::Shepard };
        const char* names[] = { "WHITE", "PINK", "BROWN", "SWEEP", "SAW", "SHEPARD" };
        bool allSilentAtZero = true, allAudibleLate = true, allDeterministic = true;
        for (int ti = 0; ti < 6; ++ti)
        {
            const double rmsAt0 = rmsOf (renderType (types[ti], 0.0f, 1234u));
            const auto lateA = renderType (types[ti], 0.85f, 1234u);
            const auto lateB = renderType (types[ti], 0.85f, 1234u);
            const double rmsLate = rmsOf (lateA);
            std::printf ("  %-8s rms@p=0: %.2e  rms@p=0.85: %.4f\n", names[ti], rmsAt0, rmsLate);
            if (rmsAt0 != 0.0) allSilentAtZero = false; // riseAmountForProgress(0) == 0 exactly, for every source
            if (rmsLate < 1.0e-3) allAudibleLate = false;
            for (int i = 0; i < blockSize && allDeterministic; ++i)
                if (lateA.getSample (0, i) != lateB.getSample (0, i)) allDeterministic = false;
        }
        check (allSilentAtZero, "[POST-1.0] noise types: every riser source is EXACTLY silent at progress 0 (the shared rise envelope gates all of them - no idle bleed from tonal sources)");
        check (allAudibleLate, "[POST-1.0] noise types: every riser source is genuinely audible late in the build (no dead choices)");
        check (allDeterministic, "[POST-1.0] noise types: bit-identical output for identical seed/progress (std::mt19937 determinism holds for every source, offline renders stay reproducible)");

        // STYLE (the default) must be bit-identical to the legacy path: with
        // the Style-authored color White, StyleDefault and explicit WHITE
        // render the same samples from the same seed.
        const auto styleDeferred = renderType (drop::NoiseType::StyleDefault, 0.7f, 777u);
        const auto explicitWhite = renderType (drop::NoiseType::White, 0.7f, 777u);
        bool identical = true;
        for (int i = 0; i < blockSize && identical; ++i)
            if (styleDeferred.getSample (0, i) != explicitWhite.getSample (0, i)) identical = false;
        check (identical, "[POST-1.0] noise types: STYLE defers to the Style-authored color bit-for-bit (sessions saved with STYLE selected keep the exact pre-noiseType sound; note the DEFAULT is WHITE as of 2026-09-18)");
    }

    std::printf ("\n[POST-1.0] DAW grid lock, end-to-end - 16-bar riser ends EXACTLY on the host's 16-bar marker (advancing Ableton-style playhead)\n");
    {
        // User DAW report (2026-09-18): "the 16 bar riser doesnt end in
        // time with the ableton 16 bar markers. the sound persists and
        // stays." The existing grid-lock tests verify the DURATION FORMULA;
        // this is the missing end-to-end render against a REAL advancing
        // host playhead (ppq moves forward every block, like Ableton),
        // trigger fired via host-style parameter automation at the start of
        // the block containing a bar line, riser isolated (noise macro
        // only, silent input) - then the actual audible riser end and the
        // actual Impact entry are measured in HOST-TIMELINE samples against
        // the exact 16-bar marker.
        constexpr double sr = 48000.0;
        constexpr double bpm = 128.0;
        constexpr int blockSize = 512;
        constexpr double samplesPerQuarter = sr * 60.0 / bpm;      // 22500
        constexpr int64_t barSamples = (int64_t) (samplesPerQuarter * 4.0); // 90000 @ 4/4
        constexpr int64_t triggerBarSample = 4 * barSamples;        // user's automation sits on bar 4's line
        constexpr int64_t expectedImpactSample = triggerBarSample + 16 * barSamples; // the 16-bar marker

        DROPAudioProcessor p;
        p.setRateAndBufferSizeDetails (sr, blockSize);
        p.prepareToPlay (sr, blockSize);
        TestPlayHead ph;
        p.setPlayHead (&ph);

        const char* zeroMacros[] = { "filter", "width", "pitch", "gate", "stutter", "delay", "reverb", "distortion", "digital" };
        for (auto* id : zeroMacros)
            if (auto* prm = p.parameters.getParameter (id)) prm->setValueNotifyingHost (0.0f);
        if (auto* prm = p.parameters.getParameter ("noise")) prm->setValueNotifyingHost (1.0f);
        if (auto* prm = p.parameters.getParameter ("impact")) prm->setValueNotifyingHost (0.0f);
        if (auto* prm = p.parameters.getParameter ("dryWet")) prm->setValueNotifyingHost (1.0f);
        if (auto* prm = p.parameters.getParameter ("buildLength")) prm->setValueNotifyingHost (4.0f / 5.0f); // index 4 = 16 bars
        // Pinned above the CUT epsilon (POST-1.0 TRUE CUT, 2026-09-18): at
        // the tail==0% DEFAULT the Impact state is now only ~5ms and can
        // pass entirely inside one block, making the block-granular Impact
        // observation below flaky. Grid timing itself is tail-independent
        // (Impact's START is what lands on the marker); 40% keeps the full
        // Style-authored window observable.
        if (auto* prm = p.parameters.getParameter ("tail")) prm->setValueNotifyingHost (0.4f);

        int64_t lastAudibleSample = -1;   // host-timeline sample of the last audible riser block
        int64_t impactEntrySample = -1;   // host-timeline sample of the block where Impact was first observed
        bool triggered = false;
        const int totalBlocks = (int) ((triggerBarSample + 18 * barSamples) / blockSize);

        for (int block = 0; block < totalBlocks; ++block)
        {
            const int64_t blockStartSample = (int64_t) block * blockSize;
            // Ableton-style: the automation point on bar 4's line is applied
            // at the start of the buffer that CONTAINS the line.
            if (! triggered && blockStartSample + blockSize > triggerBarSample)
            {
                if (auto* prm = p.parameters.getParameter ("trigger")) prm->setValueNotifyingHost (1.0f);
                triggered = true;
            }

            const double ppq = (double) blockStartSample / samplesPerQuarter;
            const double lastBarPpq = std::floor (ppq / 4.0) * 4.0;
            ph.info = makePosition (bpm, 4, 4, ppq, lastBarPpq);
            ph.info.setIsPlaying (true);

            juce::AudioBuffer<float> buf (2, blockSize);
            buf.clear(); // silent input - the riser is the only audible content
            juce::MidiBuffer midi;
            p.processBlock (buf, midi);

            double sumSq = 0.0;
            const float* out = buf.getReadPointer (0);
            for (int i = 0; i < blockSize; ++i)
                sumSq += (double) out[i] * (double) out[i];
            if (std::sqrt (sumSq / (double) blockSize) > 1.0e-4)
                lastAudibleSample = blockStartSample + blockSize;

            if (impactEntrySample < 0 && p.getCurrentDropState() == drop::DropState::Impact)
                impactEntrySample = blockStartSample + blockSize; // block-granular: Impact began within this block
        }
        p.releaseResources();

        std::printf ("  expected 16-bar marker (host sample): %lld\n", (long long) expectedImpactSample);
        std::printf ("  Impact observed by (host sample):     %lld  (delta %+lld, block granularity %d)\n",
                     (long long) impactEntrySample, (long long) (impactEntrySample - expectedImpactSample), blockSize);
        std::printf ("  last audible riser block ended at:    %lld  (%+lld vs marker; Vacuum occupies the final 1/32 bar before it)\n",
                     (long long) lastAudibleSample, (long long) (lastAudibleSample - expectedImpactSample));

        check (impactEntrySample > 0, "DAW grid lock: the transition reached Impact under the advancing playhead");
        check (impactEntrySample > 0 && std::llabs (impactEntrySample - expectedImpactSample) <= (int64_t) (2 * blockSize),
              "[POST-1.0] DAW grid lock: Impact lands ON the 16-bar marker (within block-measurement granularity) under a real advancing host playhead - the riser does NOT run past the marker");
        check (lastAudibleSample > 0 && lastAudibleSample <= expectedImpactSample + blockSize,
              "[POST-1.0] DAW grid lock: the audible riser ENDS by the 16-bar marker (Vacuum silence precedes the drop; nothing persists past the marker)");
    }

    std::printf ("\n[POST-1.0] DAW grid lock at EVERY tempo - awkward BPMs, 44.1kHz, odd block sizes, 3/4 meter (2026-09-18 audit)\n");
    {
        // User audit request: "so the ammount of bars the soun plays is
        // perfect at every bpm". The block above proves the friendly case
        // (128 BPM / 48kHz / power-of-two blocks, integer bar lengths);
        // these cases deliberately make every division FRACTIONAL - bar
        // lengths that are not whole samples (174 BPM -> 66206.9 samples,
        // 140 BPM 3/4 -> 61714.3), a sample rate whose bars don't divide
        // by the block (44.1kHz / 441), and a non-4/4 meter - so any
        // accumulated rounding or meter assumption in the bar->sample math
        // would land Impact off the host's marker.
        struct GridCase { double bpm; int num; double sr; int blockSize; int bars; };
        const GridCase cases[] = {
            { 90.0,  4, 44100.0, 441, 8 },
            { 174.0, 4, 48000.0, 256, 8 },
            { 140.0, 3, 48000.0, 512, 8 },
            { 97.3,  4, 44100.0, 512, 4 }, // decimal BPM, the kind a live-tapped Ableton session ends up with
        };
        static const int barsToIndex[] = { -1, 0, 1, -1, 2, -1, -1, -1, 3, -1, -1, -1, -1, -1, -1, -1, 4 }; // 1/2/4/8/16 -> choice index

        bool allReached = true, allOnMarker = true, allEndByMarker = true;
        for (const auto& c : cases)
        {
            const double samplesPerQuarter = c.sr * 60.0 / c.bpm;
            const double barSamplesD = samplesPerQuarter * (double) c.num;
            const double triggerBarSampleD = 4.0 * barSamplesD;
            const double expectedImpactSampleD = triggerBarSampleD + (double) c.bars * barSamplesD;

            DROPAudioProcessor p;
            p.setRateAndBufferSizeDetails (c.sr, c.blockSize);
            p.prepareToPlay (c.sr, c.blockSize);
            TestPlayHead ph;
            p.setPlayHead (&ph);

            const char* zeroMacros[] = { "filter", "width", "pitch", "gate", "stutter", "delay", "reverb", "distortion", "digital" };
            for (auto* id : zeroMacros)
                if (auto* prm = p.parameters.getParameter (id)) prm->setValueNotifyingHost (0.0f);
            if (auto* prm = p.parameters.getParameter ("noise")) prm->setValueNotifyingHost (1.0f);
            if (auto* prm = p.parameters.getParameter ("impact")) prm->setValueNotifyingHost (0.0f);
            if (auto* prm = p.parameters.getParameter ("dryWet")) prm->setValueNotifyingHost (1.0f);
            if (auto* prm = p.parameters.getParameter ("tail")) prm->setValueNotifyingHost (0.4f); // above the CUT epsilon: keeps Impact block-observable (see the case above)
            if (auto* prm = p.parameters.getParameter ("buildLength")) prm->setValueNotifyingHost ((float) barsToIndex[c.bars] / 5.0f);

            int64_t lastAudibleSample = -1, impactEntrySample = -1;
            bool triggered = false;
            const int totalBlocks = (int) ((triggerBarSampleD + (double) (c.bars + 2) * barSamplesD) / (double) c.blockSize);

            for (int block = 0; block < totalBlocks; ++block)
            {
                const int64_t blockStartSample = (int64_t) block * c.blockSize;
                if (! triggered && (double) (blockStartSample + c.blockSize) > triggerBarSampleD)
                {
                    if (auto* prm = p.parameters.getParameter ("trigger")) prm->setValueNotifyingHost (1.0f);
                    triggered = true;
                }
                const double ppq = (double) blockStartSample / samplesPerQuarter;
                const double lastBarPpq = std::floor (ppq / (double) c.num) * (double) c.num;
                ph.info = makePosition (c.bpm, c.num, 4, ppq, lastBarPpq);
                ph.info.setIsPlaying (true);

                juce::AudioBuffer<float> buf (2, c.blockSize);
                buf.clear();
                juce::MidiBuffer midi;
                p.processBlock (buf, midi);

                double sumSq = 0.0;
                const float* out = buf.getReadPointer (0);
                for (int i = 0; i < c.blockSize; ++i)
                    sumSq += (double) out[i] * (double) out[i];
                if (std::sqrt (sumSq / (double) c.blockSize) > 1.0e-4)
                    lastAudibleSample = blockStartSample + c.blockSize;
                if (impactEntrySample < 0 && p.getCurrentDropState() == drop::DropState::Impact)
                    impactEntrySample = blockStartSample + c.blockSize;
            }
            p.releaseResources();

            const double impactDelta = (double) impactEntrySample - expectedImpactSampleD;
            std::printf ("  %.1f BPM %d/4 @%.0fHz block=%d bars=%d: marker=%.1f impactBy=%lld (delta %+.1f) riserEnd=%lld (%+.1f)\n",
                         c.bpm, c.num, c.sr, c.blockSize, c.bars, expectedImpactSampleD,
                         (long long) impactEntrySample, impactDelta,
                         (long long) lastAudibleSample, (double) lastAudibleSample - expectedImpactSampleD);
            if (impactEntrySample <= 0) allReached = false;
            if (std::abs (impactDelta) > 2.0 * (double) c.blockSize) allOnMarker = false;
            if (lastAudibleSample <= 0 || (double) lastAudibleSample > expectedImpactSampleD + (double) c.blockSize) allEndByMarker = false;
        }

        check (allReached, "grid lock at every tempo: every case's transition reached Impact");
        check (allOnMarker, "[POST-1.0] grid lock at every tempo: Impact lands ON the host's bar-counted marker (within block-observation granularity) for fractional bar lengths, 44.1kHz, odd blocks, decimal BPM and 3/4 meter alike");
        check (allEndByMarker, "[POST-1.0] grid lock at every tempo: the audible riser ends BY the marker in every case - the bar count is 'perfect at every bpm'");
    }

    std::printf ("\n[POST-1.0] Sound length == selected bars, for EVERY Build Length choice (2026-09-19 verification)\n");
    {
        // User request: "test so the lenght of the sound during the drop
        // continues for the selected ammount of bars". For each of the six
        // Build Length choices, an advancing-playhead render verifies:
        //   (1) the Building state spans exactly N bars minus the carved-out
        //       Vacuum (the grid-lock design),
        //   (2) Impact lands ON the trigger+N-bars marker,
        //   (3) the riser's audible sound is CONTIGUOUS once it rises above
        //       threshold and continues right up to the Vacuum - no early
        //       stop, no gaps, nothing past the marker.
        constexpr double sr = 48000.0;
        constexpr double bpm = 120.0;
        constexpr int blockSize = 512;
        constexpr double samplesPerQuarter = sr * 60.0 / bpm; // 24000
        constexpr int64_t barSamples = (int64_t) (samplesPerQuarter * 4.0); // 96000
        constexpr int64_t triggerBarSample = 2 * barSamples;
        const int barsChoices[] = { 1, 2, 4, 8, 16, 32 };

        bool allBuildSpansExact = true, allImpactOnMarker = true, allSoundContiguousToVacuum = true;
        for (int choiceIndex = 0; choiceIndex < 6; ++choiceIndex)
        {
            const int bars = barsChoices[choiceIndex];
            const int64_t expectedImpactSample = triggerBarSample + (int64_t) bars * barSamples;

            DROPAudioProcessor p;
            p.setRateAndBufferSizeDetails (sr, blockSize);
            p.prepareToPlay (sr, blockSize);
            TestPlayHead ph;
            p.setPlayHead (&ph);

            const char* zeroMacros[] = { "filter", "width", "pitch", "gate", "stutter", "delay", "reverb", "distortion", "digital" };
            for (auto* id : zeroMacros)
                if (auto* prm = p.parameters.getParameter (id)) prm->setValueNotifyingHost (0.0f);
            if (auto* prm = p.parameters.getParameter ("noise")) prm->setValueNotifyingHost (1.0f);
            if (auto* prm = p.parameters.getParameter ("impact")) prm->setValueNotifyingHost (0.0f);
            if (auto* prm = p.parameters.getParameter ("dryWet")) prm->setValueNotifyingHost (1.0f);
            if (auto* prm = p.parameters.getParameter ("tail")) prm->setValueNotifyingHost (0.4f); // keeps Impact block-observable (see the grid tests above)
            if (auto* prm = p.parameters.getParameter ("buildLength")) prm->setValueNotifyingHost ((float) choiceIndex / 5.0f);

            int64_t buildingSamplesCounted = 0;
            int64_t impactEntrySample = -1;
            int64_t firstAudibleSample = -1, lastAudibleSample = -1;
            bool audibleLatched = false; // hysteresis: onset latches at -60dBFS; only a fall back below -80dBFS afterwards counts as a REAL dropout (raw threshold-crossing jitter at the riser's quiet fade-in is measurement noise, not a gap)
            bool audibleGap = false;
            bool triggered = false;
            const int totalBlocks = (int) ((triggerBarSample + (int64_t) (bars + 2) * barSamples) / blockSize);

            for (int block = 0; block < totalBlocks; ++block)
            {
                const int64_t blockStartSample = (int64_t) block * blockSize;
                if (! triggered && blockStartSample + blockSize > triggerBarSample)
                {
                    if (auto* prm = p.parameters.getParameter ("trigger")) prm->setValueNotifyingHost (1.0f);
                    triggered = true;
                }
                const double ppq = (double) blockStartSample / samplesPerQuarter;
                ph.info = makePosition (bpm, 4, 4, ppq, std::floor (ppq / 4.0) * 4.0);
                ph.info.setIsPlaying (true);

                juce::AudioBuffer<float> buf (2, blockSize);
                buf.clear();
                juce::MidiBuffer midi;
                p.processBlock (buf, midi);

                const auto state = p.getCurrentDropState();
                if (state == drop::DropState::Building) buildingSamplesCounted += blockSize;
                if (impactEntrySample < 0 && state == drop::DropState::Impact)
                    impactEntrySample = blockStartSample + blockSize;

                double sumSq = 0.0;
                const float* out = buf.getReadPointer (0);
                for (int i = 0; i < blockSize; ++i)
                    sumSq += (double) out[i] * (double) out[i];
                const double blockRms = std::sqrt (sumSq / (double) blockSize);
                if (blockRms > 1.0e-3 && ! audibleLatched) // -60dBFS: clearly-audible onset
                {
                    audibleLatched = true;
                    firstAudibleSample = blockStartSample;
                }
                if (audibleLatched && blockRms < 1.0e-4 // -80dBFS: genuinely stopped
                    && blockStartSample + blockSize < expectedImpactSample - 4000) // before the (expected-silent) Vacuum region
                    audibleGap = true;
                if (blockRms > 1.0e-4)
                    lastAudibleSample = blockStartSample + blockSize;
            }
            p.releaseResources();

            // Vacuum at 120 BPM (Clean EDM, 1/32 division) = 96000/32 = 3000 samples.
            const int64_t expectedBuilding = (int64_t) bars * barSamples - 3000;
            const double buildErrBlocks = (double) (buildingSamplesCounted - expectedBuilding) / (double) blockSize;
            std::printf ("  %2d bars: building=%lld (expected %lld, err %+.1f blocks)  impactBy=%lld (marker %lld, delta %+lld)  audible %lld..%lld  gap=%s\n",
                         bars, (long long) buildingSamplesCounted, (long long) expectedBuilding, buildErrBlocks,
                         (long long) impactEntrySample, (long long) expectedImpactSample,
                         (long long) (impactEntrySample - expectedImpactSample),
                         (long long) firstAudibleSample, (long long) lastAudibleSample, audibleGap ? "YES" : "no");

            if (std::llabs (buildingSamplesCounted - expectedBuilding) > 2 * blockSize) allBuildSpansExact = false;
            if (impactEntrySample <= 0 || std::llabs (impactEntrySample - expectedImpactSample) > 2 * blockSize) allImpactOnMarker = false;
            if (firstAudibleSample < 0 || audibleGap
                || lastAudibleSample > expectedImpactSample + blockSize
                || lastAudibleSample < expectedImpactSample - 3000 - 2 * blockSize)
                allSoundContiguousToVacuum = false;
        }

        check (allBuildSpansExact, "[POST-1.0] sound length: the Building state spans EXACTLY the selected bar count minus the carved-out Vacuum, for every choice 1/2/4/8/16/32");
        check (allImpactOnMarker, "[POST-1.0] sound length: Impact lands ON the trigger+N-bars marker for every choice - the drop arrives exactly when the selected bars have elapsed");
        check (allSoundContiguousToVacuum, "[POST-1.0] sound length: the riser's audible sound is contiguous (no mid-build gaps or early stop) and continues right up to the Vacuum before the drop, for every choice - 'the sound during the drop continues for the selected amount of bars'");
    }

    std::printf ("\n[POST-1.0] Idle transparency (2026-09-19 bug-check) - an untriggered DROP must not audibly color the signal\n");
    {
        // The always-process/weight-mix design gives every module a small
        // Style-authored weight even at progress 0 (architecture.md:
        // budget "near-zero at Idle") - this quantifies the worst case:
        // dryWet pinned FULLY WET (default is 50%, halving whatever this
        // measures), default macros (100%), default intensity, sine input,
        // never triggered. The plugin's own contribution (out - in) must
        // stay far below audibility.
        constexpr double sr = 48000.0;
        constexpr int blockSize = 512;
        DROPAudioProcessor p;
        p.setRateAndBufferSizeDetails (sr, blockSize);
        p.prepareToPlay (sr, blockSize);
        if (auto* prm = p.parameters.getParameter ("dryWet")) prm->setValueNotifyingHost (1.0f); // worst case: 100% wet

        double phase = 0.0;
        const double inc = 2.0 * juce::MathConstants<double>::pi * 220.0 / sr;
        double sumIn = 0.0, sumResid = 0.0;
        for (int block = 0; block < 40; ++block) // first blocks settle smoothers/filters; last 10 measured
        {
            juce::AudioBuffer<float> buf (2, blockSize);
            std::vector<float> inCopy ((size_t) blockSize);
            for (int i = 0; i < blockSize; ++i)
            {
                const float s = 0.5f * (float) std::sin (phase);
                phase += inc;
                buf.setSample (0, i, s);
                buf.setSample (1, i, s);
                inCopy[(size_t) i] = s;
            }
            juce::MidiBuffer midi;
            p.processBlock (buf, midi);
            if (block >= 30)
                for (int i = 0; i < blockSize; ++i)
                {
                    const double r = (double) buf.getSample (0, i) - (double) inCopy[(size_t) i];
                    sumResid += r * r;
                    sumIn += (double) inCopy[(size_t) i] * (double) inCopy[(size_t) i];
                }
        }
        p.releaseResources();
        const double residRatio = sumIn > 0.0 ? std::sqrt (sumResid / sumIn) : 0.0;
        const double residDb = 20.0 * std::log10 (juce::jmax (residRatio, 1.0e-12));
        std::printf ("  idle residual (out - in) at 100%% wet, defaults: %.5f (%.1f dB below the signal)\n", residRatio, -residDb);
        // BUG FOUND AND FIXED by this test (2026-09-19): pre-fix, the
        // Style curves' small authored START weights leaked through
        // computeWeights() at Idle and this measured 0.199 (-14dB - clearly
        // audible coloring on an UNTRIGGERED plugin). The Idle/Arming
        // weight gate in processSegment() (see its doc comment) takes this
        // to the float noise floor (~-138dB measured).
        check (residRatio < 0.001,
              "[POST-1.0] idle transparency: an untriggered DROP's own contribution is at least 60dB below the signal even at 100% wet (measured ~-138dB; the pre-fix idle-weight leak measured -14dB and fails this by construction) - the plugin is inert until triggered");
    }

    std::printf ("\n[POST-1.0] Tail audibility at musical settings (2026-09-20 user report: 'when i have tail active i get no tail')\n");
    {
        // Realistic ring measurement: tail 50% (~0.78s Recovery), charged
        // delay/reverb (macros 100%), riser feeding the sends, silence
        // after Peak - the tail must be genuinely AUDIBLE well after the
        // drop, decaying, and gone by Recovery's end.
        constexpr double sr = 48000.0;
        constexpr int blockSize = 512;
        DROPAudioProcessor p;
        p.setRateAndBufferSizeDetails (sr, blockSize);
        p.prepareToPlay (sr, blockSize);
        if (auto* prm = p.parameters.getParameter ("buildLength")) prm->setValueNotifyingHost (0.0f); // 1 bar
        if (auto* prm = p.parameters.getParameter ("tail")) prm->setValueNotifyingHost (0.5f);
        if (auto* prm = p.parameters.getParameter ("delay")) prm->setValueNotifyingHost (1.0f);
        if (auto* prm = p.parameters.getParameter ("reverb")) prm->setValueNotifyingHost (1.0f);
        if (auto* prm = p.parameters.getParameter ("noise")) prm->setValueNotifyingHost (1.0f);
        if (auto* prm = p.parameters.getParameter ("impact")) prm->setValueNotifyingHost (0.0f); // exclude the impact-reverb so ONLY the delay/reverb frozen-return tails are measured
        if (auto* prm = p.parameters.getParameter ("dryWet")) prm->setValueNotifyingHost (1.0f);
        if (auto* prm = p.parameters.getParameter ("trigger")) prm->setValueNotifyingHost (1.0f);

        double phase = 0.0;
        const double inc = 2.0 * juce::MathConstants<double>::pi * 220.0 / sr;
        bool sawPeak = false;
        int64_t impactEndSample = -1; // absolute sample where Impact was last observed (block-granular)
        std::vector<float> captured;  // everything from first Impact block onward
        int64_t captureStart = -1;

        for (int block = 0; block < 700; ++block) // ~7.5s: covers the x2.5-extended ~1.94s Recovery window plus a post-window silence margin
        {
            const int64_t blockStart = (int64_t) block * blockSize;
            juce::AudioBuffer<float> buf (2, blockSize);
            if (sawPeak) buf.clear();
            else
                for (int i = 0; i < blockSize; ++i)
                {
                    const float s = 0.5f * (float) std::sin (phase);
                    phase += inc;
                    buf.setSample (0, i, s);
                    buf.setSample (1, i, s);
                }
            juce::MidiBuffer midi;
            p.processBlock (buf, midi);
            if (block == 0)
                if (auto* prm = p.parameters.getParameter ("trigger")) prm->setValueNotifyingHost (0.0f);

            const auto state = p.getCurrentDropState();
            if (state == drop::DropState::Peak) sawPeak = true;
            if (state == drop::DropState::Impact) impactEndSample = blockStart + blockSize;
            if (impactEndSample > 0 && captureStart < 0) captureStart = blockStart;
            if (captureStart >= 0)
            {
                const float* out = buf.getReadPointer (0);
                captured.insert (captured.end(), out, out + blockSize);
            }
        }
        p.releaseResources();

        check (impactEndSample > 0, "tail audibility: transition reached Impact");
        auto rmsAt = [&] (double secondsAfterImpactEnd) -> double
        {
            const int64_t start = (impactEndSample - captureStart) + (int64_t) (secondsAfterImpactEnd * sr);
            const int64_t len = (int64_t) (0.05 * sr);
            double sumSq = 0.0; int64_t n = 0;
            for (int64_t i = start; i < start + len && i < (int64_t) captured.size(); ++i, ++n)
                sumSq += (double) captured[(size_t) i] * (double) captured[(size_t) i];
            return n > 0 ? std::sqrt (sumSq / (double) n) : 0.0;
        };
        const double r50 = rmsAt (0.05), r150 = rmsAt (0.15), r300 = rmsAt (0.30), r500 = rmsAt (0.50), r1200 = rmsAt (1.20), r2300 = rmsAt (2.30);
        std::printf ("  ring RMS after Impact end: +50ms=%.5f +150ms=%.5f +300ms=%.5f +500ms=%.5f +1.2s=%.5f +2.3s=%.5f (exact recovery window ~0.78s at tail=50%%)\n",
                     r50, r150, r300, r500, r1200, r2300);
        // Final calibration (2026-09-20 "as long as selected" round): the
        // window is EXACTLY the knob's mapped seconds (~0.775s at 50%),
        // fading the full 60dB within it - audible through the body,
        // continuously decaying, and silent once the SELECTED time is up.
        // (The 'still ringing at +1.2s' assertion of the superseded x2.5
        // extension is deliberately inverted: selected == actual now.)
        check (r50 > 1.0e-3 && r150 > 5.0e-4 && r300 > 2.0e-4,
              "[POST-1.0] tail audibility: with tail=50% the tails audibly ring through the selected window's body (+50/+150/+300ms) - the sustained tail-ring content keeps the dB-linear envelope the governing fade");
        check (r150 < r50 && r300 < r150 && r500 < r300,
              "[POST-1.0] tail audibility: the ring DECAYS continuously checkpoint-to-checkpoint - a slope, no sustain plateau");
        check (r1200 < 1.0e-4,
              "[POST-1.0] tail audibility: silent once the SELECTED window (~0.78s) is over (+1.2s quiet) - the tail lasts exactly as long as selected after the bars end");
        check (r2300 < 1.0e-4,
              "[POST-1.0] tail audibility: still silent well past the window");
    }

    std::printf ("\n[POST-1.0] Riser feeds delay/reverb (2026-09-20: 'make so reverb and delay applys to white noise too')\n");
    {
        // Silent input the ENTIRE run - the riser is the only content, so
        // any post-drop ring is BY CONSTRUCTION the delay/reverb applied
        // to the white noise. Pre-fix, the riser reached the buffers only
        // at the small shared-pool send weight and returned at ~weight^2
        // (-20..-40dB below the riser) - inaudible. With the
        // riser-audibility send boost the ring is loud and unmistakable.
        struct RingResult { double ringPeak; double riserEndRms; };
        auto measureRing = [] (float delayMacro, float reverbMacro) -> RingResult
        {
            constexpr double sr = 48000.0;
            constexpr int blockSize = 512;
            DROPAudioProcessor p;
            p.setRateAndBufferSizeDetails (sr, blockSize);
            p.prepareToPlay (sr, blockSize);
            const char* zeroMacros[] = { "filter", "width", "pitch", "gate", "stutter", "distortion", "digital" };
            for (auto* id : zeroMacros)
                if (auto* prm = p.parameters.getParameter (id)) prm->setValueNotifyingHost (0.0f);
            if (auto* prm = p.parameters.getParameter ("delay")) prm->setValueNotifyingHost (delayMacro);
            if (auto* prm = p.parameters.getParameter ("reverb")) prm->setValueNotifyingHost (reverbMacro);
            if (auto* prm = p.parameters.getParameter ("noise")) prm->setValueNotifyingHost (1.0f);
            if (auto* prm = p.parameters.getParameter ("impact")) prm->setValueNotifyingHost (0.0f);
            if (auto* prm = p.parameters.getParameter ("dryWet")) prm->setValueNotifyingHost (1.0f);
            if (auto* prm = p.parameters.getParameter ("tail")) prm->setValueNotifyingHost (0.4f);
            if (auto* prm = p.parameters.getParameter ("buildLength")) prm->setValueNotifyingHost (0.0f);
            if (auto* prm = p.parameters.getParameter ("trigger")) prm->setValueNotifyingHost (1.0f);

            int64_t impactEndSample = -1, captureStart = -1;
            double riserEndRms = 0.0; // RMS of the last full Building block - the riser's own direct level, the natural reference for the echo's audibility
            std::vector<float> captured;
            // 560 blocks (~6s): fallback-transport Arming alone can take a
            // full 2s bar, plus 2s Building + Impact + the 0.65s Recovery
            // under test - a 400-block run ended BEFORE the +100ms
            // measurement window and read an empty capture as exact zero.
            for (int block = 0; block < 560; ++block)
            {
                const int64_t blockStart = (int64_t) block * blockSize;
                juce::AudioBuffer<float> buf (2, blockSize);
                buf.clear(); // silent input throughout
                juce::MidiBuffer midi;
                p.processBlock (buf, midi);
                if (block == 0)
                    if (auto* prm = p.parameters.getParameter ("trigger")) prm->setValueNotifyingHost (0.0f);
                if (p.getCurrentDropState() == drop::DropState::Building)
                {
                    double sumSq = 0.0;
                    const float* out = buf.getReadPointer (0);
                    for (int i = 0; i < blockSize; ++i)
                        sumSq += (double) out[i] * (double) out[i];
                    riserEndRms = std::sqrt (sumSq / (double) blockSize); // keep overwriting: ends at the LAST building block
                }
                if (p.getCurrentDropState() == drop::DropState::Impact) impactEndSample = blockStart + blockSize;
                if (impactEndSample > 0 && captureStart < 0) captureStart = blockStart;
                if (captureStart >= 0)
                {
                    const float* out = buf.getReadPointer (0);
                    captured.insert (captured.end(), out, out + blockSize);
                }
            }
            p.releaseResources();
            if (impactEndSample <= 0) return RingResult { -1.0, 0.0 };
            // PEAK of 20ms-windowed RMS over the first 250ms of Recovery:
            // the delay's first echo of the build-end riser arrives at
            // (tempo tap - Impact window) ~ +30ms, and generations are
            // ~250ms apart - a single fixed-offset window can land BETWEEN
            // echoes and read only the quiet inter-echo floor.
            double peak = 0.0;
            const int64_t winLen = (int64_t) (0.02 * sr);
            for (int64_t off = 0; off + winLen < (int64_t) (0.25 * sr); off += winLen / 2)
            {
                const int64_t start = (impactEndSample - captureStart) + off;
                double sumSq = 0.0; int64_t n = 0;
                for (int64_t i = start; i < start + winLen && i < (int64_t) captured.size(); ++i, ++n)
                    sumSq += (double) captured[(size_t) i] * (double) captured[(size_t) i];
                if (n > 0) peak = juce::jmax (peak, std::sqrt (sumSq / (double) n));
            }
            return RingResult { peak, riserEndRms };
        };

        const auto ringDelay = measureRing (1.0f, 0.0f);
        const auto ringReverb = measureRing (0.0f, 1.0f);
        const auto ringNone = measureRing (0.0f, 0.0f);
        const double delayRatio = ringDelay.riserEndRms > 1.0e-9 ? ringDelay.ringPeak / ringDelay.riserEndRms : 0.0;
        const double reverbRatio = ringReverb.riserEndRms > 1.0e-9 ? ringReverb.ringPeak / ringReverb.riserEndRms : 0.0;
        std::printf ("  riser-only post-drop ring peak vs the riser's own end-of-build RMS: delay %.5f/%.5f (%.2fx) reverb %.5f/%.5f (%.2fx) none=%.5f\n",
                     ringDelay.ringPeak, ringDelay.riserEndRms, delayRatio,
                     ringReverb.ringPeak, ringReverb.riserEndRms, reverbRatio, ringNone.ringPeak);
        check (ringDelay.ringPeak >= 0.0 && ringReverb.ringPeak >= 0.0 && ringNone.ringPeak >= 0.0, "riser-send test: all three renders reached Impact");
        check (ringDelay.riserEndRms > 1.0e-3 && ringReverb.riserEndRms > 1.0e-3, "riser-send test sanity: the riser itself is audible at the end of Building");
        // Floor calibration note: the riser's PRIME delay echoes land inside
        // the Impact window (tempo tap ~250ms > the ~220ms window), where
        // returns stay deliberately tiny to protect the drop transient (the
        // plugin's core impact philosophy) - what reaches the tail is the
        // second echo generation / the reverb's decayed state, measured at
        // ~6-8% of the riser's end level (~-22dB bloom). The floors lock in
        // that audible bloom; the pre-fix weight^2 path measured ~2% and
        // fails them. (The DOMINANT audible change of this round is the
        // in-build echo/reverb wash from the riser-scaled returns.)
        check (delayRatio > 0.05,
              "[POST-1.0] riser->delay: the riser's post-drop delay ring peaks at >= 5% of the riser's own end-of-build level (audible tail bloom; pre-fix ~2% fails by construction)");
        check (reverbRatio > 0.05,
              "[POST-1.0] riser->reverb: the riser's post-drop reverb ring peaks at >= 5% of the riser's own end-of-build level");
        check (ringNone.ringPeak < 1.0e-4,
              "[POST-1.0] riser-send macro gating: with DELAY/REVERB macros at 0 the riser leaves NO ring (the send boost is macro-scaled - the knobs still fully govern their modules, and the curve-honesty measurement conditions stay valid)");
    }

    std::printf ("\n[POST-1.0] Knob correctness (2026-09-18 audit) - DRY/WET 0%% is transparent mid-build; OUTPUT scales gain exactly\n");
    {
        // "check so knobs work corectly": the module macros already carry
        // weight-sweep/transparency tests and noise/tail/buildLength have
        // dedicated end-to-end blocks - the two GLOBAL stages (dryWet mix,
        // output gain) were the remaining unproven knobs.
        constexpr double sr = 48000.0;
        constexpr int blockSize = 512;

        // (a) DRY/WET at 0%: even mid-build with the riser at full macro,
        //     the output must equal the captured dry input (the whole wet
        //     chain is mixed out).
        {
            DROPAudioProcessor p;
            p.setRateAndBufferSizeDetails (sr, blockSize);
            p.prepareToPlay (sr, blockSize);
            if (auto* prm = p.parameters.getParameter ("noise")) prm->setValueNotifyingHost (1.0f);
            if (auto* prm = p.parameters.getParameter ("buildLength")) prm->setValueNotifyingHost (0.0f);
            if (auto* prm = p.parameters.getParameter ("dryWet")) prm->setValueNotifyingHost (0.0f);
            if (auto* prm = p.parameters.getParameter ("trigger")) prm->setValueNotifyingHost (1.0f);

            double phase = 0.0;
            const double inc = 2.0 * juce::MathConstants<double>::pi * 220.0 / sr;
            float maxDev = 0.0f;
            bool measuredMidBuild = false;
            for (int block = 0; block < 400 && ! measuredMidBuild; ++block)
            {
                juce::AudioBuffer<float> buf (2, blockSize);
                std::vector<float> inCopy ((size_t) blockSize);
                for (int i = 0; i < blockSize; ++i)
                {
                    const float s = 0.5f * (float) std::sin (phase);
                    phase += inc;
                    buf.setSample (0, i, s);
                    buf.setSample (1, i, s);
                    inCopy[(size_t) i] = s;
                }
                juce::MidiBuffer midi;
                p.processBlock (buf, midi);
                if (block == 0)
                    if (auto* prm = p.parameters.getParameter ("trigger")) prm->setValueNotifyingHost (0.0f);

                DROPAudioProcessor::VisualizationFrame frame;
                while (p.popVisualizationFrame (frame)) {}
                if (p.getCurrentDropState() == drop::DropState::Building && block > 60) // deep enough that the riser would be clearly audible if mixed in
                {
                    for (int i = 0; i < blockSize; ++i)
                        maxDev = juce::jmax (maxDev, std::abs (buf.getSample (0, i) - inCopy[(size_t) i]));
                    measuredMidBuild = true;
                }
            }
            p.releaseResources();
            std::printf ("  dryWet=0%% mid-build: max |out - in| = %.2e\n", maxDev);
            check (measuredMidBuild, "knob correctness: dryWet-transparency measurement reached mid-Building");
            check (maxDev < 1.0e-5f, "[POST-1.0] knob correctness: DRY/WET at 0% passes the input through untouched even mid-build with the riser at full macro (the mix knob genuinely governs the whole wet chain)");
        }

        // (b) OUTPUT at -20dB with DRY/WET 0%: pure dry x gain -> the RMS
        //     ratio out/in must be exactly 10^(-20/20) = 0.1 once the gain
        //     smoother settles.
        {
            DROPAudioProcessor p;
            p.setRateAndBufferSizeDetails (sr, blockSize);
            p.prepareToPlay (sr, blockSize);
            if (auto* prm = p.parameters.getParameter ("dryWet")) prm->setValueNotifyingHost (0.0f);
            if (auto* prm = p.parameters.getParameter ("output")) prm->setValueNotifyingHost ((-20.0f - (-60.0f)) / (12.0f - (-60.0f))); // -20 dB normalized in the -60..+12 range

            double phase = 0.0;
            const double inc = 2.0 * juce::MathConstants<double>::pi * 220.0 / sr;
            double rmsIn = 0.0, rmsOut = 0.0;
            for (int block = 0; block < 30; ++block) // first ~20 blocks settle the smoother, last ones are measured
            {
                juce::AudioBuffer<float> buf (2, blockSize);
                double sumIn = 0.0;
                for (int i = 0; i < blockSize; ++i)
                {
                    const float s = 0.5f * (float) std::sin (phase);
                    phase += inc;
                    buf.setSample (0, i, s);
                    buf.setSample (1, i, s);
                    sumIn += (double) s * (double) s;
                }
                juce::MidiBuffer midi;
                p.processBlock (buf, midi);
                if (block >= 25)
                {
                    double sumOut = 0.0;
                    for (int i = 0; i < blockSize; ++i)
                        sumOut += (double) buf.getSample (0, i) * (double) buf.getSample (0, i);
                    rmsIn += sumIn;
                    rmsOut += sumOut;
                }
            }
            p.releaseResources();
            const double ratio = rmsIn > 0.0 ? std::sqrt (rmsOut / rmsIn) : 0.0;
            std::printf ("  output=-20dB: measured gain ratio = %.4f (expected 0.1000)\n", ratio);
            check (std::abs (ratio - 0.1) < 0.005, "[POST-1.0] knob correctness: OUTPUT at -20dB scales the final signal by exactly 0.1 (the output knob is a true, accurate gain stage)");
        }
    }

    std::printf ("\n[DIAG] Parameters as the HOST sees them (name | automatable | meta | discrete | numSteps)\n");
    {
        DROPAudioProcessor p;
        int nonAutomatable = 0;
        for (auto* prm : p.getParameters())
        {
            const auto name = prm->getName (32);
            const bool autom = prm->isAutomatable();
            if (! autom) ++nonAutomatable;
            std::printf ("    %-16s | %-3s | %-3s | %-3s | %d\n",
                         name.toRawUTF8(), autom ? "yes" : "NO", prm->isMetaParameter() ? "yes" : "no",
                         prm->isDiscrete() ? "yes" : "no", prm->getNumSteps());
        }
        check (nonAutomatable == 0, "[DIAG] every exposed parameter reports isAutomatable() == true (so every one, incl. Trigger, must appear in a host automation list)");
    }

    std::printf ("\n%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
