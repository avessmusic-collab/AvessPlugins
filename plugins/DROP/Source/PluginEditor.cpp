#include "PluginEditor.h"
#include "BinaryData.h"

//==============================================================================
// Constructor - CRITICAL: Initialize in correct order (Relays -> WebView -> Attachments)
//==============================================================================

DROPAudioProcessorEditor::DROPAudioProcessorEditor(DROPAudioProcessor& p)
    : AudioProcessorEditor(&p), processorRef(p)
{
    // ------------------------------------------------------------------------
    // STEP 1: CREATE RELAYS (before WebView!) - all 19 APVTS parameters,
    // parameter IDs exactly matching plugins/DROP/.ideas/parameter-spec.md /
    // PluginProcessor.cpp's createParameterLayout().
    // ------------------------------------------------------------------------

    triggerRelay     = std::make_unique<juce::WebToggleButtonRelay>("trigger");
    buildLengthRelay = std::make_unique<juce::WebComboBoxRelay>("buildLength");
    styleRelay       = std::make_unique<juce::WebComboBoxRelay>("style");

    intensityRelay = std::make_unique<juce::WebSliderRelay>("intensity");
    dryWetRelay    = std::make_unique<juce::WebSliderRelay>("dryWet");

    tensionRelay = std::make_unique<juce::WebSliderRelay>("tension");
    chaosRelay   = std::make_unique<juce::WebSliderRelay>("chaos");
    impactRelay  = std::make_unique<juce::WebSliderRelay>("impact");

    filterRelay     = std::make_unique<juce::WebSliderRelay>("filter");
    widthRelay      = std::make_unique<juce::WebSliderRelay>("width");
    pitchRelay      = std::make_unique<juce::WebSliderRelay>("pitch");
    gateRelay       = std::make_unique<juce::WebSliderRelay>("gate");
    stutterRelay    = std::make_unique<juce::WebSliderRelay>("stutter");
    delayRelay      = std::make_unique<juce::WebSliderRelay>("delay");
    reverbRelay     = std::make_unique<juce::WebSliderRelay>("reverb");
    distortionRelay = std::make_unique<juce::WebSliderRelay>("distortion");
    digitalRelay    = std::make_unique<juce::WebSliderRelay>("digital");
    noiseRelay      = std::make_unique<juce::WebSliderRelay>("noise");

    outputRelay = std::make_unique<juce::WebSliderRelay>("output");

    // ------------------------------------------------------------------------
    // STEP 2: CREATE WEBVIEW (with relay options + the non-APVTS
    // "triggerMode" native-function bridge, see PluginEditor.h's top doc
    // comment for the full design/rationale)
    // ------------------------------------------------------------------------
    webView = std::make_unique<juce::WebBrowserComponent>(
        juce::WebBrowserComponent::Options{}
            .withNativeIntegrationEnabled()
            .withResourceProvider([this](const auto& url) { return getResource(url); })
            .withKeepPageLoadedWhenBrowserIsHidden()

            .withOptionsFrom(*triggerRelay)
            .withOptionsFrom(*buildLengthRelay)
            .withOptionsFrom(*styleRelay)
            .withOptionsFrom(*intensityRelay)
            .withOptionsFrom(*dryWetRelay)
            .withOptionsFrom(*tensionRelay)
            .withOptionsFrom(*chaosRelay)
            .withOptionsFrom(*impactRelay)
            .withOptionsFrom(*filterRelay)
            .withOptionsFrom(*widthRelay)
            .withOptionsFrom(*pitchRelay)
            .withOptionsFrom(*gateRelay)
            .withOptionsFrom(*stutterRelay)
            .withOptionsFrom(*delayRelay)
            .withOptionsFrom(*reverbRelay)
            .withOptionsFrom(*distortionRelay)
            .withOptionsFrom(*digitalRelay)
            .withOptionsFrom(*noiseRelay)
            .withOptionsFrom(*outputRelay)

            // --- Non-APVTS "trigger mode" bridge (architecture.md: manual/
            // MIDI/auto-sync is non-parameter state, not an APVTS parameter -
            // cannot use a Relay/Attachment pair, same reasoning as
            // CORRUPTR's Rhythmic Sequencer pattern-data bridge) ---
            .withNativeFunction("setTriggerMode",
                [this](const juce::Array<juce::var>& args, juce::WebBrowserComponent::NativeFunctionCompletion completion)
                {
                    if (args.size() >= 1)
                        setTriggerModeIndex(static_cast<int>(args[0]));
                    completion(true);
                })
            .withNativeFunction("getInitialUIState",
                [this](const juce::Array<juce::var>&, juce::WebBrowserComponent::NativeFunctionCompletion completion)
                {
                    auto* obj = new juce::DynamicObject();
                    obj->setProperty("triggerMode", getTriggerModeIndex());
                    completion(juce::var(obj));
                })
    );

    // ------------------------------------------------------------------------
    // STEP 3: CREATE PARAMETER ATTACHMENTS (after WebView!) - 19 total.
    // JUCE 8 requires the 3-argument constructor (parameter, relay,
    // undoManager) - juce8-critical-patterns.md #12.
    //
    // CORRECTED from the v10-PluginEditor.cpp template's ASSUMPTION: the
    // real PluginProcessor has no `getAPVTS()` accessor - the
    // AudioProcessorValueTreeState is the public member `parameters`
    // (matches this repo's convention, e.g. plugins/Transitionist's
    // `processorRef.parameters.getParameter(...)`).
    // ------------------------------------------------------------------------

    triggerAttachment = std::make_unique<juce::WebToggleButtonParameterAttachment>(
        *processorRef.parameters.getParameter("trigger"), *triggerRelay, nullptr);
    buildLengthAttachment = std::make_unique<juce::WebComboBoxParameterAttachment>(
        *processorRef.parameters.getParameter("buildLength"), *buildLengthRelay, nullptr);
    styleAttachment = std::make_unique<juce::WebComboBoxParameterAttachment>(
        *processorRef.parameters.getParameter("style"), *styleRelay, nullptr);

    intensityAttachment = std::make_unique<juce::WebSliderParameterAttachment>(
        *processorRef.parameters.getParameter("intensity"), *intensityRelay, nullptr);
    dryWetAttachment = std::make_unique<juce::WebSliderParameterAttachment>(
        *processorRef.parameters.getParameter("dryWet"), *dryWetRelay, nullptr);

    tensionAttachment = std::make_unique<juce::WebSliderParameterAttachment>(
        *processorRef.parameters.getParameter("tension"), *tensionRelay, nullptr);
    chaosAttachment = std::make_unique<juce::WebSliderParameterAttachment>(
        *processorRef.parameters.getParameter("chaos"), *chaosRelay, nullptr);
    impactAttachment = std::make_unique<juce::WebSliderParameterAttachment>(
        *processorRef.parameters.getParameter("impact"), *impactRelay, nullptr);

    filterAttachment = std::make_unique<juce::WebSliderParameterAttachment>(
        *processorRef.parameters.getParameter("filter"), *filterRelay, nullptr);
    widthAttachment = std::make_unique<juce::WebSliderParameterAttachment>(
        *processorRef.parameters.getParameter("width"), *widthRelay, nullptr);
    pitchAttachment = std::make_unique<juce::WebSliderParameterAttachment>(
        *processorRef.parameters.getParameter("pitch"), *pitchRelay, nullptr);
    gateAttachment = std::make_unique<juce::WebSliderParameterAttachment>(
        *processorRef.parameters.getParameter("gate"), *gateRelay, nullptr);
    stutterAttachment = std::make_unique<juce::WebSliderParameterAttachment>(
        *processorRef.parameters.getParameter("stutter"), *stutterRelay, nullptr);
    delayAttachment = std::make_unique<juce::WebSliderParameterAttachment>(
        *processorRef.parameters.getParameter("delay"), *delayRelay, nullptr);
    reverbAttachment = std::make_unique<juce::WebSliderParameterAttachment>(
        *processorRef.parameters.getParameter("reverb"), *reverbRelay, nullptr);
    distortionAttachment = std::make_unique<juce::WebSliderParameterAttachment>(
        *processorRef.parameters.getParameter("distortion"), *distortionRelay, nullptr);
    digitalAttachment = std::make_unique<juce::WebSliderParameterAttachment>(
        *processorRef.parameters.getParameter("digital"), *digitalRelay, nullptr);
    noiseAttachment = std::make_unique<juce::WebSliderParameterAttachment>(
        *processorRef.parameters.getParameter("noise"), *noiseRelay, nullptr);

    outputAttachment = std::make_unique<juce::WebSliderParameterAttachment>(
        *processorRef.parameters.getParameter("output"), *outputRelay, nullptr);

    // ------------------------------------------------------------------------
    // WEBVIEW SETUP
    // ------------------------------------------------------------------------

    // Cache-bust every launch (juce8-critical-patterns.md #24) - avoids the
    // WebView serving a stale snapshot of a previous load of this exact URL
    // during active development.
    const juce::String cacheBuster = "?t=" + juce::String(juce::Time::getMillisecondCounterHiRes(), 0);
    webView->goToURL(juce::WebBrowserComponent::getResourceProviderRoot() + cacheBuster);

    addAndMakeVisible(*webView);

    // Window size from v10-ui.yaml `window` block: base 1000x560, resizable,
    // min 800x450, max 1400x790. v10-ui.html's own CSS scaling model
    // (root-em ResizeObserver, clamp 0.75x-1.5x) is a superset safety
    // margin on top of this native clamp - the two are independent, both
    // intentional (see v10-ui.html's applyScale() doc comment). Matches
    // Stage 1's placeholder editor sizing exactly (no host-visible size
    // jump on this swap).
    setSize(1000, 560);
    setResizable(true, true);
    setResizeLimits(800, 450, 1400, 790);

    // Start the 30Hz visualization push (progress/tension/state/input-RMS/
    // active-module-weights - see this class's header doc comment,
    // "VISUALIZATION"; full drain-correctness verification is Phase 4.3's
    // job, this phase only needs the plumbing to compile and not crash).
    // Started LAST, after webView/relays/attachments all exist, so the very
    // first timerCallback() (which can fire as soon as ~33ms from now)
    // never races construction.
    startTimerHz(30);
}

