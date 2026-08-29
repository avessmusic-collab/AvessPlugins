#include "DSP/ClickGenerator.h"

#include <cmath>

namespace kickr
{
    void ClickGenerator::prepare (double fsOversampled) noexcept
    {
        fs           = juce::jmax (1.0, fsOversampled);
        nyquistLimit = static_cast<float> (fs * 0.45);

        juce::dsp::ProcessSpec spec;
        spec.sampleRate       = fs;
        spec.maximumBlockSize = static_cast<juce::uint32> (32);   // per-sample processing
        spec.numChannels      = static_cast<juce::uint32> (1);

        const float toneInit = juce::jlimit (20.0f, nyquistLimit, 4000.0f);

        noiseFilter.prepare (spec);
        noiseFilter.setType (juce::dsp::StateVariableTPTFilterType::bandpass);
        noiseFilter.setResonance (0.7f);
        noiseFilter.setCutoffFrequency (toneInit);

        // PHASE 2.9 — decorrelated R noise stream (independent state, same tuning).
        noiseFilterR.prepare (spec);
        noiseFilterR.setType (juce::dsp::StateVariableTPTFilterType::bandpass);
        noiseFilterR.setResonance (0.7f);
        noiseFilterR.setCutoffFrequency (toneInit);

        impulseFilter.prepare (spec);
        impulseFilter.setType (juce::dsp::StateVariableTPTFilterType::lowpass);
        impulseFilter.setResonance (2.0f);
        impulseFilter.setCutoffFrequency (toneInit);

        // Fixed normalised-exp pitch-drop coefficients for component B (curve fixed).
        expNegKDrop  = std::exp (-kPitchDropK);
        invDenomDrop = 1.0f / juce::jmax (1.0e-9f, 1.0f - expNegKDrop);

        // Raised-cosine impulse window: kClickImpulseWindowMs at fs, clamped to
        // [8, kWindowLutSize] samples — never a bare 1-sample spike (research S2).
        const int lenFromMs = static_cast<int> (std::lround (
                                  static_cast<double> (kClickImpulseWindowMs) * 0.001 * fs));
        impulseLen = juce::jlimit (8, static_cast<int> (kWindowLutSize),
                                   juce::jmax (8, lenFromMs));

        windowLut.fill (0.0f);
        for (int n = 0; n < impulseLen; ++n)
            windowLut[static_cast<size_t> (n)] =
                0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi
                                        * static_cast<float> (n)
                                        / static_cast<float> (impulseLen - 1));

        cRingSamples = static_cast<int> (std::lround (0.003 * fs));   // ~3 ms LP ring-out

        bcDelay.fill (0.0f);
        bcDelayWrite = 0;

