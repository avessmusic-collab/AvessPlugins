#pragma once

#include <juce_core/juce_core.h>
#include <memory>
#include "AnalysisSnapshot.h"

namespace prism
{
    // Worker -> message-thread hand-off of immutable snapshots.
    // SpinLock guards only a shared_ptr copy/swap (~10 ns); never used on the audio thread.
    class SnapshotMailbox
    {
    public:
        using Ptr = std::shared_ptr<const AnalysisSnapshot>;

        void publish(Ptr p)
        {
            Ptr old;   // release the previous snapshot outside the lock
            {
                const juce::SpinLock::ScopedLockType l(lock);
                old = std::move(latest);
                latest = std::move(p);
            }
        }

        // Drop the published snapshot (engine rebuilt: anything published before is stale). Message / prepare thread.
        void clear()
        {
            Ptr old;
            {
                const juce::SpinLock::ScopedLockType l(lock);
                old = std::move(latest);
                latest.reset();
            }
        }

        Ptr fetch() const
        {
            const juce::SpinLock::ScopedLockType l(lock);
            return latest;
        }

    private:
        mutable juce::SpinLock lock;
        Ptr latest;
    };
} // namespace prism
