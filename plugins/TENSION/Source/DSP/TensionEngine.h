#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <atomic>

#include "OutputSafety.h"
#include "TensionMapping.h"

// Host transport snapshot, read once per host block (JUCE 8 getPlayHead()->getPosition()).
struct HostTransport
{
    double bpm       = 120.0;   // fallback tempo when the host provides none
    double ppq       = 0.0;
    bool   isPlaying = false;
    bool   hasBpm    = false;
    bool   hasPpq    = false;
};

// Per-block parameter snapshot (plain values, already read from the APVTS atomics).
struct EngineParams
{
    float tension  = 0.0f;   // 0..1
    float mix      = 1.0f;   // 0..1
    float outputDb = 0.0f;   // -24..+12
    bool  sync     = true;
    int   riseMode = 1;      // 0 Slow Burn, 1 Cinematic, 2 Aggressive
};

// Lock-free audio -> UI telemetry (written once per block, relaxed).
struct TensionTelemetry
{
    std::atomic<float> tensionSmoothed { 0.0f };
    std::atomic<float> outPeak         { 0.0f };
    std::atomic<float> wetRms          { 0.0f };
    std::atomic<float> noiseLevel      { 0.0f };
    std::atomic<float> lfoPhaseA       { 0.0f };
};

// TensionEngine - per-block orchestrator.
//
// Phase 3.1: skeleton only. Series processors are identity stubs; noise / delay /
// reverb returns are zero. What IS real: control-rate sub-blocking (<= 32 samples,
// aligned to an absolute sample counter so output is block-size invariant), the
// per-destination smoothers, the dry/proc/wet bus with engagement e, MIX, OutputSafety,
// output trim AFTER safety, mono/stereo handling and telemetry.
//
//   wet = dry + e * (proc - dry)          (== dry*(1-e) + proc*e, exact at e == 0 / 1)
//   out = dry + mix * (wet - dry)         (== dry*(1-mix) + wet*mix, exact when wet == dry)
//   -> OutputSafety (scrub + knee blended by s) -> output trim
//
// The delta forms are algebraically identical to the spec's forms but make t = 0
// BIT-EXACT: with e == 0 the wet bus IS the dry sample (no arithmetic), and when
// wet == dry the mix stage returns dry untouched.
class TensionEngine
{
public:
    static constexpr int controlBlockSize = 32;

    TensionEngine() = default;

    // Message / host thread: allocates everything.
    void prepare (double sampleRate, int maxBlockSize, int numChannels = 2);
    void reset();

    // Audio thread: allocation-, lock- and IO-free. Accepts 1 or 2 host channels
    // (internally always 2; mono is duplicated on entry and averaged on exit).
    void process (juce::AudioBuffer<float>& buffer, const EngineParams& params, const HostTransport& transport);

    const TensionTelemetry& getTelemetry() const noexcept { return telemetry; }
    const OutputSafety&     getSafety() const noexcept    { return safety; }
    int                     getFeedbackResetCount() const noexcept { return feedbackResetCount.load (std::memory_order_relaxed); }

    // True when e == 0, s == 0, tension settled at 0 and no tails are active:
    // the engine is a bit-exact passthrough and the wet chain is skipped.
    bool isAtRest() const noexcept;

    // TEST HOOK (Phase 3.1): scales the identity `proc` stub so the bus algebra
    // (e / MIX) is observable. Default 1.0 == pure identity. Never set by the plugin.
    void setStubProcGainForTesting (float g) noexcept { stubProcGain = g; }

private:
    void processSegment (float* out0, float* out1, int n);
    void controlTick();
    void resetFeedbackModules() noexcept;

    using Smoother = juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear>;

    double sampleRate = 44100.0;
    int    maxBlock   = 0;
    bool   prepared   = false;

    // Preallocated scratch (2 channels, maxBlock)
    juce::AudioBuffer<float> dryBuf, procBuf, wetBuf;

    // Per-destination smoothers
    Smoother tensionSm;   // 20 ms
    Smoother engageSm;    // 15 ms  (e)
    Smoother safetySm;    // 15 ms  (s)
    Smoother mixSm;       // 20 ms
    Smoother trimSm;      // 20 ms  (linear gain)

    OutputSafety safety;

    TensionDSP::RiseMode riseMode = TensionDSP::RiseMode::Cinematic;
    HostTransport        transport;
    bool                 syncEnabled = true;

    bool  firstBlock = true;
    int   controlPos = 0;       // samples elapsed inside the current 32-sample control period
    float stubProcGain = 1.0f;

    // Per-process() accumulators
    float  statOutPeak = 0.0f;
    double statWetSq   = 0.0;
    long long statWetCount = 0;

    float  peakHold = 0.0f;
    double peakDecayTimeSec = 0.3;

    std::atomic<int> feedbackResetCount { 0 };

    TensionTelemetry telemetry;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TensionEngine)
};