        reset();
    }

    void ClickGenerator::reset() noexcept
    {
        noiseFilter.reset();
        noiseFilterR.reset();
        impulseFilter.reset();
        envA.reset();
        envB.reset();

        bPhase     = 0.0;
        bPos       = 0;
        impulsePos = 0;
        cActive    = false;
        active     = false;

        bcDelay.fill (0.0f);
        bcDelayWrite = 0;
    }

    void ClickGenerator::setParams (float level, float toneHz, float timeMs,
                                    float pitchHz, float widthAmt) noexcept
    {
        clickLevel   = juce::jlimit (0.0f, 1.0f, level);
        clickTimeMs  = juce::jlimit (0.1f, 50.0f, timeMs);
        clickPitchHz = juce::jlimit (20.0f, nyquistLimit, pitchHz);
        clickWidth   = juce::jlimit (0.0f, 1.0f, widthAmt);

        const float toneClamped = juce::jlimit (20.0f, nyquistLimit, toneHz);
        noiseFilter.setCutoffFrequency  (toneClamped);
        noiseFilterR.setCutoffFrequency (toneClamped);
        impulseFilter.setCutoffFrequency (toneClamped);

        pitchDropSamples = juce::jmax (1, static_cast<int> (std::lround (
                               static_cast<double> (juce::jmin (clickTimeMs, 8.0f)) * 0.001 * fs)));

        bcDelaySamples = juce::jlimit (0, kDelayLutSize - 1,
                             static_cast<int> (std::lround (
                                 static_cast<double> (clickWidth)
                                 * static_cast<double> (kMaxDelayMs) * 0.001 * fs)));
    }

    void ClickGenerator::noteOn (float velClickGain) noexcept
    {
        velClick = juce::jmax (0.0f, velClickGain);

        // Deterministic noise burst — every trigger renders the identical click
        // (a kick designer wants consistency; variation is Randomize/Mutate's job,
        //  and offline renders must be reproducible). Fixed seed, re-applied per note.
        rng.setSeed  (0x6b69636bLL);   // "kick"
        rngR.setSeed (0x6b696352LL);   // "kicR" — decorrelated R stream (fixed seed)

        // Layer off -> do no work and don't extend the voice.
        if (clickLevel < 1.0e-6f)
        {
            reset();
            return;
        }

        envA.arm (0.05f, clickTimeMs, fs);
        envB.arm (0.02f, 0.8f * clickTimeMs, fs);

        bPhase = 0.0;
        bPos   = 0;

        impulsePos = 0;
        cActive    = true;

        noiseFilter.reset();
        noiseFilterR.reset();
        impulseFilter.reset();

        bcDelay.fill (0.0f);
        bcDelayWrite = 0;

        active = true;
    }

    void ClickGenerator::renderStereo (float& l, float& r) noexcept
    {
        if (! active)
        {
            l = 0.0f;
            r = 0.0f;
            return;
        }

        // A — filtered white-noise burst (weight 0.50): base + decorrelated R stream.
        float aBase = 0.0f;
        float aDec  = 0.0f;
        if (envA.isActive())
        {
            const float envAv = envA.tick();
            const float wBase = rng.nextFloat()  * 2.0f - 1.0f;
            const float wDec  = rngR.nextFloat() * 2.0f - 1.0f;
            aBase = noiseFilter.processSample  (0, wBase) * envAv;
            aDec  = noiseFilterR.processSample (0, wDec)  * envAv;
        }

        // B — transient sine with its own fast pitch drop (weight 0.35).
        // sin() BEFORE the phase increment so the first sample is exactly sin(0) = 0.
        float b = 0.0f;
        if (envB.isActive())
        {
            const double tau = static_cast<double> (bPos)
                             / static_cast<double> (juce::jmax (1, pitchDropSamples));

            float e = 0.0f;
            if (tau < 1.0)
                e = (std::exp (-kPitchDropK * static_cast<float> (tau)) - expNegKDrop)
                        * invDenomDrop;

            const float freq = 0.5f * clickPitchHz * std::exp2 (e);

            b = std::sin (static_cast<float> (bPhase)) * envB.tick();

            bPhase += juce::MathConstants<double>::twoPi
                        * static_cast<double> (freq) / fs;
            if (bPhase >= juce::MathConstants<double>::twoPi)
                bPhase -= juce::MathConstants<double>::twoPi;
            ++bPos;
        }

        // C — windowed raised-cosine impulse -> resonant low-pass (weight 0.15).
        float c = 0.0f;
        if (cActive)
        {
            const float imp = (impulsePos < impulseLen)
                                ? windowLut[static_cast<size_t> (impulsePos)]
                                : 0.0f;
            c = impulseFilter.processSample (0, imp);

            if (++impulsePos >= impulseLen + cRingSamples)
                cActive = false;
        }

        noiseFilter.snapToZero();
        noiseFilterR.snapToZero();
        impulseFilter.snapToZero();

        if (! envA.isActive() && ! envB.isActive() && ! cActive)
            active = false;

        // clickWidth decorrelation:
        //   noise  : L = base, R = lerp(base, decorrelated, clickWidth)
        //   osc + impulse : <1 ms inter-channel sample delay on R (0 delay at clickWidth 0)
        const float noiseL = 0.50f * aBase;
        const float noiseR = 0.50f * (aBase + clickWidth * (aDec - aBase));

        const float bc = 0.35f * b + 0.15f * c;
        bcDelay[static_cast<size_t> (bcDelayWrite)] = bc;

        int readIdx = bcDelayWrite - bcDelaySamples;
        if (readIdx < 0)
            readIdx += kDelayLutSize;
        const float bcR = bcDelay[static_cast<size_t> (readIdx)];

        bcDelayWrite = (bcDelayWrite + 1) % kDelayLutSize;

        const float gain = clickLevel * velClick;
        l = dsputils::sanitize ((noiseL + bc)  * gain);
        r = dsputils::sanitize ((noiseR + bcR) * gain);
    }

    float ClickGenerator::renderSample() noexcept
    {
        float l = 0.0f;
        float r = 0.0f;
        renderStereo (l, r);
        return 0.5f * (l + r);
    }
}
