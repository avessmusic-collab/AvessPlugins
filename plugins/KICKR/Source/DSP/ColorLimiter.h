#pragma once

#include <vector>
#include <cstdint>

#include <juce_core/juce_core.h>

#include "Utilities/DSPUtils.h"

namespace kickr
{
    /**
        2026-09-02 (user request: "change the limiter to an identical to abletons colour
        limiter") — replaces the one-button -0.5 dBFS tanh soft-clip with a limiter that
        has Ableton Color Limiter's control set and behaviour:

          LOUDNESS   input gain, 0..+24 dB                 (default 0 dB)
          CEILING    maximum output level, -24..0 dB       (default -0.5 dB — the old ceiling)
          LOOKAHEAD  how far ahead peaks are seen, 0.1..10 ms (default 1.5 ms)
          RELEASE    gain-recovery time, 1..1000 ms         (default 50 ms)
          SATURATION amount of pre-limiter saturation, 0..1 (default 0 = none, exactly transparent)
          COLOR      tonal character of that saturation, 0..1 (default 0.5 = neutral):
                     low = warm / round (the lows drive the shaper), high = bright / crunchy
                     (the highs drive it).

        Ableton's algorithm is proprietary Max for Live code; this is a from-scratch limiter
        with the same controls. Signal path, per sample, stereo, in-region (fsOversampled):

          1. loudness gain (smoothed)
          2. saturation: a peak-normalised tanh shaper (drive 1 + 9*sat) generates the
             harmonics; COLOR then tilts ONLY the generated part (shaped - dry) with a
             +/-8 dB shelf tilt pivoting at 500 Hz — low = warm/round (harmonics kept
             below the pivot, the ones above cut), high = bright/crunchy (the reverse) —
             before it is mixed back in at SAT. Drive doesn't change with COLOR, so on a
             kick (nearly all energy below the pivot) COLOR is purely a tone control for
             the saturation, never a "less saturation" control. SAT = 0 is bit-exact
             pass-through.
          3. look-ahead brickwall gain computer, stereo-linked: required gain
             g = min (1, ceiling / |peak|) per input sample -> sliding-window MINIMUM over the
             look-ahead window (monotonic deque, O(1) amortised, preallocated) -> one-pole
             attack (time constant = lookahead / 3) / RELEASE envelope, applied to the
             signal delayed by exactly the look-ahead. So the gain is already down when the
             peak reaches the output.
          4. hard clamp at the ceiling as the final guarantee (catches the few-percent
             smoothing residual and anything the envelope can't, e.g. a release shorter
             than the look-ahead). A clamp, not the old tanh: the look-ahead brickwall does
             the limiting now, so the last stage should add nothing of its own — otherwise
             its harmonics mask COLOR.

        Latency = the look-ahead (reported to the host by the processor). `enabled == false`
        bypasses everything INCLUDING the delay (latency 0), like Live's device.
        RT-safe: all buffers sized in prepare() for the 10 ms maximum; no allocation after.
    */
    class ColorLimiter
    {
    public:
        struct Params
        {
            bool  enabled      { true };
            float loudnessDb   { 0.0f };
            float ceilingDb    { -0.5f };
            float lookaheadMs  { 1.5f };
            float releaseMs    { 50.0f };
            float saturation01 { 0.0f };
            float color01      { 0.5f };
        };

        static constexpr float kMaxLookaheadMs = 10.0f;
        static constexpr float kMinLookaheadMs = 0.1f;
        static constexpr float kColorTiltDb    = 8.0f;      // +/- tilt at the COLOR extremes
        static constexpr float kMaxDrive       = 4.0f;      // shaper drive at SAT = 1 (1 + 3): up to +12 dB of small-signal lift
        static constexpr float kSplitHz        = 500.0f;    // COLOR tilt pivot
        static constexpr int   kMaxDelay       = 15362;     // 10 ms @ 8 x 192 kHz + 2 — fixed, so OS switches never allocate

        void prepare (double fsOversampled);   // allocates ONCE for the 10 ms @ 8x192k worst case (prepareToPlay only)
        void reset() noexcept;
        void updateOversampledRate (double newFsOversampled) noexcept;   // coefficient-only, no allocation (audio-thread OS switch)

        void setParams (const Params& p) noexcept;

        /** Per sample, in place. */
        void process (float& l, float& r) noexcept;

        /** Current look-ahead delay in (oversampled) samples — 0 when disabled. */
        int  latencySamples() const noexcept { return enabled ? lookaheadSamples : 0; }

        static int lookaheadSamplesFor (float lookaheadMs, double fs) noexcept
        {
            return juce::jmax (1, static_cast<int> (std::lround (juce::jlimit (kMinLookaheadMs, kMaxLookaheadMs, lookaheadMs) * 0.001 * fs)));
        }

    private:
        float saturate (float x, int ch) noexcept;
        void  pushPeak (float gReq) noexcept;   // sliding-window minimum

        double fs { 44100.0 };
        bool   enabled { true };

        // params -> derived
        int   lookaheadSamples { 66 };
        float lookaheadMsSet   { 1.5f };
        float releaseMsSet     { 50.0f };
        float colorSet         { 0.5f };
        float attackCoef  { 0.05f };
        float releaseCoef { 0.0005f };
        float sat         { 0.0f };
        float drive       { 1.0f };
        float driveNorm   { 1.0f };      // 1 / tanh (drive)
        float tiltHi      { 1.0f };      // COLOR: high-band pre-gain (low band gets 1/tiltHi)
        float splitCoef   { 0.13f };     // one-pole LP coefficient @ kSplitHz
        juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> loudGain { 1.0f };
        juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> ceilSm   { 0.944060876f };

        // COLOR tilt: one-pole split of the generated-harmonics residual, per channel
        float lpPost[2] { 0.0f, 0.0f };

        // look-ahead delay (ring), one per channel
        std::vector<float> delayL, delayR;
        std::vector<float> switchScratch;   // 2 x kMaxDelay, for the OS-switch resample (allocated in prepare)
        int delayCap { 0 };
        int writePos { 0 };

        // sliding-window minimum of the required gain (monotonic deque on a ring)
        std::vector<float>   dqVal;
        std::vector<int64_t> dqIdx;
        int     dqCap   { 0 };
        int     dqFront { 0 };
        int     dqSize  { 0 };
        int64_t counter { 0 };

        float env { 1.0f };   // applied gain envelope

        // OS-switch junction fade (see updateOversampledRate): the resampled held content
        // and the first new-rate samples meet `lookahead` samples after the switch — past
        // the engine's own 64-sample fade — so the limiter fades that seam itself.
        int heldRemaining { 0 };
        int fadeInRemaining { 0 };
        int junctionFade { 0 };
    };
}
