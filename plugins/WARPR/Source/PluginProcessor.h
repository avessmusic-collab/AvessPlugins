#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <atomic>

#include "Backend/UiBackendImpl.h"
#include "DSP/CpuGuard.h"
#include "DSP/MasterStage.h"
#include "DSP/ModEngine.h"
#include "DSP/SampleStore.h"
#include "DSP/ShaperEngine.h"
#include "DSP/VizAnalyzer.h"
#include "DSP/WarprSynth.h"
#include "Parameters/ParamSnapshot.h"
#include "Parameters/ParameterLayout.h"
#include "PluginIdentity.h"
#include "State/AppSettings.h"
#include "State/PresetManager.h"
#include "State/SampleCodec.h"
#include "State/StateIO.h"
#include "Util/LockFreeSwap.h"

/**
 * WARPR processor (Stage 2, phase D4: warp framework + folds/distortions + A/B transfer-curve shaper + viz feeds).
 *
 * Instrument: MIDI in, ONE stereo output bus, no input bus (pattern #4).
 * Signal path (D4): MIDI -> WarprSynth (32 voices + 2 ghost slots; per oscillator: read -> warp modes 0-43 (44-59 identity)
 * -> shaper (A/B LUT + ADAA)) -> MasterStage (DC block, masterGain, safety knee soft-clip) -> VizAnalyzer (UI feed only).
 * D5a adds the per-voice filter stage (types 0-19) between the voice DC blocker and the amp envelope; the remaining
 * filter types, granular and the LFO system arrive in D5b-D8.
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

    /** The current sample + its loader (message thread: publish / latest / load). */
    warpr::SampleStore& getSampleStore() noexcept { return sampleStore; }

    /** The shaper's published LUT sets and crossfade state (diagnostics / tests; the backend publishes). */
    warpr::ShaperEngine& getShaperEngine() noexcept { return shaperEngine; }

    /** The LFO tables / mod matrix / lfoCount publication point (D8; the backend publishes). */
    warpr::ModEngine& getModEngine() noexcept { return modEngine; }

    /** Voice engine access for diagnostics, settings (oscillator budget, interpolation quality) and tests. */
    warpr::WarprSynth& getSynth() noexcept { return synth; }

    /** Voices still sounding at the end of the last block (ghost fades included). */
    int getNumActiveVoices() const noexcept { return activeVoices.load (std::memory_order_relaxed); }

    /** QWERTY keyboard in the UI: queues a note event from the message thread; processBlock drains
        the queue wait-free into the block's MIDI stream (timestamped at sample 0). */
    void queueKeyboardNote (int midiNote, int velocity, bool noteOn) noexcept;

    //==========================================================================
    // D9: state, presets, settings
    /** The complete state as a container (APVTS + UI state + the embedded sample, FLAC24 / float32; the default wave is
        not embedded).  Message thread or the host's state thread - never the audio thread. */
    juce::MemoryBlock captureState();

    /** Loads a container (or legacy plain XML).  All-or-nothing for parameters and UI state; a corrupt sample blob falls
        back to the default wave.  `rejected` leaves the current state untouched.  Same threads as captureState(). */
    warpr::state::LoadOutcome loadStateBlock (const void* data, std::size_t numBytes);

    warpr::PresetManager& getPresetManager() noexcept { return presetManager; }
    warpr::samplecodec::BlobCache& getSampleBlobCache() noexcept { return blobCache; }

    /** Per-user app settings (Settings dialog).  loadAppSettings() reads the file and applies it; it is called by the
        plugin factory only, so direct construction (tests) never depends on the user's settings file. */
    warpr::AppSettings& getAppSettings() noexcept { return appSettings; }
    void loadAppSettings();
    void applyAppSettings (const warpr::AppSettingsData& data);
    bool isCpuGuardRequested() const noexcept { return cpuGuard.load (std::memory_order_relaxed); }

    //==========================================================================
    // D10: CPU guard.  When the setting is on, processBlock measures its own wall time per block (two high-resolution tick
    // reads, no allocation), feeds load = time / block duration to a CpuGuard and publishes the level to the synth for
    // the NEXT block (it caps the oversampling latched by new notes; see CpuGuard.h / WarprSynth.h).
    /** Current guard level 0..3 (0 whenever the setting is off). */
    int getGuardLevel() const noexcept { return synth.getGuardLevel(); }

    /** TEST-ONLY hook: a value >= 0 replaces the measured load of every block (so tests never depend on the wall clock);
        a negative value (the default) uses the measurement.  Relaxed atomic; harmless in production. */
    void setGuardLoadInjectionForTests (double load) noexcept { injectedGuardLoad.store (load, std::memory_order_relaxed); }

private:
    struct KbdEvent { uint8_t note = 0, vel = 0; bool on = false; };
    juce::AbstractFifo kbdFifo { 128 };
    std::array<KbdEvent, 128> kbdEvents {};

    /** Drains the retire list on the message thread even when no editor is open. */
    struct RetireDrainTimer : private juce::Timer
    {
        explicit RetireDrainTimer (PluginProcessor& ownerProcessor) : owner (ownerProcessor) {}
        ~RetireDrainTimer() override { stopTimer(); }

        void begin() { startTimer (100); }
        void end()   { stopTimer(); }

    private:
        void timerCallback() override
        {
            owner.drainRetired();
            owner.getBackendImpl().servicePending();         // shaperPreset / shaperSymmetry changes (message thread)
        }
        PluginProcessor& owner;
    };

    // Declaration order is destruction order in reverse: voices / backend / snapshot go first,
    // the sample store (which releases its references through the retire list's objects) next,
    // the retire list drains last, then the APVTS.
    warpr::RetireList    retireList;
    warpr::SampleStore   sampleStore { retireList };
    warpr::ShaperEngine  shaperEngine { retireList };
    warpr::ModEngine     modEngine { retireList };
    warpr::ParamSnapshot paramSnapshot;
    warpr::samplecodec::BlobCache blobCache;
    warpr::PresetManager presetManager { warpr::PresetManager::defaultDirectory() };   // before the backend that uses it
    warpr::AppSettings   appSettings { warpr::AppSettings::defaultFile() };
    std::atomic<bool>    cpuGuard { false };
    std::atomic<double>  injectedGuardLoad { -1.0 };        // test-only load injection (< 0 = measure)
    warpr::CpuGuard      guardState;                         // audio thread only
    bool                 guardWasActive = false;             // audio thread only
    UiBackendImpl        backend;
    warpr::WarprSynth    synth { retireList };
    warpr::MasterStage   master;
    warpr::VizAnalyzer   vizAnalyzer;            // pushes WarprVizFrame into backend.vizFifo() (~30 Hz)
    warpr::ParamBlock    paramBlock;
    std::atomic<int>     activeVoices { 0 };
    RetireDrainTimer     drainTimer { *this };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginProcessor)
};
