#include "DSP/ShaperEngine.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace warpr
{
    namespace
    {
        constexpr int kHalf = ShaperLutSet::kCells / 2;                  // table index of x = 0

        double nodeX (int i) noexcept
        {
            return (double) (i - kHalf) * ShaperLutSet::kCell;           // exact: i - 2048 over 2048
        }

        /** 3-point Gauss-Legendre quadrature of f over [a, b] (exact for polynomials up to degree 5, so a
            cubic Hermite / linear segment is integrated exactly; it never evaluates at the end points, which
            keeps it robust against a jump sitting exactly on a control point). */
        template <class F>
        double gauss3 (F&& f, double a, double b)
        {
            const double c = 0.5 * (a + b), h = 0.5 * (b - a);
            constexpr double k = 0.7745966692414834;                     // sqrt(3/5)
            return h * ((5.0 / 9.0) * (f (c - k * h) + f (c + k * h)) + (8.0 / 9.0) * f (c));
        }
    }

    //==========================================================================
    // LUT builder (message thread)
    //==========================================================================
    void ShaperLutSet::fillCurve (ShaperLutSet& s, int c, const ShaperCurve& curve, bool symmetric)
    {
        auto& R = s.r_[(size_t) c];
        auto& G = s.g_[(size_t) c];

        // Identity special case: both interpolators reproduce y = x through collinear points analytically,
        // but evaluating the Hermite form and the Gauss panels leaves ~1 ulp dust in the tables.  The
        // remainder of the identity is EXACTLY zero, and the bit-exact null guarantee depends on it.
        if (curve.isIdentity())
        {
            R.fill (0.0);
            G.fill (0.0);
            s.kinkOf[(size_t) c] = 0.0;
            s.peakOf[(size_t) c] = 1.0;
            s.minOsOf[(size_t) c] = 1;
            return;
        }

        const ShaperCurve::Evaluator ev (curve, symmetric);

        // ---- nodes: r_i = f(x_i) - x_i.  Symmetric curves are built for x >= 0 and mirrored (exactly odd).
        const int first = symmetric ? kHalf : 0;

        for (int i = first; i <= kCells; ++i)
        {
            const double x = nodeX (i);
            R[(size_t) i] = ev (x) - x;
        }

        if (symmetric)
            for (int i = 0; i < kHalf; ++i)
                R[(size_t) i] = -R[(size_t) (kCells - i)];

        // ---- antiderivative: exact cell integrals of the TRUE piecewise polynomial remainder.  Cells that
        //      contain control points are split there, so every panel lies inside ONE polynomial segment.
        const std::vector<double> splits = ev.breakpoints();
        auto rTrue = [&ev] (double x) { return ev (x) - x; };

        auto cellIntegral = [&] (int i)
        {
            const double a = nodeX (i), b = nodeX (i + 1);
            double total = 0.0, lo = a;

            for (auto it = std::upper_bound (splits.begin(), splits.end(), a); it != splits.end() && *it < b; ++it)
            {
                total += gauss3 (rTrue, lo, *it);
                lo = *it;
            }

            return total + gauss3 (rTrue, lo, b);
        };

        if (symmetric)
        {
            G[(size_t) kHalf] = 0.0;

            for (int i = kHalf; i < kCells; ++i)
                G[(size_t) (i + 1)] = G[(size_t) i] + cellIntegral (i);

            for (int i = 0; i < kHalf; ++i)
                G[(size_t) i] = G[(size_t) (kCells - i)];                // g is exactly even
        }
        else
        {
            G[0] = 0.0;

            for (int i = 0; i < kCells; ++i)
                G[(size_t) (i + 1)] = G[(size_t) i] + cellIntegral (i);
        }

        // ---- hardness: the largest slope jump per cell, |r[i+1] - 2 r[i] + r[i-1]| / h
        double kinkMax = 0.0, peakMax = 0.0;

        for (int i = 1; i < kCells; ++i)
            kinkMax = std::max (kinkMax, std::abs (R[(size_t) (i + 1)] - 2.0 * R[(size_t) i] + R[(size_t) (i - 1)]) * kPerUnit);

        for (int i = 0; i <= kCells; ++i)
            peakMax = std::max (peakMax, std::abs (R[(size_t) i] + nodeX (i)));

        s.kinkOf[(size_t) c] = kinkMax;
        s.peakOf[(size_t) c] = peakMax;
        s.minOsOf[(size_t) c] = minOsFromKink (kinkMax);
    }

    ShaperLutSet::Ptr ShaperLutSet::build (const ShaperCurve& a, const ShaperCurve& b, bool symmetric)
    {
        Ptr set (new ShaperLutSet());
        set->odd = symmetric;
        fillCurve (*set, 0, a, symmetric);
        fillCurve (*set, 1, b, symmetric);
        return set;
    }

    //==========================================================================
    // Engine
    //==========================================================================
    ShaperEngine::~ShaperEngine()
    {
        if (prev != nullptr)
            prev->decReferenceCount();                     // processor teardown: audio is stopped
    }

    void ShaperEngine::prepare (double sampleRate)
    {
        fadeLen = std::max (1, (int) std::lround (0.010 * sampleRate));
        fadeDone = fadeLen;
        fading = false;

        if (prev != nullptr)
        {
            prev->decReferenceCount();                     // audio is stopped: direct release is safe
            prev = nullptr;
        }
    }

    void ShaperEngine::publish (ShaperLutSet::Ptr set)
    {
        if (set == nullptr)
            return;

        {
            const juce::ScopedLock sl (latestLock);
            latestPtr = set;
        }

        set->incReferenceCount();                          // one reference travels with the publication
        swap.publish (set.get());
    }

    ShaperLutSet::Ptr ShaperEngine::latest() const
    {
        const juce::ScopedLock sl (latestLock);
        return latestPtr;
    }

    void ShaperEngine::releaseThunk (void* p) noexcept
    {
        static_cast<ShaperLutSet*> (p)->decReferenceCount();
    }

    void ShaperEngine::tryReleasePrev() noexcept
    {
        if (prev != nullptr && ! fading && retire.hasSpace() && retire.push (prev, &ShaperEngine::releaseThunk))
            prev = nullptr;
    }

    void ShaperEngine::beginBlock() noexcept
    {
        tryReleasePrev();

        if (prev != nullptr)
            return;                                        // finish the running crossfade first (<= 10 ms)

        ShaperLutSet* before = swap.get();

        // Take our own reference BEFORE the swap retires its unit: once the retire list holds it, the message
        // thread may release it at any moment.
        if (before != nullptr)
            before->incReferenceCount();

        if (swap.acquire())
        {
            if (before != nullptr)
            {
                prev = before;
                fadeDone = 0;
                fading = true;
            }
        }
        else if (before != nullptr)
        {
            before->decReferenceCount();                   // the swap still owns one: this is never the last reference
        }
    }

    void ShaperEngine::advance (int n) noexcept
    {
        if (! fading)
            return;

        fadeDone += n;

        if (fadeDone >= fadeLen)
        {
            fadeDone = fadeLen;
            fading = false;
            tryReleasePrev();
        }
    }
}
