#include "Sampling/SampleLibrary.h"
#include "BinaryData.h"   // KICKR_FactorySamples (juce_add_binary_data) — 50 shipped kicks

#include <algorithm>

namespace kickr
{
    juce::File SampleLibrary::defaultFolder()
    {
        // ~/Music/KICKR/Samples — user-writable, easy to find in Finder, and the
        // convention for synth/sampler content (Vital, Kick 2, Serum-style libraries).
        // NOT ~/Library/Audio/Presets: on many machines an installer created that
        // directory as root and a plugin cannot write into it.
        auto base = juce::File::getSpecialLocation (juce::File::userMusicDirectory);
        if (base == juce::File() || base.getFullPathName().isEmpty())
            base = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory);

        return base.getChildFile ("KICKR").getChildFile ("Samples");
    }

    SampleLibrary::SampleLibrary (const juce::File& folderToManage)
    {
        formatManager.registerBasicFormats();   // WAV / AIFF + FLAC / CAF where available
        setFolder (folderToManage);
    }

    void SampleLibrary::setFolder (const juce::File& newFolder)
    {
        folder = newFolder;

        // If the managed folder can't be created (e.g. a root-owned parent), fall back
        // to a temp folder so the plugin still runs — the user just can't persist a bank.
        if (! folder.createDirectory().wasOk() && ! folder.isDirectory())
            folder = juce::File::getSpecialLocation (juce::File::tempDirectory)
                         .getChildFile ("KICKR").getChildFile ("Samples");
        folder.createDirectory();

        currentIdx = -1;
        rescan();
    }

    bool SampleLibrary::isSupported (const juce::File& f) const
    {
        // Ask the format manager whether it has a reader for this extension.
        return formatManager.findFormatForFileExtension (f.getFileExtension()) != nullptr;
    }

    void SampleLibrary::rescan()
    {
        const juce::String keep = (currentIdx >= 0 && currentIdx < files.size())
                                      ? files.getReference (currentIdx).getFileName()
                                      : juce::String();

        files.clearQuick();

        if (folder.isDirectory())
        {
            for (const auto& entry : juce::RangedDirectoryIterator (folder, false, "*",
                                                                    juce::File::findFiles))
            {
                const auto f = entry.getFile();
                if (isSupported (f))
                    files.add (f);
            }
        }

        std::sort (files.begin(), files.end(),
                   [] (const juce::File& a, const juce::File& b)
                   {
                       return a.getFileName().compareIgnoreCase (b.getFileName()) < 0;
                   });

        if (files.isEmpty())
            currentIdx = -1;
        else
        {
            const int found = keep.isNotEmpty() ? indexOfName (keep) : -1;
            currentIdx = found >= 0 ? found : juce::jlimit (0, files.size() - 1, currentIdx);
        }
    }

    juce::StringArray SampleLibrary::getNames() const
    {
        juce::StringArray names;
        for (const auto& f : files)
            names.add (f.getFileName());
        return names;
    }

    juce::String SampleLibrary::nameAt (int index) const
    {
        if (index < 0 || index >= files.size())
            return {};
        return files.getReference (index).getFileName();
    }

    int SampleLibrary::indexOfName (const juce::String& bareName) const
    {
        for (int i = 0; i < files.size(); ++i)
            if (files.getReference (i).getFileName().equalsIgnoreCase (bareName))
                return i;
        return -1;
    }

    juce::String SampleLibrary::prev()
    {
        if (files.isEmpty())
            return {};
        currentIdx = (currentIdx <= 0) ? files.size() - 1 : currentIdx - 1;
        return currentName();
    }

    juce::String SampleLibrary::next()
    {
        if (files.isEmpty())
            return {};
        currentIdx = (currentIdx + 1) % files.size();
        return currentName();
    }

    juce::String SampleLibrary::importFile (const juce::File& src)
    {
        if (! src.existsAsFile() || ! isSupported (src))
            return {};

        {
            std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (src));
            if (reader == nullptr)
                return {};

            const int numCh = static_cast<int> (reader->numChannels);
            if (reader->sampleRate <= 0.0 || numCh < 1 || numCh > 2)
                return {};

            const double seconds = static_cast<double> (reader->lengthInSamples) / reader->sampleRate;
            if (seconds > kMaxSampleSeconds || reader->lengthInSamples < 1)
                return {};
        }

        // Deduplicate the destination name: name.wav -> name-2.wav -> name-3.wav ...
        // Also dedupe against the factory bank so a user import can never shadow one of
        // the shipped samples (both would otherwise answer to the same bare name).
        const juce::String stem = src.getFileNameWithoutExtension();
        const juce::String ext  = src.getFileExtension();

        juce::File dest = folder.getChildFile (stem + ext);
        for (int n = 2; dest.existsAsFile() || isFactoryName (dest.getFileName()); ++n)
            dest = folder.getChildFile (stem + "-" + juce::String (n) + ext);

        if (! src.copyFileTo (dest))
            return {};

        rescan();
        return dest.getFileName();
    }

    namespace
    {
        // Find an embedded factory sample's bytes by its ORIGINAL filename (e.g.
        // "Kick01.wav"). BinaryData::getNamedResource() itself keys off the SANITISED
        // symbol name (e.g. "Kick01_wav"), not the original filename, so this does the
        // one-time-per-call reverse lookup via the parallel originalFilenames table.
        // 50 entries, message thread, not remotely hot — a linear scan is fine.
        const char* findFactoryResource (const juce::String& bareName, int& sizeOut) noexcept
        {
            for (int i = 0; i < BinaryData::namedResourceListSize; ++i)
            {
                const char* symbol = BinaryData::namedResourceList[i];
                const char* orig   = BinaryData::getNamedResourceOriginalFilename (symbol);
                if (orig != nullptr && bareName.equalsIgnoreCase (orig))
                    return BinaryData::getNamedResource (symbol, sizeOut);
            }
            return nullptr;
        }
    }

    int SampleLibrary::getFactoryCount() noexcept
    {
        return BinaryData::namedResourceListSize;
    }

    juce::StringArray SampleLibrary::getFactoryNames()
    {
        juce::StringArray names;
        for (int i = 0; i < BinaryData::namedResourceListSize; ++i)
            if (const char* orig = BinaryData::getNamedResourceOriginalFilename (BinaryData::namedResourceList[i]))
                names.add (orig);
        names.sortNatural();   // Kick01 < Kick02 < ... < Kick10 < Kick11, despite inconsistent padding
        return names;
    }

    bool SampleLibrary::isFactoryName (const juce::String& bareName)
    {
        return getFactoryNames().contains (bareName, true);   // ignoreCase
    }

    juce::StringArray SampleLibrary::getTotalNames() const
    {
        auto names = getFactoryNames();
        names.addArray (getNames());
        return names;
    }

    juce::String SampleLibrary::totalNameAt (int index) const
    {
        const auto factory = getFactoryNames();
        if (index < 0)
            return {};
        if (index < factory.size())
            return factory[index];
        return nameAt (index - factory.size());
    }

    int SampleLibrary::indexOfNameTotal (const juce::String& bareName) const
    {
        const auto factory = getFactoryNames();
        const int fi = factory.indexOf (bareName, true);   // ignoreCase overload
        if (fi >= 0)
            return fi;
        const int ui = indexOfName (bareName);
        return ui >= 0 ? factory.size() + ui : -1;
    }

    juce::String SampleLibrary::prevTotal()
    {
        const int total = getTotalCount();
        if (total <= 0)
            return {};
        currentTotalIdx = (currentTotalIdx <= 0) ? total - 1 : currentTotalIdx - 1;
        return currentTotalName();
    }

    juce::String SampleLibrary::nextTotal()
    {
        const int total = getTotalCount();
        if (total <= 0)
            return {};
        currentTotalIdx = (currentTotalIdx + 1) % total;
        return currentTotalName();
    }

    std::unique_ptr<SampleBuffer> SampleLibrary::load (const juce::String& bareName)
    {
        if (bareName.isEmpty())
            return nullptr;

        std::unique_ptr<juce::AudioFormatReader> reader;

        // Disk (the user's own imports) first, then the embedded factory bank. The
        // factory bytes are static data baked into the binary (BinaryData) — safe to
        // reference from a non-owning MemoryInputStream for the program's whole lifetime,
        // regardless of the reader/stream's own lifetime.
        const juce::File file = folder.getChildFile (bareName);
        if (file.existsAsFile())
        {
            reader.reset (formatManager.createReaderFor (file));
        }
        else
        {
            int size = 0;
            if (const char* data = findFactoryResource (bareName, size))
            {
                auto stream = std::make_unique<juce::MemoryInputStream> (data, static_cast<size_t> (size), false);
                reader.reset (formatManager.createReaderFor (std::move (stream)));
            }
        }

        if (reader == nullptr || reader->sampleRate <= 0.0)
            return nullptr;

        const int numCh = juce::jlimit (1, 2, static_cast<int> (reader->numChannels));

        const juce::int64 capSamples = static_cast<juce::int64> (kMaxSampleSeconds * reader->sampleRate);
        const juce::int64 lenToRead  = juce::jmin (reader->lengthInSamples, capSamples);
        if (lenToRead < 1)
            return nullptr;

        auto sb = std::make_unique<SampleBuffer>();
        sb->audio.setSize (numCh, static_cast<int> (lenToRead));
        sb->audio.clear();

        reader->read (&sb->audio, 0, static_cast<int> (lenToRead), 0, true, numCh > 1);

        sb->sourceRate = reader->sampleRate;
        sb->rootNote   = 60;   // C3 (MIDI 60, Ableton's middle C) -> unpitched trigger note (AD-11, v1)

        return sb;
    }
}
