#pragma once

#include <memory>

#include <juce_core/juce_core.h>
#include <juce_audio_formats/juce_audio_formats.h>

#include "Sampling/SampleBuffer.h"

namespace kickr
{
    /**
        Managed sample bank (architecture.md -> SampleLibrary; AD-11 / AD-12).

        MESSAGE THREAD ONLY. Never touched from processBlock. Owns the managed folder,
        exposes the ordered bank, decodes a file to a `SampleBuffer`. No atomics inside —
        the lock-free hand-off to the audio thread lives in PluginProcessor.

        - Folder (default): ~/Music/KICKR/Samples  — user-writable, easy to find in Finder,
          the convention for synth/sampler content. NOT ~/Library/Audio/Presets: on many
          machines an installer created that directory as root and a plugin cannot write
          into it. `setFolder()` re-points it (tests use a temp dir); falls back to a temp
          dir if the parent is unwritable. Created on construction.
        - Bank: sorted, non-recursive list of supported audio files (WAV / AIFF / FLAC / CAF
          where the platform provides a reader). `prev()/next()` cycle the current index.
        - Import: validate (supported extension, `AudioFormatReader` opens, length
          <= kMaxSampleSeconds, <= 2 channels) -> copy into the folder (name deduped
          `name-2.wav`, `name-3.wav`) -> rescan -> return the new bare file name ("" on
          failure).
        - Decode: read the whole file (capped at 5 s) into an `AudioBuffer<float>`, keep
          `sourceRate`, `rootNote = 60` (C3 — 2026-08-31, was C1). Synchronous — files are
          <= ~3.7 MB.

        **Factory bank (2026-08-31, supersedes the original AD-12 "zero samples" decision
        — user-provided, user-owned content, not third-party licensed material):** 50 kick
        samples embedded via `juce_add_binary_data` (`KICKR_FactorySamples` target,
        `Source/FactorySamples`), decoded straight from memory — never touch disk,
        always present even on a machine with an empty `~/Music/KICKR/Samples/`. The
        EXISTING disk-only API above (`getCount`/`getNames`/`nameAt`/`indexOfName`/
        `prev`/`next`) is UNCHANGED — it still means "your own imported files only" (kept
        that way so nothing above breaks / needs to know factory samples exist). The
        factory bank is a parallel set of accessors below; `getTotalXxx`/`xxxTotal` methods
        present the COMBINED (factory-first, then user) list the UI actually browses.
        `load(name)` transparently checks disk first, then the factory bank, so callers
        that already just have a name (`PluginProcessor::loadSampleByName`,
        `SampleWaveformView`) need no changes at all.
    */
    class SampleLibrary
    {
    public:
        /** ~/Music/KICKR/Samples (created lazily by the constructor / setFolder). */
        static juce::File defaultFolder();

        explicit SampleLibrary (const juce::File& folderToManage = defaultFolder());

        /** Re-point the managed folder (creates it, or falls back to a temp dir if the
            parent is not writable), then rescan. Used by tests and, later, a folder pref. */
        void setFolder (const juce::File& newFolder);

        /** Re-read the managed folder. Keeps the current selection on the same file name
            where possible, otherwise clamps. */
        void rescan();

        int          getCount() const noexcept          { return files.size(); }
        juce::StringArray getNames() const;
        juce::String nameAt (int index) const;
        int          indexOfName (const juce::String& bareName) const;

        int          currentIndex() const noexcept      { return currentIdx; }
        juce::String currentName() const                { return nameAt (currentIdx); }

        /** Move the current index and return the new current bare name (empty bank -> ""). */
        juce::String prev();
        juce::String next();

        /** Validate + copy `src` into the managed folder. Returns the new bare file name,
            or "" if the file is unsupported / unreadable / too long / too many channels /
            the copy failed (also fails if the (deduped) name would collide with a factory
            sample name — dedupe checks the factory bank too). */
        juce::String importFile (const juce::File& src);

        /** Decode a bank file to a `SampleBuffer` (message thread). Checks the user's disk
            bank first, then the embedded factory bank. nullptr if the name is in neither /
            unreadable. */
        std::unique_ptr<SampleBuffer> load (const juce::String& bareName);

        const juce::File& getFolder() const noexcept { return folder; }

        //====================================================================== factory bank
        /** Number of embedded factory samples (fixed; never touches disk). */
        static int         getFactoryCount() noexcept;
        /** Original filenames of the embedded factory samples, naturally sorted
            (Kick01 < Kick02 < ... < Kick10 < Kick11, regardless of zero-padding). */
        static juce::StringArray getFactoryNames();
        static bool         isFactoryName (const juce::String& bareName);

        //=============================================================== combined (factory+user)
        /** Total browsable count = factory + your own imports. What the UI shows. */
        int                getTotalCount() const noexcept { return getFactoryCount() + getCount(); }
        juce::StringArray  getTotalNames() const;
        juce::String       totalNameAt (int index) const;
        /** -1 if `bareName` is in neither bank. */
        int                indexOfNameTotal (const juce::String& bareName) const;

        juce::String currentTotalName() const { return totalNameAt (currentTotalIdx); }
        /** Move the COMBINED-list index (independent of prev()/next()'s disk-only index)
            and return the new current bare name ("" only if the total bank is empty, which
            can't happen once the factory bank links correctly). */
        juce::String prevTotal();
        juce::String nextTotal();

    private:
        bool isSupported (const juce::File& f) const;

        juce::File                folder;
        juce::AudioFormatManager  formatManager;
        juce::Array<juce::File>   files;
        int                       currentIdx { -1 };
        int                       currentTotalIdx { 0 };   // combined-list index, see prevTotal/nextTotal

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SampleLibrary)
    };
}
