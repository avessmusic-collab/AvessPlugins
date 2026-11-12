#pragma once
/*
  FilterStage - the per-voice stereo filter stage (architecture C9 / section G), phase D5a: types 0-19.

  Position in the voice chain:  decimator -> voice DC blocker -> FilterStage -> amp ADSR.

  What one process() call does for a chunk (<= 32 base samples, any length works):

      dry   = input
      wetIn = drive (dry)                      ADAA1 tanh, unity small-signal gain (see below)
      wet   = filter (wetIn)                   TPT sections, coefficients re-derived every 8 samples
      out   = dry + (wet - dry) * mix * enable

  Parameter smoothing (per chunk, like the warp / shaper stages):
      cutoff       one-pole in the LOG domain, 15 ms; the 8-sample sub-blocks interpolate the log frequency, so a
                   sweep is exponential (equal steps per octave) and the TPT coefficient g = tan(pi fc / fs) is
                   recomputed from it (std::tan in double: 8 times per 32 samples is cheap, and the display
                   uses the identical function)
      res/drive/morph/mix   one-pole 10 ms (linear domain), interpolated per sub-block / per sample for the mix
      enable       5 ms linear ramp of the wet/dry mix (never a hard switch)

  Drive: y = x + (tanh(P x) / P - x) * min(1, dB),  P = 10^(dB/20), evaluated with first-order ADAA
  (antiderivative ln cosh(P x) / P^2).  tanh(P x) / P is the saturator normalised by its small-signal gain
  (P tanh'(0)), so quiet material passes unchanged at any drive and only peaks are rounded; the min(1, dB) blend
  makes the stage continuous at 0 dB, where it is skipped entirely (bit-exact identity).

  Type switch (rate class D): the voice owns TWO FilterCore instances.  A change of `type` starts a 10 ms linear
  crossfade from the running core (its design keeps following the live parameters) to a freshly reset core of the new
  type.  The new core is fed the input for 64 samples at weight 0 first ("input-primed"), so its state is warm when it
  becomes audible; the total fade is therefore 64 + 480 samples at 48 kHz.  DiscreteSwitchFade does the bookkeeping.
  A second switch before the first fade ends supersedes it (documented limitation of DiscreteSwitchFade).

  Bypass: when the effective mix is exactly 0 at both ends of a chunk (filterEnable off, filterMix 0 or the "open
  low-pass" convention of FilterDesign.h) the samples are not touched at all - the output is bit-identical to the
  input - and the cores are parked (state cleared) so re-enabling starts clean behind the 5 ms ramp.

  D5b (types 20-27: combs, all-passes, formant / vowel banks, phaser) ride the same machinery: a comb core owns its
  delay lines (pre-allocated in prepare()), a type switch resets the NEW core (its line is silent and is primed with the
  input for 64 samples like every other core), filterEnable off / mix 0 are the same bit-exact bypass, and the "open
  low-pass" rule does not apply to them (it is limited to types 0-5).

  Types >= 28 are not implemented yet (D5c): their design is the identity, so the stage passes the signal through
  unchanged while still honouring drive and mix.

  Safety: a non-finite or runaway (|y| >= 1e6) result replaces that sample by the dry sample and clears all filter
  state; a stable TPT filter never triggers it, it only guards against a poisoned input.

  Real-time: fixed-size members only, no allocation, no locks.
*/

#include "DSP/DiscreteSwitchFade.h"
#include "DSP/FilterDesign.h"

#include <array>
#include <vector>

namespace warpr
{
    /** Raw (unsmoothed) per-chunk targets.  `cutoffHz` already includes key tracking and any modulation. */
    struct FilterTargets
    {
        bool  enable = true;
        int   type = 3;
        float cutoffHz = 20000.0f;
        float res = 0.0f;
        float driveDb = 0.0f;
        float morph = 0.0f;
        float mix = 1.0f;
    };

    /** One stereo instance of a design: the TPT state of up to 16 sections per channel, the phaser feedback sample and
        (D5b) the comb delay lines.  The delay lines are allocated ONCE in prepare() at their maximum size (fs / 20 Hz);
        nothing in the audio path allocates. */
    class FilterCore
    {
    public:
        /** Allocates the comb delay lines for `sampleRate` (NOT real-time safe; no-op when the size is unchanged). */
        void prepare (double sampleRate);

        /** Clears all state.  The delay lines are only wiped when a comb actually wrote to them. */
        void reset() noexcept;

        /** Takes the design for the next process() calls.  Comb delay changes ramp linearly from the previous design's
            delay to the new one over the next call (per-sample interpolation, no zipper). */
        void setDesign (const FilterDesign& d) noexcept;

        /** Filters `n` samples of channel `ch` (0 / 1) in double precision. */
        void process (int ch, const double* in, double* out, int n) noexcept;

    private:
        double runChain (std::size_t c, double x) noexcept;

        FilterDesign design;
        std::array<std::array<double, static_cast<std::size_t> (FilterDesign::maxSections)>, 2> ic1 {}, ic2 {};
        std::array<double, 2> fbState {};                      // phaser: previous chain output

        // comb
        std::array<std::vector<double>, 2> line;               // y history, circular
        std::array<int, 2> writePos {};
        std::array<double, 2> lpState {};
        double delayStart = 2.0, delayEnd = 2.0;
        bool haveDelay = false;
        bool lineDirty = false;
    };

    class FilterStage
    {
    public:
        static constexpr int subBlock     = 8;      // coefficient update interval (samples)
        static constexpr int primeSamples = 64;     // new core runs silently this long before the fade starts

        void prepare (double newSampleRate) noexcept;

        /** Clears all audio state (cores, ADAA history, fade).  Keeps the smoothed parameter values. */
        void reset() noexcept;

        /** Voice start: adopts the targets as the current values (no smoothing, no fade) and clears the state. */
        void snap (const FilterTargets& t) noexcept;

        /** Processes `n` samples of both channels in place. */
        void process (float* left, float* right, int n, const FilterTargets& t) noexcept;

        // diagnostics / tests
        bool isFading() const noexcept       { return fade.fading(); }
        bool isParked() const noexcept       { return parked; }
        int  currentType() const noexcept    { return fade.current(); }
        int  fadeLengthSamples() const noexcept { return fadeTotal; }

    private:
        void adoptSilently (int type) noexcept;

        double sampleRate = 48000.0;

        std::array<FilterCore, 2> cores {};
        std::array<int, 2> coreType { { -1, -1 } };
        int current = 0;                              // index of the core of the CURRENT type
        DiscreteSwitchFade fade;
        int fadeTotal = primeSamples + 480;
        bool parked = true;

        // smoothed values
        double cutLogCur = 9.9035;                    // ln (20000)
        double resCur = 0.0, driveCur = 0.0, morphCur = 0.0, mixCur = 1.0, enCur = 1.0;

        std::array<double, 2> adaaPrev {};            // previous drive-stage input per channel
        FilterDesign scratchCurrent, scratchOutgoing;
    };
}
