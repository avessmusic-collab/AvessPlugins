// CHOPLINE — MasterFX (spec §7; locked v17 master sections)
// The master chain AFTER Beat Repeat, in the user-decided order (2026-10-25):
//
//   FILTER (12 dB/oct state-variable LP/BP/HP — master-only per the locked v17)
//   → DISTORTION (6 types, 2× oversampled via juce::dsp::Oversampling IIR
//     polyphase; Downsample intentionally NOT oversampled — aliasing intended;
//     drive / mix / auto-gain)
//   → DELAY + REVERB RETURNS (tempo-synced delay and a small plate — fed by
//     the per-chop DELAY/VERB sends tapped post-voice, pre-master)
//   → WIDTH (mid/side 0–200 % + elliptical bass mono below a set frequency)
//   → OUTPUT gain → transparent safety limiter (clip-guarded instant-attack /
//     smooth-release gain computer — NOT juce::dsp::Limiter, which hides a
//     fixed 4:1 compressor stage; below the ceiling the path is bit-exact).
//
// Zero added latency by design: this is a performance instrument (Beat Repeat
// HOLD is played live), so the oversampler is IIR polyphase (sub-sample group
// delay) and the limiter has no lookahead. The rounded oversampler latency is
// reported through getLatencySamples() and never changes while playing.
//
// Every audible parameter is smoothed (10–50 ms); per-section bypass crossfades
// over ~15 ms; a fully-bypassed section is skipped and bit-transparent.
// RT safety: everything allocated in prepare(); process() never allocates.

#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>

#include <atomic>
#include <vector>

namespace chopline::engine
{

/// All values map 1:1 to the APVTS parameters (filled by the processor each
/// block). Defaults keep the whole chain bypassed and bit-transparent, so
/// engine tests of earlier phases are unaffected unless they opt in.
struct MasterParams
{
    bool filterOn = false;
    int filterType = 0;        // 0 LP, 1 BP, 2 HP (12 dB/oct)
    float cutoffHz = 20000.0f; // 20–20000
    float resoPct = 10.0f;     // 0–100

    bool distOn = false;
    int distType = 2;          // 0 Soft, 1 Hard, 2 Tape, 3 Fold, 4 Bitcrush, 5 Downsample
    float drivePct = 10.0f;
    float mixPct = 100.0f;
    bool autoGain = true;

    bool delayOn = false;
    int delayTimeIndex = 2;    // v17 options (1/16 … 1/4T), default 1/8.
    float delayFbPct = 35.0f;  // 0–95
    float delayRtnPct = 0.0f;

    bool verbOn = false;
    float verbDecayPct = 40.0f;
    float verbDampPct = 50.0f;
    float verbRtnPct = 0.0f;

    bool widthOn = false;
    float widthPct = 100.0f;   // 0–200
    float monoHz = 120.0f;     // 20–300 (≤ 20 disables the bass-mono filter)

    bool outOn = false;        // OUTPUT section (gain + limiter) bypass
    float outDb = 0.0f;        // −24…+12
    float ceilingDb = -0.3f;   // −12…0

    // Phase 5 Eco quality: run Soft clip / Tape at BASE rate (low-order smooth
    // saturators — aliasing negligible at musical drives). Hard clip and Fold
    // keep the 2× oversampling even in Eco (audible aliasing otherwise);
    // Bitcrush/Downsample are base-rate by design.
    bool ecoNoOversample = false;
};

class MasterFX final
{
public:
    MasterFX() = default;

    // ---- shared shaping (engine + UI transfer-curve display + tests) ------------
    /// Drive gain for the waveshaper types (up to ~+24 dB).
    static float driveGain (float drive01) noexcept
    {
        return 1.0f + 15.0f * juce::jlimit (0.0f, 1.0f, drive01);
    }

    /// Waveshaper for types 0–4 (Downsample is a rate reducer, not a shaper).
    static float shapeSample (int type, float x, float drive01) noexcept;

    /// Auto-gain compensation factor (multiplied into the shaped signal).
    static float autoGainFor (int type, float drive01) noexcept;

    // ---- lifecycle (message thread) ------------------------------------------------
    void prepare (double sampleRate, int maxBlockSize);
    void reset() noexcept;

    /// Rounded, constant total latency (IIR polyphase oversampler ≈ 0).
    int getLatencySamples() const noexcept { return latencySamples; }

    // ---- audio thread ------------------------------------------------------------------
    /// In-place on `main`. `delaySend` / `verbSend` are the per-chop send buses
    /// (post-voice, pre-master); their returns mix in after the distortion.
    void process (juce::AudioBuffer<float>& main,
                  const juce::AudioBuffer<float>& delaySend,
                  const juce::AudioBuffer<float>& verbSend,
                  const MasterParams& params, double bpm) noexcept;

    // ---- UI feedback (message thread, lock-free) -------------------------------------
    float getGainReductionDb() const noexcept { return grDb.load (std::memory_order_relaxed); }
    float getOutPeak() const noexcept { return outPeak.load (std::memory_order_relaxed); }

private:
    void processFilter (juce::AudioBuffer<float>& main, const MasterParams& p, int n) noexcept;
    void processDistortion (juce::AudioBuffer<float>& main, const MasterParams& p, int n) noexcept;
    void processDelay (juce::AudioBuffer<float>& main, const juce::AudioBuffer<float>& sendBus,
                       const MasterParams& p, double bpm, int n) noexcept;
    void processReverb (juce::AudioBuffer<float>& main, const juce::AudioBuffer<float>& sendBus,
                        const MasterParams& p, int n) noexcept;
    void processWidth (juce::AudioBuffer<float>& main, const MasterParams& p, int n) noexcept;
    void processOutput (juce::AudioBuffer<float>& main, const MasterParams& p, int n) noexcept;

    double rate = 48000.0;
    int maxBlock = 512;
    int latencySamples = 0;
    bool snapSmoothers = true;

    using Smoothed = juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear>;

    // Filter.
    juce::dsp::StateVariableTPTFilter<float> svf;
    int svfType = -1;
    Smoothed fOn, cutoff, reso;
    juce::AudioBuffer<float> fScratch;

    // Distortion.
    std::unique_ptr<juce::dsp::Oversampling<float>> oversampler;
    Smoothed dOn, drive, distMix;
    juce::AudioBuffer<float> dScratch;
    float dsHoldL = 0.0f, dsHoldR = 0.0f; // Downsample sample-and-hold state
    int dsCounter = 0;

    // Delay.
    std::vector<float> delayRingL, delayRingR;
    int delayLen = 0, delayWrite = 0;
    bool delayWasActive = false; // idle skip; clean ring on re-activation
    Smoothed delayTime, delayFb, delayRtn;

    // Reverb.
    juce::Reverb reverb;
    bool reverbWasActive = false;
    float verbRtnCurrent = 0.0f; // per-sample ramped return level
    float lastVerbDecay = -1.0f, lastVerbDamp = -1.0f;
    juce::AudioBuffer<float> vScratch;

    // Width.
    Smoothed wOn, width, monoFreq;
    double wIc1 = 0.0, wIc2 = 0.0; // bass-mono HP state (side channel)
    double wK = 1.414;

    // Output + limiter.
    Smoothed oOn, outGain, ceiling;
    float limEnv = 1.0f;
    float limRelease = 0.0f;

    std::atomic<float> grDb { 0.0f };
    std::atomic<float> outPeak { 0.0f };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MasterFX)
};

} // namespace chopline::engine
