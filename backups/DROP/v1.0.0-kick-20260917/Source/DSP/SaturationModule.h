#pragma once
#include <juce_dsp/juce_dsp.h>
#include <memory>

/**
 * SaturationModule - 5-mode waveshaping with selective 2x/4x oversampling.
 *
 * See plugins/DROP/.ideas/architecture.md > Core Components > SaturationModule
 * and plugins/DROP/.ideas/implementation-spec.md > SATURATIONMODULE.
 *
 * Stage 3 DSP Phase 3.2 scope: mode is fixed (default Warm; setMode()
 * exposed for tests/future Style-wiring) - DropStyle-driven mode
 * selection/blend lands in Phase 3.5. Drive rises with progress*weight
 * (placeholder curve). Both Oversampling<float> instances (2x, 4x) are
 * always pre-built in prepare() (never allocated in process()); selection
 * of which one runs is a per-segment branch based on the current effective
 * drive, per architecture.md "Oversampling factor is chosen once per
 * processing segment... avoids any runtime allocation while still being
 * 'selective' about when the extra CPU is spent."
 *
 * All 5 waveshapers are bounded (tanh/atan/soft-division-based), so no mode
 * can blow up regardless of drive. DC blocking (one-pole HP ~5Hz) guards
 * against asymmetric-clip DC buildup (Crushed mode). Final weight-mix blend
 * (architecture.md "Always-Process, Weight-Mix") guarantees exact
 * transparency at weight == 0.
 */
namespace drop
{

enum class SaturationMode
{
    Clean = 0,
    Warm,
    Driven,
    Distorted,
    Crushed
};

class SaturationModule
{
public:
    void prepare(const juce::dsp::ProcessSpec& spec);
    void reset();

    void setMode(SaturationMode newMode) noexcept { mode = newMode; }
    SaturationMode getMode() const noexcept { return mode; }

    void process(juce::dsp::AudioBlock<float> block, float weight, float progress) noexcept;

private:
    static float shape(float x, SaturationMode m) noexcept;

    std::unique_ptr<juce::dsp::Oversampling<float>> oversampler2x, oversampler4x;
    juce::dsp::IIR::Filter<float> dcBlockers[2];

    juce::AudioBuffer<float> dryBuffer, wetBuffer;
    juce::SmoothedValue<float> weightSmoothed;

    double sampleRate = 44100.0;
    SaturationMode mode = SaturationMode::Warm; // Phase 3.2 fixed placeholder (Style-driven in Phase 3.5)

    // architecture.md: oversampling engages when effective drive crosses
    // "drive equivalent > ~6dB of gain into the shaper".
    static constexpr float kOversampleThresholdDriveLinear = 2.0f; // ~6dB
    static constexpr float kMaxDriveDb = 18.0f;
};

} // namespace drop
