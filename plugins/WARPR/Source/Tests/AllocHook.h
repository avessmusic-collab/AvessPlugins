#pragma once
/*
  AllocHook - proves "zero allocations on the audio thread".

  AllocHook.cpp replaces the global operator new / delete (WARPR_Tests only).  Counting is
  per-thread and only while a ScopedAllocCount is alive on that thread, so the test harness
  itself can allocate freely.

      {
          warpr::tests::ScopedAllocCount counter;
          processor.processBlock (buffer, midi);
          check (counter.count() == 0, "processBlock does not allocate");
      }
*/

#include <cstddef>

namespace warpr::tests
{
    class ScopedAllocCount
    {
    public:
        ScopedAllocCount();
        ~ScopedAllocCount();

        ScopedAllocCount (const ScopedAllocCount&) = delete;
        ScopedAllocCount& operator= (const ScopedAllocCount&) = delete;

        /** Allocations (operator new / new[]) made on THIS thread since construction. */
        std::size_t count() const;

        /** D10: bytes REQUESTED by those allocations (gross; frees are not subtracted, over-aligned allocations are not seen). */
        std::size_t bytes() const;

    private:
        std::size_t startCount = 0;
        std::size_t startBytes = 0;
    };
}
