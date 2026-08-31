#include "DSP/SamplePlayer.h"
#include "Utilities/DSPUtils.h"

#include <cmath>

namespace kickr
{
    void SamplePlayer::prepare (double fsOversampled) noexcept
    {
        fs           = juce::jmax (1.0, fsOversampled);
        nyquistLimit = static_cast<float> (fs * 0.45);
        fadeLen      = juce::jmax (1, static_cast<int> (std::lround (0.001 * fs)));

        juce::dsp::ProcessSpec spec;
        spec.sampleRate       = fs;
        spec.maximumBlockSize = static_cast<juce::uint32> (32);   // per-sample processing
        spec.numChannels      = static_cast<juce::uint32> (1);

        hp.prepare (spec);
        hp.setType (juce::dsp::StateVariableTPTFilterType::highpass);
        hp.setResonance (0.707f);

        lp.prepare (spec);
        lp.setType (juce::dsp::StateVariableTPTFilterType::lowpass);
        lp.setResonance (0.707f);

        reset();
    }

    void SamplePlayer::reset() noexcept
    {
        buf          = nullptr;
        nSrc         = 0;
        readPos      = 0.0;
        sinceOn      = 0;
        env          = 0.0f;
        attackPos    = 0;
        attacking    = false;
        crushCounter = 0;
        crushHeld    = 0.0f;
        active       = false;
        finished     = true;
        hp.reset();
        lp.reset();
    }

    void SamplePlayer::updateOversampledRate (double newFsOversampled) noexcept
    {
        const double oldFs = fs;
        fs           = juce::jmax (1.0, newFsOversampled);
        nyquistLimit = static_cast<float> (fs * 0.45);
        fadeLen      = juce::jmax (1, static_cast<int> (std::lround (0.001 * fs)));

        juce::dsp::ProcessSpec spec;
        spec.sampleRate       = fs;
        spec.maximumBlockSize = static_cast<juce::uint32> (32);
        spec.numChannels      = static_cast<juce::uint32> (1);
        hp.prepare (spec);   // same numChannels -> no allocation
        hp.setType (juce::dsp::StateVariableTPTFilterType::highpass);
        hp.setResonance (0.707f);
        lp.prepare (spec);
        lp.setType (juce::dsp::StateVariableTPTFilterType::lowpass);
        lp.setResonance (0.707f);

        if (buf != nullptr)
            updateDerived();   // ratio / decayCoef / cutoffs / crush vs the new fs

        if (active && ! finished)
        {
            const double r = fs / juce::jmax (1.0, oldFs);
            auto scale = [r] (int n) noexcept
            {
                return juce::jmax (0, static_cast<int> (std::llround (static_cast<double> (n) * r)));
            };
            attackSamples = scale (attackSamples);
            attackPos     = scale (attackPos);
            sinceOn       = scale (sinceOn);
        }
    }

    void SamplePlayer::updateDerived() noexcept
    {
        // Resample ratio (source samples advanced per output sample).
        const int midiOffset = params.midiTrack ? (noteNum - rootNote) : 0;
        const double semis = static_cast<double> (params.tuneSemis)
                           + static_cast<double> (params.fineCents) / 100.0
                           + static_cast<double> (midiOffset);
        ratio = juce::jlimit (0.03125, 32.0,
                              (srcRate / fs) * std::pow (2.0, semis / 12.0));

        // Trim window (source-sample indices).
        if (nSrc > 1)
        {
            float a = juce::jlimit (0.0f, 1.0f, params.start01);
            float b = juce::jlimit (0.0f, 1.0f, params.end01);
            if (b < a + 0.001f)
                b = juce::jmin (1.0f, a + 0.001f);

            startIdx = juce::jlimit (0, nSrc - 1,
                                     static_cast<int> (std::floor (a * static_cast<float> (nSrc))));
            endIdx   = juce::jlimit (startIdx + 1, nSrc,
                                     static_cast<int> (std::ceil (b * static_cast<float> (nSrc))));
        }

        decayCoef = dsputils::expDecayCoef (params.decayMs, fs);

        // Filters — bypass at the range extremes.
        hpActive = params.hpHz > 20.0001f;
        lpActive = params.lpHz < 19999.9f;
        if (hpActive)
            hp.setCutoffFrequency (juce::jlimit (20.0f, nyquistLimit, params.hpHz));
        if (lpActive)
            lp.setCutoffFrequency (juce::jlimit (20.0f, nyquistLimit, params.lpHz));

        // Crush — crush01 = 0 is exactly bit-transparent (both stages skipped).
        crushActive = params.crush01 > 1.0e-6f;
        if (crushActive)
        {
            const float bits = 16.0f + (4.0f - 16.0f) * juce::jlimit (0.0f, 1.0f, params.crush01);
            crushQ    = std::pow (2.0f, juce::jmax (1.0f, bits - 1.0f));
            invCrushQ = 1.0f / crushQ;
            crushHold = juce::jmax (1, static_cast<int> (std::lround (
                            1.0f + 15.0f * juce::jlimit (0.0f, 1.0f, params.crush01))));
        }
    }

