#pragma once

#include <array>
#include <cmath>

#include <juce_dsp/juce_dsp.h>

#include "Utilities/DSPUtils.h"

namespace kickr
{
    /**
        Fully synthesised transient / click — NO samples, ever (creative-brief hard rule).
        Per-voice layer inside KickVoice; summed with the body BEFORE the voice's mono
        output reaches the engine mix / TransientShaper. Runs at `fsOversampled`
        (in-region, AD-10). One-shot: armed on `noteOn`, silent once all parts finish.

        Blend (Open Q 7 — RESOLVED weights, DO NOT CHANGE):

            out = ( 0.50 * A  +  0.35 * B  +  0.15 * C ) * clickLevel * velClick

          A — filtered noise burst      white noise (own juce::Random, NOT shared) ->
                                        StateVariableTPTFilter band-pass @ clickTone, Q 0.7.
                                        AD: attack 0.05 ms, exp decay = clickTime.
          B — transient oscillator      sine (phase-0 on trigger) @ clickPitch with its own
                                        fast drop clickPitch -> 0.5*clickPitch over
                                        min(clickTime, 8 ms) (fixed-curve normalised exp).
                                        AD: attack 0.02 ms, exp decay = 0.8*clickTime.
          C — windowed impulse          raised-cosine window of kClickImpulseWindowMs,
                                        converted to samples at `fsOversampled` and clamped
                                        to >= 8 samples (NEVER a bare 1-sample spike),
                                        amplitude 1.0, through a StateVariableTPTFilter
                                        low-pass @ clickTone, Q 2.0. Window baked into a
                                        fixed-size LUT in prepare().

        Band-limited by construction at the oversampled rate (pre-filtered noise,
        band-limited osc, raised-cosine impulse >= 8 samples) so the OS headroom is real.

        Phase 2.9: `clickWidth` L/R decorrelation makes this stereo. This phase is mono.

        RT-safe: no allocation / lock / log / IO in the audio path. `juce::Random::nextFloat`
        is fine on the audio thread. AD running values + SVF states are denormal-flushed.
    */
    class ClickGenerator
    {
    public:
        // ~0.18 ms raised-cosine impulse window (Stage-0 Addendum).
        static constexpr float  kClickImpulseWindowMs = 0.18f;
        // Fixed pitch-drop contour sharpness for component B.
        static constexpr float  kPitchDropK           = 4.0f;
        // 0.18 ms * 8x * 96 kHz ~= 138 samples -> 256 is a safe cap.
        static constexpr size_t kWindowLutSize        = 256;

        void prepare (double fsOversampled) noexcept;
        void reset() noexcept;

        /** Per-block, from KickEngine -> KickVoice. No APVTS reads in here. */
        void setParams (float level, float toneHz, float timeMs, float pitchHz) noexcept;

        /** Trigger: arm all 3 AD envelopes, reset phases, prime the windowed impulse. */
        void noteOn (float velClickGain) noexcept;

        /** Summed mono click sample. Returns exactly 0 once every part has finished. */
        float renderSample() noexcept;

        /** The body's AmplitudeEnvelope governs KickVoice::isActive(); this just reports
            whether the click still contributes anything. */
        bool isActive() const noexcept { return active; }

    private:
        /** Tiny attack-decay envelope: raised-cosine attack then one-pole exp decay. */
        struct ClickAD
        {
            static constexpr float kFloorGain = 3.16227766e-5f;   // -90 dB

            int   attackSamples { 1 };
            int   attackPos     { 0 };
            float decayCoef     { 0.0f };
            float value         { 0.0f };
            bool  attacking     { false };
            bool  running       { false };

            void reset() noexcept
            {
                attackPos = 0;
                value     = 0.0f;
                attacking = false;
                running   = false;
            }

            void arm (float attackMs, float decayMs, double sr) noexcept
            {
                attackSamples = juce::jmax (1, static_cast<int> (std::lround (
                                    static_cast<double> (juce::jmax (0.0f, attackMs))
                                        * 0.001 * juce::jmax (1.0, sr))));
                attackPos = 0;
                decayCoef = dsputils::expDecayCoef (decayMs, sr);
                value     = 0.0f;
                attacking = true;
                running   = true;
            }

            float tick() noexcept
            {
                if (! running)
                    return 0.0f;

                if (attacking)
                {
                    const float x = static_cast<float> (attackPos)
                                  / static_cast<float> (attackSamples);
                    value = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::pi * x);

                    if (++attackPos >= attackSamples)
                    {
                        attacking = false;
                        value     = 1.0f;
                    }
                }
                else
                {
                    value *= decayCoef;

                    if (value < kFloorGain)
                    {
                        value   = 0.0f;
                        running = false;
                    }
                }

                dsputils::flushDenormal (value);
                return value;
            }

            bool isActive() const noexcept { return running; }
        };

        double fs           { 44100.0 };
        float  nyquistLimit { 19845.0f };   // fs * 0.45

        // Per-block params.
        float clickLevel   { 0.4f };
        float clickTimeMs  { 3.0f };
        float clickPitchHz { 5000.0f };
        int   pitchDropSamples { 64 };

        // Trigger state.
        float velClick { 1.0f };
        bool  active   { false };

        // Component A — filtered noise burst.
        juce::Random rng;   // Phase 2.9: a second stream (rngR) for clickWidth decorrelation
        juce::dsp::StateVariableTPTFilter<float> noiseFilter;
        ClickAD envA;

        // Component B — transient oscillator with its own pitch drop.
        double bPhase { 0.0 };
        int    bPos   { 0 };
        float  expNegKDrop  { 0.0f };
        float  invDenomDrop { 1.0f };
        ClickAD envB;

        // Component C — windowed raised-cosine impulse -> resonant low-pass.
        juce::dsp::StateVariableTPTFilter<float> impulseFilter;
        std::array<float, kWindowLutSize> windowLut {};
        int  impulseLen   { 8 };
        int  impulsePos   { 0 };
        int  cRingSamples { 0 };   // let the LP ring out after the window ends
        bool cActive      { false };
    };
}
