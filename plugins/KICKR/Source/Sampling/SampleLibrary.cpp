#include "Sampling/SampleLibrary.h"

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
        const juce::String stem = src.getFileNameWithoutExtension();
        const juce::String ext  = src.getFileExtension();

        juce::File dest = folder.getChildFile (stem + ext);
        for (int n = 2; dest.existsAsFile(); ++n)
            dest = folder.getChildFile (stem + "-" + juce::String (n) + ext);

        if (! src.copyFileTo (dest))
            return {};

        rescan();
        return dest.getFileName();
    }

    std::unique_ptr<SampleBuffer> SampleLibrary::load (const juce::String& bareName)
    {
        if (bareName.isEmpty())
            return nullptr;

        const juce::File file = folder.getChildFile (bareName);
        if (! file.existsAsFile())
            return nullptr;

        std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (file));
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
        sb->rootNote   = 24;   // C1 (AD-11, v1)

        return sb;
    }
}