    void SamplePlayer::setParams (const SampleParams& p) noexcept
    {
        params = p;
        if (buf != nullptr)
            updateDerived();
    }

    void SamplePlayer::noteOn (const SampleBuffer* newBuf, int noteNumber, float vel) noexcept
    {
        noteNum   = noteNumber;
        velFactor = juce::jlimit (0.0f, 4.0f, vel);

        const bool disabled = params.enable < 0.5f;

        if (newBuf == nullptr || disabled
            || newBuf->audio.getNumSamples() < 4)
        {
            buf      = nullptr;
            active   = false;
            finished = true;
            return;
        }

        buf      = newBuf;
        nSrc     = newBuf->audio.getNumSamples();
        nSrcCh   = juce::jlimit (1, 2, newBuf->audio.getNumChannels());
        srcRate  = newBuf->sourceRate > 0.0 ? newBuf->sourceRate : 44100.0;
        rootNote = newBuf->rootNote;

        // Window + ratio need valid nSrc before updateDerived().
        startIdx = 0;
        endIdx   = nSrc;
        updateDerived();

        reverse = params.reverse;
        readPos = reverse ? static_cast<double> (endIdx - 1)
                          : static_cast<double> (startIdx);

        attackSamples = static_cast<int> (std::lround (
                            static_cast<double> (juce::jmax (0.0f, params.attackMs)) * 0.001 * fs));
        attackPos = 0;
        if (attackSamples > 0)
        {
            attacking = true;
            env       = 0.0f;
        }
        else
        {
            attacking = false;
            env       = 1.0f;
        }

        hp.reset();
        lp.reset();

        crushCounter = 0;
        crushHeld    = 0.0f;
        sinceOn      = 0;
        active       = true;
        finished     = false;
    }

    float SamplePlayer::sampleAt (int idx) const noexcept
    {
        idx = juce::jlimit (0, nSrc - 1, idx);
        if (nSrcCh <= 1)
            return buf->audio.getSample (0, idx);
        return 0.5f * (buf->audio.getSample (0, idx) + buf->audio.getSample (1, idx));
    }

    float SamplePlayer::interpolate() noexcept
    {
        const int   i = static_cast<int> (std::floor (readPos));
        const float f = static_cast<float> (readPos - static_cast<double> (i));

        const float xm1 = sampleAt (i - 1);
        const float x0  = sampleAt (i);
        const float x1  = sampleAt (i + 1);
        const float x2  = sampleAt (i + 2);

        // 4-point, 3rd-order Lagrange (Olli Niemitalo / JUCE LagrangeInterpolator form).
        const float c0 = x0;
        const float c1 = x1 - (1.0f / 3.0f) * xm1 - 0.5f * x0 - (1.0f / 6.0f) * x2;
        const float c2 = 0.5f * (xm1 + x1) - x0;
        const float c3 = (1.0f / 6.0f) * (x2 - xm1) + 0.5f * (x0 - x1);

        return ((c3 * f + c2) * f + c1) * f + c0;
    }

