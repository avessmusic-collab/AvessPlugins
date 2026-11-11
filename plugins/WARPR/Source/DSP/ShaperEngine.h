#pragma once
/*
  ShaperEngine - the A/B transfer-curve waveshaper (architecture C6, phase D4).

  DATA  (ShaperLutSet, built on the MESSAGE thread, immutable, ref-counted, published through LockFreeSwap)
    Two curves (A, B) sampled on a uniform 4097-point grid over [-1, 1] (cell width h = 1/2048).  Per curve:
        r[i] = f(x_i) - x_i                       the REMAINDER of the curve (f minus identity)
        g[i] = integral_{-1}^{x_i} r(s) ds        its antiderivative, accumulated in double from the EXACT piecewise
                                                  polynomial curve (Gauss quadrature split at every control point)
    Everything is stored in double: a float antiderivative table would turn the ADAA quotient into noise as dx -> 0.
    Reconstruction inside a cell (t = (x - x_i)/h):
        G(x) = cubic Hermite of (g_i, g_i+1) with derivatives (r_i, r_i+1)           -> the ADAA kernel F1
        r(x) = G'(x) = 6 t (1-t) d + r_i (1 - 4t + 3t^2) + r_i+1 (3t^2 - 2t),  d = (g_i+1 - g_i)/h
    so F1' == f_lut by construction (the pair is exactly consistent) and the cell integral of the LUT is exact.
    Outside [-1, 1] the curve is held flat (f = f(+-1)); r and G extend analytically, so the clamp itself is
    antialiased and nothing is cut off at the table edge.

    Why the REMAINDER (the D2 lesson): plain first-order ADAA of the identity is a two-sample average.  With
    y = x + ADAA(r) the identity curve stores r == 0, g == 0 and the stage returns the input BIT-EXACTLY; a stage
    whose curve is nearly the identity is inaudibly different from a bypass.

    SYMMETRIC sets are exactly odd: the tables are built for x >= 0 and mirrored (r odd, g even, bit-exact) and the
    evaluator works on |x| and restores the sign.

    Morph: y = (1 - m) A(x) + m B(x), and the ADAA kernel is the same blend of the two antiderivatives, so morph is
    linear in f (tested).  m == 0 / m == 1 read one table only.

    HARDNESS (risk R1(b), the shaper's analogue of the WarpModeTable minOS hints).  First-order ADAA cannot give
    a hard-clip-like curve 12 dB at 1x (it measured ~7 dB on the D3 hard clip); so each curve carries a kink
    metric K = max_i |second difference of r| / h  (the largest slope jump per LUT cell: ~2 for a slope-2 hard clip,
    ~f''/2048 for a smooth curve) and a minOS derived from it:
        K >= 0.25  -> 4     (hard-clip / fold / zigzag: a real corner)
        K >= 0.02  -> 2     (a tight but rounded knee)
        otherwise  -> 1     (smooth curves, the identity)
    WarprSynth enforces osLatched = max(user, minOS) while the shaper is enabled with mix > 0 (same path as the warp
    stage's minOS).  Thresholds are tuning points (see PhaseD4Tests, the printed alias table).

  AUDIO THREAD
    ShaperEngine::beginBlock() adopts a pending set (one atomic exchange).  The previous set stays referenced by the
    engine for a 10 ms crossfade (weight of the NEW set rises linearly per base sample) and is then handed to the
    RetireList - the last release never happens on the audio thread.  Publications that arrive during a fade wait
    for it to finish (<= 10 ms) and are then adopted; intermediate ones are simply skipped.

    ShaperOscState is the per-oscillator stage (one ADAA history per channel).  Bypass (span.active == false) never
    touches the signal; enabled with an identity curve or mix 0 it returns the input bit-exactly.
*/

#include "DSP/Adaa.h"
#include "DSP/ShaperCurve.h"
#include "Util/LockFreeSwap.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <array>
#include <cmath>

namespace warpr
{
    //==========================================================================
    class ShaperLutSet : public juce::ReferenceCountedObject
    {
    public:
        using Ptr = juce::ReferenceCountedObjectPtr<ShaperLutSet>;

        static constexpr int    kCells   = 4096;
        static constexpr int    kPoints  = kCells + 1;
        static constexpr double kPerUnit = 2048.0;             // cells per unit x
        static constexpr double kCell    = 1.0 / 2048.0;       // h

        /** MESSAGE THREAD.  Builds both curves (A = index 0, B = index 1).  `symmetric` = shaperSymmetry == Symmetric. */
        static Ptr build (const ShaperCurve& a, const ShaperCurve& b, bool symmetric);

        /** kink metric -> minOS (see the header comment). */
        static int minOsFromKink (double kink) noexcept
        {
            return kink >= 0.25 ? 4 : (kink >= 0.02 ? 2 : 1);
        }

