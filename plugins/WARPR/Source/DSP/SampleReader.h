#pragma once
/*
  SampleReader - fractional-position stereo read with mip selection (architecture C2).

  Primary interpolator: 8-tap Kaiser-windowed sinc, 512 sub-phases, linear interpolation between
  adjacent phase rows.  The kernel is an INTERPOLATING sinc (cutoff = Nyquist, window beta 5): at
  integer positions it returns the stored sample exactly (row 0 is an exact delta), which keeps
  rho = 1 playback and the single-cycle wave bit-exact.  An 8-tap kernel cannot also be a sharp
  anti-imaging filter; band-limiting for pitched-up playback is done by the mip pyramid, not here.
  (The architecture's ">80 dB stopband" figure for the 8-tap kernel is NOT claimed or measured here.)

  Draft interpolator (Settings "Interpolation: Eco"): 4-point Hermite (Catmull-Rom).

  INTERPOLATION QUALITY (Settings, D10) - exactly what each level changes.  The level reaches makeReadParams as `quality`
  (WarprSynth -> VoiceParams::interpQuality -> UnisonOsc::render -> ReadParamsCache / makeReadParams) and sets
  ReadParams::hermite for EVERY read of the voice: Classic playback, warp-shifted reads (the phase warps, the dry read),
  the Sample-source modulator (rpMod) and every grain read.  Nothing else changes: the mip pyramid, the mip level
  selection / blend weights, the loop-wrapped taps, the seam crossfade and the one-shot end fade are identical at both levels.
      0  High (default)  8-tap Kaiser sinc, 512 sub-phases + row interpolation: 8 loads from the source, 8 x 2 table loads,
                         ~24 flops per channel per read.  This is the pre-D10 behaviour, bit-exact.
      1  Eco             4 loads, ~10 flops per channel per read (about 1/3 of the arithmetic and 1/4 of the source
                         loads).  Catmull-Rom is exact at integer positions (so unity-ratio playback is identical) but its
                         passband droops (worst case, a half-sample read: -0.46 dB at 0.2 fs, -4.0 dB at 0.35 fs) and it images
                         more near Nyquist, so very bright material played at non-integer ratios loses some top end and
                         gains a little aliasing.  Expected saving: the read is the largest single part of an oscillator
                         sample, so `--bench` (rows "typical" vs "typical eco") measures it per patch.
  The level is applied to sounding voices as well as new ones (it is a global per-block setting, not latched per note).

  Mip selection: lambda = log2(rho) for rho > 1 (source frames per host sample), clamped to the
  available levels.  Level floor(lambda) is blended with level floor(lambda)+1 by
  w = min(1, 2 * frac(lambda)): a pure level at exact octaves, a full upper level from half an
  octave on (this keeps the lower, less band-limited level from leaking aliasing for most of
  the octave - a deliberate refinement of the plain linear blend).

  Loop-wrapped neighbourhood: for seamless forward loops (no seam crossfade) the taps that fall
  outside the loop region are fetched from the opposite end of the region instead of the file
  neighbours, so periodic material loops without a band-limiting error at the seam.

  Everything here is allocation-free and safe on the audio thread.
*/

#include "DSP/MipBuilder.h"
#include "DSP/SampleData.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <new>

namespace warpr
{
    //==========================================================================
    class SincTable
    {
    public:
        static constexpr int taps   = 8;
        static constexpr int phases = 512;
        static constexpr int rows   = phases + 2;           // +1 for the interpolation partner, +1 guard

        static const SincTable& get()
        {
            static const SincTable instance;
            return instance;
        }

        const float* row (int r) const noexcept { return table.data() + (size_t) r * (size_t) taps; }

    private:
        SincTable()
        {
            constexpr double beta = 5.0;
            const double pi = 3.14159265358979323846;
            const double i0Beta = mip::besselI0 (beta);

            for (int r = 0; r < rows; ++r)
            {
                const double frac = (double) r / (double) phases;
                std::array<double, (size_t) taps> h {};
                double sum = 0.0;

                for (int t = 0; t < taps; ++t)
                {
                    const double x = (double) (t - 3) - frac;
                    const double nearest = std::round (x);
                    double v;

                    if (std::abs (x - nearest) < 1.0e-12)
                        v = std::abs (nearest) < 0.5 ? 1.0 : 0.0;       // exact interpolating kernel at integer offsets
                    else
                        v = std::sin (pi * x) / (pi * x);

                    const double u = x / 4.0;
                    const double w = std::abs (u) >= 1.0 ? 0.0 : mip::besselI0 (beta * std::sqrt (1.0 - u * u)) / i0Beta;

                    h[(size_t) t] = v * w;
                    sum += h[(size_t) t];
                }

                for (int t = 0; t < taps; ++t)
                    table[(size_t) r * (size_t) taps + (size_t) t] = (float) (h[(size_t) t] / sum);
            }
        }

