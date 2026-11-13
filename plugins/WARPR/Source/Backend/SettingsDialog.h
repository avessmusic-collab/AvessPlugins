#pragma once
/*
  SettingsWindow - the native Settings dialog (architecture C18: "small native juce::DialogWindow").

  D9 delivers the working shell: three controls (oscillator budget, interpolation quality, CPU guard)
  that report every change through `onChange` (UiBackendImpl persists them via AppSettings and applies the
  budget to the synth).  D10 gives interpolation quality and the CPU guard their DSP behaviour and may
  restyle / extend the page.  Message thread only.  The window hides on close (the backend owns and reuses it).
*/

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

#include "State/AppSettings.h"

class SettingsWindow : public juce::DialogWindow
{
public:
    using OnChange = std::function<void (const warpr::AppSettingsData&)>;

    SettingsWindow (const warpr::AppSettingsData& initial, OnChange onChangeCallback);

    void closeButtonPressed() override       { setVisible (false); }

    /** Shows the given values without firing onChange. */
    void setValues (const warpr::AppSettingsData& data);
    warpr::AppSettingsData getValues() const;

private:
    class Content;
    Content* content = nullptr;              // owned by the window (setContentOwned)
};