        //======================================================================
        bool   isOdd() const noexcept                       { return odd; }
        int    minOs (int curve) const noexcept             { return minOsOf[(size_t) curve]; }
        double kink (int curve) const noexcept              { return kinkOf[(size_t) curve]; }
        double peak (int curve) const noexcept              { return peakOf[(size_t) curve]; }

        /** minOS for the curve mix the given morph value uses (morph ~ 0: A only, ~ 1: B only). */
        int minOsFor (float morph) const noexcept
        {
            if (morph <= 0.001f) return minOsOf[0];
            if (morph >= 0.999f) return minOsOf[1];
            return std::max (minOsOf[0], minOsOf[1]);
        }

        const std::array<double, kPoints>& remainder (int curve) const noexcept       { return r_[(size_t) curve]; }
        const std::array<double, kPoints>& antiderivative (int curve) const noexcept  { return g_[(size_t) curve]; }

        //======================================================================
        // Evaluation (audio-thread safe, allocation free).  x must be finite.

        /** r(x) = f(x) - x of one curve. */
        double rAt (int c, double x) const noexcept
        {
            if (odd)
            {
                const double v = rRaw (c, std::abs (x));
                return x < 0.0 ? -v : v;
            }

            return rRaw (c, x);
        }

        /** G(x) = integral of r of one curve (the ADAA kernel F1 of the remainder). */
        double gAt (int c, double x) const noexcept         { return gRaw (c, odd ? std::abs (x) : x); }

        /** f(x) of one curve (the LUT reconstruction). */
        double fAt (int c, double x) const noexcept         { return x + rAt (c, x); }

        double rBlend (double m, double x) const noexcept
        {
            if (m <= 0.0) return rAt (0, x);
            if (m >= 1.0) return rAt (1, x);
            return (1.0 - m) * rAt (0, x) + m * rAt (1, x);
        }

        double gBlend (double m, double x) const noexcept
        {
            if (m <= 0.0) return gAt (0, x);
            if (m >= 1.0) return gAt (1, x);
            return (1.0 - m) * gAt (0, x) + m * gAt (1, x);
        }

        double fBlend (double m, double x) const noexcept   { return x + rBlend (m, x); }

    private:
        ShaperLutSet() = default;

        static void fillCurve (ShaperLutSet& set, int c, const ShaperCurve& curve, bool symmetric);

        double rRaw (int c, double x) const noexcept
        {
            const double* R = r_[(size_t) c].data();

            if (x > 1.0)  return R[kCells] + 1.0 - x;          // flat extension: f = f(1)
            if (x < -1.0) return R[0] - 1.0 - x;               // f = f(-1)

            const double u = (x + 1.0) * kPerUnit;
            int i = (int) u;
            if (i >= kCells) i = kCells - 1;

            const double t = u - (double) i;
            const double* G = g_[(size_t) c].data();
            const double d = (G[i + 1] - G[i]) * kPerUnit;

            return d * 6.0 * t * (1.0 - t) + R[i] * (1.0 - t * (4.0 - 3.0 * t)) + R[i + 1] * (t * (3.0 * t - 2.0));
        }

        double gRaw (int c, double x) const noexcept
        {
            const double* R = r_[(size_t) c].data();
            const double* G = g_[(size_t) c].data();

            if (x > 1.0)
                return G[kCells] + (R[kCells] + 1.0) * (x - 1.0) - 0.5 * (x * x - 1.0);
            if (x < -1.0)
                return G[0] + (R[0] - 1.0) * (x + 1.0) - 0.5 * (x * x - 1.0);

            const double u = (x + 1.0) * kPerUnit;
            int i = (int) u;
            if (i >= kCells) i = kCells - 1;

            const double t = u - (double) i;
            const double t2 = t * t, t3 = t2 * t;

            return (2.0 * t3 - 3.0 * t2 + 1.0) * G[i] + (t3 - 2.0 * t2 + t) * kCell * R[i]
                 + (3.0 * t2 - 2.0 * t3) * G[i + 1] + (t3 - t2) * kCell * R[i + 1];
        }

        std::array<double, kPoints> r_[2] {}, g_[2] {};
        bool odd = false;
        std::array<int, 2> minOsOf { 1, 1 };
        std::array<double, 2> kinkOf { 0.0, 0.0 };
        std::array<double, 2> peakOf { 0.0, 0.0 };
    };

    //==========================================================================
    /** Per-render-span description of the shaper stage (built by the voice once per 32-sample chunk). */
    struct ShaperSpan
    {
        bool  active = false;                        // false: stage bypassed (bit-exact), ADAA history dropped
        const ShaperLutSet* cur = nullptr;           // the current set
        const ShaperLutSet* prev = nullptr;          // the set being faded out (nullptr when no fade is running)
        float morph0 = 0.0f, morph1 = 0.0f;
        float mix0 = 1.0f,   mix1 = 1.0f;            // shaperMix * enable ramp
        float w0 = 1.0f,     w1 = 1.0f;              // weight of `cur` in the A/B-set crossfade (1 = fully new)
    };