        std::array<float, (size_t) (rows * taps)> table {};
    };

    //==========================================================================
    /** One mip level as seen by the reader. */
    struct LevelView
    {
        const float* ch0 = nullptr;
        const float* ch1 = nullptr;          // == ch0 for mono data
        double invScale = 1.0;               // level position = level-0 position * invScale
        bool   wrap = false;                 // loop-wrapped neighbourhood active
        double wrapLo = 0.0, wrapHi = 0.0;   // region in level frames
        int    wrapLoI = 0, wrapLen = 1;
    };

    struct ReadParams
    {
        LevelView lv[2];
        float wHigh = 0.0f;                  // weight of lv[1]
        bool hermite = false;
        bool stereo = false;
    };

    /** Builds the per-chunk read parameters.  `rho` = source frames per host sample (average over
        the chunk).  `wrapRegion` enables the loop-wrapped neighbourhood for [lo, hi) (level-0 frames). */
    inline ReadParams makeReadParams (const SampleData& sd, double rho, int quality,
                                      bool wrapRegion, double lo, double hi) noexcept
    {
        ReadParams rp;
        rp.hermite = quality == 1;
        rp.stereo = sd.numChannels > 1;

        const int maxLevel = sd.numLevels - 1;
        double lambda = rho > 1.0 ? std::log2 (rho) : 0.0;
        lambda = std::min (lambda, (double) maxLevel);

        int lowLevel = (int) std::floor (lambda);
        double frac = lambda - (double) lowLevel;

        if (lowLevel >= maxLevel)
        {
            lowLevel = maxLevel;
            frac = 0.0;
        }

        const float w = (float) std::min (1.0, 2.0 * frac);
        rp.wHigh = w > 1.0e-4f ? w : 0.0f;

        const int used = rp.wHigh > 0.0f ? 2 : 1;

        for (int i = 0; i < used; ++i)
        {
            const int k = lowLevel + i;
            const auto& level = sd.levels[(size_t) k];
            auto& v = rp.lv[i];

            v.ch0 = level.channel (0);
            v.ch1 = rp.stereo ? level.channel (1) : v.ch0;
            v.invScale = std::ldexp (1.0, -k);

            if (wrapRegion)
            {
                v.wrap = true;
                v.wrapLo = lo * v.invScale;
                v.wrapHi = hi * v.invScale;
                v.wrapLoI = std::max (0, (int) std::llround (v.wrapLo));
                const int hiI = std::min (level.numFrames, (int) std::llround (v.wrapHi));
                v.wrapLen = std::max (1, hiI - v.wrapLoI);
            }
        }

        return rp;
    }

    //==========================================================================
    inline float readLevel (const float* base, double levelPos, const LevelView& v, bool hermite) noexcept
    {
        const double fl = std::floor (levelPos);
        const int i = (int) fl;
        const float f = (float) (levelPos - fl);

        const int count = hermite ? 4 : 8;
        const int first = hermite ? i - 1 : i - 3;

        float gathered[8];
        const float* src;

        if (v.wrap && (levelPos - v.wrapLo < 6.0 || v.wrapHi - levelPos < 7.0))
        {
            for (int t = 0; t < count; ++t)
            {
                int rel = first + t - v.wrapLoI;

                if (rel < 0 || rel >= v.wrapLen)
                {
                    rel %= v.wrapLen;
                    if (rel < 0)
                        rel += v.wrapLen;
                }

                gathered[t] = base[v.wrapLoI + rel];
            }

            src = gathered;
        }
        else
        {
            src = base + first;
        }

        if (hermite)
        {
            const float ym1 = src[0], y0 = src[1], y1 = src[2], y2 = src[3];
            const float c1 = 0.5f * (y1 - ym1);
            const float c2 = ym1 - 2.5f * y0 + 2.0f * y1 - 0.5f * y2;
            const float c3 = 0.5f * (y2 - ym1) + 1.5f * (y0 - y1);
            return ((c3 * f + c2) * f + c1) * f + y0;
        }

        const auto& table = SincTable::get();
        const float ph = f * (float) SincTable::phases;
        int r = (int) ph;

        if (r > SincTable::phases)
            r = SincTable::phases;

        const float w = ph - (float) r;
        const float* h0 = table.row (r);
        const float* h1 = table.row (r + 1);

        float acc = 0.0f;

        for (int t = 0; t < SincTable::taps; ++t)
            acc += (h0[t] + w * (h1[t] - h0[t])) * src[t];

        return acc;
    }

