#include "State/AppSettings.h"
#include "PluginIdentity.h"

#include <utility>

namespace warpr
{
    juce::File AppSettings::defaultFile()
    {
        return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                   .getChildFile (identity::companyDirName)
                   .getChildFile (identity::productName)
                   .getChildFile ("Settings.xml");
    }

    AppSettingsData AppSettings::sanitise (AppSettingsData data) noexcept
    {
        data.oscillatorBudget = data.oscillatorBudget <= 0 ? 0 : juce::jlimit (64, 1024, data.oscillatorBudget);
        data.interpolationQuality = juce::jlimit (0, 1, data.interpolationQuality);
        return data;
    }

    AppSettingsData AppSettings::load() const
    {
        AppSettingsData data;

        if (file.existsAsFile())
        {
            if (const auto xml = juce::parseXML (file))
            {
                data.oscillatorBudget = xml->getIntAttribute ("oscillatorBudget", data.oscillatorBudget);
                data.interpolationQuality = xml->getIntAttribute ("interpolationQuality", data.interpolationQuality);
                data.cpuGuard = xml->getBoolAttribute ("cpuGuard", data.cpuGuard);
            }
        }

        return sanitise (data);
    }

    bool AppSettings::save (const AppSettingsData& requested) const
    {
        const auto data = sanitise (requested);

        juce::XmlElement root ("Settings");
        root.setAttribute ("oscillatorBudget", data.oscillatorBudget);
        root.setAttribute ("interpolationQuality", data.interpolationQuality);
        root.setAttribute ("cpuGuard", data.cpuGuard ? 1 : 0);

        return file.getParentDirectory().createDirectory() && file.replaceWithText (root.toString());
    }
}
