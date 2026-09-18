#include "NoiseModule.h"
#include <cmath>

namespace drop
{

void NoiseModule::prepare(const juce::dsp::ProcessSpec& spec)
{
    sampleRate = spec.sampleRate > 0.0 ? spec.sampleRate : 44100.0;
    weightSmoothed.reset(sampleRate, 0.02);
    reset();
}

void NoiseModule::reset()
{
    pinkStateL = PinkFilterState {};
    pinkStateR = PinkFilterState {};
    brightnessStateL = brightnessStateR = 0.0f;
    brownStateL = brownStateR = 0.0f;
    svfS1L = svfS2L = svfS1R = svfS2R = 0.0f;
    sawPhaseL = sawPhaseR = 0.0;
    for (int i = 0; i < kShepardPartials; ++i)
        shepardPhase[i] = 0.0;
    weightSmoothed.setCurrentAndTargetValue(0.0f);
}

void NoiseModule::setSeed(uint32_t masterSeed) noexcept
{
    rngL.seed(masterSeed);
    rngR.seed(masterSeed ^ kSeedXorConstant);
}

float NoiseModule::nextWhite(std::mt19937& rng) noexcept
{
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
    return dist(rng);
}

float NoiseModule::nextPink(float white, PinkFilterState& s) noexcept
{
    // POST-1.0 (2026-09-18): takes the caller's already-drawn white sample
    // instead of drawing internally, so EVERY NoiseType consumes exactly
    // one RNG draw per channel per sample - RNG history stays bit-identical
    // to the legacy build for White/Pink AND independent of `noiseType`
    // automation (the documented determinism guarantee).
    // Paul Kellett's refined cascaded one-pole pink-noise approximation.
    s.b0 = 0.99886f * s.b0 + white * 0.0555179f;
    s.b1 = 0.99332f * s.b1 + white * 0.0750759f;
    s.b2 = 0.96900f * s.b2 + white * 0.1538520f;
    s.b3 = 0.86650f * s.b3 + white * 0.3104856f;
    s.b4 = 0.55000f * s.b4 + white * 0.5329522f;
    s.b5 = -0.7616f * s.b5 - white * 0.0168980f;
    const float pink = s.b0 + s.b1 + s.b2 + s.b3 + s.b4 + s.b5 + s.b6 + white * 0.5362f;
    s.b6 = white * 0.115926f;
    return pink * 0.11f;
}

void NoiseModule::process(juce::dsp::AudioBlock<float> block, float weight, float progress, float stereoWidth01,
                           float noiseTension01) noexcept
{
    const int numSamples = (int) block.getNumSamples();
    const int numChannels = (int) block.getNumChannels();
    if (numSamples <= 0 || numChannels <= 0)
        return;

    weight = juce::jlimit(0.0f, 1.0f, weight);
    progress = juce::jlimit(0.0f, 1.0f, progress);
    stereoWidth01 = juce::jlimit(0.0f, 1.0f, stereoWidth01);
    noiseTension01 = juce::jlimit(0.0f, 1.0f, noiseTension01);

    // POST-1.0 fix (2026-09-17, user-directed: "make so the noise adds
    // gradualy"). DOCUMENTED SUPERSESSION of the Phase 3.3 placeholder
    // `progress * progress` quadratic shape: that curve stayed almost
    // inaudible until very late in the build, then arrived abruptly (the
    // exact user complaint). Replaced with the shared, pure
    // riseAmountForProgress() (smoothstep, see this class's header doc
    // comment for the full shape justification/comparison table) - the
    // SAME math Source/ui/public/index.html's TENSION CURVE display plots
    // as its second ("NOISE") curve, so what the user sees on the graph is
    // provably what they hear. This module still runs unconditionally
    // (always-process pattern); the "sharp cutoff right at Vacuum" that
    // architecture.md describes still comes from VacuumModule attenuating
    // the whole bus during Peak - see processSegment().
    //
    // The brightness sweep below is left as a plain linear jmap
    // (unchanged) - it does not share the old rise curve's "abrupt"
    // character (it was never quadratic to begin with), so no equivalent
    // gradual-ization was needed there.
    const float riseAmount = riseAmountForProgress(progress, noiseTension01);
    const float brightnessCutoffHz = juce::jmap(progress, 0.0f, 1.0f, 400.0f, 12000.0f);
    const float lpCoeff = 1.0f - std::exp(-2.0f * juce::MathConstants<float>::pi * brightnessCutoffHz / (float) sampleRate);

    // POST-1.0 (2026-09-18, user-directed "add choices for different kinds
    // of noise"): resolve the user-facing NoiseType to an effective source.
    // StyleDefault defers to the Style-authored NoiseColor (exact legacy
    // behavior); the explicit types map White/Pink directly and add four
    // new riser sources from the standard sound-design palette (see the
    // NoiseType enum doc in NoiseModule.h).
    NoiseType effectiveType;
    switch (type)
    {
        case NoiseType::StyleDefault:
            effectiveType = (color == NoiseColor::Pink) ? NoiseType::Pink : NoiseType::White; // White & Filtered both start from white (as before)
            break;
        default:
            effectiveType = type;
            break;
    }

    // Sweep: TPT state-variable low-pass (Zavalishin), cutoff swept
    // EXPONENTIALLY 200Hz -> 12kHz with progress (the classic resonant
    // riser sweep), moderate resonance. Coefficients are per-segment
    // (progress is segment-constant); state is per-sample/per-channel.
    const float sweepCutoffHz = 200.0f * std::pow(12000.0f / 200.0f, juce::jlimit(0.0f, 1.0f, progress));
    const float svfG = std::tan(juce::MathConstants<float>::pi * juce::jmin(sweepCutoffHz, 0.45f * (float) sampleRate) / (float) sampleRate);
    constexpr float kSweepResonanceQ = 2.5f;
    const float svfK = 1.0f / kSweepResonanceQ;
    const float svfA1 = 1.0f / (1.0f + svfG * (svfG + svfK));
    constexpr float kSweepMakeupGain = 2.0f; // resonant LP of white sits well below the raw input's RMS

    // Saw: tonal riser, pitch climbing 4 octaves (55Hz A1 -> 880Hz A5)
    // exponentially with progress; right channel detuned +0.6% for width.
    // PolyBLEP band-limiting on the phase wrap.
    const double sawFreq = 55.0 * std::pow(2.0, 4.0 * (double) juce::jlimit(0.0f, 1.0f, progress));
    const double sawIncL = sawFreq / sampleRate;
    const double sawIncR = sawFreq * 1.006 / sampleRate;
    constexpr float kSawLevel = 0.9f;
    auto polyBlep = [] (double t, double dt) -> float
    {
        if (t < dt)      { const double x = t / dt;               return (float) (x + x - x * x - 1.0); }
        if (t > 1.0 - dt){ const double x = (t - 1.0) / dt;       return (float) (x * x + x + x + 1.0); }
        return 0.0f;
    };

    // Shepard: kShepardPartials octave-spaced sines across 55Hz-3520Hz,
    // each partial's log-position advancing ONE full octave over the build
    // (wrapping), amplitude = sin^2 window in log space (silent at both
    // spectral edges) - the standard endless-rise construction.
    // OPTIMIZED (2026-09-18 audit): partial frequencies and windows depend
    // only on `progress`, which is SEGMENT-constant - they were previously
    // recomputed per SAMPLE (6x pow + 6x window sin + 6x floor every
    // sample); now hoisted here, leaving only the phase advance + one sin
    // per partial in the loop. Numerically identical (same expressions,
    // same segment-constant inputs).
    constexpr double kShepardBaseHz = 55.0;
    const double shepardShift = (double) juce::jlimit(0.0f, 1.0f, progress); // octaves of upward shift, wraps per partial
    constexpr float kShepardLevel = 0.55f;
    double shepardInc[kShepardPartials];
    float shepardWin2[kShepardPartials];
    if (effectiveType == NoiseType::Shepard)
    {
        for (int k2 = 0; k2 < kShepardPartials; ++k2)
        {
            double pos = ((double) k2 + shepardShift) / (double) kShepardPartials;
            pos -= std::floor(pos);
            const double freq = kShepardBaseHz * std::pow(2.0, pos * (double) kShepardPartials);
            const float win = (float) std::sin(juce::MathConstants<double>::pi * pos);
            shepardInc[k2] = freq / sampleRate;
            shepardWin2[k2] = win * win;
        }
    }

    weightSmoothed.setTargetValue(weight);

    for (int i = 0; i < numSamples; ++i)
    {
        // The L/R white streams are ALWAYS drawn, whatever the type -
        // preserves the documented determinism guarantee (RNG history is
        // independent of automation, now including `noiseType` automation).
        const float whiteL = nextWhite(rngL);
        const float whiteR = nextWhite(rngR);

        float rawL, rawR;
        switch (effectiveType)
        {
            case NoiseType::Pink:
                rawL = nextPink(whiteL, pinkStateL);
                rawR = nextPink(whiteR, pinkStateR);
                break;

            case NoiseType::Brown:
            {
                // Leaky-integrated white (1/f^2); standard "Paul Kellett"
                // brown-noise form with a 3.5x level make-up.
                brownStateL = (brownStateL + 0.02f * whiteL) / 1.02f;
                brownStateR = (brownStateR + 0.02f * whiteR) / 1.02f;
                rawL = brownStateL * 3.5f;
                rawR = brownStateR * 3.5f;
                break;
            }

            case NoiseType::Sweep:
            {
                // TPT SVF low-pass per channel (v1 = band, v2 = low).
                const float v1L = svfA1 * (svfS1L + svfG * (whiteL - svfS2L));
                const float v2L = svfS2L + svfG * v1L;
                svfS1L = 2.0f * v1L - svfS1L;
                svfS2L = 2.0f * v2L - svfS2L;
                const float v1R = svfA1 * (svfS1R + svfG * (whiteR - svfS2R));
                const float v2R = svfS2R + svfG * v1R;
                svfS1R = 2.0f * v1R - svfS1R;
                svfS2R = 2.0f * v2R - svfS2R;
                rawL = v2L * kSweepMakeupGain;
                rawR = v2R * kSweepMakeupGain;
                break;
            }

            case NoiseType::Saw:
            {
                sawPhaseL += sawIncL; if (sawPhaseL >= 1.0) sawPhaseL -= 1.0;
                sawPhaseR += sawIncR; if (sawPhaseR >= 1.0) sawPhaseR -= 1.0;
                rawL = ((float) (2.0 * sawPhaseL - 1.0) - polyBlep(sawPhaseL, sawIncL)) * kSawLevel;
                rawR = ((float) (2.0 * sawPhaseR - 1.0) - polyBlep(sawPhaseR, sawIncR)) * kSawLevel;
                break;
            }

            case NoiseType::Shepard:
            {
                float s = 0.0f;
                for (int k2 = 0; k2 < kShepardPartials; ++k2)
                {
                    // Per-segment increments/windows hoisted above (audit
                    // optimization) - only the oscillator itself runs here.
                    shepardPhase[k2] += shepardInc[k2];
                    if (shepardPhase[k2] >= 1.0) shepardPhase[k2] -= 1.0;
                    s += (float) std::sin(juce::MathConstants<double>::twoPi * shepardPhase[k2]) * shepardWin2[k2];
                }
                rawL = rawR = s * kShepardLevel; // mono by construction (shared partial bank)
                break;
            }

            case NoiseType::StyleDefault: // unreachable (resolved above) - falls through to White
            case NoiseType::White:
            default:
                // White and Filtered both start from white noise; Filtered's
                // distinguishing brightness sweep is applied uniformly below.
                rawL = whiteL;
                rawR = whiteR;
                break;
        }

        brightnessStateL += lpCoeff * (rawL - brightnessStateL);
        brightnessStateR += lpCoeff * (rawR - brightnessStateR);

        const float mono = 0.5f * (brightnessStateL + brightnessStateR);
        const float outL = mono + (brightnessStateL - mono) * stereoWidth01;
        const float outR = mono + (brightnessStateR - mono) * stereoWidth01;

        // kBaseLevel: POST-1.0 FIFTH IMPROVEMENT SESSION flat ~6dB level
        // reduction (user bug-audit report: "too strong") - see this class's
        // header doc comment on kBaseLevel.
        const float w = weightSmoothed.getNextValue() * riseAmount * kBaseLevel;

        if (numChannels >= 2)
        {
            block.getChannelPointer(0)[i] += outL * w;
            block.getChannelPointer(1)[i] += outR * w;
        }
        else
        {
            block.getChannelPointer(0)[i] += mono * w;
        }
    }
}

} // namespace drop