    //==========================================================================
    /** SPEED-AWARE MIP SELECTION (architecture C2, finalised in D6).

        A read that travels `mult` times faster than the chunk's nominal speed (a phase warp that speeds the read up, a
        modulator playhead at ratio x, a Cloud grain transposed up) must come from a mip level chosen for the SPEED IT
        ACTUALLY HAS, or its content folds.  `mipIndexForMult` quantises the multiplier to half-octave steps ROUNDED UP
        (so the effective speed is never underestimated): 0 for mult < 1.09 (the chunk's own parameters, i.e. a read at
        nominal speed is bit-identical to the old behaviour), else ceil (2 log2 mult), at most 8 (16x).  The reader
        parameters of index i are makeReadParams (rho * 2^(i/2)).  ReadParamsCache builds them lazily (placement into
        raw storage, no initialisation of unused slots): the common case touches slot 0 only. */
    inline int mipIndexForMult (double mult) noexcept
    {
        if (! (mult >= 1.09))
            return 0;

        const double v = 2.0 * std::log2 (mult);
        const int i = (int) std::ceil (v - 1.0e-9);
        return i < 1 ? 1 : (i > 8 ? 8 : i);
    }

    class ReadParamsCache
    {
    public:
        static constexpr int slots = 9;

        void reset (const SampleData& data, double rhoBase, int qualityIn, bool wrapRegionIn, double loIn, double hiIn) noexcept
        {
            sd = &data;
            rho = rhoBase;
            quality = qualityIn;
            wrapRegion = wrapRegionIn;
            lo = loIn;
            hi = hiIn;
            valid = 0u;
        }

        const ReadParams& get (int index) noexcept
        {
            const int i = index < 0 ? 0 : (index >= slots ? slots - 1 : index);
            auto* slot = reinterpret_cast<ReadParams*> (&storage[(size_t) i]);

            if ((valid & (1u << i)) == 0u)
            {
                new (slot) ReadParams (makeReadParams (*sd, i == 0 ? rho : rho * std::exp2 (0.5 * (double) i), quality, wrapRegion, lo, hi));
                valid |= 1u << i;
            }

            return *slot;
        }

        const ReadParams& forMult (double mult) noexcept { return get (mipIndexForMult (mult)); }

    private:
        const SampleData* sd = nullptr;
        double rho = 1.0, lo = 0.0, hi = 1.0;
        int quality = 0;
        bool wrapRegion = false;
        unsigned valid = 0u;

        struct Slot
        {
            alignas (ReadParams) unsigned char bytes[sizeof (ReadParams)];
        };

        Slot storage[(size_t) slots];
    };

    /** Reads one frame at level-0 position `pos` (source frames), both channels. */
    inline void readFrame (const ReadParams& rp, double pos, float& outL, float& outR) noexcept
    {
        const auto& a = rp.lv[0];
        const double pa = pos * a.invScale;

        float l = readLevel (a.ch0, pa, a, rp.hermite);
        float r = rp.stereo ? readLevel (a.ch1, pa, a, rp.hermite) : l;

        if (rp.wHigh > 0.0f)
        {
            const auto& b = rp.lv[1];
            const double pb = pos * b.invScale;

            const float l2 = readLevel (b.ch0, pb, b, rp.hermite);
            const float r2 = rp.stereo ? readLevel (b.ch1, pb, b, rp.hermite) : l2;

            l += rp.wHigh * (l2 - l);
            r += rp.wHigh * (r2 - r);
        }

        outL = l;
        outR = r;
    }
}
