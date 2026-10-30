#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <atomic>

#include "DelayEngine.h"
#include "FilterEngine.h"
#include "LFOEngine.h"
#include "NoiseGenerator.h"
#include "OutputSafety.h"
#include "ReverbEngine.h"
#include "SaturationEngine.h"
#include "StereoWidth.h"
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
// Phase 3.2: the series chain is now real (driven by the TEMPORARY LINEAR stub mapping):
//   input trim -> FilterEngine (HP + BP sweep, lfoA cutoff) -> tremolo (lfoA)
//   -> SaturationEngine (ADAA tanh) -> StereoWidth (LR4 + M/S, lfoB)  = proc
// all driven from controlTick() on the absolute 32-sample grid.
//
// Phase 3.3: NoiseGenerator (parallel add), DelayEngine and ReverbEngine (parallel sends from proc,
// asymmetric return smoothing) are summed into the wet bus:
//   wet = dry*(1-e) + proc*e + noise + delayReturn + reverbReturn
// All three are skipped while the engine is at rest (RNG still advances); isAtRest() now also
// requires the noise to be silent and the delay / reverb tail trackers to be below -120 dBFS.
//
// Phase 3.4: the temporary linear stub mapping is replaced by the real TensionMapping. controlTick()
// asks a TensionDSP::ModeBlender (600 ms rise-mode crossfade, evaluates every profile with weight > 0
// and lerps the whole TensionTargets struct) for the targets. LFO shapes are per destination and per
// mode (amp / filter on lfoA, stereo / delay on lfoB): every destination evaluates the shapes of the
// profiles that currently carry weight, mixed with the SAME blend weights, so a shape change is a
// click-free crossfade (no zero-crossing switching needed). The amplitude LFO passes a 1.5 ms one-pole
// (so Aggressive's reverse-saw edge is a ramp, never a hard step) and the filter LFO sampled at the
// control rate a 4 ms one-pole (S&H plateaus do not zipper the cutoff).
//
// Phase 3.1 (unchanged): control-rate sub-blocking (<= 32 samples,
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
    // Any value != 1.0 also bypasses the real series modules (proc = g * dry), keeping the
    // Phase 3.1 bus-algebra tests meaningful with the real modules in the build.
    void setStubProcGainForTesting (float g) noexcept { stubProcGain = g; }

    // TEST HOOK (Phase 3.2): false == series modules bypassed (proc = dry, the Phase 3.1 identity).
    void setSeriesChainEnabledForTesting (bool enabled) noexcept { seriesChainEnabled = enabled; }

    const LFOEngine& getLfoA() const noexcept { return lfoA; }
    const LFOEngine& getLfoB() const noexcept { return lfoB; }

    // Phase 3.4 introspection (tests / UI): the mode crossfade state and the targets of the last control tick.
    const TensionDSP::ModeBlender&    getModeBlender() const noexcept { return modeBlender; }
    const TensionDSP::TensionTargets& getLastTargets() const noexcept { return lastTargets; }

    // Phase 3.5 FALLBACK (architecture.md, Implementation Risks -> TensionMapping): slow (1 s RMS)
    // wet-bus level compensator. Default OFF; the plugin never enables it unless Phase 3.5 metrics
    // prove the retuned tables still exceed the loudness budget. When enabled it measures the dry
    // input and the (pre-compensation) wet bus with 1 s one-pole power trackers and, only while the
    // wet bus is more than kCompMaxOverInputDb above the input (and the input is above
    // kCompMinInputPower), pulls the wet bus down towards that limit with a 1 s gain slew. Gain is
    // exactly 1.0 otherwise, so t = 0 stays bit-exact even with the compensator enabled.
    void  setLevelCompensatorEnabled (bool enabled) noexcept { levelCompEnabled = enabled; }
    bool  isLevelCompensatorEnabled() const noexcept         { return levelCompEnabled; }
    float getLevelCompensatorGain() const noexcept           { return compGain; }

    static constexpr float kCompMaxOverInputDb = 6.0f;
    static constexpr float kCompMinInputPower  = 1.0e-5f;    // mean-square, about -50 dBFS RMS

private:
    void processSegment (float* out0, float* out1, int n);
    void controlTick();
    void runSeriesChain (int pos, int count) noexcept;
    void resetSeriesModules() noexcept;
    void resetSendModules() noexcept;
    void resetFeedbackModules() noexcept;
    void runWetSources (int pos, int count) noexcept;
    void zeroWetSources() noexcept;
    void fillLfoBuffers (int count) noexcept;
    bool sendsEnabled() const noexcept { return stubProcGain == 1.0f && seriesChainEnabled; }

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

    // Phase 3.2 series modules
    LFOEngine        lfoA, lfoB;               // A: amplitude + filter | B: stereo (+ delay later)
    FilterEngine     filter;
    SaturationEngine saturation;
    StereoWidth      stereoWidth;
    Smoother         ampDepthSm;               // 50 ms
    Smoother         inputTrimSm;              // 50 ms (linear gain)
    std::array<float, controlBlockSize> lfoBBuf {};     // stereo-destination LFO (per sample)
    std::array<float, controlBlockSize> lfoABuf {};     // amplitude-destination LFO (per sample, slewed)
    std::array<float, controlBlockSize> lfoDBuf {};     // delay-destination LFO (per sample)

    // Phase 3.4: rise-mode crossfade + per-destination LFO shape mixes
    TensionDSP::ModeBlender    modeBlender;
    TensionDSP::TensionTargets lastTargets;
    TensionDSP::ShapeMix       ampMix, filterMix, stereoMix, delayMix;
    float ampLfoState = 0.0f, ampLfoCoef = 0.01f;           // 1.5 ms one-pole, per sample
    float filterLfoState = 0.0f, filterLfoCoef = 0.1f;      // 4 ms one-pole, per control tick

    // Phase 3.3 wet-bus sources
    NoiseGenerator noise;
    ReverbEngine   reverb;
    DelayEngine    delay;
    std::array<float, controlBlockSize> nzL {}, nzR {}, dlL {}, dlR {}, rvL {}, rvR {};
    bool sendsCleared = true;      // delay / reverb / noise filters already reset during the current rest period

    // Phase 3.5 optional level compensator (default OFF)
    bool  levelCompEnabled = false;
    float compInPow = 0.0f, compWetPow = 0.0f;       // 1 s one-pole mean-square trackers (per channel average)
    float compGain = 1.0f;                           // slewed (1 s) wet-bus gain, exactly 1.0 when idle
    float compPowCoef = 0.0001f, compGainCoef = 0.001f;

    TensionDSP::RiseMode riseMode = TensionDSP::RiseMode::Cinematic;
    HostTransport        transport;
    bool                 syncEnabled = true;
    bool                 prevPlaying = false;

    bool  firstBlock = true;
    bool  snapModules = true;       // next controlTick snaps the module smoothers (first block / at rest)
    bool  seriesCleared = false;    // module states already reset during the current rest period
    bool  seriesChainEnabled = true;
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