//==============================================================================
// Destructor
//==============================================================================

DROPAudioProcessorEditor::~DROPAudioProcessorEditor()
{
    // Stop the Timer FIRST, before any member (webView in particular) starts
    // tearing down - juce::Timer callbacks can in principle still fire
    // during destruction otherwise. Explicit stopTimer() guarantees
    // timerCallback() can never touch a partially-destroyed webView.
    stopTimer();

    // Remaining members are automatically destroyed in reverse order of
    // declaration: Attachments -> WebView -> Relays. No further manual
    // cleanup needed.
}

//==============================================================================
// AudioProcessorEditor Overrides
//==============================================================================

void DROPAudioProcessorEditor::paint(juce::Graphics& g)
{
    // WebView fills the entire editor, so no custom painting needed.
    g.fillAll(juce::Colours::black);
}

void DROPAudioProcessorEditor::resized()
{
    if (webView)
        webView->setBounds(getLocalBounds());
}

//==============================================================================
// Resource Provider (JUCE 8 Required Pattern - explicit URL mapping,
// juce8-critical-patterns.md #8)
//==============================================================================

std::optional<juce::WebBrowserComponent::Resource> DROPAudioProcessorEditor::getResource(
    const juce::String& url)
{
    auto makeVector = [](const char* data, int size)
    {
        return std::vector<std::byte>(
            reinterpret_cast<const std::byte*>(data),
            reinterpret_cast<const std::byte*>(data) + size);
    };

    // Strip query string (cache-buster, Pattern #24) before matching.
    const juce::String path = url.upToFirstOccurrenceOf("?", false, false);

    if (path == "/" || path == "/index.html")
    {
        return juce::WebBrowserComponent::Resource {
            makeVector(BinaryData::index_html, BinaryData::index_htmlSize),
            juce::String("text/html")
        };
    }

    if (path == "/js/juce/index.js")
    {
        return juce::WebBrowserComponent::Resource {
            makeVector(BinaryData::index_js, BinaryData::index_jsSize),
            juce::String("text/javascript")
        };
    }

    if (path == "/js/juce/check_native_interop.js")
    {
        return juce::WebBrowserComponent::Resource {
            makeVector(BinaryData::check_native_interop_js, BinaryData::check_native_interop_jsSize),
            juce::String("text/javascript")
        };
    }

    // Bundled marble surface texture (v10-ui.yaml `assets` block) - CMake's
    // juce_add_binary_data() mangles "v10-marble2-texture-hd.jpg" into the
    // BinaryData symbol v10marble2texturehd_jpg (all non-alphanumeric
    // characters become underscores).
    if (path == "/v10-marble2-texture-hd.jpg")
    {
        return juce::WebBrowserComponent::Resource {
            makeVector(BinaryData::v10marble2texturehd_jpg, BinaryData::v10marble2texturehd_jpgSize),
            juce::String("image/jpeg")
        };
    }

    return std::nullopt;
}

