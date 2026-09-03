#include "DSP/BodyOscillator.h"
#include "Utilities/DSPUtils.h"

#include <cmath>

namespace kickr
{
    void BodyOscillator::prepare (double fsOS) noexcept
    {
        fsOversampled = juce::jmax (1.0, fsOS);
        morphSm.reset (fsOversampled, kMorphSmoothSeconds);   // also snaps current -> target
        shapeCoef = static_cast<float> (std::exp (-1.0 / (kShapeDecaySeconds * fsOversampled)));
        reset();
        setFrequency (frequencyHz);
    }

    void BodyOscillator::reset() noexcept
    {
        phase     = 0.0;
        syncPhase = 0.0;
        modPhase  = 0.0;
        shapeEnv  = 0.0f;   // silent voice: no shaping armed until the next noteOn
        morphSm.setCurrentAndTargetValue (morphSm.getTargetValue());   // a fresh/stolen voice starts AT the current morph
    }

    void BodyOscillator::updateOversampledRate (double newFsOversampled) noexcept
    {
        fsOversampled = juce::jmax (1.0, newFsOversampled);
        setFrequency (frequencyHz);   // recompute the increment only; phase is kept
        shapeCoef = static_cast<float> (std::exp (-1.0 / (kShapeDecaySeconds * fsOversampled)));
        // Re-rate the morph ramp for the new fsOversampled. This snaps current -> target,
        // which is only a discontinuity if a morph ramp was mid-flight at the exact OS
        // switch — and that instant is already covered by KickEngine's OS-switch fade.
        morphSm.reset (fsOversampled, kMorphSmoothSeconds);
    }

    void BodyOscillator::setFrequency (float hz) noexcept
    {
        frequencyHz    = hz;
        phaseIncrement = juce::MathConstants<double>::twoPi
                       * static_cast<double> (hz) / fsOversampled;
        modPhaseIncrement = phaseIncrement * kModRatio;   // fm/am/rm internal modulator
    }

    namespace
    {
        // CZ-style "saw" phase distortion (see BodyOscillator.h): run the phase fast
        // through the first `d` of the cycle and slow through the rest, then read a sine.
        // `k` in [0, kMaxSkew] (< 1) keeps `d` strictly positive. Shared by Mode::bendSkew
        // (k = amount x attack-envelope) and Mode::pd (k = amount, continuous).
        float warpedSine (double phase, double k) noexcept
        {
            const double t = phase / juce::MathConstants<double>::twoPi;   // [0, 1) — phase is wrapped
            const double d = 0.5 * (1.0 - k);
            const double warped = t < d ? t * (0.5 / d)
                                        : 0.5 + (t - d) * (0.5 / (1.0 - d));
            return static_cast<float> (std::sin (juce::MathConstants<double>::twoPi * warped));
        }
    }

    float BodyOscillator::renderSample (float externalMod) noexcept
    {
        float s;

        // Effective amount this sample: smoothed knob value, optionally x the
        // attack-only envelope (every mode except Mode::pd — see header). When the
        // knob target is 0 and the ramp has settled, getNextValue() returns exactly
        // 0.0f -> every mode's amount collapses to 0 -> the original pure-sine path
        // (bit-identical output, independent of which mode is selected).
        const float morphAmt = morphSm.getNextValue();
        const float atkAmt   = morphAmt * shapeEnv;   // attack-only shaped amount

        shapeEnv *= shapeCoef;
        dsputils::flushDenormal (shapeEnv);

        const float amt = (morphMode == pd) ? morphAmt : atkAmt;

        if (amt <= 0.0f)
        {
            s = static_cast<float> (std::sin (phase));
        }
        else switch (static_cast<Mode> (morphMode))
        {
            case sync:
            {
                // Hard sync: a faster accumulator (the "master") wraps and resets the
                // audible `phase` (the "slave") to 0 — the slave's own pitch/timbre
                // carries through, but its effective rate locks toward the master's.
                const double syncInc = phaseIncrement * (1.0 + static_cast<double> (amt) * kSyncMaxRatio);
                syncPhase += syncInc;
                if (syncPhase >= juce::MathConstants<double>::twoPi)
                {
                    syncPhase -= juce::MathConstants<double>::twoPi;
                    phase = 0.0;
                }
                s = static_cast<float> (std::sin (phase));
                break;
            }

            case fold:
            {
                // Sine wavefolder: y = sin(gain x x) — always bounded to [-1, 1]
                // regardless of gain (Jacobi-Anger expansion), so amount=1 can't runaway.
                const float x    = static_cast<float> (std::sin (phase));
                const float gain = 1.0f + amt * kFoldGainMax;
                s = std::sin (gain * x);
                break;
            }

            case fm:
            {
                // Self-FM via phase modulation: bounded index keeps the argument to the
                // outer sin() bounded, so the result stays in [-1, 1] at any amount.
                modPhase += modPhaseIncrement;
                if (modPhase >= juce::MathConstants<double>::twoPi) modPhase -= juce::MathConstants<double>::twoPi;
                const float modSin = static_cast<float> (std::sin (modPhase));
                const float idx    = amt * kFMIndexMax;
                s = static_cast<float> (std::sin (phase + static_cast<double> (idx * modSin)));
                break;
            }

            case fmFromSample:
            {
                // Same phase-modulation math as Mode::fm, but the modulator is the
                // Sample layer's live output instead of an internal oscillator. A
                // missing/disabled sample layer already renders 0 here (SamplePlayer),
                // so this degrades gracefully to "no effect" with no extra handling.
                const float modClamped = juce::jlimit (-1.0f, 1.0f, externalMod);
                const float idx        = amt * kFMIndexMax;
                s = static_cast<float> (std::sin (phase + static_cast<double> (idx * modClamped)));
                break;
            }

            case pd:
                // Continuous (NOT attack-only, unlike every other mode) — reuses the
                // exact bendSkew warp shape, but stays applied through the tail, which is
                // what makes it read as a genuinely different, sustained character rather
                // than a duplicate of bendSkew.
                s = warpedSine (phase, static_cast<double> (amt * kMaxSkew));
                break;

            case am:
            {
                // Unipolar modulator (carrier frequency preserved) x depth = amount.
                modPhase += modPhaseIncrement;
                if (modPhase >= juce::MathConstants<double>::twoPi) modPhase -= juce::MathConstants<double>::twoPi;
                const float carrier = static_cast<float> (std::sin (phase));
                const float modUni  = 0.5f * (1.0f + static_cast<float> (std::sin (modPhase)));
                s = carrier * (1.0f - amt + amt * modUni);   // amt=0 -> carrier; amt=1 -> full AM depth
                break;
            }

            case rm:
            {
                // Bipolar modulator (carrier frequency cancelled) x depth = amount.
                modPhase += modPhaseIncrement;
                if (modPhase >= juce::MathConstants<double>::twoPi) modPhase -= juce::MathConstants<double>::twoPi;
                const float carrier = static_cast<float> (std::sin (phase));
                const float modBi   = static_cast<float> (std::sin (modPhase));
                s = carrier * (1.0f - amt + amt * modBi);    // lerp(carrier, ring, amt)
                break;
            }

            case bendSkew:
            default:
                s = warpedSine (phase, static_cast<double> (amt * kMaxSkew));
                break;
        }

        phase += phaseIncrement;
        if (phase >= juce::MathConstants<double>::twoPi)
            phase -= juce::MathConstants<double>::twoPi;
        else if (phase < 0.0)
            phase += juce::MathConstants<double>::twoPi;

        return dsputils::sanitize (s);
    }
}
