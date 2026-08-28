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
          `sourceRate`, `rootNote = 24`. Synchronous — files are <= ~3.7 MB.

        No factory samples (AD-12): the bank is empty until the user drops files in.
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
            the copy failed. */
        juce::String importFile (const juce::File& src);

        /** Decode a bank file to a `SampleBuffer` (message thread). nullptr if missing /
            unreadable. */
        std::unique_ptr<SampleBuffer> load (const juce::String& bareName);

        const juce::File& getFolder() const noexcept { return folder; }

    private:
        bool isSupported (const juce::File& f) const;

        juce::File                folder;
        juce::AudioFormatManager  formatManager;
        juce::Array<juce::File>   files;
        int                       currentIdx { -1 };

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SampleLibrary)
    };
}