//==============================================================================
// Non-APVTS "trigger mode" bridge
//
// CORRECTED from the v10-PluginEditor.cpp template's ASSUMPTION: the real
// PluginProcessor does NOT expose a `getCustomStateTree()` ValueTree
// accessor - trigger mode is a plain `std::atomic<int>` on
// DROPAudioProcessor, read/written via the strongly-typed
// `getTriggerMode()`/`setTriggerMode(TriggerMode)` accessors (see
// PluginProcessor.h's `TriggerMode` enum). No ValueTree lookup needed here.
//==============================================================================

int DROPAudioProcessorEditor::getTriggerModeIndex() const
{
    return juce::jlimit(0, 2, static_cast<int>(processorRef.getTriggerMode()));
}

void DROPAudioProcessorEditor::setTriggerModeIndex(int index)
{
    index = juce::jlimit(0, 2, index);
    processorRef.setTriggerMode(static_cast<DROPAudioProcessor::TriggerMode>(index));
}

//==============================================================================
// 30Hz visualization push (see this class's header doc comment,
// "VISUALIZATION").
//==============================================================================

juce::String DROPAudioProcessorEditor::dropStateToString(int dropStateEnumValue)
{
    // Mirrors drop::DropState's enum order exactly (Source/DSP/
    // DropStateMachine.h: Idle=0, Arming=1, Building=2, Peak=3, Impact=4,
    // Recovery=5 - verified against the real enum, not assumed).
    switch (dropStateEnumValue)
    {
        case 0: return "idle";
        case 1: return "arming";
        case 2: return "building";
        case 3: return "peak";
        case 4: return "impact";
        case 5: return "recovery";
        default: return "idle";
    }
}

