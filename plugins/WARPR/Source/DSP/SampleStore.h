#pragma once
/*
  SampleStore - owns the current SampleData and the background loader (architecture C1 / C20).

    MESSAGE / WORKER threads : publish(), latest(), loadFileAsync() (a single low-priority loader
                               thread, created on first use; the newest request wins)
    AUDIO thread             : acquire() once per block (single atomic exchange) and get()

  Ownership: the LockFreeSwap holds ONE reference to the current and ONE to the pending object
  (RefCountRelease policy).  A voice that plays a sample holds its OWN reference (taken on the
  audio thread with incReferenceCount) and hands it back through RetireList when the voice ends,
  so the last release - and therefore the delete of megabytes of mips - never happens on the
  audio thread.  RetireList::drain() (editor timer / processor timer / prepareToPlay) frees them.

  A default single-cycle wave is generated at construction (regenerated deterministically, never
  stored in plugin state).
*/

#include "DSP/SampleData.h"
#include "Util/LockFreeSwap.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <functional>
#include <memory>

namespace warpr
{
    class SampleStore
    {
    public:
        using LoadedCallback = std::function<void (bool ok, const juce::String& errorMessage)>;

        explicit SampleStore (RetireList& retireListToUse);
        ~SampleStore();

        SampleStore (const SampleStore&) = delete;
        SampleStore& operator= (const SampleStore&) = delete;

        /** MESSAGE THREAD: stops the loader thread (idempotent).  Call before objects the load
            callback refers to are destroyed. */
        void shutdown();

        //======================================================================
        // Non-audio threads

        /** Publishes `data` for the audio thread and remembers it for the UI / state code. */
        void publish (SampleData::Ptr data);

        /** The most recently published sample (never nullptr after construction). */
        SampleData::Ptr latest() const;

        /** Decodes `file` on the loader thread and publishes it.  `onFinished` runs on the loader
            thread (keep it wait-free / queue work to the message thread). */
        void loadFileAsync (const juce::File& file, LoadedCallback onFinished);

        /** Blocking decode (loader thread and tests).  Returns nullptr and fills `errorMessage` on failure. */
        static SampleData::Ptr decodeFile (const juce::File& file, juce::AudioFormatManager& manager,
                                           juce::String& errorMessage);

        /** Blocking decode + publish on the calling thread (tests, state restore). */
        SampleData::Ptr loadFileBlocking (const juce::File& file, juce::String& errorMessage);

        /** The registered decoders (WAV / AIFF / FLAC ...).  Not thread-safe against registration. */
        juce::AudioFormatManager& formats() noexcept { return formatManager; }

        //======================================================================
        // Audio thread

        /** Adopts a pending publication.  Wait-free, never allocates or frees. */
        bool acquire() noexcept { return swap.acquire(); }

        /** The sample new notes will use (nullptr before the first acquire()). */
        SampleData* get() const noexcept { return swap.get(); }

        /** RetireList release function for one SampleData reference. */
        static void releaseRef (void* object) noexcept;

    private:
        class Loader;

        LockFreeSwap<SampleData, RefCountRelease<SampleData>> swap;

        mutable juce::CriticalSection latestLock;       // non-audio threads only
        SampleData::Ptr latestPtr;

        juce::AudioFormatManager formatManager;
        std::unique_ptr<Loader> loader;
    };
}
