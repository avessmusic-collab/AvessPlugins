#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <atomic>
#include <cstdint>
#include <memory>

#include "Analysis/AnalysisEngine.h"
#include "Analysis/AnalysisSnapshot.h"
#include "Analysis/AnalysisWorker.h"
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
    // Message thread. Non-blocking (SpinLock-guarded pointer copy). Also records the
    // fetch heartbeat the worker uses for editor-activity gating (and, later, column acks).
    std::shared_ptr<const prism::AnalysisSnapshot> getLatestAnalysisSnapshot() const;
    void requestResetAll()        { resetAllEpoch.fetch_add(1, std::memory_order_relaxed); }
    void requestResetIntegrated() { resetIntegratedEpoch.fetch_add(1, std::memory_order_relaxed); }
    void setEditorActive(bool active);
    bool isEditorActive() const   { return editorActiveCount.load(std::memory_order_relaxed) > 0; }

    // Engine/worker introspection (tests, debug stats).
    prism::AnalysisEngine* getAnalysisEngine() noexcept { return engine.get(); }
    prism::AnalysisWorker* getAnalysisWorker() noexcept { return worker.get(); }
    int getEngineRebuildCount() const noexcept { return engineRebuildCount; }
    std::uint64_t getLastFetchedSequence() const noexcept { return lastFetchedSequence.load(std::memory_order_relaxed); }

    // Defaults for the state entries / UI state (used by state init and sanitizer)
    static void applyStateDefaults(juce::ValueTree& state);
    static void sanitizeState(juce::ValueTree& state);

    // Public: the v8 editor scaffolding accesses processorRef.apvts directly.
    juce::AudioProcessorValueTreeState apvts;

private:
    prism::EngineConfig makeEngineConfig(double sampleRate) const;
    void stopWorker();

    prism::SnapshotMailbox mailbox;
    std::atomic<std::uint32_t> resetAllEpoch { 0 };
    std::atomic<std::uint32_t> resetIntegratedEpoch { 0 };
    std::atomic<int> editorActiveCount { 0 };
    mutable std::atomic<std::uint64_t> lastFetchedSequence { 0 };
    mutable std::atomic<std::int64_t>  lastFetchTimeMs { 0 };
    int engineRebuildCount = 0;

    // Declaration order matters: the worker references the engine, mailbox and atomics
    // above, so it is declared last (destroyed first).
    std::unique_ptr<prism::AnalysisEngine> engine;
    std::unique_ptr<prism::AnalysisWorker> worker;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PRISMAudioProcessor)
};
