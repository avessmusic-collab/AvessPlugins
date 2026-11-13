#pragma once
/*
  AppSettings - the non-parameter, per-user settings behind the Settings dialog (architecture C18 "Settings"):
  oscillator budget, interpolation quality, CPU guard.  Stored as a small XML file next to the presets
  (<userApplicationData>/<company>/<product>/Settings.xml); they are NOT part of the plugin state, presets or
  projects.  D9 delivered the persistence + the dialog shell + the budget hook; D10 gave the other two values their
  behaviour: interpolationQuality = SampleReader (0 sinc8 / 1 Hermite4, see SampleReader.h) and cpuGuard = CpuGuard.h
  (block-time EMA > 85 % caps the oversampling latched by NEW notes; WarprSynth.h).

  Loading is tolerant (missing / unreadable / out-of-range -> defaults or clamped values).
*/

#include <juce_core/juce_core.h>

#include <array>
#include <utility>

namespace warpr
{
    struct AppSettingsData
    {
        int oscillatorBudget = 256;          // OS-weighted units; 0 = governor off, else 64..1024
        int interpolationQuality = 0;        // 0 = high (sinc8), 1 = eco (hermite4)
        bool cpuGuard = false;               // opt-in CPU guard (D10 acts on it)

        bool operator== (const AppSettingsData& o) const noexcept
        {
            return oscillatorBudget == o.oscillatorBudget && interpolationQuality == o.interpolationQuality && cpuGuard == o.cpuGuard;
        }
    };

    class AppSettings
    {
    public:
        /** The budgets the dialog offers (0 = off). */
        static constexpr std::array<int, 6> budgetChoices { 0, 64, 128, 256, 512, 1024 };

        explicit AppSettings (juce::File settingsFile) : file (std::move (settingsFile)) {}

        static juce::File defaultFile();
        static AppSettingsData sanitise (AppSettingsData data) noexcept;

        const juce::File& getFile() const noexcept    { return file; }

        /** Defaults when the file is missing or unreadable. */
        AppSettingsData load() const;
        bool save (const AppSettingsData& data) const;

    private:
        juce::File file;
    };
}
