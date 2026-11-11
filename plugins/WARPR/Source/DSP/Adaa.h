#pragma once
/*
  Adaa - first-order antiderivative antialiasing helpers (architecture C5).

      y[n] = (F1(x[n]) - F1(x[n-1])) / (x[n] - x[n-1])

  A "paired kernel" is any type with
      double f  (double x) const noexcept;     // the static nonlinearity
      double F1 (double x) const noexcept;     // its antiderivative, F1' == f
  Both are built from the same FastMath primitives so the pair stays consistent (property-tested).

  When |x[n] - x[n-1]| < dxEps the quotient is ill-conditioned; the fallback is the 3-point
  Simpson mean of f over [x[n-1], x[n]] (exact for cubics, O(dx^4) otherwise) - better than the
  plain midpoint f((x[n]+x[n-1])/2) of the textbook scheme.  The same Simpson mean is also the
  "numerical ADAA" for functions without a closed-form antiderivative (D3: Soft Fold, non-integer
  Chebyshev fold).

  All arithmetic is double.  State is per channel; `first` samples use the naive f(x).
*/

#include <cmath>

namespace warpr::adaa
{
    inline constexpr double dxEps = 1.0e-5;

    struct State
    {
        double xPrev = 0.0;
        bool   valid = false;

        void reset() noexcept { xPrev = 0.0; valid = false; }
    };

    /** Mean of f over [a, b] by 3-point Simpson quadrature. */
    template <class Kernel>
    inline double simpsonMean (const Kernel& k, double a, double b) noexcept
    {
        return (k.f (a) + 4.0 * k.f (0.5 * (a + b)) + k.f (b)) * (1.0 / 6.0);
    }

    /** First-order ADAA from a paired kernel (stateless form). */
    template <class Kernel>
    inline double firstOrder (const Kernel& k, double x, double xPrev) noexcept
    {
        const double dx = x - xPrev;

        if (std::abs (dx) < dxEps)
            return simpsonMean (k, xPrev, x);

        return (k.F1 (x) - k.F1 (xPrev)) / dx;
    }

    /** Stateful step: updates `st` and returns the antialiased output for input x. */
    template <class Kernel>
    inline double process (State& st, const Kernel& k, double x) noexcept
    {
        const double y = st.valid ? firstOrder (k, x, st.xPrev) : k.f (x);

        st.xPrev = x;
        st.valid = true;
        return y;
    }

    /** Numerical ADAA for kernels that only provide f (Simpson mean over the input interval). */
    template <class Kernel>
    inline double processNumerical (State& st, const Kernel& k, double x) noexcept
    {
        const double y = st.valid ? simpsonMean (k, st.xPrev, x) : k.f (x);

        st.xPrev = x;
        st.valid = true;
        return y;
    }
}
