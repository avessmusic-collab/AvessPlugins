#pragma once
/*
  UiBackendImpl - implementation of WarprUiBackend owned by the processor.

  STATUS after phase D4.  Every interface method exists so the editor compiles and opens
  without "unknown to the backend" console errors.  Real today:
    - revision counters (atomics) and bumpRevision()
    - the message-thread UiModel: lfoCount / shape JSON / mod-matrix JSON / preset name are stored and
      returned (and persisted by StateIO).  Nothing parses the LFO documents yet (D8).
    - SHAPER CURVES (D4): setShaperCurveJson parses + validates the wire format into a ShaperCurve, stores the
      CANONICAL text, rebuilds the A/B LUT set on the message thread and publishes it to the audio thread
      (ShaperEngine / LockFreeSwap, 10 ms crossfade).  getShaperCurveJson returns the canonical text.
      shaperReset -> identity, shaperNormalize -> y *= 1 / max|y| of the effective curve; both write the curve
      back, rebuild and bump Revision::shaperCurves.  A UI-originated edit NEVER bumps Revision::shaperCurves
      (echo avoidance) and flips the shaperPreset parameter to Custom; a shaperPreset change (host, automation or
      UI) loads the preset curve into the curve selected by shaperCurveSelect and DOES bump the revision.
    - loadSampleFile() decodes on the SampleStore loader thread and publishes through the
      lock-free swap (bumps Revision::sample on success, explains failures with an async alert and
      keeps the previous sample); getSampleDisplay() reads the current sample's name / format / peaks.
    - FILTER RESPONSE (D5a): computeFilterResponse reads the 8 filter parameters (raw atomics) and evaluates
      FilterResponse::magnitudeDbLogSpaced at the processor's sample rate (setSampleRate, from prepareToPlay) -
      the SAME coefficient code the voices run.  Linear types 0-19 only; unimplemented types (20-33, D5b/D5c)
      and a fully open low-pass report a flat 0 dB.  Revision::filterResponse is bumped (wait-free, from the
      APVTS listener, any thread) on every change of any of the 8 filter parameters.
    - LFO SYSTEM (D8): setLfoShapeJson parses the wire document (LfoShapeDesc), builds the two-sided shape table on the
      calling thread and publishes it to the audio thread (ModEngine / LockFreeSwap); setModMatrixJson compiles the
      matrix (unknown / Choice / Bool destinations and invalid routes dropped, depth clamped) and publishes the snapshot;
      setLfoCount publishes the atomic count.  The model keeps the UI's documents VERBATIM (state round trip and
      getUiStateJson serve exactly what was sent); adoptModel republishes everything.  A document that does not parse
      builds the default shape (the UI's newShape()).
    - PRESETS + SETTINGS (D9): the backend drives a PresetManager (attachPresetManager) and the processor's state
      capture / load functions (attachStateHost).  presetStep cycles the preset list (factory first, then user files by
      name, wrapping; an unreadable file is skipped), presetSaveDialog / presetLoadDialog open async JUCE file choosers
      (message thread) that end in savePresetToFile / loadPresetFile, and every preset operation bumps Revision::preset
      (the editor's "warprPreset" event).  The preset name lives in the UiModel (persisted in the state), a loaded
      preset shows its FILE name.  Failures never touch the current state; they go to the error reporter (default: an
      async AlertWindow).  openSettings shows the SettingsWindow shell (oscillator budget, interpolation quality, CPU
      guard) persisted through AppSettings.
  vizFifo() receives real WarprVizFrames from the audio thread since D2 (PluginProcessor ->
  warpr::VizAnalyzer, ~30 Hz, wait-free; voices[] come from the synth's playhead snapshot).

  THREADING of the shaper side
    All curve parsing, LUT building and publication happen on the calling NON-audio thread (message thread for the
    editor, the host's state thread for a state load).  The APVTS listener on shaperPreset / shaperSymmetry may be
    called from ANY thread (host automation): it only sets two atomic dirty flags.  servicePending() - called by
    the processor's message-thread timer (and by tests) - does the actual work.
*/

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <atomic>
#include <functional>
#include <memory>

#include "Backend/UiBackend.h"
#include "Backend/UiModel.h"
#include "DSP/LfoTable.h"
#include "DSP/ModEngine.h"
#include "DSP/SampleStore.h"
#include "DSP/ShaperCurve.h"
#include "DSP/ShaperEngine.h"
#include "DSP/UnisonOsc.h"
#include "DSP/WarprSynth.h"
#include "State/AppSettings.h"
#include "State/PresetManager.h"
#include "State/StateIO.h"

class SettingsWindow;

class UiBackendImpl : public WarprUiBackend
{
public:
    UiBackendImpl();
    ~UiBackendImpl() override;

    /** The processor's sample store (outlives the backend's use of it; the processor shuts the
        loader down before destroying the backend). */
    void attachSampleStore (warpr::SampleStore* store) noexcept { sampleStore = store; }