    namespace shaperdsp
    {
        /** First-order ADAA of the REMAINDER r with the given morph (D2 lesson: ADAA on f - x, then add x back).
            Fallback when |dx| < 1e-5 is the 3-point Simpson mean of r over [xPrev, x]. */
        inline double adaaRemainder (const ShaperLutSet& s, double m, double x, double xPrev, bool valid) noexcept
        {
            if (! valid)
                return s.rBlend (m, x);

            const double dx = x - xPrev;

            if (std::abs (dx) < adaa::dxEps)
                return (s.rBlend (m, xPrev) + 4.0 * s.rBlend (m, 0.5 * (xPrev + x)) + s.rBlend (m, x)) * (1.0 / 6.0);

            return (s.gBlend (m, x) - s.gBlend (m, xPrev)) / dx;
        }
    }

    /** One channel's ADAA history + the A/B-set crossfade. */
    struct ShaperChannelState
    {
        adaa::State st;

        void reset() noexcept { st.reset(); }

        /** Returns the antialiased REMAINDER for input x (output = x + mix * remainder). */
        double step (const ShaperSpan& sp, double morph, double w, double x) noexcept
        {
            if (! (std::abs (x) < 1.0e6))
                x = 0.0;                                         // NaN / inf / absurd: the stage is the identity for it

            double yr = shaperdsp::adaaRemainder (*sp.cur, morph, x, st.xPrev, st.valid);

            if (sp.prev != nullptr && w < 1.0)
            {
                const double yp = shaperdsp::adaaRemainder (*sp.prev, morph, x, st.xPrev, st.valid);
                yr = yp + w * (yr - yp);
            }

            st.xPrev = x;
            st.valid = true;
            return yr;
        }
    };

    /** The per-oscillator shaper stage (lives in UnisonOsc). */
    class ShaperOscState
    {
    public:
        /** The stage is bypassed this span: forget the ADAA history (a re-enable starts clean). */
        void idle() noexcept
        {
            ch[0].reset();
            ch[1].reset();
        }

        /** l / r: the (warped) read value in, the shaped value out.  A mono source runs one channel and duplicates. */
        void process (const ShaperSpan& sp, double morph, double mix, double w, float& l, float& r, bool stereo) noexcept
        {
            const double xl = (double) l;
            const float outL = (float) (xl + mix * ch[0].step (sp, morph, w, xl));

            if (stereo)
            {
                const double xr = (double) r;
                r = (float) (xr + mix * ch[1].step (sp, morph, w, xr));
            }
            else
            {
                r = outL;
            }

            l = outL;
        }

    private:
        ShaperChannelState ch[2];
    };

    //==========================================================================
    /** Owns the published sets and the audio-side crossfade bookkeeping. */
    class ShaperEngine
    {
    public:
        explicit ShaperEngine (RetireList& retireListToUse) noexcept : retire (retireListToUse), swap (retireListToUse) {}
        ~ShaperEngine();

        ShaperEngine (const ShaperEngine&) = delete;
        ShaperEngine& operator= (const ShaperEngine&) = delete;

        //======================================================================
        // NON-AUDIO threads

        /** Sample-rate dependent: the crossfade length (10 ms).  Audio must be stopped; drops a running fade. */
        void prepare (double sampleRate);

        /** Publishes a freshly built set to the audio thread. */
        void publish (ShaperLutSet::Ptr set);

        /** The most recently published set (never the audio thread's private view). */
        ShaperLutSet::Ptr latest() const;

        //======================================================================
        // AUDIO thread

        /** Block start: adopts a pending set; a replaced current set is faded out over 10 ms. */
        void beginBlock() noexcept;

        const ShaperLutSet* current() const noexcept        { return swap.get(); }
        const ShaperLutSet* previous() const noexcept       { return fading ? prev : nullptr; }
        bool isFading() const noexcept                      { return fading && prev != nullptr; }
        int  fadeBase() const noexcept                      { return fadeDone; }       // base samples elapsed in the fade
        int  fadeLength() const noexcept                    { return fadeLen; }

        /** Called after every rendered sub-range of `n` base samples. */
        void advance (int n) noexcept;

        /** minOS of the current set for the given morph (1 before the first set is adopted). */
        int minOsFor (float morph) const noexcept
        {
            const auto* s = swap.get();
            return s != nullptr ? s->minOsFor (morph) : 1;
        }

    private:
        void tryReleasePrev() noexcept;
        static void releaseThunk (void* p) noexcept;

        RetireList& retire;
        LockFreeSwap<ShaperLutSet, RefCountRelease<ShaperLutSet>> swap;

        ShaperLutSet* prev = nullptr;                  // audio-thread owned; holds ONE extra reference
        bool fading = false;
        int  fadeDone = 0, fadeLen = 480;

        mutable juce::CriticalSection latestLock;      // non-audio threads only
        ShaperLutSet::Ptr latestPtr;
    };
}
