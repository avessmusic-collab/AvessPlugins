// CHOPLINE — PresetManager implementation. See header.

#include "PresetManager.h"

#include <algorithm>

namespace chopline::state
{

PresetManager::PresetManager (juce::File rootDir) : root (std::move (rootDir))
{
    rescan();
}

juce::File PresetManager::defaultRoot()
{
    return juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
        .getChildFile ("CHOPLINE")
        .getChildFile ("Presets");
}

juce::StringArray PresetManager::factoryCategories()
{
    return { "Chops", "Glitch", "Stutter", "Tape", "Granular", "User" };
}

void PresetManager::rescan()
{
    presets.clear();

    juce::StringArray categories = factoryCategories();
    for (const auto& dir : root.findChildFiles (juce::File::findDirectories, false))
        categories.addIfNotAlreadyThere (dir.getFileName());

    for (const auto& category : categories)
    {
        auto files = root.getChildFile (category)
                         .findChildFiles (juce::File::findFiles, false, "*.xml");
        std::sort (files.begin(), files.end(),
                   [] (const juce::File& a, const juce::File& b)
                   {
                       return a.getFileNameWithoutExtension()
                                  .compareNatural (b.getFileNameWithoutExtension()) < 0;
                   });
        for (const auto& f : files)
            presets.push_back ({ category, f.getFileNameWithoutExtension(), f });
    }
}

int PresetManager::indexOfFile (const juce::File& f) const noexcept
{
    for (size_t i = 0; i < presets.size(); ++i)
        if (presets[i].file == f)
            return static_cast<int> (i);
    return -1;
}

juce::ValueTree PresetManager::stripSample (juce::ValueTree fullState)
{
    auto copy = fullState.createCopy();
    auto sample = copy.getChildWithName ("SAMPLE");
    if (sample.isValid())
    {
        // Keep the sample BPM (it informs Grid slicing); drop the audio and
        // the file reference, so loading keeps whatever sample is loaded.
        sample.removeProperty ("flac", nullptr);
        sample.removeProperty ("path", nullptr);
        sample.removeProperty ("hash", nullptr);
    }
    copy.setProperty ("kitOnly", true, nullptr);
    return copy;
}

juce::File PresetManager::save (const juce::ValueTree& fullState, const juce::String& name,
                                const juce::String& category, bool kitOnly,
                                juce::String& error)
{
    error.clear();
    const auto safeName = juce::File::createLegalFileName (name.trim());
    const auto safeCategory = juce::File::createLegalFileName (
        category.trim().isEmpty() ? juce::String ("User") : category.trim());
    if (safeName.isEmpty())
    {
        error = "Preset name is empty";
        return {};
    }

    auto dir = root.getChildFile (safeCategory);
    if (! dir.createDirectory())
    {
        error = "Cannot create " + dir.getFullPathName();
        return {};
    }

    const auto tree = kitOnly ? stripSample (fullState) : fullState.createCopy();
    const auto file = dir.getChildFile (safeName + ".xml");
    if (auto xml = tree.createXml())
    {
        if (! xml->writeTo (file))
        {
            error = "Cannot write " + file.getFullPathName();
            return {};
        }
    }
    else
    {
        error = "State could not be serialised";
        return {};
    }

    rescan();
    return file;
}

juce::ValueTree PresetManager::load (const juce::File& file) const
{
    if (auto xml = juce::parseXML (file))
        return juce::ValueTree::fromXml (*xml);
    return {};
}

} // namespace chopline::state
