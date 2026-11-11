#pragma once
/*
  UiBackendImpl - implementation of WarprUiBackend owned by the processor.

  STATUS after phase D1.  Every interface method exists so the editor compiles and opens
  without "unknown to the backend" console errors.  Real today:
    - revision counters (atomics) and bumpRevision()
    - the message-thread UiModel: lfoCount / shape JSON / mod-matrix JSON / shaper-curve JSON /
      preset name are stored and returned (and persisted by StateIO).  Nothing parses them yet.
    - shaperReset() restores the identity curve document and bumps Revision::shaperCurves.
    - loadSampleFile() decodes on the SampleStore loader thread and publishes through the
      lock-free swap (bumps Revision::sample on success, explains failures with an async alert and
      keeps the previous sample); getSampleDisplay() reads the current sample's name / format / peaks.
  Everything else that needs DSP is a documented no-op until its phase:
    shaperNormalize, shaperPreset side effects  D4
    computeFilterResponse .................... D5      (flat 0 dB, no filter DSP exists yet)
    setLfoShape/setModMatrix publication ..... D8
    presets / settings ....................... D9
  vizFifo() is returned but nothing pushes into it yet (organism stays still).
*/

#include <array>
#include <atomic>

#include "Backend/UiBackend.h"
#include "Backend/UiModel.h"
#include "DSP/SampleStore.h"

class UiBackendImpl : public WarprUiBackend
{
public:
    UiBackendImpl() = default;
    ~UiBackendImpl() override = default;

    /** The processor's sample store (outlives the backend's use of it; the processor shuts the
        loader down before destroying the backend). */
    void attachSampleStore (warpr::SampleStore* store) noexcept { sampleStore = store; }

    //==========================================================================
    // WarprUiBackend
    WarprVizFifo& vizFifo() override                      { return viz; }
    uint32_t revision (Revision r) const override         { return revisions[slotOf (r)].load (std::memory_order_relaxed); }

    juce::String getUiStateJson() override;
    void setLfoCount (int n) override;
    void setLfoShapeJson (int index, const juce::String& json) override;
    void setModMatrixJson (const juce::String& json) override;
    void setShaperCurveJson (const juce::String& curveId, const juce::String& json) override;
    juce::String getShaperCurveJson (const juce::String& curveId) override;
    void shaperReset (const juce::String& curveId) override;
    void shaperNormalize (const juce::String& curveId) override;

    void loadSampleFile (const juce::File& file) override;
    WarprSampleDisplay getSampleDisplay() override;

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
        filterResponse and preset so the editor re-emits everything. */
    void adoptModel (const warpr::UiModel& newModel);

private:
    static size_t slotOf (Revision r) noexcept            { return static_cast<size_t> (r); }

    WarprVizFifo viz;
    std::array<std::atomic<uint32_t>, 5> revisions {};
    warpr::SampleStore* sampleStore = nullptr;

    mutable juce::CriticalSection modelLock;              // message thread + host state thread; never the audio thread
    warpr::UiModel model;
};
