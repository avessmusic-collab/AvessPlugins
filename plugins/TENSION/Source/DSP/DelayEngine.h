#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>

#include <atomic>

#include "AsymSmoother.h"

// DelayEngine - stereo ping-pong delay with filtered, soft-clipped feedback (Phase 3.3).
//
//   mono-ish send (0.5 (L + R) * sendGain) -> L line
//   L line out -> [HPF 180 Hz -> LPF -> tanh] * fb -> R line in
//   R line out -> [HPF 180 Hz -> LPF -> tanh] * fb -> L line in          (cross feedback)
//   returns: L out, R out  * returnGain (asymmetric 120 ms attack / 600 ms release)
//
// * 2x juce::dsp::DelayLine<float, Lagrange3rd>, one channel each. CRITICAL (Pattern #25): every
//   line is popped EXACTLY once per sample with updateReadPointer == true, BEFORE the push, so the
//   read pointer advances in lockstep with the write pointer and `delay` means "N samples ago".
// * Buffers: 2.5 s at 192 kHz per line, allocated in prepare().
// * Delay time: target passes a 250 ms one-pole slew applied PER SAMPLE (tape glide); lfoB adds a
//   tiny wobble (0..0.9 ms, opposite sign in R). Tension never switches divisions.
// * Feedback is hard-capped at 0.85 in code. In the loop: HPF 180 Hz (also blocks DC), LPF (9 kHz ->
//   3.2 kHz as feedback rises), tanh soft clip. Per-sample isfinite guard on the loop write: on
//   failure the lines + filters are reset and the failure flag is raised (the engine forwards it to
//   OutputSafety). Writes with |x| < 1e-20 are flushed to 0.
// * Tail tracker: quietRun counts consecutive samples whose loop WRITE is below -120 dBFS; once it
//   exceeds the longest possible delay (2.05 s) the lines hold nothing audible -> isQuiet().
class DelayEngine
{
public:
    static constexpr float  kHardFeedbackCap   = 0.85f;
    static constexpr double kMaxDelaySec       = 2.0;
    static constexpr double kBufferSec         = 2.5;
    static constexpr double kBufferRate        = 192000.0;
    static constexpr double kTimeSlewSec       = 0.250;
    static constexpr double kMaxSlewRate       = 0.9;      // max |d(delay)/dt| in samples/sample: read speed stays in [0.1, 1.9]
    static constexpr double kSlewVelSmoothSec  = 0.020;    // smooths the slew VELOCITY (no speed step at a time jump)
    static constexpr double kReturnAttackSec   = 0.120;
    static constexpr double kReturnReleaseSec  = 0.600;
    static constexpr double kFeedbackSmoothSec = 0.200;
    static constexpr double kSendSmoothSec     = 0.030;
    static constexpr float  kFbHpfHz           = 180.0f;
    static constexpr float  kQuietThreshold    = 1.0e-6f;
    static constexpr double kQuietHoldSec      = 2.05;

    struct Targets
    {
        float sendGain   = 0.0f;
        float returnGain = 0.0f;       // linear, exactly 0 at rest
        float feedback   = 0.0f;       // capped to 0.85
        float fbLpfHz    = 9000.0f;
        float delayMs    = 375.0f;     // <= 2000
        float lfoDepthMs = 0.0f;       // lfoB wobble depth
    };

    // sync ON: division from the rise mode (0 Slow Burn dotted 1/4, 1 Cinematic dotted 1/8,
    // 2 Aggressive 1/16 triplet), ms = 60000 / bpm * beats, halved until <= 2 s.
    // sync OFF: fixed 560 / 375 / 190 ms.
    static double computeDelayMs (bool sync, double bpm, int riseModeIndex) noexcept;

    void prepare (double sampleRate);
    void reset();

    void setTargets (const Targets& t, bool snap, int controlSamples);

    // Writes (overwrites) n output samples. lfo: bipolar lfoB per sample (may be nullptr == 0).
    void process (const float* inL, const float* inR, float* outL, float* outR, int n, const float* lfo) noexcept;

    bool   isQuiet() const noexcept              { return quietRun >= quietHold; }
    bool   consumeFailure() noexcept             { return failed.exchange (false, std::memory_order_relaxed); }
    double getCurrentDelaySamples() const noexcept { return curDelay; }
    double getTargetDelaySamples() const noexcept  { return targetDelay; }
    float  getFeedbackTarget() const noexcept    { return fbSm.getTargetValue(); }
    float  getReturnGain() const noexcept        { return retSm.getCurrent(); }

private:
    using Line  = juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Lagrange3rd>;
    using LinSm = juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear>;
    using MulSm = juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative>;

    void resetLoop() noexcept;

    double sampleRate = 44100.0;
    float  maxHz = 19845.0f;
    float  maxDelaySamples = 96000.0f;

    Line lineL, lineR;
    juce::dsp::StateVariableTPTFilter<float> fbHpf, fbLpf;     // channel 0 -> feeds the L line, channel 1 -> R line

    LinSm sendSm, fbSm, depthSm;
    MulSm lpfSm;
    AsymSmoother retSm;

    double curDelay = 0.0, targetDelay = 0.0, slewCoef = 0.001, slewVel = 0.0, velCoef = 0.001;
    bool   timeSnap = true;

    int quietRun = 0, quietHold = 98400;
    std::atomic<bool> failed { false };
};
