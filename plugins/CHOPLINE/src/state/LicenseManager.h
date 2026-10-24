// CHOPLINE — LicenseManager (spec §2: clean interface stub only)
// No implementation until Phase 6 (release prep). Nothing in the plugin may
// depend on a concrete licensing backend; everything goes through this
// interface so copy protection can be added without touching DSP or UI.

#pragma once

#include <juce_core/juce_core.h>

namespace chopline::state
{

class LicenseManager
{
public:
    enum class Status
    {
        unlicensed,
        trial,
        licensed,
    };

    virtual ~LicenseManager() = default;

    /// Current licence status. Must be cheap and callable from the message
    /// thread; never from the audio thread.
    virtual Status getStatus() const = 0;

    /// Days left in the trial, or 0 when not in trial.
    virtual int getTrialDaysRemaining() const = 0;

    /// Attempts activation with a serial/key. Asynchronous completion via
    /// the callback on the message thread.
    virtual void activate (const juce::String& licenseKey,
                           std::function<void (bool success, juce::String message)> onComplete) = 0;

    /// Deactivates this machine.
    virtual void deactivate() = 0;
};

} // namespace chopline::state
