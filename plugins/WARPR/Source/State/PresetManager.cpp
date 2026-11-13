#include "State/PresetManager.h"
#include "PluginIdentity.h"

#include <algorithm>
#include <iterator>
#include <utility>

namespace warpr
{
    namespace
    {
        constexpr juce::int64 kMaxPresetBytes = juce::int64 (1) << 30;     // 1 GiB, far beyond any sane preset

        bool entryLess (const PresetEntry& a, const PresetEntry& b)
        {
            const int byName = a.name.compareNatural (b.name, false);
            return byName != 0 ? byName < 0 : a.file.getFullPathName() < b.file.getFullPathName();
        }
    }

    PresetManager::PresetManager (juce::File dir, std::vector<FactoryPreset> factoryPresets)
        : directory (std::move (dir)), factory (std::move (factoryPresets))
    {
    }

    juce::File PresetManager::defaultDirectory()
    {
        return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                   .getChildFile (identity::companyDirName)
                   .getChildFile (identity::productName)
                   .getChildFile (identity::presetDirName);
    }

    std::vector<FactoryPreset> PresetManager::builtInFactoryPresets()
    {
        return {};                                   // D9: infrastructure only; the factory bank arrives with sound design
    }

    juce::String PresetManager::extension()
    {
        return identity::presetExtension;
    }

    juce::String PresetManager::sanitiseName (const juce::String& name)
    {
        auto s = juce::File::createLegalFileName (name.trim()).trim();

        while (s.startsWithChar ('.'))
            s = s.substring (1).trim();

        return s.isEmpty() ? juce::String ("Untitled") : s;
    }

    juce::File PresetManager::fileForName (const juce::String& name) const
    {
        return directory.getChildFile (sanitiseName (name) + extension());
    }

    std::vector<PresetEntry> PresetManager::listEntries() const
    {
        std::vector<PresetEntry> entries;

        for (size_t i = 0; i < factory.size(); ++i)
        {
            PresetEntry e;
            e.name = factory[i].name;
            e.factory = true;
            e.factoryIndex = static_cast<int> (i);
            entries.push_back (std::move (e));
        }

        std::vector<PresetEntry> user;

        if (directory.isDirectory())
        {
            for (const auto& f : directory.findChildFiles (juce::File::findFiles, false, "*" + extension()))
            {
                if (f.getFileName().startsWithChar ('.'))
                    continue;                        // resource forks / hidden files

                PresetEntry e;
                e.name = f.getFileNameWithoutExtension();
                e.file = f;
                user.push_back (std::move (e));
            }

            std::sort (user.begin(), user.end(), entryLess);
        }

        entries.insert (entries.end(), std::make_move_iterator (user.begin()), std::make_move_iterator (user.end()));
        return entries;
    }

    bool PresetManager::stepFrom (const juce::String& currentName, int delta, PresetEntry& result) const
    {
        const auto entries = listEntries();

        if (entries.empty())
            return false;

        const int n = static_cast<int> (entries.size());
        int current = -1;

        for (int i = 0; i < n; ++i)
            if (entries[static_cast<size_t> (i)].name == currentName)
            {
                current = i;
                break;
            }

        int next = 0;

        if (current < 0)
            next = delta < 0 ? n - 1 : 0;
        else
            next = ((current + (delta < 0 ? -1 : 1)) % n + n) % n;

        result = entries[static_cast<size_t> (next)];
        return true;
    }

    bool PresetManager::read (const PresetEntry& entry, juce::MemoryBlock& data, juce::String& error) const
    {
        if (entry.factory)
        {
            if (entry.factoryIndex < 0 || static_cast<size_t> (entry.factoryIndex) >= factory.size())
            {
                error = "Unknown factory preset";
                return false;
            }

            data = factory[static_cast<size_t> (entry.factoryIndex)].data;
            return true;
        }

        return readFile (entry.file, data, error);
    }

    bool PresetManager::readFile (const juce::File& file, juce::MemoryBlock& data, juce::String& error)
    {
        if (! file.existsAsFile())
        {
            error = "The preset file does not exist";
            return false;
        }

        if (file.getSize() > kMaxPresetBytes)
        {
            error = "The preset file is too large";
            return false;
        }

        data.reset();

        if (! file.loadFileAsData (data) || data.getSize() == 0)
        {
            error = "The preset file could not be read";
            return false;
        }

        return true;
    }

    bool PresetManager::writeFile (const juce::File& file, const juce::MemoryBlock& data, juce::String& error)
    {
        if (! file.getParentDirectory().createDirectory())
        {
            error = "The preset folder could not be created";
            return false;
        }

        juce::TemporaryFile temporary (file);

        if (! temporary.getFile().replaceWithData (data.getData(), data.getSize()))
        {
            error = "The preset file could not be written";
            return false;
        }

        if (! temporary.overwriteTargetFileWithTemporary())
        {
            error = "The preset file could not be replaced";
            return false;
        }

        return true;
    }
}