    float SamplePlayer::renderSample() noexcept
    {
        if (! active || finished || buf == nullptr)
            return 0.0f;

        // 2. Trim — past the window edge -> silence for the rest of the voice.
        const bool pastEnd = reverse ? (readPos <= static_cast<double> (startIdx))
                                     : (readPos >= static_cast<double> (endIdx));
        if (pastEnd)
        {
            finished = true;
            return 0.0f;
        }

        // 1. Resample (mono-summed).
        float s = interpolate();

        readPos += reverse ? -ratio : ratio;
        ++sinceOn;

        // 2. Fades — 1 ms raised-cosine at noteOn and approaching the window end.
        float g = 1.0f;
        if (sinceOn < fadeLen)
            g *= 0.5f - 0.5f * std::cos (juce::MathConstants<float>::pi
                                         * static_cast<float> (sinceOn) / static_cast<float> (fadeLen));

        const double remainSrc = reverse ? (readPos - static_cast<double> (startIdx))
                                         : (static_cast<double> (endIdx) - readPos);
        const double remainOut = remainSrc / juce::jmax (1.0e-9, ratio);
        if (remainOut < static_cast<double> (fadeLen))
            g *= 0.5f - 0.5f * std::cos (juce::MathConstants<float>::pi
                                         * juce::jlimit (0.0f, 1.0f,
                                                         static_cast<float> (remainOut) / static_cast<float> (fadeLen)));
        s *= g;

        // 3. AD envelope — raised-cosine attack -> one-pole exponential decay, in real
        //    time from noteOn (voice age), NOT tied to read position.
        //
        //    Fix (2026-08-31, user report: "the sample doesn't play whole [when]
        //    reversed"): a normal one-shot's loud content sits at its recorded START.
        //    In reverse, that content is read LAST — but this envelope still decays
        //    loud-to-quiet from t=0 regardless of direction, so by the time playback
        //    reached it the envelope had already decayed it to silence (or the
        //    envelope-floor early-finish below had already killed the whole voice).
        //    For REVERSE, apply the MIRROR of the decay curve (rises 0->1 across
        //    `sampleDecay`, then holds near 1) so the shape lines up with the
        //    reversed audio instead of fighting it. `env` itself is left as the
        //    un-mirrored decay curve — used by the floor check below.
        if (attacking)
        {
            const float x = static_cast<float> (attackPos) / static_cast<float> (juce::jmax (1, attackSamples));
            env = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::pi * x);
            if (++attackPos >= attackSamples)
            {
                attacking = false;
                env       = 1.0f;
            }
        }
        else
        {
            env *= decayCoef;
        }
        dsputils::flushDenormal (env);
        s *= reverse ? (1.0f - env) : env;

        // 4. HP then LP (bypassed at the range extremes).
        if (hpActive)
            s = hp.processSample (0, s);
        if (lpActive)
            s = lp.processSample (0, s);
        hp.snapToZero();
        lp.snapToZero();

        // 5. Crush — bit quantise + sample-and-hold decimation. crush01 = 0 skips both.
        if (crushActive)
        {
            if (crushCounter <= 0)
            {
                crushHeld    = std::round (s * crushQ) * invCrushQ;
                crushCounter = crushHold;
            }
            --crushCounter;
            s = crushHeld;
        }

        // End the layer once a short decay has run out (window may be much longer).
        // NOT for reverse: there, `env` decaying toward the floor means the applied
        // (mirrored) envelope is approaching FULL volume, not silence — the window
        // boundary (`pastEnd`, checked every sample above) is the only correct
        // end-of-playback signal for reverse.
        if (! reverse && ! attacking && env < kFloorGain && sinceOn > fadeLen + attackSamples)
        {
            finished = true;
            env      = 0.0f;
        }

        // 6. x level x velocity.
        return dsputils::sanitize (s * params.level * velFactor);
    }
}
