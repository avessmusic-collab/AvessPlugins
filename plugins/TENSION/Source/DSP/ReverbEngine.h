#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>

#include <atomic>

#include "AsymSmoother.h"

// ReverbEngine - parallel atmospheric reverb with send / return architecture (Phase 3.3).
//
//   in -> send gain -> send HPF (120..220 Hz) -> pre-delay (DelayLine<Linear>, 0..80 ms)
//      -> juce::dsp::Reverb (ALWAYS 100% internally wet: wetLevel = 1, dryLevel = 0)
//      -> return LPF (14 kHz -> 7 kHz) -> return gain (asymmetric 150 ms attack / 700 ms release)
//
// * Modern juce::dsp::Reverb API (prepare(ProcessSpec) / process(ProcessContext)) - Pattern #17.
// * The reverb instance is processed EVERY chunk (also with send == 0 or return == 0), so tails keep
//   evolving and ring out after tension drops (DROP lesson).
// * setParameters() is called once per control period (setTargets) with smoothed values.
// * The algorithm is hidden behind prepare / setTargets / process / reset / isQuiet so a custom FDN
//   can replace juce::dsp::Reverb later without touching TensionEngine.
// * Tail energy tracker: isQuiet() == input and output below -120 dBFS for > 0.3 s.
//
// PHASE 3.5 VERDICT (Freeverb = juce::dsp::Reverb vs custom FDN upgrade):
//   PROVISIONAL: GO - keep juce::dsp::Reverb for v1; the FDN is NOT implemented in Phase 3.5.
//   Basis: the TENSION_Profiles harness renders an impulse through this class with the real
//   mapping targets at t = 0.5 / 0.8 / 1.0 in every rise mode and reports (a) a T20-based RT60 estimate
//   and (b) a "ringing index" of the tail (std-dev in dB of the 16-bin-smoothed spectrum after dividing
//   out a 1/3-octave envelope, 200 Hz .. 5 kHz) next to the same index of an exponentially decaying white
//   noise baseline with the same decay. Freeverb's 8 parallel combs would show up as an index excess
//   (comb peaks / metallic ringing). NO-GO trigger (-> FDN behind this interface, ~150 lines): excess
//   > 1.5 dB at any t >= 0.8 point, OR listening rejects the 100 % tail. In the design the tail is also
//   defined by the send HPF (120..220 Hz), the return LPF (14 -> 7 kHz), pre-delay and the -7/-9/-11 dB
//   return, i.e. it is a quiet atmosphere layer rather than an exposed reverb, which is why a GO is the
//   expected result. The final verdict is whatever the harness prints in summary.txt ("Freeverb verdict").
//   The interface (prepare / setTargets / process / reset / isQuiet) already isolates the algorithm.
class ReverbEngine
{
public:
    static constexpr int    kChunk              = 32;
    static constexpr double kReturnAttackSec    = 0.150;
    static constexpr double kReturnReleaseSec   = 0.700;
    static constexpr double kParamSmoothSec     = 0.250;
    static constexpr double kFilterSmoothSec    = 0.080;
    static constexpr double kSendSmoothSec      = 0.030;
    static constexpr double kMaxPreDelaySec     = 0.080;
    static constexpr float  kQuietThreshold     = 1.0e-6f;      // -120 dBFS
    static constexpr double kQuietHoldSec       = 0.300;

    struct Targets
    {
        float sendGain     = 0.0f;
        float returnGain   = 0.0f;      // linear, exactly 0 at rest
        float roomSize     = 0.35f;
        float damping      = 0.7f;
        float width        = 0.6f;
        float preDelayMs   = 0.0f;
        float returnLpfHz  = 14000.0f;
        float sendHpfHz    = 120.0f;
    };

    void prepare (double sampleRate);
    void reset();

    void setTargets (const Targets& t, bool snap, int controlSamples);

    // Writes (overwrites) n output samples. Any n is accepted (processed in chunks of <= 32).
    void process (const float* inL, const float* inR, float* outL, float* outR, int n) noexcept;

    bool  isQuiet() const noexcept          { return quietRun >= quietHold; }
    bool  consumeFailure() noexcept         { return failed.exchange (false, std::memory_order_relaxed); }
    float getReturnGain() const noexcept    { return retSm.getCurrent(); }

private:
    using LinSm = juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear>;
    using MulSm = juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative>;

    void processChunk (const float* inL, const float* inR, float* outL, float* outR, int n) noexcept;
    void clearState() noexcept;

    double sampleRate = 44100.0;
    float  maxHz = 19845.0f;

    juce::dsp::Reverb reverb;
    juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Linear> preDelay;
    juce::dsp::StateVariableTPTFilter<float> sendHpf, returnLpf;
    juce::AudioBuffer<float> work;

    LinSm sendSm, roomSm, dampSm, widthSm;
    MulSm hpfSm, lpfSm;
    AsymSmoother retSm;

    double preSamples = 0.0, preTarget = 0.0, preCoef = 0.001;

    int quietRun = 0, quietHold = 14400;
    std::atomic<bool> failed { false };
};
