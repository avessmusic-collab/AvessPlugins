#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

// SaturationEngine - first-order ADAA tanh waveshaper (Phase 3.2). Zero latency, no oversampling.
//
//   u_n = drive * x_n + bias                      bias = 0.08 * driveScale  (even harmonics)
//   y_n = (F(u_n) - F(u_{n-1})) / (u_n - u_{n-1}),   F(u) = log(cosh(u))
//         |x_n - x_{n-1}| < 1e-5  ->  tanh(0.5 * (u_n + u_{n-1}))
//   F is evaluated in double as |u| + log1p(exp(-2|u|)) - ln2 (overflow-free), with the fast path
//   F(u) ~= |u| - ln2 for |u| > 9.
//   wet = DCblock_12Hz( y_n - tanh(bias) ) * makeup(drive)          (wet path only)
//   out = x + amount * (wet - x)                  (parallel blend; amount == 0 returns x exactly)
//
// makeup(drive): the architecture's ~ 1/sqrt(drive) is only a first guess; the shipped curve is the
// exact level compensation for a Gaussian signal at the -18 dBFS reference RMS (pink noise is
// near-Gaussian): makeup(d) = rms_ac(tanh(1*z*s + b(1))) / rms_ac(tanh(d*z*s + b(d))), z ~ N(0,1),
// tabulated once at prepare() (0.1 drive steps, 1..8, linear interpolation). This is what keeps the
// loudness change < 1.5 dB across the drive range at the reference level.
//
// Per-channel state (previous input, DC-blocker). 50 ms linear smoothing of drive and amount, advanced
// per sample.
class SaturationEngine
{
public:
    static constexpr float  kMinDrive = 1.0f;
    static constexpr float  kMaxDrive = 8.0f;
    static constexpr float  kBias     = 0.08f;
    static constexpr float  kReferenceRms = 0.12589254f;   // -18 dBFS
    static constexpr double kSmoothSec = 0.050;
    static constexpr double kDcHz = 12.0;

    void prepare (double sampleRate);
    void reset();

    // Control-rate update. snap == true jumps the smoothers to the targets.
    void setTargets (float drive, float amount, bool snap);

    void process (float* l, float* r, int n);

    static float makeupFor (float drive) noexcept;       // tabulated level compensation
    static float biasFor (float drive) noexcept;         // 0 at drive 1 .. kBias at drive >= 4.2

private:
    inline float shape (int ch, float x, float drive, float bias, float makeup) noexcept;

    using Sm = juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear>;

    double sampleRate = 44100.0;
    float  dcR = 0.998f;

    Sm driveSm, amountSm;

    float x1[2]    { 0.0f, 0.0f };
    float dcX1[2]  { 0.0f, 0.0f };
    float dcY1[2]  { 0.0f, 0.0f };
};