juce::var DROPAudioProcessorEditor::buildDropVisUpdateVar()
{
    // CORRECTED from the v10-PluginEditor.cpp template's ASSUMPTION: the
    // real PluginProcessor has no single struct-returning
    // `getLatestVisualizationSnapshot()` call - it exposes the lock-free
    // SPSC ring buffer directly via
    // `bool popVisualizationFrame(VisualizationFrame&) noexcept`
    // (message-thread-only). Drain every pending frame this tick, keeping
    // the most recent one; if the FIFO was empty, fall back to the last
    // frame we already have cached (so closing/reopening the editor
    // mid-transition - or a UI tick landing between audio-thread pushes -
    // still reflects the current state rather than snapping back to
    // Idle/zero). Full drain-correctness verification is Phase 4.3's job;
    // this phase only needs a real, non-placeholder accessor call.
    DROPAudioProcessor::VisualizationFrame frame;
    bool gotFrame = false;
    DROPAudioProcessor::VisualizationFrame latest;
    while (processorRef.popVisualizationFrame(frame))
    {
        latest = frame;
        gotFrame = true;
    }
    if (gotFrame)
        lastVisualizationFrame = latest;

    auto* obj = new juce::DynamicObject();

    obj->setProperty("currentState", dropStateToString(lastVisualizationFrame.currentState));
    obj->setProperty("progress", lastVisualizationFrame.progress);
    obj->setProperty("tensionLevel", lastVisualizationFrame.tensionLevel);
    obj->setProperty("inputRMS", lastVisualizationFrame.inputRMS);

    // 10 IntensityBudget module weights - PINNED order per
    // PluginProcessor.h's VisualizationFrame doc comment: Filter, Width,
    // Pitch, Gate, Stutter, Delay, Reverb, Distortion, Digital, Noise
    // (matches drop::BudgetModule's enum declaration order exactly).
    juce::Array<juce::var> weights;
    for (float weight : lastVisualizationFrame.activeModuleWeights)
        weights.add(weight);
    obj->setProperty("activeModuleWeights", weights);

    return juce::var(obj);
}

void DROPAudioProcessorEditor::timerCallback()
{
    // Defensive null check only - webView is always non-null by the time
    // this Timer's callbacks can fire (started at the very end of the
    // constructor, after webView is constructed), but the check costs
    // nothing and guards against any future reordering.
    if (webView)
        webView->emitEventIfBrowserIsVisible("dropVisUpdate", buildDropVisUpdateVar());
}
