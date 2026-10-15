#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

namespace prism
{
    // Rekordbox-style 3-band waveform columns (architecture C10), one instance per source. Worker-thread only.
    //
    //  - Input = (L+R)/2 (mono source: its single channel).
    //  - LR4 crossovers at 200 Hz / 2.5 kHz (each LP/HP = two cascaded Butterworth biquads, double precision):
    //      low = LP200(x), rest = HP200(x), mid = LP2500(rest), high = HP2500(rest)
    //  - Column rate is EXACTLY 253/3 = 84.333.. columns/s (1012 px / 12 s window) in integer arithmetic:
    //      column k covers the frames [ceil(k*3*fs/253), ceil((k+1)*3*fs/253)),  i.e. frame f is in column
    //      floor(f*253/(3*fs)).  No accumulated rounding, so there is no drift at any length (fs rounded to an
    //      integer Hz; every standard rate is integral).
    //  - Per column and band: peak |band| -> v = clamp((20 log10(peak) + 54) / 54, 0, 1)  (-54..0 dBFS window;
    //    digital silence gives exactly 0).
    //  - flag: 1 = beat, 2 = bar (overrides) from the audio thread's beat queue, keyed to the absolute frame index.
    //  - 4096-column log; consumers pull "everything newer than index X, newest 16 at most".
    class WaveformExtractor
    {
    public:
        static constexpr int kLogSize = 4096;
        static constexpr std::uint64_t kRateNum = 253, kRateDen = 3;   // columns per second = 253/3
        static constexpr double kFloorDb = -54.0;

        struct Column
        {
            std::uint64_t index = 0;
            float bass = 0.0f, mid = 0.0f, high = 0.0f, flag = 0.0f;
        };

        void prepare(double sampleRate, int numChannels) noexcept
        {
            fsInt = std::max<std::uint64_t>(1, (std::uint64_t) std::llround(sampleRate));
            channels = std::clamp(numChannels, 1, 2);
            designFilters(sampleRate);
            log.assign((size_t) kLogSize, Column {});
            logCount = 0;
            pendingCount = 0;
            skipTo(0);
        }

        // Jump the stream position (used at start and after an editor-inactive period). A column that would
        // start before `abs` is incomplete and is not logged.
        void skipTo(std::uint64_t abs) noexcept
        {
            nextAbs = abs;
            colsDone = abs * kRateNum / (kRateDen * fsInt);
            const std::uint64_t startOfCol = (colsDone * kRateDen * fsInt + kRateNum - 1) / kRateNum;
            dropCurrent = abs > startOfCol;
            resetFilters();
            peaks[0] = peaks[1] = peaks[2] = 0.0;
            // pending beat flags are kept on purpose: events are popped before the first chunk of a pump is
            // processed (which is where a resync happens); stale ones are purged as columns finish.
        }

        void notifyGap() noexcept { resetFilters(); }

        std::uint64_t columnOfFrame(std::uint64_t frame) const noexcept { return frame * kRateNum / (kRateDen * fsInt); }

        // Mark the column that contains `frame` with a beat (1) or bar (2) flag.
        void markBeat(std::uint64_t frame, bool isBar) noexcept
        {
            const float f = isBar ? 2.0f : 1.0f;
            const std::uint64_t k = columnOfFrame(frame);
            if (k < colsDone)   // column already finished: patch the log if it is still there
            {
                for (int i = 0; i < std::min<std::uint64_t>(logCount, kLogSize); ++i)
                {
                    auto& c = log[(size_t) ((logCount - 1 - (std::uint64_t) i) % kLogSize)];
                    if (c.index == k) { c.flag = std::max(c.flag, f); return; }
                    if (c.index < k) return;
                }
                return;
            }
            for (int i = 0; i < pendingCount; ++i)
                if (pending[(size_t) i].first == k) { pending[(size_t) i].second = std::max(pending[(size_t) i].second, f); return; }
            if (pendingCount < (int) pending.size())
                pending[(size_t) pendingCount++] = { k, f };
        }

        void process(const float* l, const float* r, int n) noexcept
        {
            for (int i = 0; i < n; ++i)
            {
                const double x = channels == 2 ? 0.5 * ((double) l[i] + (double) r[i]) : (double) l[i];
                const double low  = cascade(lp200, x);
                const double rest = cascade(hp200, x);
                const double mid  = cascade(lp2500, rest);
                const double high = cascade(hp2500, rest);
                peaks[0] = std::max(peaks[0], std::abs(low));
                peaks[1] = std::max(peaks[1], std::abs(mid));
                peaks[2] = std::max(peaks[2], std::abs(high));

                ++nextAbs;
                if (nextAbs * kRateNum >= (colsDone + 1) * kRateDen * fsInt)
                    finishColumn();
            }
        }

        //----------------------------------------------------------------------
        std::uint64_t columnsDone() const noexcept { return colsDone; }
        std::uint64_t columnsLogged() const noexcept { return logCount; }
        std::uint64_t nextFrame() const noexcept { return nextAbs; }
        std::int64_t newestIndex() const noexcept
        {
            return logCount == 0 ? -1 : (std::int64_t) log[(size_t) ((logCount - 1) % kLogSize)].index;
        }

