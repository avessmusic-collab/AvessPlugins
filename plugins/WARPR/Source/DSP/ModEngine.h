#pragma once
/*
  ModEngine - the cross-thread publication point of the modulation system (architecture "Thread Boundaries":
  LfoShapeTable x10, ModMatrixSnapshot, lfoCount).

    MESSAGE thread : publishTable(i, ...), publishMatrix(...), setLfoCount(n)   (UiBackendImpl, state loads)
    AUDIO thread   : beginBlock() once per block - adopts pending publications (single atomic exchange each; the previous
                     objects go to the RetireList, never freed here) and refreshes the plain pointers the synth reads.

  Before the first publication every table pointer is nullptr (an LFO then outputs 0.5 = modulation 0) and the matrix is
  nullptr (no modulation at all).  UiBackendImpl::attachModEngine publishes the defaults immediately.
*/

#include "DSP/LfoTable.h"
#include "DSP/ModMatrix.h"
#include "DSP/SliceMap.h"
#include "Util/LockFreeSwap.h"

#include <array>
#include <atomic>
#include <memory>

namespace warpr
{
    class ModEngine
    {
    public:
        static constexpr int numLfos = 10;

        explicit ModEngine (RetireList& retireList) : matrixSwap (retireList), sliceSwap (retireList)
        {
            for (auto& t : tableSwaps)
                t = std::make_unique<LockFreeSwap<LfoShapeTable>> (retireList);
        }

        //======================================================================
        // MESSAGE thread
        void publishTable (int index, std::unique_ptr<LfoShapeTable> table)
        {
            if (index >= 0 && index < numLfos && table != nullptr)
                tableSwaps[(size_t) index]->publish (std::move (table));
        }

        void publishMatrix (std::unique_ptr<ModMatrixSnapshot> snapshot)
        {
            if (snapshot != nullptr)
                matrixSwap.publish (std::move (snapshot));
        }

        /** D11 SLICE MODE: the compiled slice map (SliceMap.h).  Same swap protocol as the matrix. */
        void publishSliceMap (std::unique_ptr<SliceMap> map)
        {
            if (map != nullptr)
                sliceSwap.publish (std::move (map));
        }

        void setLfoCount (int n) noexcept
        {
            lfoCountAtomic.store (n < 1 ? 1 : (n > numLfos ? numLfos : n), std::memory_order_relaxed);
        }

        int getLfoCountRequested() const noexcept { return lfoCountAtomic.load (std::memory_order_relaxed); }

        //======================================================================
        // AUDIO thread
        void beginBlock() noexcept
        {
            for (int i = 0; i < numLfos; ++i)
            {
                tableSwaps[(size_t) i]->acquire();
                tablePtrs[(size_t) i] = tableSwaps[(size_t) i]->get();
            }

            matrixSwap.acquire();
            matrixPtr = matrixSwap.get();
            sliceSwap.acquire();
            slicePtr = sliceSwap.get();
            lfoCountBlock = lfoCountAtomic.load (std::memory_order_relaxed);
        }

        const LfoShapeTable* const* tables() const noexcept { return tablePtrs.data(); }
        const ModMatrixSnapshot* matrix() const noexcept    { return matrixPtr; }
        int lfoCount() const noexcept                       { return lfoCountBlock; }

        /** The slice map of this block (nullptr before the first publication = slicing off).  Immutable while a voice or a
            note-on reads it; a replacement mid-note never touches voices (they latched their region at note-on). */
        const SliceMap* sliceMap() const noexcept           { return slicePtr; }

    private:
        std::array<std::unique_ptr<LockFreeSwap<LfoShapeTable>>, (size_t) numLfos> tableSwaps;
        LockFreeSwap<ModMatrixSnapshot> matrixSwap;
        LockFreeSwap<SliceMap> sliceSwap;
        const SliceMap* slicePtr = nullptr;
        std::atomic<int> lfoCountAtomic { 3 };

        std::array<const LfoShapeTable*, (size_t) numLfos> tablePtrs {};
        const ModMatrixSnapshot* matrixPtr = nullptr;
        int lfoCountBlock = 3;
    };
}
