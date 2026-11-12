#pragma once
/*
  GrainEngine - per-oscillator granular source (architecture C7), phases D7a (CLASSIC + LOOP) and D7b (CLOUD +
  per-grain phase warps).

  WHERE IT SITS.  A granular voice replaces the Classic sample read at the very top of the oscillator
  (UnisonOsc::render).  The grain pool is rendered at S = min(OS, 2) times the host rate (architecture:
  "granular source rate"), up-sampled with the same half-band all-pass design as the voice decimator to the
  OS rate, and then fed into the SAME warp -> shaper -> pan -> decimator -> DC block -> filter -> ADSR chain as
  the Classic read.  Amplitude-domain warp modes and the shaper therefore work on grains unchanged.

  PER-GRAIN PHASE WARPS (D7b).  The phase-domain warps (kind Phase in WarpModeTable: today the H family 0, 1, 2, 3,
  7, 9, 14) move the READ position, which only a grain has.  So the engine evaluates them itself, inside the grain
  loop: every grain head (left / right) carries its own cycle clock `theta` (seeded at spawn from the head's start
  position exactly like UnisonOsc::start seeds the Classic clock, advanced by step / Lc per grain-rate sample), and
  reads the sample at  fold(P + dir * Lc * Delta(theta))  with phasewarp::delta - the same function the Classic
  oscillator uses.  warpMix is applied per grain (dry/wet of the head value), the Mirror SeamBridge is per head,
  and a warp-mode change cross-fades old and new offset over 10 ms (engine level, frozen old parameters).  The
  oscillator-level warp stage still runs on top for the amplitude-domain modes (it sees the grain value, so a phase
  mode is an identity there, never applied twice).

  D6.  Every implemented phase-kind row is applied per grain now: the map modes 44-53 (PhaseWarp.h evalMap: up to two
  read taps per head, an amplitude window, Soft Sync's parity) and the modulation kernels 54, 55, 58, 59 (PhaseKernel.h).
  The per-grain STATE lives in the head: its cycle clock theta and parity, a ModState (modulator phase, Sample-source
  follower / integrator, Self FM loop, Cross Mod previous value) and a second ModState for the outgoing mode while the
  10 ms mode cross-fade runs.  Each head is its own channel, so Self FM / Cross Mod close their loop per head (the
  architecture's "per channel").  The Sample-source modulator follows the head: P_m = lo + ratio (P_head - lo), read
  through the same reader as the grain with its own speed multiplier (ratio x grain ratio).  AM / RM (amplitude kind)
  stay with the oscillator-level stage; its Sample modulator follows the oscillator's virtual playhead.

  SPEED-AWARE MIP SELECTION.  Every read the engine asks of the oscillator carries `mult`: how much faster than the
  chunk's nominal speed it travels (the grain's own ratio - Cloud transposition and spread detune - times the measured
  speed of the warped read).  The oscillator picks the mip level for rho x mult (SampleReader.h, ReadParamsCache), so a
  grain transposed up by an octave or sped up by a phase warp no longer reads folded content.

  GRAINS.  One Grain is a STEREO pair: a left and a right read head with their own detune (+-30 cents x
  grainSpread) and start offset (<= size/4 x grainSpread) but one shared window and one shared random draw.
  With grainSpread == 0 both heads read the same position, so a mono sample stays bit-identical in L and R.
  Positions are source frames; every grain READS through the oscillator's own readAt (interpolation, mip
  level, forward-loop seam crossfade, one-shot end fade) so grains obey loopMode exactly like Classic.
  Everything the grain needs (position, size, shape, variation draw, direction) is LATCHED when it is
  spawned, so no parameter can click a sounding grain; only the pitch (step) follows live.

  SCHEDULERS (parameter-spec: Classic = 2 overlapping grains, Loop = crossfading playback)
    CLASSIC  hop = size / 2, window = attack fraction a = shape (clamped >= 0.5 ms), release 1 - a, raised-cosine
             ramps (shape < 0.5 bends the release toward an exponential decay, shape > 0.5 bends the attack
             toward an exponential rise).  shape 0.5 is a Hann window.  Two grains always overlap and, because
             a + r = 1 with raised-cosine ramps, the windows of a hop-spaced train sum to exactly 1.
    LOOP     hop = size * (1 - x), x = clamp(shape / 2, 0.5 ms / size, 1/2): flat-top window with equal-power
             (quarter-sine) ramps of length x * size - a looped segment with a crossfade.
    CLOUD    (D7b) grainDensity N (1..20) = number of simultaneous UNSYNCED grains per channel.  There is no hop: the
             scheduler keeps N grains alive.  A note / scheduler switch starts with N grains of uniformly random
             age ("running forever"); afterwards every grain RESPAWNS WHEN IT ENDS (at scanPos + variation, size /
             transposition / volume jitter) after a short random stagger (about size / (4 N) x (0.5 .. 1.5)) that
             also spreads grains added by a density increase.  A density decrease simply stops respawning (the extra
             grains finish naturally).  Fixed Hann window (grainShape ignored), per-grain gain sqrt(2 / N) = the
             architecture's 1 / sqrt(0.5 N): uncorrelated material comes out at the same -1.25 dB as Classic for
             every N.  Variation also transposes a Cloud grain by +-12 v semitones.
  A grain train is started "as if it had been running forever": the first call spawns the grain one hop old
  AND a fresh one, so the level is steady from the first sample (the amp envelope owns the note attack).

  LEVEL NORMALISATION.  Window gain = 1 / (2 mean(window)), i.e. the mean overlap-add of a hop-spaced train is
  exactly 1 (a cosine-ramp window has mean 1/2 whatever its shape, so the gain is 1; the exponential ramps are
  corrected from the table mean, clamped to 0.5 .. 2).  This keeps COHERENT material (a Classic grain whose
  hop is a whole number of cycles) bit-faithful to Classic playback.  Uncorrelated material (noise) comes out
  sqrt(2 * mean(w^2)) = 0.866 (-1.25 dB) for any size, which is what the normalisation gate measures.

  SCAN.  scanPhase runs 0 -> 1 over D = 20 s * 0.0004^scan (scan < 0.001: off); SCAN KEY divides D by
  2^((pitch - 60) / 12).  Loop: the phase wraps; Off: it holds at 1.  A grain spawned at grain-rate sample n
  starts at norm = pos + (clamp(pos + dist, 0, 1) - pos) * scanPhase(n) (+ variation), scanPhase(n) = n / (D * rate).

  VARIATION v (one master amount, fixed internal weights): start position +- v * 2 s (source time), size
  x 2^(+-0.5 v), volume 0 .. -6 dB x v, Cloud only: transposition +-12 v semitones.  Four random numbers are drawn
  per Classic / Loop spawn whatever v is (D7a stream, unchanged), SIX per Cloud spawn (the two extra: transposition
  and the random pre-age used by the start-up fill) plus one stagger draw per scheduler-driven Cloud respawn, so
  the stream never depends on the parameters, only on the scheduler.  PRNG: per-oscillator xorshift32 seeded from
  the note counter.

  SWITCHES.  playMode (Classic <-> Granular) is a 10 ms weight ramp owned by the voice/oscillator.  A scheduler
  change (grainMode) lets every sounding grain fade out over 10 ms while the new scheduler pre-rolls and fades in
  over 10 ms.  A window can never have a ramp shorter than 0.5 ms (or half the grain).

  Real-time rules: fixed pool (poolSize grains, a spawn with no free slot is dropped and counted), no
  allocation, no locks.  The window table is built by GrainWindowTable::get() (WarprSynth::prepare calls it).
  Pool: 24 stereo grains per oscillator = the 20 of a full Cloud + 4 of headroom so that the 10 ms fade-out of
  the old scheduler's grains and the pre-roll of the new one can overlap without dropping spawns.
*/