    /** The processor's synth, read through its wait-free playhead snapshot only. */
    void attachSynth (const warpr::WarprSynth* s) noexcept { synthRef = s; }

    /** The processor's APVTS (shaperPreset / shaperSymmetry / shaperCurveSelect live there).  Registers the
        parameter listeners; call once, from the message thread, before audio starts. */
    void attachApvts (juce::AudioProcessorValueTreeState* apvtsToUse);

    /** The engine the built LUT sets are published to.  Builds and publishes the current curves immediately. */
    void attachShaper (warpr::ShaperEngine* engine);

    /** The publication point of the LFO tables / mod matrix / lfoCount (D8).  Publishes the current model (defaults on
        a fresh processor) immediately.  Message thread, before audio starts. */
    void attachModEngine (warpr::ModEngine* engine);

    /** Host sample rate for the filter response display (the processor's prepareToPlay).  Wait-free, any thread. */
    void setSampleRate (double fs) noexcept               { responseSampleRate.store (fs > 0.0 ? fs : 48000.0, std::memory_order_relaxed); }

    /** MESSAGE THREAD: applies a pending shaperPreset change (loads the preset curve into the selected curve and
        bumps Revision::shaperCurves) and a pending shaperSymmetry change (rebuilds the LUT set). */
    void servicePending();

    /** The LUT set most recently built (message-thread copy; tests and later phases). */
    warpr::ShaperLutSet::Ptr getShaperLut() const;

    int getPlayheads (WarprVizFrame::Voice* out, int maxOut) override
    {
        if (synthRef == nullptr || out == nullptr)
            return 0;
        float pos[WarprVizFrame::maxVoices], lvl[WarprVizFrame::maxVoices];
        const int n = synthRef->readPlayheads (pos, lvl, std::min (maxOut, (int) WarprVizFrame::maxVoices));
        for (int i = 0; i < n; ++i)
            out[i] = { pos[i], lvl[i] };
        return n;
    }

    //==========================================================================
    // WarprUiBackend
    WarprVizFifo& vizFifo() override                      { return viz; }
    uint32_t revision (Revision r) const override         { return revisions[slotOf (r)].load (std::memory_order_relaxed); }

    juce::String getUiStateJson() override;
    void setLfoCount (int n) override;
    void setLfoShapeJson (int index, const juce::String& json) override;
    void setModMatrixJson (const juce::String& json) override;
    void setSliceMapJson (const juce::String& json) override;
    bool computeSliceEdges (int count, bool byTransients, std::vector<double>& out) override;
    void setShaperCurveJson (const juce::String& curveId, const juce::String& json) override;
    juce::String getShaperCurveJson (const juce::String& curveId) override;
    void shaperReset (const juce::String& curveId) override;
    void shaperNormalize (const juce::String& curveId) override;

    void loadSampleFile (const juce::File& file) override;
    WarprSampleDisplay getSampleDisplay() override;
    std::vector<float> getRangePeaks (double startNorm, double endNorm, int numPairs) override;

    int computeProcessedPeaks (float* minMaxInterleaved, int pairs, double startNorm, double endNorm) override;

    void computeFilterResponse (float* dbOut, int n) override;

    juce::String getPresetName() override;
    void presetStep (int delta) override;
    void presetSaveDialog() override;
    void presetLoadDialog() override;
    void openSettings() override;

    //==========================================================================
    // Processor-side API

    /** Wait-free; legal from any thread (including the audio thread and parameter listeners). */
    void bumpRevision (Revision r) noexcept               { revisions[slotOf (r)].fetch_add (1, std::memory_order_relaxed); }

    /** Thread-safe copy of the non-parameter model (host state thread / message thread). */
    warpr::UiModel copyModel() const;

    /** State load / preset load: replaces the model and bumps uiState, sample, shaperCurves,
        filterResponse and preset so the editor re-emits everything.  The shaper curves are re-parsed
        (invalid documents fall back to the identity), the LUT set is rebuilt and published, and any pending
        shaperPreset / shaperSymmetry change from the state load itself is discarded (the stored curve wins). */
    void adoptModel (const warpr::UiModel& newModel);

    //==========================================================================
    // D9: presets, state host, settings (all MESSAGE thread)
    using StateCapture   = std::function<juce::MemoryBlock()>;
    using StateApply     = std::function<warpr::state::LoadOutcome (const void*, std::size_t)>;
    using ErrorReporter  = std::function<void (const juce::String&)>;
    using SettingsGetter = std::function<warpr::AppSettingsData()>;
    using SettingsSetter = std::function<void (const warpr::AppSettingsData&)>;

    /** The preset list / directory (outlives the backend's use of it).  nullptr = presets unavailable. */
    void attachPresetManager (warpr::PresetManager* manager) noexcept   { presetManager = manager; }

