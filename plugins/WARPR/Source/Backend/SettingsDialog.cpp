#include "Backend/SettingsDialog.h"
#include "PluginIdentity.h"

#include <cstdlib>

class SettingsWindow::Content : public juce::Component
{
public:
    Content (const warpr::AppSettingsData& initial, OnChange onChangeCallback)
        : onChange (std::move (onChangeCallback))
    {
        budgetLabel.setText ("Oscillator budget", juce::dontSendNotification);
        interpLabel.setText ("Interpolation", juce::dontSendNotification);
        note.setText ("Settings are stored per user. They apply to this instance immediately and to new instances when they open.",
                      juce::dontSendNotification);
        note.setJustificationType (juce::Justification::topLeft);
        note.setMinimumHorizontalScale (1.0f);

        for (size_t i = 0; i < warpr::AppSettings::budgetChoices.size(); ++i)
        {
            const int v = warpr::AppSettings::budgetChoices[i];
            budgetBox.addItem (v == 0 ? juce::String ("Off (no limit)") : juce::String (v) + " units", static_cast<int> (i) + 1);
        }

        interpBox.addItem ("High quality (8-tap sinc)", 1);
        interpBox.addItem ("Eco (4-point Hermite)", 2);
        guardToggle.setButtonText ("CPU guard: lower oversampling for new notes when the CPU is overloaded");

        for (juce::Component* c : { static_cast<juce::Component*> (&budgetLabel), static_cast<juce::Component*> (&interpLabel),
                                    static_cast<juce::Component*> (&note), static_cast<juce::Component*> (&budgetBox),
                                    static_cast<juce::Component*> (&interpBox), static_cast<juce::Component*> (&guardToggle) })
            addAndMakeVisible (*c);

        budgetBox.onChange = [this] { notify(); };
        interpBox.onChange = [this] { notify(); };
        guardToggle.onClick = [this] { notify(); };

        setValues (initial);
        setSize (480, 190);
    }

    void setValues (const warpr::AppSettingsData& data)
    {
        const auto d = warpr::AppSettings::sanitise (data);

        size_t best = 0;
        for (size_t i = 1; i < warpr::AppSettings::budgetChoices.size(); ++i)
            if (std::abs (warpr::AppSettings::budgetChoices[i] - d.oscillatorBudget)
                < std::abs (warpr::AppSettings::budgetChoices[best] - d.oscillatorBudget))
                best = i;

        budgetBox.setSelectedId (static_cast<int> (best) + 1, juce::dontSendNotification);
        interpBox.setSelectedId (d.interpolationQuality + 1, juce::dontSendNotification);
        guardToggle.setToggleState (d.cpuGuard, juce::dontSendNotification);
    }

    warpr::AppSettingsData getValues() const
    {
        warpr::AppSettingsData d;
        const int budgetIndex = juce::jlimit (0, static_cast<int> (warpr::AppSettings::budgetChoices.size()) - 1, budgetBox.getSelectedId() - 1);
        d.oscillatorBudget = warpr::AppSettings::budgetChoices[static_cast<size_t> (budgetIndex)];
        d.interpolationQuality = juce::jmax (0, interpBox.getSelectedId() - 1);
        d.cpuGuard = guardToggle.getToggleState();
        return warpr::AppSettings::sanitise (d);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (16);
        auto row = [&r] { auto a = r.removeFromTop (28); r.removeFromTop (8); return a; };

        auto a = row();  budgetLabel.setBounds (a.removeFromLeft (150));  budgetBox.setBounds (a);
        auto b = row();  interpLabel.setBounds (b.removeFromLeft (150));  interpBox.setBounds (b);
        guardToggle.setBounds (row());
        note.setBounds (r);
    }

private:
    void notify()
    {
        if (onChange)
            onChange (getValues());
    }

    OnChange onChange;
    juce::Label budgetLabel, interpLabel, note;
    juce::ComboBox budgetBox, interpBox;
    juce::ToggleButton guardToggle;
};

SettingsWindow::SettingsWindow (const warpr::AppSettingsData& initial, OnChange onChangeCallback)
    : juce::DialogWindow (juce::String (warpr::identity::productName) + " Settings", juce::Colour (0xff1c1c20), true)
{
    content = new Content (initial, std::move (onChangeCallback));
    setContentOwned (content, true);
    setUsingNativeTitleBar (true);
    setResizable (false, false);
    centreWithSize (getWidth(), getHeight());
}

void SettingsWindow::setValues (const warpr::AppSettingsData& data)
{
    if (content != nullptr)
        content->setValues (data);
}

warpr::AppSettingsData SettingsWindow::getValues() const
{
    return content != nullptr ? content->getValues() : warpr::AppSettingsData{};
}
