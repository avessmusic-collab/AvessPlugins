#pragma once
/*
  GrainEngine - per-oscillator granular source (architecture C7), phase D7a: CLASSIC + LOOP schedulers.

  WHERE IT SITS.  A granular voice replaces the Classic sample read at the very top of the oscillator
  (UnisonOsc::render).  The grain pool is rendered at S = min(OS, 2) times the host rate (architecture:
  "granular source rate"), up-sampled with the same half-band all-pass design as the voice decimator to the
  OS rate, and then fed into the SAME warp -> shaper -> pan -> decimator -> DC block -> filter -> ADSR chain as
  the Classic read.  Amplitude-domain warp modes and the shaper therefore work on grains unchanged; the
  phase-domain warps (H family, FM, sync ...) have no read position to move in granular mode and are an
  identity there until D7b (per-grain phase warps).

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
    CLOUD    (D7b) plays as CLASSIC until the cloud scheduler lands.
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
  x 2^(+-0.5 v), volume 0 .. -6 dB x v.  Four random numbers are drawn per spawn whatever v is, so the stream
  never depends on the parameters.  PRNG: per-oscillator xorshift32 seeded from the note counter.

  SWITCHES.  playMode (Classic <-> Granular) is a 10 ms weight ramp owned by the voice/oscillator.  A scheduler
  change (grainMode) lets every sounding grain fade out over 10 ms while the new scheduler pre-rolls and fades in
  over 10 ms.  A window can never have a ramp shorter than 0.5 ms (or half the grain).

  Real-time rules: fixed pool (poolSize grains, a spawn with no free slot is dropped and counted), no
  allocation, no locks.  The window table is built by GrainWindowTable::get() (WarprSynth::prepare calls it).
*/

#include "DSP/HalfbandIIR.h"
#include "DSP/RegionFold.h"
#include "DSP/SampleData.h"

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

    /** Per-chunk description of the granular source (built by the voice). */
    struct GrainSpan
    {
        bool   active = false;                 // false: the engine is idle (Classic only)
        float  w0 = 0.0f, w1 = 0.0f;           // granular share (playMode crossfade) at the start / end of the span
        int    mode = 0;                       // grainMode: 0 Classic, 1 Loop, 2 Cloud (D7b; plays as Classic)
        float  position = 0.0f, scan = 0.0f, scanDist = 0.0f;
        bool   scanLoop = true, scanKey = false;
        float  sizeMs = 80.0f, shape = 0.5f, variation = 0.0f, spread = 0.0f, reverse = 0.0f;
        int    density = 8;                    // Cloud only (D7b)
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
        static constexpr int poolSize = 16;                // stereo grains per oscillator (D7b Cloud will raise it)
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
            const Ctx c { sp, rg, sd, grainRate, rhoHost / (double) grainFactor, fadeLen };
            const int sched = sp.mode == 1 ? 1 : 0;

            if (! running)
            {
                running = true;
                curSched = sched;
                scanPhaseV = 0.0;

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

            for (int j = 0; j < nS; ++j)
            {
                int guard = 0;

                while (countdown <= 0.0 && guard++ < 4)
                {
                    spawn (c, 0.0, false);
                    countdown += std::max (1.0, hopSeconds (curSched, sp.sizeMs, sp.shape) * grainRate);
                }

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
                        float l0 = 0.0f, l1 = 0.0f, rv = 0.0f;
                        const bool haveL = ! g.endedL, haveR = ! g.endedR;

                        if (haveL)
                            readGrain (g.posL, g.dirL, l0, l1);

                        if (haveR)
                        {
                            if (haveL && juce::exactlyEqual (g.posR, g.posL) && g.dirR == g.dirL)
                            {
                                rv = l1;
                            }
                            else
                            {
                                float r0 = 0.0f, r1 = 0.0f;
                                readGrain (g.posR, g.dirR, r0, r1);
                                rv = r1;
                            }
                        }

                        sumL += w * l0;
                        sumR += w * rv;
                    }

                    if (! g.endedL && ! rg.advance (g.posL, g.dirL, c.stepBase * (double) g.ratioL))
                        g.endedL = true;

                    if (! g.endedR && ! rg.advance (g.posR, g.dirR, c.stepBase * (double) g.ratioR))
                        g.endedR = true;

                    ++g.age;

                    if (finished || g.age >= g.len || (g.endedL && g.endedR))
                        g.active = false;
                }

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
        struct Grain
        {
            bool active = false, endedL = false, endedR = false;
            int age = 0, len = 2;
            int dirL = 1, dirR = 1;
            double posL = 0.0, posR = 0.0;
            float ratioL = 1.0f, ratioR = 1.0f, gain = 1.0f;
            GrainWindow win;
            int relLeft = 0, relTotal = 1;
            int inLeft = 0, inTotal = 1;
            std::uint32_t serial = 0;
        };

        struct Ctx
        {
            const GrainSpan& sp;
            const RegionFold& rg;
            const SampleData& sd;
            double grainRate;
            double stepBase;                     // source frames per grain-rate sample at the voice pitch
            int fadeLen;
        };

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
            const double hop = std::max (1.0, hopSeconds (curSched, c.sp.sizeMs, c.sp.shape) * c.grainRate);
            spawn (c, std::round (hop), fadeIn);                 // rounded: a hop that is a whole number of cycles stays coherent
            spawn (c, 0.0, fadeIn);
            countdown = hop;
        }

        void spawn (const Ctx& c, double ageSamples, bool fadeIn) noexcept
        {
            // four draws per spawn, always (the stream must not depend on the parameters)
            const float rPos = bipolar(), rSize = bipolar(), rRev = unit(), rVol = unit();

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
            const int age0 = (int) std::floor (ageSamples);

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
            const float ratioL = (float) (1.0 / detuneRatio), ratioR = (float) detuneRatio;
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

            bool endL = false, endR = false;

            if (age0 > 0)
            {
                endL = ! rg.advance (pL, dL, (double) age0 * c.stepBase * (double) ratioL);
                endR = ! rg.advance (pR, dR, (double) age0 * c.stepBase * (double) ratioR);
            }

            Grain g;
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
            g.win = makeWindow (loop, sp.shape, sizeSec);
            g.gain = g.win.gain * (float) std::pow (10.0, -6.0 * v * (double) rVol / 20.0);
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
            }
        }

        std::array<Grain, static_cast<std::size_t> (poolSize)> pool {};
        std::array<HalfbandStageState, 2> up {};

        std::uint32_t rng = 0x9E3779B9u;
        bool running = false;
        int curSched = 0;
        double scanPhaseV = 0.0;
        double countdown = 0.0;
        std::int64_t sampleClock = 0;
        std::uint32_t serialCounter = 0, newestSerial = 0;
        int newestSlot = -1;
        double lastSpawnFrames = -1.0;
        int spawnedCount = 0, droppedCount = 0;
    };
}