    /** The processor's complete-state capture and load (the same functions get/setStateInformation use). */
    void attachStateHost (StateCapture capture, StateApply apply);

    /** Settings persistence (read for the dialog, called on every change).  Without them openSettings reports the fact. */
    void attachSettings (SettingsGetter getter, SettingsSetter setter);

    /** Where user-visible failures go.  Default: an async native alert.  Tests install a recorder. */
    void setErrorReporter (ErrorReporter reporter);

    /** Names the preset (model + Revision::preset). */
    void setPresetName (const juce::String& name);

    /** Writes the current state to `file` (the preset extension is appended when missing) and names the preset after
        the file.  Returns false (name and files unchanged) on failure. */
    bool savePresetToFile (const juce::File& file);

    /** Save into the user preset directory under `name`. */
    bool saveUserPreset (const juce::String& name);

    /** Loads a preset file / list entry: all-or-nothing, the displayed name becomes the entry / file name. */
    bool loadPresetFile (const juce::File& file);
    bool loadPresetEntry (const warpr::PresetEntry& entry);

    bool isSettingsWindowShowing() const;

private:
    static constexpr size_t kNumRevisions = 5;
    static_assert (static_cast<size_t> (Revision::preset) + 1 == kNumRevisions,
                   "Revision enum changed: resize the revision counter array and re-audit every bumpRevision site");

    static size_t slotOf (Revision r) noexcept            { return static_cast<size_t> (r); }

    // ---- D9 helpers
    bool applyPresetBytes (const juce::MemoryBlock& block, const juce::String& displayName, juce::String& error);
    void reportError (const juce::String& message);
    void quietlySetPresetName (const juce::String& name);

    // ---- LFO / matrix publication (message / state threads)
    void publishLfoTable (int index, const juce::String& json);
    void publishAllModulation();

    // ---- shaper helpers (message / state threads)
    void rebuildShaper();
    void applyShaperPreset (int preset);
    void storeCurve (int curve, const warpr::ShaperCurve& c);
    void flipPresetToCustom();
    void setChoiceParam (const char* id, int index);
    bool isSymmetric() const;
    int  currentChoice (const char* id) const;

    struct ShaperParamListener : juce::AudioProcessorValueTreeState::Listener
    {
        explicit ShaperParamListener (UiBackendImpl& o) noexcept : owner (o) {}
        void parameterChanged (const juce::String& parameterID, float newValue) override;
        UiBackendImpl& owner;
    };

    /** Any of the 8 filter parameters changed: bump Revision::filterResponse (a single atomic increment, legal on any thread). */
    struct FilterParamListener : juce::AudioProcessorValueTreeState::Listener
    {
        explicit FilterParamListener (UiBackendImpl& o) noexcept : owner (o) {}
        void parameterChanged (const juce::String&, float) override { owner.bumpRevision (Revision::filterResponse); }
        UiBackendImpl& owner;
    };

    WarprVizFifo viz;
    std::array<std::atomic<uint32_t>, kNumRevisions> revisions {};
    std::atomic<double> responseSampleRate { 48000.0 };
    FilterParamListener filterListener { *this };
    warpr::SampleStore* sampleStore = nullptr;
    const warpr::WarprSynth* synthRef = nullptr;

    mutable juce::CriticalSection modelLock;              // message thread + host state thread; never the audio thread
    warpr::UiModel model;
    std::array<warpr::ShaperCurve, 2> shaperCurves;       // parsed twin of model.shaperCurveJson (guarded by modelLock)

    juce::AudioProcessorValueTreeState* apvts = nullptr;
    warpr::ShaperEngine* shaperEngine = nullptr;
    warpr::ModEngine* modEngine = nullptr;
    juce::CriticalSection lfoPublishLock;                 // serialises LFO table build + publish (message / state threads)
    std::array<juce::String, (size_t) warpr::UiModel::maxLfos> publishedLfoJson;
    std::array<bool, (size_t) warpr::UiModel::maxLfos> lfoPublished {};
    ShaperParamListener shaperListener { *this };
    std::atomic<bool> presetDirty { false }, symmetryDirty { false };
    mutable juce::CriticalSection shaperLock;             // serialises build + publish (taken BEFORE modelLock)
    warpr::ShaperLutSet::Ptr lastLut;                     // guarded by shaperLock

    // ---- processed-waveform preview (message thread only): a dedicated oscillator, never a voice's
    std::unique_ptr<warpr::UnisonOsc> previewOsc;

    // ---- D9 (message thread only)
    warpr::PresetManager* presetManager = nullptr;
    StateCapture stateCapture;
    StateApply stateApply;
    ErrorReporter errorReporter;
    SettingsGetter settingsGetter;
    SettingsSetter settingsSetter;
    std::unique_ptr<juce::FileChooser> chooser;           // the open async dialog (preset save / load)
    std::unique_ptr<SettingsWindow> settingsWindow;
};
