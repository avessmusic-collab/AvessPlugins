#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "Backend/UiBackendImpl.h"
#include "Parameters/ParamSnapshot.h"
#include "Parameters/ParameterLayout.h"
#include "PluginIdentity.h"
#include "Util/LockFreeSwap.h"

/**
 * WARPR processor (Stage 1: Foundation + Shell).
 *
 * Instrument: MIDI in, ONE stereo output bus, no input bus (pattern #4).
 * No DSP yet - processBlock() renders silence (honest stub; the voice engine starts in D1).
 * Parameters: all 135 from parameter-spec.md via warpr::createParameterLayout().
 */
class PluginProcessor : public juce::AudioProcessor
{
public:
    PluginProcessor();
    ~PluginProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using juce::AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return warpr::identity::productName; }
    bool acceptsMidi() const override  { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    //==========================================================================
    /** All 135 parameters (WebView relays attach to these by id). */
    juce::AudioProcessorValueTreeState apvts;

    /** What the editor needs (declared in Backend/UiBackend.h). */
    WarprUiBackend& getUiBackend() noexcept { return backend; }
    UiBackendImpl&  getBackendImpl() noexcept { return backend; }

    /** Cached atomic pointers to every raw parameter value (audio-thread reads). */
    const warpr::ParamSnapshot& getParamSnapshot() const noexcept { return paramSnapshot; }

    /** Cross-thread retire list: the audio thread pushes, the message thread drains. */
    warpr::RetireList& getRetireList() noexcept { return retireList; }

    /** MESSAGE THREAD: frees objects retired by the audio thread. */
    void drainRetired() { retireList.drain(); }

private:
    // Declaration order is destruction order in reverse: backend and snapshot go first,
    // the retire list drains after every LockFreeSwap that may use it, then the APVTS.
    warpr::RetireList    retireList;
    warpr::ParamSnapshot paramSnapshot;
    UiBackendImpl        backend;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginProcessor)
};
