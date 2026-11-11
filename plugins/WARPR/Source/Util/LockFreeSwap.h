#pragma once
/*
  LockFreeSwap<T> + RetireList - the ONLY cross-thread publication primitive for big objects
  (SampleData, shaper LUTs, LFO shape tables, mod-matrix snapshots).

  Protocol
    MESSAGE thread : allocates a new object and publish()es it.
    AUDIO thread   : calls acquire() once at block start (a single atomic exchange).  If a new
                     object was pending it becomes current() and the PREVIOUS object is pushed
                     into the RetireList - it is never deleted on the audio thread.
    MESSAGE thread : RetireList::drain() (editor timer / prepareToPlay / destructors) releases
                     the retired objects.
    Back-pressure  : if the RetireList is full, acquire() consumes nothing and the audio thread
                     keeps its current object (publication stays pending).  Never a free.

  Ownership policy: `publish(T*)` transfers ONE unit of ownership.
    DeleteRelease<T>   (default)  the unit is the pointer itself -> `delete`.
    RefCountRelease<T>            the unit is one reference of a juce::ReferenceCountedObject
                                  (caller increments before publishing, e.g. via
                                  ReferenceCountedObjectPtr::get() + incReferenceCount()) ->
                                  decReferenceCount().  Voices that copied their own reference
                                  keep the object alive; the last release still happens on
                                  the message thread through the RetireList.

  Threading contract: exactly ONE audio thread calls acquire()/get(); any number of non-audio
  threads may publish() (it is an atomic exchange); drain() is serialised by a mutex and must
  never be called from the audio thread.
*/

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <type_traits>

namespace warpr
{
    //==========================================================================
    class RetireList
    {
    public:
        using ReleaseFn = void (*) (void*);

        /** 64 slots (AbstractFifo keeps one slot free, so 63 objects can be in flight). */
        static constexpr int capacity = 64;

        RetireList() = default;
        RetireList (const RetireList&) = delete;
        RetireList& operator= (const RetireList&) = delete;
        ~RetireList() { drain(); }

        /** AUDIO THREAD (single producer): true if a push() is guaranteed to succeed. */
        bool hasSpace() const noexcept { return fifo.getFreeSpace() > 0; }

        /** AUDIO THREAD: wait-free, never allocates.  Returns false when full (nothing stored). */
        bool push (void* object, ReleaseFn release) noexcept
        {
            int s1 = 0, n1 = 0, s2 = 0, n2 = 0;
            fifo.prepareToWrite (1, s1, n1, s2, n2);
            if (n1 <= 0)
                return false;

            items[(size_t) s1] = { object, release };
            fifo.finishedWrite (1);
            return true;
        }

        /** MESSAGE THREAD: releases everything retired so far.  Returns the number released. */
        int drain()
        {
            const std::scoped_lock lock (drainMutex);
            int released = 0;

            while (fifo.getNumReady() > 0)
            {
                int s1 = 0, n1 = 0, s2 = 0, n2 = 0;
                fifo.prepareToRead (1, s1, n1, s2, n2);
                if (n1 <= 0)
                    break;

                const Item item = items[(size_t) s1];
                fifo.finishedRead (1);

                if (item.release != nullptr && item.object != nullptr)
                    item.release (item.object);

                ++released;
            }

            return released;
        }

        /** Number of objects waiting to be released (diagnostics / tests). */
        int numPending() const noexcept { return fifo.getNumReady(); }

    private:
        struct Item { void* object = nullptr; ReleaseFn release = nullptr; };

        juce::AbstractFifo fifo { capacity };
        std::array<Item, (size_t) capacity> items {};
        std::mutex drainMutex;
    };

    //==========================================================================
    template <typename T>
    struct DeleteRelease
    {
        static void release (T* p) noexcept { delete p; }
    };

    template <typename T>
    struct RefCountRelease
    {
        static void release (T* p) noexcept { p->decReferenceCount(); }
    };

    //==========================================================================
    template <typename T, typename Release = DeleteRelease<T>>
    class LockFreeSwap
    {
    public:
        explicit LockFreeSwap (RetireList& retireListToUse) noexcept : retire (retireListToUse) {}
        LockFreeSwap (const LockFreeSwap&) = delete;
        LockFreeSwap& operator= (const LockFreeSwap&) = delete;

        /** Destruction happens off the audio thread (processor teardown): release both slots. */
        ~LockFreeSwap()
        {
            if (T* p = pending.exchange (nullptr))
                Release::release (p);
            if (current != nullptr)
                Release::release (current);
        }

        //======================================================================
        // MESSAGE THREAD (or any non-audio thread)

        /** Takes ownership of one unit of `owned` (see header comment).  An older, never
            consumed publication is released immediately (the audio thread never saw it). */
        void publish (T* owned)
        {
            if (T* stale = pending.exchange (owned, std::memory_order_acq_rel))
                Release::release (stale);
        }

        void publish (std::unique_ptr<T> owned)
        {
            static_assert (std::is_same_v<Release, DeleteRelease<T>>,
                           "unique_ptr publication is only valid with the delete policy");
            publish (owned.release());
        }

        //======================================================================
        // AUDIO THREAD (single consumer)

        /** Adopts a pending publication, retiring the previous object.  Returns true if
            current() changed.  Wait-free; never allocates or frees. */
        bool acquire() noexcept
        {
            if (pending.load (std::memory_order_relaxed) == nullptr)
                return false;

            if (current != nullptr && ! retire.hasSpace())
                return false;                                   // back-pressure: keep the old object

            T* incoming = pending.exchange (nullptr, std::memory_order_acq_rel);
            if (incoming == nullptr)
                return false;

            T* outgoing = current;
            current = incoming;

            if (outgoing != nullptr)
                retire.push (outgoing, &LockFreeSwap::releaseThunk);   // space was checked above

            return true;
        }

        /** The object the audio thread is currently using (nullptr before the first publication). */
        T* get() const noexcept { return current; }

    private:
        static void releaseThunk (void* p) { Release::release (static_cast<T*> (p)); }

        static_assert (std::atomic<T*>::is_always_lock_free, "LockFreeSwap requires lock-free pointer atomics");

        RetireList& retire;
        std::atomic<T*> pending { nullptr };
        T* current = nullptr;                                   // audio-thread owned
    };
}
