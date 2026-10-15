#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <atomic>
#include <cstdint>
#include <memory>

#include "Analysis/AnalysisSnapshot.h"
#include "Analysis/SnapshotMailbox.h"
#include "Parameters/ParameterIDs.h"

//==============================================================================
// PRISM - bit-transparent analyzer. processBlock() never writes to the buffer.
// None of the 17 APVTS parameters affect the audio path.
//==============================================================================
class PRISMAudioProcessor : public juce::AudioProcessor
{
public:
    PRISMAudioProcessor();
    ~PRISMAudioProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "PRISM"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    //==========================================================================
    // Analysis seams (Stage 2 fills these in; Stage 1 stubs keep the API stable
    // for the v8 editor scaffolding).
    std::shared_ptr<const prism::AnalysisSnapshot> getLatestAnalysisSnapshot() const { return mailbox.fetch(); }
    void requestResetAll()        { resetAllEpoch.fetch_add(1, std::memory_order_relaxed); }
    void requestResetIntegrated() { resetIntegratedEpoch.fetch_add(1, std::memory_order_relaxed); }
    void setEditorActive(bool active);
    bool isEditorActive() const   { return editorActiveCount.load(std::memory_order_relaxed) > 0; }

    // Defaults for the state entries / UI state (used by state init and sanitizer)
    static void applyStateDefaults(juce::ValueTree& state);
    static void sanitizeState(juce::ValueTree& state);

    // Public: the v8 editor scaffolding accesses processorRef.apvts directly.
    juce::AudioProcessorValueTreeState apvts;

private:
    prism::SnapshotMailbox mailbox;
    std::atomic<std::uint32_t> resetAllEpoch { 0 };
    std::atomic<std::uint32_t> resetIntegratedEpoch { 0 };
    std::atomic<int> editorActiveCount { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PRISMAudioProcessor)
};
