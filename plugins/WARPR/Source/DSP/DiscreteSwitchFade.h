#pragma once
/*
  DiscreteSwitchFade - bookkeeping for crossfaded discrete switches (architecture "D" rate class).

  A discrete choice (warp mode now; filter type in D5, playMode / grainMode later) is never switched
  hard.  The owner keeps TWO kernel instances alive for the length of the fade: the outgoing one with
  frozen parameters and the incoming one with reset state, and blends their outputs with the weight
  this class hands out.  This class only does the bookkeeping (which index is current, which is
  outgoing, how far the fade is); it knows nothing about the kernels.

      switcher.request (mode, fadeSamples)   -> true when a crossfade just started
      while sounding:
          const bool fading = switcher.fading();
          ... evaluate current (and outgoing, if fading) ...
          const float wIncoming = switcher.advance();   // 1/N ... 1, then 1 forever

  The first request ever (current index < 0) adopts the index silently (no fade).  A request made
  while a fade is running supersedes it: the older outgoing kernel is dropped and the fade restarts
  from the kernel that was current (a switch faster than the fade length is therefore not click-free;
  documented limitation, the parameter cannot change that fast musically).
*/

#include <algorithm>

namespace warpr
{
    class DiscreteSwitchFade
    {
    public:
        void reset() noexcept
        {
            cur = -1;
            old = -1;
            left = 0;
            total = 1;
        }

        /** Returns true when a crossfade from the previous index to `index` started. */
        bool request (int index, int fadeSamples) noexcept
        {
            if (cur < 0)
            {
                cur = index;
                return false;
            }

            if (index == cur)
                return false;

            old = cur;
            cur = index;
            total = std::max (1, fadeSamples);
            left = total;
            return true;
        }

        bool fading() const noexcept     { return left > 0; }
        int  current() const noexcept    { return cur; }
        int  outgoing() const noexcept   { return old; }

        /** Weight of the INCOMING kernel for the sample being rendered, then advances. 1 when idle. */
        float advance() noexcept
        {
            if (left <= 0)
                return 1.0f;

            --left;
            return 1.0f - static_cast<float> (left) / static_cast<float> (total);
        }

    private:
        int cur = -1, old = -1;
        int left = 0, total = 1;
    };
}
