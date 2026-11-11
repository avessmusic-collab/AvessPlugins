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
  vizFifo() receives real WarprVizFrames from the audio thread since D2 (PluginProcessor ->
  warpr::VizAnalyzer, ~30 Hz, wait-free; voices[] come from the synth's playhead snapshot).
*/

#include <array>
#include <atomic>

#include "Backend/UiBackend.h"
#include "Backend/UiModel.h"
#include "DSP/SampleStore.h"
#include "DSP/WarprSynth.h"

class UiBackendImpl : public WarprUiBackend
{
public:
    UiBackendImpl() = default;
    ~UiBackendImpl() override = default;

    /** The processor's sample store (outlives the backend's use of it; the processor shuts the
        loader down before destroying the backend). */
    void attachSampleStore (warpr::SampleStore* store) noexcept { sampleStore = store; }

    /** The processor's synth, read through its wait-free playhead snapshot only. */
    void attachSynth (const warpr::WarprSynth* s) noexcept { synthRef = s; }

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
    const warpr::WarprSynth* synthRef = nullptr;

    mutable juce::CriticalSection modelLock;              // message thread + host state thread; never the audio thread
    warpr::UiModel model;
};