        // Logged columns with index > acked.
        int pendingNewer(std::int64_t acked) const noexcept
        {
            int n = 0;
            const int avail = (int) std::min<std::uint64_t>(logCount, kLogSize);
            for (int i = 0; i < avail; ++i)
            {
                const auto& c = log[(size_t) ((logCount - 1 - (std::uint64_t) i) % kLogSize)];
                if ((std::int64_t) c.index <= acked) break;
                ++n;
            }
            return n;
        }

        // The newest `maxCount` logged columns with index > acked, oldest first. Returns how many were written.
        int collectNewer(std::int64_t acked, int maxCount, Column* out) const noexcept
        {
            const int pend = std::min(pendingNewer(acked), maxCount);
            for (int i = 0; i < pend; ++i)
                out[pend - 1 - i] = log[(size_t) ((logCount - 1 - (std::uint64_t) i) % kLogSize)];
            return pend;
        }

        // Logged columns with lowExcl < index < highExcl.
        int countBetween(std::int64_t lowExcl, std::int64_t highExcl) const noexcept
        {
            int n = 0;
            const int avail = (int) std::min<std::uint64_t>(logCount, kLogSize);
            for (int i = 0; i < avail; ++i)
            {
                const auto& c = log[(size_t) ((logCount - 1 - (std::uint64_t) i) % kLogSize)];
                if ((std::int64_t) c.index <= lowExcl) break;
                if ((std::int64_t) c.index < highExcl) ++n;
            }
            return n;
        }

        static float toUnit(double peak) noexcept
        {
            if (! (peak > 0.0)) return 0.0f;
            return (float) std::clamp((20.0 * std::log10(peak) - kFloorDb) / -kFloorDb, 0.0, 1.0);
        }

    private:
        struct Bq
        {
            double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
            double run(double x) noexcept
            {
                const double y = b0 * x + z1;
                z1 = b1 * x - a1 * y + z2;
                z2 = b2 * x - a2 * y;
                return y;
            }
        };
        using Cascade = std::array<Bq, 2>;   // 2 x Butterworth(Q = 1/sqrt2) = Linkwitz-Riley 4th order

        static double cascade(Cascade& c, double x) noexcept { return c[1].run(c[0].run(x)); }

        static Bq biquad(double fs, double fc, bool highpass)
        {
            const double w0 = 2.0 * 3.14159265358979323846 * fc / fs, cs = std::cos(w0);
            const double alpha = std::sin(w0) / (2.0 * 0.70710678118654752440);
            const double a0 = 1.0 + alpha;
            Bq b;
            if (highpass) { b.b0 = (1.0 + cs) / 2.0 / a0; b.b1 = -(1.0 + cs) / a0; b.b2 = b.b0; }
            else          { b.b0 = (1.0 - cs) / 2.0 / a0; b.b1 = (1.0 - cs) / a0;  b.b2 = b.b0; }
            b.a1 = -2.0 * cs / a0;
            b.a2 = (1.0 - alpha) / a0;
            return b;
        }

        void designFilters(double fs)
        {
            lp200  = { biquad(fs, 200.0, false),  biquad(fs, 200.0, false) };
            hp200  = { biquad(fs, 200.0, true),   biquad(fs, 200.0, true) };
            lp2500 = { biquad(fs, 2500.0, false), biquad(fs, 2500.0, false) };
            hp2500 = { biquad(fs, 2500.0, true),  biquad(fs, 2500.0, true) };
        }

        void resetFilters() noexcept
        {
            for (auto* c : { &lp200, &hp200, &lp2500, &hp2500 })
                for (auto& b : *c) { b.z1 = 0.0; b.z2 = 0.0; }
        }

        void finishColumn() noexcept
        {
            if (! dropCurrent)
            {
                Column c;
                c.index = colsDone;
                c.bass = toUnit(peaks[0]);
                c.mid  = toUnit(peaks[1]);
                c.high = toUnit(peaks[2]);
                for (int i = 0; i < pendingCount; ++i)
                    if (pending[(size_t) i].first == colsDone) c.flag = std::max(c.flag, pending[(size_t) i].second);
                log[(size_t) (logCount % kLogSize)] = c;
                ++logCount;
            }
            dropCurrent = false;

            // forget flags for columns that are now in the past
            int w = 0;
            for (int i = 0; i < pendingCount; ++i)
                if (pending[(size_t) i].first > colsDone) pending[(size_t) w++] = pending[(size_t) i];
            pendingCount = w;

            ++colsDone;
            peaks[0] = peaks[1] = peaks[2] = 0.0;
        }

        std::uint64_t fsInt = 48000;
        int channels = 2;
        Cascade lp200, hp200, lp2500, hp2500;

        std::uint64_t nextAbs = 0, colsDone = 0, logCount = 0;
        bool dropCurrent = false;
        double peaks[3] = { 0.0, 0.0, 0.0 };
        std::vector<Column> log;   // heap: 4096 x 24 B per active source (keeps the engine object small)
        std::array<std::pair<std::uint64_t, float>, 32> pending {};
        int pendingCount = 0;
    };
} // namespace prism
