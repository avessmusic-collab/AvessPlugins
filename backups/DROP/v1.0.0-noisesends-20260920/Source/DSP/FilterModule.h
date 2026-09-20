#pragma once
#include <juce_dsp/juce_dsp.h>

/**
 * FilterModule - HP/LP/BP/Dual musical filtering.
 *
 * See plugins/DROP/.ideas/architecture.md > Core Components > FilterModule
 * and plugins/DROP/.ideas/implementation-spec.md > FILTERMODULE.
 *
 * Stage 3 DSP Phase 3.2 scope: real DSP with a FIXED internal cutoff-sweep
 * curve (log-frequency interpolation) and a fixed resonance ceiling -
 * DropStyle-authored per-Style curves land in Phase 3.5. Always processes
 * (architecture.md "Always-Process, Weight-Mix Architecture" - no bypass,
 * ever); `weight` blends the module's own dry (pre-Filter, local) signal
 * against its fully-processed wet signal, guaranteeing exact transparency at
 * weight == 0 regardless of internal coefficient state.
 *
 * Uses juce::dsp::StateVariableTPTFilter (Topology-Preserving Transform),
 * chosen per architecture.md for stability under fast per-segment
 * cutoff/resonance modulation. RT-safe: all buffers preallocated in
 * prepare(), no allocation in process().
 */
namespace drop
{

enum class FilterMode
{
    LowPass = 0,
    HighPass,
    BandPass,
    Dual // parallel HP+LP, summed 50/50 (Style-configurable crossfade lands in Phase 3.5)
};

class FilterModule
{
public:
    void prepare(const juce::dsp::ProcessSpec& spec);
    void reset();

    void setMode(FilterMode newMode) noexcept { mode = newMode; }
    FilterMode getMode() const noexcept { return mode; }

    /**
     * Processes `block` in place (may be a sub-block, per architecture.md
     * sample-accurate segmentation). `weight` is the module's final
     * IntensityBudget weight (0-1, already macro-scaled and budget-capped).
     * `progress` (0-1) drives the musical cutoff sweep.
     */
    void process(juce::dsp::AudioBlock<float> block, float weight, float progress) noexcept;

private:
    juce::dsp::StateVariableTPTFilter<float> lowFilter, highFilter;
    juce::AudioBuffer<float> dryBuffer, wetBuffer, dualBuffer;
    juce::SmoothedValue<float> weightSmoothed;

    double sampleRate = 44100.0;
    FilterMode mode = FilterMode::LowPass;

    // Low-Frequency Protection (architecture.md / implementation-spec.md):
    // cutoff sweep never crosses below this floor in LP/Dual mode.
    static constexpr float kLowFreqFloorHz = 100.0f;

    // Phase 3.2 fixed placeholder sweep endpoints (Style-driven in Phase 3.5).
    static constexpr float kStartCutoffHz = 18000.0f;
    static constexpr float kEndCutoffHz = 400.0f;

    // Resonance ceiling ("Protect against unstable resonance").
    static constexpr float kMaxResonance = 8.0f;
    static constexpr float kMinResonance = 0.707f; // Butterworth Q, no peaking
};

} // namespace drop
