#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace prism
{
    // Correlation / width / balance over a 400 ms rolling window (architecture C8).
    // 40 buckets of 10 ms; sums in double; totals are re-summed from the buckets on every
    // read (no running add/subtract, so no drift). Keyed to frame counts, never block sizes.
    class StereoMetrics
    {
    public:
        static constexpr int kBuckets = 40;
        static constexpr double kSilenceGuard = 1.0e-9;   // |den| below this -> "silence"
        static constexpr double kBalanceClampDb = 60.0;

        struct Sums { double ll = 0.0, rr = 0.0, lr = 0.0; };
        struct Result { float correlation = 0.0f, widthPercent = 0.0f, balanceDb = 0.0f; };

        void prepare(double sampleRate)
        {
            chunk = std::max<std::int64_t>(1, std::llround(0.01 * sampleRate));
            reset();
        }

        void reset() noexcept
        {
            buckets.fill({});
            cur = 0;
            filled = 0;
        }

        void process(const float* l, const float* r, int n) noexcept
        {
            for (int i = 0; i < n; ++i)
            {
                const double a = l[i], b = r[i];
                auto& s = buckets[(size_t) cur];
                s.ll += a * a;
                s.rr += b * b;
                s.lr += a * b;
                if (++filled >= chunk)
                {
                    filled = 0;
                    cur = (cur + 1) % kBuckets;
                    buckets[(size_t) cur] = {};   // oldest bucket leaves the window
                }
            }
        }

        Sums getSums() const noexcept
        {
            Sums t;
            for (const auto& s : buckets) { t.ll += s.ll; t.rr += s.rr; t.lr += s.lr; }
            return t;
        }

        Result compute() const noexcept { return fromSums(getSums()); }

        // Meter Definitions (parameter-spec.md), implemented exactly.
        static Result fromSums(const Sums& s) noexcept
        {
            Result r;

            const double den = std::sqrt(s.ll * s.rr);
            if (den >= kSilenceGuard)
                r.correlation = (float) std::clamp(s.lr / den, -1.0, 1.0);

            // M = (L+R)/sqrt2, S = (L-R)/sqrt2  ->  Em = (ll+rr+2lr)/2, Es = (ll+rr-2lr)/2
            const double em = std::max(0.0, 0.5 * (s.ll + s.rr + 2.0 * s.lr));
            const double es = std::max(0.0, 0.5 * (s.ll + s.rr - 2.0 * s.lr));
            const double sm = std::sqrt(em), ss = std::sqrt(es);
            if (sm + ss >= kSilenceGuard)
                r.widthPercent = (float) (100.0 * ss / (ss + sm));

            const double eps = kSilenceGuard * kSilenceGuard;
            if (s.ll < eps && s.rr < eps)
                r.balanceDb = 0.0f;
            else if (s.ll < eps)
                r.balanceDb = (float) kBalanceClampDb;
            else if (s.rr < eps)
                r.balanceDb = (float) -kBalanceClampDb;
            else
                r.balanceDb = (float) std::clamp(10.0 * std::log10(s.rr / s.ll), -kBalanceClampDb, kBalanceClampDb);

            return r;
        }

    private:
        std::array<Sums, kBuckets> buckets {};
        std::int64_t chunk = 480;
        std::int64_t filled = 0;
        int cur = 0;
    };
} // namespace prism