#include "DSP/HalfbandIIR.h"
#include "DSP/ModOsc.h"
#include "DSP/PhaseKernel.h"
#include "DSP/PhaseWarp.h"
#include "DSP/RegionFold.h"
#include "DSP/SampleData.h"
#include "DSP/WarpModeTable.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace warpr
{
    //==========================================================================
    /** Rising ramps 0 -> 1 on a 1024-interval grid (+2 guard entries). */
    struct GrainWindowTable
    {
        static constexpr int size = 1024;
        using Ramp = std::array<float, static_cast<std::size_t> (size + 2)>;

        Ramp cosine {}, quarterSine {}, expo {};
        double meanExpo = 0.2;

        static const GrainWindowTable& get()
        {
            static const GrainWindowTable instance;
            return instance;
        }

    private:
        GrainWindowTable()
        {
            const double pi = 3.14159265358979323846;
            const double e5 = std::exp (5.0) - 1.0;

            for (int i = 0; i < size + 2; ++i)
            {
                const double x = std::min (1.0, (double) i / (double) size);
                const auto k = static_cast<std::size_t> (i);
                cosine[k] = (float) (0.5 - 0.5 * std::cos (pi * x));
                quarterSine[k] = (float) std::sin (0.5 * pi * x);
                expo[k] = (float) ((std::exp (5.0 * x) - 1.0) / e5);
            }

            meanExpo = 0.2 - 1.0 / e5;                 // integral of (e^{5x} - 1) / (e^5 - 1) over [0, 1]
        }
    };

    inline float lookupRamp (const GrainWindowTable::Ramp& t, float x) noexcept
    {
        const float p = std::min (1.0f, std::max (0.0f, x)) * (float) GrainWindowTable::size;
        const int i = (int) p;
        const float f = p - (float) i;
        const auto k = static_cast<std::size_t> (i);
        return t[k] + f * (t[k + 1] - t[k]);
    }

    //==========================================================================
    /** One grain window: attack fraction a, release fraction r (a + r <= 1, flat top between). */
    struct GrainWindow
    {
        float a = 0.5f, r = 0.5f;
        float kA = 0.0f, kR = 0.0f;          // exponential-curve blend of the attack / release ramps
        bool  quarterSine = false;           // Loop crossfade ramps (equal power)
        float gain = 1.0f;                   // level normalisation

        float rise (const GrainWindowTable& t, float x, float k) const noexcept
        {
            if (quarterSine)
                return lookupRamp (t.quarterSine, x);

            const float c = lookupRamp (t.cosine, x);
            return k > 0.0f ? c + k * (lookupRamp (t.expo, x) - c) : c;
        }

        /** u in [0, 1]. */
        float at (const GrainWindowTable& t, float u) const noexcept
        {
            if (u < a)
                return rise (t, u / a, kA);

            const float v = 1.0f - u;

            if (v < r)
                return rise (t, v / r, kR);

            return 1.0f;
        }
    };

    //==========================================================================
    /** Test-only observer of spawns (nullptr in production). */
    struct GrainSpawnLog
    {
        static constexpr int capacity = 512;

        struct Entry
        {
            std::int64_t sample = 0;          // grain-rate sample index of the spawn (0 for the pre-roll pair)
            double startNorm = 0.0;           // start position as a fraction of the region (after scan + variation)
            double scanPhase = 0.0;           // scan phase at the spawn
            int    len = 0;                   // grain length, grain-rate samples
            bool   reversed = false;
            bool   preAged = false;
            double ratio = 1.0;               // Cloud transposition jitter (1 for Classic / Loop), before the spread detune
        };

        std::array<Entry, static_cast<std::size_t> (capacity)> entries {};
        int count = 0;
    };

    /** Per-voice scratch for the grain-rate / OS-rate staging (32 base samples x 8). */
    struct GrainScratch
    {
        static constexpr int capacity = 256;
        std::array<float, static_cast<std::size_t> (capacity)> aL {}, aR {}, bL {}, bR {};
    };

    /** The warp stage as the grains need it (D7b).  Filled by the voice from its WarpSpan; the amounts ramp linearly
        across the span exactly like the oscillator-level stage.  `active` false (or Lc <= 0): no per-grain warp. */
    struct GrainWarp
    {
        bool   active = false;
        int    mode = 17;
        float  a0 = 0.0f,  a1 = 0.0f;
        float  A0 = 0.5f,  A1 = 0.5f;
        float  B0 = 0.0f,  B1 = 0.0f;
        float  mix0 = 1.0f, mix1 = 1.0f;       // warpMix * enable ramp
        double Lc = 0.0;                       // source frames per root-note cycle (0 = unknown: no cycle clock)

        // D6
        int    modSource = 4;                  // modOscSource 0..4
        float  ratio0 = 1.0f, ratio1 = 1.0f;   // modOscRatio ramp
        const ShaperLutSet* curve = nullptr;   // Phase Remapping curves (nullptr = identity)
        bool   blep = true;                    // Hard Sync polyBLEP
    };

    /** Per-chunk description of the granular source (built by the voice). */
    struct GrainSpan
    {
        bool   active = false;                 // false: the engine is idle (Classic only)
        float  w0 = 0.0f, w1 = 0.0f;           // granular share (playMode crossfade) at the start / end of the span
        int    mode = 0;                       // grainMode: 0 Classic, 1 Loop, 2 Cloud
        GrainWarp warp;                        // per-grain phase warp (D7b)
        float  position = 0.0f, scan = 0.0f, scanDist = 0.0f;
        bool   scanLoop = true, scanKey = false;
        float  sizeMs = 80.0f, shape = 0.5f, variation = 0.0f, spread = 0.0f, reverse = 0.0f;
        int    density = 8;                    // Cloud only: simultaneous grains per channel, 1..20
        bool   reverseDir = false;             // direction parameter
        double pitchSemis = 60.0;              // glide-smoothed note pitch (SCAN KEY)
        double sampleRate = 44100.0;           // host rate
        const HalfbandCoeffs* hb = nullptr;    // up-sampler coefficients
        GrainScratch* scratch = nullptr;
        GrainSpawnLog* log = nullptr;
    };

    struct GrainOut
    {
        const float* l = nullptr;
        const float* r = nullptr;
    };

    //==========================================================================
    /** One half-band interpolation stage (x2), mirror image of decimateStage: the direct path feeds the even
        outputs, the delayed path the odd ones (unity DC gain).  Not in place: out holds 2 * numIn samples. */
    inline void upsampleStage (const HalfbandCoeffs& c, HalfbandStageState& s,
                               const float* inL, const float* inR, float* outL, float* outR, int numIn) noexcept
    {
        const int total = c.numStages;
        const int direct = c.directStages;

        for (int i = 0; i < numIn; ++i)
        {
            float xl = inL[i], xr = inR[i];

            for (int n = 0; n < direct; ++n)
            {
                const float a = c.alpha[static_cast<std::size_t> (n)];
                const auto k = static_cast<std::size_t> (n);
                const float oL = a * xl + s.vL[k];
                const float oR = a * xr + s.vR[k];
                s.vL[k] = xl - a * oL;
                s.vR[k] = xr - a * oR;
                xl = oL;
                xr = oR;
            }

            outL[2 * i] = xl;
            outR[2 * i] = xr;

            xl = inL[i];
            xr = inR[i];

            for (int n = direct; n < total; ++n)
            {
                const float a = c.alpha[static_cast<std::size_t> (n)];
                const auto k = static_cast<std::size_t> (n);
                const float oL = a * xl + s.vL[k];
                const float oR = a * xr + s.vR[k];
                s.vL[k] = xl - a * oL;
                s.vR[k] = xr - a * oR;
                xl = oL;
                xr = oR;
            }

            outL[2 * i + 1] = xl;
            outR[2 * i + 1] = xr;
        }
    }

    //==========================================================================
    class GrainEngine
    {
    public:
        static constexpr int maxCloudGrains = 20;          // grainDensity maximum (simultaneous grains per channel)
        static constexpr int poolSize = 24;                // stereo grains per oscillator: 20 Cloud + 4 switch-overlap headroom
        static constexpr double fadeSeconds = 0.010;       // scheduler-switch fade

        //----------------------------------------------------------------------
        // pure helpers (static, unit-tested)

        /** Window of a grain of `sizeSec` seconds.  Ramps are never shorter than 0.5 ms (or half the grain). */
        static GrainWindow makeWindow (bool loop, float shape, double sizeSec) noexcept
        {
            const auto& table = GrainWindowTable::get();
            const double minRamp = std::min (0.5, 0.0005 / std::max (sizeSec, 1.0e-4));
            GrainWindow w;

            if (loop)
            {
                const double x = std::clamp ((double) shape * 0.5, minRamp, 0.5);
                w.a = w.r = (float) x;
                w.quarterSine = true;
                w.gain = 1.0f;
                return w;
            }

            const double s = std::clamp ((double) shape, 0.0, 1.0);
            const double a = std::clamp (s, minRamp, 1.0 - minRamp);
            w.a = (float) a;
            w.r = (float) (1.0 - a);
            w.kA = (float) std::clamp ((s - 0.5) * 2.0, 0.0, 1.0);
            w.kR = (float) std::clamp ((0.5 - s) * 2.0, 0.0, 1.0);

            const double mA = 0.5 + (double) w.kA * (table.meanExpo - 0.5);
            const double mR = 0.5 + (double) w.kR * (table.meanExpo - 0.5);
            const double mean = a * mA + (1.0 - a) * mR;
            w.gain = (float) std::clamp (0.5 / mean, 0.5, 2.0);
            return w;
        }

        /** Scan ramp duration in seconds; 0 = scan off. */
        static double scanDurationSeconds (float scan, bool key, double pitchSemis) noexcept
        {
            if (scan < 0.001f)
                return 0.0;

            double d = 20.0 * std::pow (0.0004, (double) scan);

            if (key)
                d /= std::exp2 ((pitchSemis - 60.0) / 12.0);

            return d;
        }

        /** Spawn interval of the scheduler in seconds. */
        static double hopSeconds (int mode, float sizeMs, float shape) noexcept
        {
            const double nominal = std::clamp ((double) sizeMs * 0.001, 0.002, 2.0);

            if (mode == 1)
            {
                const double x = std::clamp ((double) shape * 0.5, std::min (0.5, 0.0005 / nominal), 0.5);
                return (1.0 - x) * nominal;
            }

            return 0.5 * nominal;
        }

        //----------------------------------------------------------------------
        /** Forgets everything and seeds the PRNG (note start). */
        void start (std::uint32_t seed) noexcept
        {
            *this = GrainEngine {};
            rng = seed != 0u ? seed : 0x9E3779B9u;
        }

        /** Granular is not in use: drop the grains so a later re-enable starts clean (PRNG state is kept). */
        void idle() noexcept
        {
            if (! running)
                return;

            running = false;

            for (auto& g : pool)
                g.active = false;

            for (auto& s : up)
                s.reset();

            scanPhaseV = 0.0;
            countdown = 0.0;
            cloudTimer = 0.0;
            wctl = WarpCtl {};
        }

        bool isRunning() const noexcept        { return running; }
        double scanPhase() const noexcept      { return scanPhaseV; }
        std::int64_t samplesRendered() const noexcept { return sampleClock; }
        int spawnCount() const noexcept        { return spawnedCount; }
        int droppedSpawns() const noexcept     { return droppedCount; }

        int activeCount() const noexcept
        {
            int n = 0;

            for (const auto& g : pool)
                if (g.active)
                    ++n;

            return n;
        }

        /** Cloud grains that are not fading out (the "live" count the scheduler keeps at grainDensity). */
        int cloudGrainCount() const noexcept
        {
            int n = 0;

            for (const auto& g : pool)
                if (g.active && g.sched == 2 && g.relLeft == 0)
                    ++n;

            return n;
        }

        /** Read position (source frames) of the most recently spawned grain on the left channel; false if none. */
        bool newestPosition (double& frames) const noexcept
        {
            if (newestSlot >= 0)
            {
                const auto& g = pool[static_cast<std::size_t> (newestSlot)];

                if (g.active && g.serial == newestSerial)
                {
                    frames = g.posL;
                    return true;
                }
            }

            if (lastSpawnFrames >= 0.0)
            {
                frames = lastSpawnFrames;
                return true;
            }

            return false;
        }

        /** Viz feed: position (0..1 of the sample) and window level of every sounding grain (left head). */
        int collect (float* posOut, float* levelOut, int maxOut, const SampleData& sd) const noexcept
        {
            const auto& table = GrainWindowTable::get();
            const double inv = sd.numFrames > 0 ? 1.0 / (double) sd.numFrames : 0.0;
            int n = 0;

            for (const auto& g : pool)
            {
                if (n >= maxOut)
                    break;

                if (! g.active)
                    continue;

                const double p = std::clamp (g.posL * inv, 0.0, 1.0);
                const float lv = g.win.at (table, (float) g.age / (float) g.len) * g.gain;
                posOut[n] = (float) p;
                levelOut[n] = std::min (1.0f, std::max (0.0f, lv));
                ++n;
            }

            return n;
        }

        //----------------------------------------------------------------------
        /** Renders `nOS` OS-rate samples (nOS = base samples x os, <= 256).  `read (pos, dir, l, r)` is the
            oscillator's interpolated read with seam / end fades for that position and travel direction.
            The result points into `sp.scratch`. */
        template <class Read>
        GrainOut render (const GrainSpan& sp, const RegionFold& rg, const SampleData& sd,
                         double rhoHost, int os, int nOS, Read& readGrain) noexcept
        {
            GrainScratch& sc = *sp.scratch;
            const auto& table = GrainWindowTable::get();

            const int grainFactor = os >= 2 ? 2 : 1;
            const int ratio = std::max (1, os / grainFactor);
            const int nS = std::max (0, std::min (nOS / ratio, GrainScratch::capacity / ratio));
            const double grainRate = sp.sampleRate * (double) grainFactor;
            const int fadeLen = std::max (1, (int) std::lround (fadeSeconds * grainRate));
            const double invLc = sp.warp.Lc > 0.0 ? 1.0 / sp.warp.Lc : 0.0;
            const Ctx c { sp, rg, sd, grainRate, rhoHost / (double) grainFactor, fadeLen, invLc,
                          modosc::coefsFor (grainRate), 1.0 / (1.0 + 0.0002 * grainRate) };
            const int sched = sp.mode == 2 ? 2 : (sp.mode == 1 ? 1 : 0);

            if (! running)
            {
                running = true;
                curSched = sched;
                scanPhaseV = 0.0;
                cloudTimer = 0.0;
                wctl = WarpCtl {};

                for (auto& g : pool)
                    g.active = false;

                for (auto& s : up)
                    s.reset();

                preroll (c, false);
            }
            else if (sched != curSched)
            {
                for (auto& g : pool)
                    if (g.active && g.relLeft == 0)
                        g.relLeft = g.relTotal = fadeLen;

                curSched = sched;
                preroll (c, true);
            }

            // keep sounding grains inside the (possibly moved) region
            for (auto& g : pool)
            {
                if (! g.active)
                    continue;

                if (! g.endedL && ! rg.constrain (g.posL, g.dirL))
                    g.endedL = true;

                if (! g.endedR && ! rg.constrain (g.posR, g.dirR))
                    g.endedR = true;
            }

            const double scanSeconds = scanDurationSeconds (sp.scan, sp.scanKey, sp.pitchSemis);
            const double scanInc = scanSeconds > 0.0 ? 1.0 / (scanSeconds * grainRate) : 0.0;

            // ---- per-grain phase warp control (D7b): which phase mode the grains apply, and its 10 ms switch fade
            updateWarpCtl (sp.warp, fadeLen);

            const int cloudTarget = std::clamp (sp.density, 1, maxCloudGrains);
            const double cloudMeanLen = std::clamp ((double) sp.sizeMs * 0.001, 0.002, 2.0) * grainRate;

            for (int j = 0; j < nS; ++j)
            {
                if (curSched == 2)
                {
                    // CLOUD: keep `cloudTarget` live grains.  A missing grain is respawned when the stagger timer has run
                    // out (one per grain-rate sample at most), so grains that end together do not restart together.
                    cloudTimer = std::max (0.0, cloudTimer - 1.0);

                    if (cloudTimer <= 0.0 && cloudGrainCount() < cloudTarget)
                    {
                        spawn (c, 0.0, false);
                        cloudTimer = std::max (1.0, 0.5 * cloudMeanLen / (double) cloudTarget * (0.5 + (double) unit()));
                    }
                }
                else
                {
                    int guard = 0;

                    while (countdown <= 0.0 && guard++ < 4)
                    {
                        spawn (c, 0.0, false);
                        countdown += std::max (1.0, hopSeconds (curSched, sp.sizeMs, sp.shape) * grainRate);
                    }
                }

                WarpNow wn;
                const bool warpOn = wctl.cur >= 0 || wctl.fadeLeft > 0;

                if (warpOn)
                    wn = warpNowAt (sp.warp, (float) j / (float) nS);

                float sumL = 0.0f, sumR = 0.0f;

                for (auto& g : pool)
                {
                    if (! g.active)
                        continue;

                    float w = g.win.at (table, (float) g.age / (float) g.len) * g.gain;
                    bool finished = false;

                    if (g.relLeft > 0)
                    {
                        w *= (float) g.relLeft / (float) g.relTotal;

                        if (--g.relLeft == 0)
                            finished = true;
                    }

                    if (g.inLeft > 0)
                    {
                        w *= 1.0f - (float) g.inLeft / (float) g.inTotal;
                        --g.inLeft;
                    }

                    if (w > 0.0f)
                    {
                        float vL = 0.0f, vR = 0.0f;
                        const bool haveL = ! g.endedL, haveR = ! g.endedR;

                        if (warpOn)
                            warpedHeads (readGrain, g, c, wn, haveL, haveR, vL, vR);
                        else
                            readHeads (readGrain, g.posL, g.dirL, (double) g.ratioL, haveL, g.posR, g.dirR, (double) g.ratioR, haveR, vL, vR);

                        sumL += w * vL;
                        sumR += w * vR;
                    }

                    if (! g.endedL && ! rg.advance (g.posL, g.dirL, c.stepBase * (double) g.ratioL))
                        g.endedL = true;

                    if (! g.endedR && ! rg.advance (g.posR, g.dirR, c.stepBase * (double) g.ratioR))
                        g.endedR = true;

                    // per-grain cycle clocks advance with the head's own step, also while no warp is applied
                    if (c.invLc > 0.0)
                    {
                        g.hL.advance (c.stepBase * (double) g.ratioL * c.invLc);
                        g.hR.advance (c.stepBase * (double) g.ratioR * c.invLc);
                    }

                    ++g.age;

                    if (finished || g.age >= g.len || (g.endedL && g.endedR))
                        g.active = false;
                }

                if (warpOn)
                    finishWarpSample (wn);

                const auto jj = static_cast<std::size_t> (j);
                sc.aL[jj] = sumL;
                sc.aR[jj] = sumR;

                if (scanInc > 0.0)
                {
                    scanPhaseV += scanInc;

                    if (sp.scanLoop)
                        scanPhaseV -= std::floor (scanPhaseV);
                    else
                        scanPhaseV = std::min (1.0, scanPhaseV);
                }

                countdown -= 1.0;
                ++sampleClock;
            }

            // grain rate -> OS rate (ratio 1: nothing to do; 2: one stage; 4: two stages)
            const float* inL = sc.aL.data();
            const float* inR = sc.aR.data();
            int n = nS;
            bool toB = true;

            for (int stage = 0; stage < 2 && (ratio >> (stage + 1)) > 0; ++stage)
            {
                float* oL = toB ? sc.bL.data() : sc.aL.data();
                float* oR = toB ? sc.bR.data() : sc.aR.data();

                if (sp.hb != nullptr)
                {
                    upsampleStage (*sp.hb, up[static_cast<std::size_t> (stage)], inL, inR, oL, oR, n);
                }
                else
                {
                    for (int i = 0; i < n; ++i)                      // no coefficients: sample-and-hold (never in the plugin)
                    {
                        oL[2 * i] = oL[2 * i + 1] = inL[i];
                        oR[2 * i] = oR[2 * i + 1] = inR[i];
                    }
                }

                inL = oL;
                inR = oR;
                n *= 2;
                toB = ! toB;
            }

            return { inL, inR };
        }

    private:
        /** Per-head (left / right read head) phase-warp state of ONE grain (D7b).  `theta` is the head's cycle clock;
            the rest is the Mirror SeamBridge of that head. */
        struct HeadWarp
        {
            double theta = 0.0;                      // note-cycle phase of this head, [0, 1)
            bool   wrapped = false;                  // the clock wrapped on the previous sample
            int    parity = 0;                       // master cycles elapsed mod 2 (Soft Sync)
            double lastPw = 0.0, lastStep = 0.0;     // SeamBridge: previous warped read position / step
            bool   lastValid = false;
            float  bridgeOff = 0.0f;
            int    bridgeLeft = 0;

            // D6 per-grain kernel state: the modulation kernels (modulator phase, Sample follower / integrator, Self FM
            // loop, Cross Mod previous value) of the current mode and of the outgoing one during the mode cross-fade,
            // each tagged with the mode it belongs to (stateFor), and the measured read speed (mip selection)
            phasewarp::ModState msCur, msOld;
            int    modeCur = -2, modeOld = -2;
            double qOff = 0.0, qSm = 1.0;
            bool   qValid = false;

            void advance (double dTheta) noexcept
            {
                theta += dTheta;
                wrapped = false;

                if (theta >= 1.0)
                {
                    theta -= std::floor (theta);
                    wrapped = true;
                    parity ^= 1;
                }
            }
        };

        struct Grain
        {
            bool active = false, endedL = false, endedR = false;
            std::uint8_t sched = 0;                  // scheduler that spawned the grain (0 Classic, 1 Loop, 2 Cloud)
            int age = 0, len = 2;
            int dirL = 1, dirR = 1;
            double posL = 0.0, posR = 0.0;
            float ratioL = 1.0f, ratioR = 1.0f, gain = 1.0f;
            GrainWindow win;
            int relLeft = 0, relTotal = 1;
            int inLeft = 0, inTotal = 1;
            std::uint32_t serial = 0;
            HeadWarp hL, hR;                         // per-grain phase warp state (D7b, D6)
        };

        struct Ctx
        {
            const GrainSpan& sp;
            const RegionFold& rg;
            const SampleData& sd;
            double grainRate;
            double stepBase;                     // source frames per grain-rate sample at the voice pitch
            int fadeLen;
            double invLc;                        // 1 / Lc (0 = no cycle clock)
            modosc::Coefs coefs;                 // Sample-modulator time constants at the grain rate (D6)
            double kq;                           // read-speed release coefficient per grain-rate sample (D6)
        };

        /** Engine-level state of the per-grain phase warp: the mode the grains currently apply (-1 = none) and the
            10 ms cross-fade from the previous one (frozen parameters, like WarpOscState). */
        struct WarpCtl
        {
            bool   primed = false;
            int    cur = -1, old = -1;
            double oldA = 0.0, oldAA = 0.5, oldB = 0.0;        // frozen parameters of the outgoing mode
            double lastA = 0.0, lastAA = 0.5, lastB = 0.0;     // parameters of the previous grain-rate sample
            int    fadeLeft = 0, fadeTotal = 1;
        };

        /** The warp parameters for ONE grain-rate sample. */
        struct WarpNow
        {
            int    cur = -1, old = -1;
            double a = 0.0, A = 0.5, B = 0.0;
            double oldA = 0.0, oldAA = 0.5, oldB = 0.0;
            float  mix = 1.0f;
            double ratio = 1.0;                  // modOscRatio at this sample
            bool   fading = false;
            float  t = 1.0f;                     // cross-fade weight of the new mode (1 = new only)
        };

        void updateWarpCtl (const GrainWarp& gw, int fadeLen) noexcept
        {
            if (! gw.active || gw.Lc <= 0.0)
            {
                wctl = WarpCtl {};               // stage bypassed (mix is 0 / ramping out): nothing audible to cross-fade
                return;
            }

            // A mode is applied per grain when it is an IMPLEMENTED phase-kind row of WarpModeTable (D6: 0-3, 7, 9, 14,
            // 44-55, 58, 59); the amplitude-kind rows (4-6, 8, 10-13, 15-43, 56, 57) stay with the oscillator-level stage.
            const auto& info = warpModeInfo (gw.mode);
            const int eff = info.implemented && info.kind == WarpKind::Phase ? gw.mode : -1;

            if (! wctl.primed)
            {
                wctl.primed = true;
                wctl.cur = eff;
            }
            else if (eff != wctl.cur)
            {
                wctl.old = wctl.cur;
                wctl.oldA = wctl.lastA;
                wctl.oldAA = wctl.lastAA;
                wctl.oldB = wctl.lastB;
                wctl.cur = eff;
                wctl.fadeLeft = wctl.fadeTotal = std::max (1, fadeLen);
            }
        }

        WarpNow warpNowAt (const GrainWarp& gw, float tt) const noexcept
        {
            WarpNow n;
            n.cur = wctl.cur;
            n.a = (double) (gw.a0 + (gw.a1 - gw.a0) * tt);
            n.A = (double) (gw.A0 + (gw.A1 - gw.A0) * tt);
            n.B = (double) (gw.B0 + (gw.B1 - gw.B0) * tt);
            n.mix = gw.mix0 + (gw.mix1 - gw.mix0) * tt;
            n.ratio = (double) (gw.ratio0 + (gw.ratio1 - gw.ratio0) * tt);

            if (wctl.fadeLeft > 0)
            {
                n.fading = true;
                n.old = wctl.old;
                n.oldA = wctl.oldA;
                n.oldAA = wctl.oldAA;
                n.oldB = wctl.oldB;
                n.t = (float) (wctl.fadeTotal - wctl.fadeLeft + 1) / (float) wctl.fadeTotal;
            }

            return n;
        }

        void finishWarpSample (const WarpNow& wn) noexcept
        {
            wctl.lastA = wn.a;
            wctl.lastAA = wn.A;
            wctl.lastB = wn.B;

            if (wctl.fadeLeft > 0 && --wctl.fadeLeft == 0)
                wctl.old = -1;
        }

        /** Reads the two heads (left head: channel 0 at posL; right head: channel 1 at posR).  A head that has ended
            reads 0.  Identical head positions share one read. */
        template <class Read>
        static void readHeads (Read& rd, double pL, int dL, double mL, bool haveL, double pR, int dR, double mR, bool haveR,
                               float& outL, float& outR) noexcept
        {
            float l0 = 0.0f, l1 = 0.0f, rv = 0.0f;

            if (haveL)
                rd (pL, dL, mL, l0, l1);

            if (haveR)
            {
                if (haveL && juce::exactlyEqual (pR, pL) && dR == dL && juce::exactlyEqual (mR, mL))
                {
                    rv = l1;
                }
                else
                {
                    float r0 = 0.0f, r1 = 0.0f;
                    rd (pR, dR, mR, r0, r1);
                    rv = r1;
                }
            }

            outL = l0;
            outR = rv;
        }

        /** The ModState a head uses for `mode`: the current one, the outgoing one, or (a mode the head has not run yet)
            a fresh one that pushes the current state into the outgoing slot. */
        static phasewarp::ModState& stateFor (HeadWarp& h, int mode) noexcept
        {
            if (mode == h.modeCur)
                return h.msCur;

            if (mode == h.modeOld)
                return h.msOld;

            h.msOld = h.msCur;
            h.modeOld = h.modeCur;
            h.msCur.reset();
            h.modeCur = mode;
            return h.msCur;
        }

        /** What one head does under one phase mode in one grain-rate sample: the PhaseOut, the read positions of its
            taps, and the speed multiplier its warped reads need (the head's ratio x the measured read speed). */
        struct HeadTap
        {
            phasewarp::PhaseOut po;
            double pw0 = 0.0, pw1 = 0.0;
            double mult = 1.0;
            bool   moved = false;                // false: the read is exactly the plain read (bit-exact warp at amount 0)
        };

        template <class Read>
        static HeadTap evalHead (Read& rd, HeadWarp& h, int mode, double a, double A, double B, double dT, const WarpNow& wn,
                                 const Ctx& c, double p, int dir, float ratio, double step, bool trackSpeed) noexcept
        {
            HeadTap t;
            t.pw0 = t.pw1 = p;
            t.mult = (double) ratio;

            if (mode < 0)
                return t;

            const GrainWarp& gw = c.sp.warp;
            const RegionFold& rg = c.rg;
            const double Lc = gw.Lc;

            if (warpModeInfo (mode).family == WarpFamily::Mod)
            {
                phasewarp::ModState& st = stateFor (h, mode);
                const phasewarp::ModIn mi { h.theta, dT, wn.ratio, gw.modSource, &c.coefs };

                auto rdm = [&] (double off) noexcept
                {
                    const double pm = rg.foldRead (rg.lo + wn.ratio * (p - rg.lo) + (double) dir * Lc * off);
                    float m0 = 0.0f, m1 = 0.0f;
                    rd (pm, dir, (double) ratio * wn.ratio, m0, m1);
                    return 0.5 * ((double) m0 + (double) m1);
                };

                t.po = phasewarp::evalMod (mode, a, A, B, mi, st, rdm);
            }
            else
            {
                const phasewarp::MapCtx cx { gw.curve, h.parity, gw.blep };
                t.po = phasewarp::evalMap (mode, a, A, B, h.theta, dT, cx);
            }

            t.moved = ! t.po.identity();

            if (t.moved)
            {
                t.pw0 = rg.foldRead (p + (double) dir * Lc * t.po.d0);

                if (t.po.g1 > 0.0f)
                    t.pw1 = rg.foldRead (p + (double) dir * Lc * t.po.d1);
            }

            if (trackSpeed)
            {
                double q = 1.0;

                // measured on the UNFOLDED offset: region wraps of the head do not count as speed
                const double off = t.moved ? (double) dir * Lc * t.po.d0 : 0.0;

                if (h.qValid)
                {
                    q = std::abs ((double) dir * step + (off - h.qOff)) / std::max (step, 1.0e-12);

                    if (! (q < 40.0))
                        q = 1.0;                                    // a reset / jump, not a speed
                }

                h.qOff = off;
                h.qValid = true;
                h.qSm = q > h.qSm ? q : h.qSm + c.kq * (q - h.qSm);
            }

            t.mult = (double) ratio * h.qSm;
            return t;
        }

        /** The wet values of the two heads from their taps: tap 0 (+ tap 1, equal-power / linear weights) times the amplitude
            window.  `wetL` / `wetR` come in holding nothing meaningful and go out as the heads' wet values. */
        template <class Read>
        static void wetHeads (Read& rd, const Grain& g, const HeadTap& tL, const HeadTap& tR, bool haveL, bool haveR,
                              float& wetL, float& wetR) noexcept
        {
            readHeads (rd, tL.pw0, g.dirL, tL.mult, haveL, tR.pw0, g.dirR, tR.mult, haveR, wetL, wetR);

            if (tL.po.g1 > 0.0f || tR.po.g1 > 0.0f)
            {
                float l1 = 0.0f, r1 = 0.0f;
                readHeads (rd, tL.pw1, g.dirL, tL.mult, haveL, tR.pw1, g.dirR, tR.mult, haveR, l1, r1);
                wetL = tL.po.g0 * wetL + tL.po.g1 * l1;               // a head without a second tap has g0 = 1, g1 = 0
                wetR = tR.po.g0 * wetR + tR.po.g1 * r1;
            }

            if (! juce::exactlyEqual (tL.po.amp, 1.0f))
                wetL *= tL.po.amp;

            if (! juce::exactlyEqual (tR.po.amp, 1.0f))
                wetR *= tR.po.amp;
        }

        /** Mirror-style SeamBridge of one head: at a cycle wrap the warp map may jump; read where continuity of the
            read position would have led and split the output difference over two grain-rate samples. */
        template <class Read>
        static void bridgeHead (Read& rd, const RegionFold& rg, HeadWarp& h, float& wet, double pw, double step,
                                int dir, double mult, bool have, bool rightChannel) noexcept
        {
            if (! have)
            {
                h.bridgeLeft = 0;
                return;
            }

            if (h.wrapped && h.lastValid)
            {
                float a = 0.0f, b = 0.0f;
                rd (rg.foldRead (h.lastPw + (double) dir * h.lastStep), dir, mult, a, b);
                h.bridgeOff = (rightChannel ? b : a) - wet;
                h.bridgeLeft = 1;                                  // total 2: weights 1/2 then 0
            }

            if (h.bridgeLeft > 0)
            {
                wet += h.bridgeOff * (float) h.bridgeLeft * 0.5f;
                --h.bridgeLeft;
            }

            h.lastPw = pw;
            h.lastStep = step;
            h.lastValid = true;
        }

        /** The head values of one grain with the phase warp applied: per-head kernel (map / modulation, up to two taps and
            an amplitude window), dry / wet, SeamBridge, old -> new mode cross-fade, warpMix.  Everything is per grain
            head: its cycle clock, parity, modulator and feedback state live in g.hL / g.hR. */
        template <class Read>
        void warpedHeads (Read& rd, Grain& g, const Ctx& c, const WarpNow& wn, bool haveL, bool haveR,
                          float& outL, float& outR) noexcept
        {
            const RegionFold& rg = c.rg;
            const double stepL = c.stepBase * (double) g.ratioL, stepR = c.stepBase * (double) g.ratioR;
            const double dTL = stepL * c.invLc, dTR = stepR * c.invLc;
            const double mL = (double) g.ratioL, mR = (double) g.ratioR;

            const HeadTap tL = evalHead (rd, g.hL, wn.cur, wn.a, wn.A, wn.B, dTL, wn, c, g.posL, g.dirL, g.ratioL, stepL, true);
            const HeadTap tR = evalHead (rd, g.hR, wn.cur, wn.a, wn.A, wn.B, dTR, wn, c, g.posR, g.dirR, g.ratioR, stepR, true);
            const bool curMoved = tL.moved || tR.moved;

            // the dry read is only skipped for a fully wet, non-fading, moved read
            const bool needDry = wn.mix < 1.0f || wn.fading || ! curMoved;

            float dryL = 0.0f, dryR = 0.0f;

            if (needDry)
                readHeads (rd, g.posL, g.dirL, mL, haveL, g.posR, g.dirR, mR, haveR, dryL, dryR);

            float wetL = dryL, wetR = dryR;

            if (curMoved)
                wetHeads (rd, g, tL, tR, haveL, haveR, wetL, wetR);

            // Self FM / Cross Mod close their loop on each head's own wet value (the feedback guard lives in feed())
            if ((wn.cur == 58 || wn.cur == 59) && wn.a > 0.0)
            {
                phasewarp::feed (wn.cur, wn.A, (double) wetL, c.grainRate, stateFor (g.hL, wn.cur));
                phasewarp::feed (wn.cur, wn.A, (double) wetR, c.grainRate, stateFor (g.hR, wn.cur));
            }

            if (wn.cur >= 0 && warpModeInfo (wn.cur).seamJump)
            {
                bridgeHead (rd, rg, g.hL, wetL, tL.pw0, stepL, g.dirL, mL, haveL, false);
                bridgeHead (rd, rg, g.hR, wetR, tR.pw0, stepR, g.dirR, mR, haveR, true);
            }
            else
            {
                g.hL.bridgeLeft = g.hR.bridgeLeft = 0;
                g.hL.lastValid = g.hR.lastValid = false;
            }

            if (wn.fading)
            {
                float oldL = dryL, oldR = dryR;

                const HeadTap oL = evalHead (rd, g.hL, wn.old, wn.oldA, wn.oldAA, wn.oldB, dTL, wn, c, g.posL, g.dirL, g.ratioL, stepL, false);
                const HeadTap oR = evalHead (rd, g.hR, wn.old, wn.oldA, wn.oldAA, wn.oldB, dTR, wn, c, g.posR, g.dirR, g.ratioR, stepR, false);

                if (oL.moved || oR.moved)
                    wetHeads (rd, g, oL, oR, haveL, haveR, oldL, oldR);

                if ((wn.old == 58 || wn.old == 59) && wn.oldA > 0.0)
                {
                    phasewarp::feed (wn.old, wn.oldAA, (double) oldL, c.grainRate, stateFor (g.hL, wn.old));
                    phasewarp::feed (wn.old, wn.oldAA, (double) oldR, c.grainRate, stateFor (g.hR, wn.old));
                }

                wetL = oldL + wn.t * (wetL - oldL);
                wetR = oldR + wn.t * (wetR - oldR);
            }

            if (wn.mix >= 1.0f)
            {
                outL = wetL;
                outR = wetR;
            }
            else
            {
                outL = dryL + wn.mix * (wetL - dryL);
                outR = dryR + wn.mix * (wetR - dryR);
            }
        }

        std::uint32_t nextRandom() noexcept
        {
            rng ^= rng << 13;
            rng ^= rng >> 17;
            rng ^= rng << 5;
            return rng;
        }

        float unit() noexcept     { return (float) (nextRandom() >> 8) * (1.0f / 16777216.0f); }
        float bipolar() noexcept  { return 2.0f * unit() - 1.0f; }

        /** The grain train starts "as if it had been running": one grain a hop old plus a fresh one. */
        void preroll (const Ctx& c, bool fadeIn) noexcept
        {
            if (curSched == 2)
            {
                // Cloud: N grains of uniformly random age (negative age = "draw it"), no hop clock
                const int n = std::clamp (c.sp.density, 1, maxCloudGrains);

                for (int i = 0; i < n; ++i)
                    spawn (c, -1.0, fadeIn);

                cloudTimer = 0.0;
                return;
            }

            const double hop = std::max (1.0, hopSeconds (curSched, c.sp.sizeMs, c.sp.shape) * c.grainRate);
            spawn (c, std::round (hop), fadeIn);                 // rounded: a hop that is a whole number of cycles stays coherent
            spawn (c, 0.0, fadeIn);
            countdown = hop;
        }

        void spawn (const Ctx& c, double ageSamples, bool fadeIn) noexcept
        {
            // four draws per spawn, always (the stream must not depend on the parameters); Cloud draws two more
            const float rPos = bipolar(), rSize = bipolar(), rRev = unit(), rVol = unit();
            const bool cloud = curSched == 2;
            float rPitch = 0.0f, rAux = 0.0f;

            if (cloud)
            {
                rPitch = bipolar();                               // transposition jitter
                rAux = unit();                                    // start-up fill: the grain's random age
            }

            const GrainSpan& sp = c.sp;
            const RegionFold& rg = c.rg;
            const bool loop = curSched == 1;

            int slot = -1;

            for (int i = 0; i < poolSize; ++i)
            {
                if (! pool[static_cast<std::size_t> (i)].active)
                {
                    slot = i;
                    break;
                }
            }

            if (slot < 0)
            {
                ++droppedCount;
                return;
            }

            const double v = (double) std::clamp (sp.variation, 0.0f, 1.0f);
            const double nominal = std::clamp ((double) sp.sizeMs * 0.001, 0.002, 2.0);
            const double sizeSec = std::clamp (nominal * std::exp2 (v * (double) rSize * 0.5), 0.001, 4.0);
            const int len = std::max (2, (int) std::lround (sizeSec * c.grainRate));
            const double ageUse = ageSamples < 0.0 ? (double) rAux * (double) len : ageSamples;
            const int age0 = (int) std::floor (ageUse);

            if (age0 >= len)
                return;                                           // older than its own length: already over

            // start position: scan ramp, then the variation offset (up to 2 s of source time)
            const double pos0 = std::clamp ((double) sp.position, 0.0, 1.0);
            const double scanEnd = std::clamp (pos0 + (double) sp.scanDist, 0.0, 1.0);
            const double scanned = pos0 + (scanEnd - pos0) * scanPhaseV;
            const double offsetFrames = v * (double) rPos * 2.0 * c.sd.fileSampleRate;
            const double norm = std::clamp (scanned + offsetFrames / std::max (1.0, rg.length), 0.0, 1.0);
            const double startFrame = rg.lo + norm * rg.length;

            const bool reversed = sp.reverseDir != (loop && (double) rRev < (double) sp.reverse);

            // stereo spread: opposite detune of the two heads and a start offset of the right head
            const double spread = (double) std::clamp (sp.spread, 0.0f, 1.0f);
            const double detuneRatio = std::exp2 (spread * 30.0 / 1200.0);
            const double jitter = cloud ? std::exp2 (v * (double) rPitch) : 1.0;     // Cloud: +-12 v semitones
            const float ratioL = (float) (jitter / detuneRatio), ratioR = (float) (jitter * detuneRatio);
            const double offsetR = spread * 0.25 * sizeSec * c.sd.fileSampleRate;

            const double span = (double) len * c.stepBase;
            double pL = startFrame, pR = startFrame + offsetR;
            int dL = reversed ? -1 : 1, dR = dL;

            if (reversed)                                        // same segment, played backwards
            {
                pL += span * (double) ratioL;
                pR += span * (double) ratioR;
            }

            pL = rg.foldRead (pL);
            pR = rg.foldRead (pR);

            // per-head cycle clocks (D7b): seeded from the head's start position like the Classic clock in
            // UnisonOsc::start, then aged with the pre-roll
            double thL = 0.0, thR = 0.0;

            if (c.invLc > 0.0)
            {
                thL = (pL - rg.lo) * c.invLc;
                thR = (pR - rg.lo) * c.invLc;

                if (age0 > 0)
                {
                    thL += (double) age0 * c.stepBase * (double) ratioL * c.invLc;
                    thR += (double) age0 * c.stepBase * (double) ratioR * c.invLc;
                }

                thL -= std::floor (thL);
                thR -= std::floor (thR);
            }

            bool endL = false, endR = false;

            if (age0 > 0)
            {
                endL = ! rg.advance (pL, dL, (double) age0 * c.stepBase * (double) ratioL);
                endR = ! rg.advance (pR, dR, (double) age0 * c.stepBase * (double) ratioR);
            }

            Grain g;
            g.hL.theta = thL;
            g.hR.theta = thR;
            g.sched = static_cast<std::uint8_t> (curSched);
            g.active = true;
            g.endedL = endL;
            g.endedR = endR;
            g.age = age0;
            g.len = len;
            g.dirL = dL;
            g.dirR = dR;
            g.posL = pL;
            g.posR = pR;
            g.ratioL = ratioL;
            g.ratioR = ratioR;
            g.win = makeWindow (loop, cloud ? 0.5f : sp.shape, sizeSec);      // Cloud: fixed Hann (grainShape ignored)
            g.gain = g.win.gain * (float) std::pow (10.0, -6.0 * v * (double) rVol / 20.0);

            if (cloud)                                           // 1 / sqrt(0.5 N): uncorrelated grains sum to the Classic level
                g.gain *= std::sqrt (2.0f / (float) std::clamp (sp.density, 1, maxCloudGrains));
            g.serial = ++serialCounter;

            if (fadeIn)
                g.inLeft = g.inTotal = c.fadeLen;

            pool[static_cast<std::size_t> (slot)] = g;
            ++spawnedCount;

            if (age0 == 0)
            {
                newestSlot = slot;
                newestSerial = g.serial;
                lastSpawnFrames = startFrame;
            }

            if (sp.log != nullptr && sp.log->count < GrainSpawnLog::capacity)
            {
                auto& e = sp.log->entries[static_cast<std::size_t> (sp.log->count++)];
                e.sample = sampleClock;
                e.startNorm = norm;
                e.scanPhase = scanPhaseV;
                e.len = len;
                e.reversed = reversed;
                e.preAged = age0 > 0;
                e.ratio = jitter;
            }
        }

        std::array<Grain, static_cast<std::size_t> (poolSize)> pool {};
        std::array<HalfbandStageState, 2> up {};

        std::uint32_t rng = 0x9E3779B9u;
        bool running = false;
        int curSched = 0;
        double scanPhaseV = 0.0;
        double countdown = 0.0;
        double cloudTimer = 0.0;                 // Cloud respawn stagger, grain-rate samples (0 = ready)
        WarpCtl wctl;                            // per-grain phase-warp control (D7b)
        std::int64_t sampleClock = 0;
        std::uint32_t serialCounter = 0, newestSerial = 0;
        int newestSlot = -1;
        double lastSpawnFrames = -1.0;
        int spawnedCount = 0, droppedCount = 0;
    };
}
