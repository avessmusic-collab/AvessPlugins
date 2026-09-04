#include "PluginEditor.h"
#include "BinaryData.h"

//==============================================================================
// Constructor - CRITICAL: Initialize in correct order (Relays -> WebView -> Attachments)
//==============================================================================

CORRUPTRAudioProcessorEditor::CORRUPTRAudioProcessorEditor(CORRUPTRAudioProcessor& p)
    : AudioProcessorEditor(&p), processorRef(p)
{
    // ------------------------------------------------------------------------
    // STEP 1: CREATE RELAYS (before WebView!) - Phase 5.1 scope, 14 parameters
    // ------------------------------------------------------------------------

    // --- Top Bar / Global ---
    qualityModeRelay = std::make_unique<juce::WebComboBoxRelay>("qualityMode");

    // --- Distortion ---
    graphBypassSaturationRelay = std::make_unique<juce::WebToggleButtonRelay>("graphBypassSaturation");
    distortionAlgorithmRelay = std::make_unique<juce::WebComboBoxRelay>("distortionAlgorithm");
    driveRelay = std::make_unique<juce::WebSliderRelay>("drive");
    foldRelay = std::make_unique<juce::WebSliderRelay>("fold");
    graphBypassWaveshaperRelay = std::make_unique<juce::WebToggleButtonRelay>("graphBypassWaveshaper");

    // --- Filter & EQ ---
    graphBypassFilterRelay = std::make_unique<juce::WebToggleButtonRelay>("graphBypassFilter");
    filterTypeRelay = std::make_unique<juce::WebComboBoxRelay>("filterType");
    filterCutoffRelay = std::make_unique<juce::WebSliderRelay>("filterCutoff");
    filterResonanceRelay = std::make_unique<juce::WebSliderRelay>("filterResonance");

    // --- Center Master (Output/Global) ---
    mixRelay = std::make_unique<juce::WebSliderRelay>("mix");
    outputLimiterStyleRelay = std::make_unique<juce::WebComboBoxRelay>("outputLimiterStyle");
    inputGainRelay = std::make_unique<juce::WebSliderRelay>("inputGain");
    outputGainRelay = std::make_unique<juce::WebSliderRelay>("outputGain");

    // --- Phase 5.2: Bitcrush ---
    graphBypassBitcrushRelay = std::make_unique<juce::WebToggleButtonRelay>("graphBypassBitcrush");
    bitDepthRelay = std::make_unique<juce::WebSliderRelay>("bitDepth");

    // --- Phase 5.2: Glitch ---
    graphBypassGlitchRelay = std::make_unique<juce::WebToggleButtonRelay>("graphBypassGlitch");
    glitchModeRelay = std::make_unique<juce::WebComboBoxRelay>("glitchMode");
    glitchBufferLengthRelay = std::make_unique<juce::WebComboBoxRelay>("glitchBufferLength");
    glitchProbabilityRelay = std::make_unique<juce::WebSliderRelay>("glitchProbability");

    // --- Phase 5.2: Rhythmic Sequencer ---
    sequencerEnabledRelay = std::make_unique<juce::WebToggleButtonRelay>("sequencerEnabled");
    sequencerRateRelay = std::make_unique<juce::WebComboBoxRelay>("sequencerRate");
    sequencerStepsRelay = std::make_unique<juce::WebComboBoxRelay>("sequencerSteps");

    // ------------------------------------------------------------------------
    // STEP 2: CREATE WEBVIEW (with relay options + Phase 5.2's sequencer
    // pattern-data native-function bridge - see PluginEditor.h's "PHASE 5.2
    // ADDITIONS" doc comment for the full design)
    // ------------------------------------------------------------------------
    webView = std::make_unique<juce::WebBrowserComponent>(
        juce::WebBrowserComponent::Options{}
            .withNativeIntegrationEnabled()
            .withResourceProvider([this](const auto& url) { return getResource(url); })
            .withKeepPageLoadedWhenBrowserIsHidden()

            .withOptionsFrom(*qualityModeRelay)
            .withOptionsFrom(*graphBypassSaturationRelay)
            .withOptionsFrom(*distortionAlgorithmRelay)
            .withOptionsFrom(*driveRelay)
            .withOptionsFrom(*foldRelay)
            .withOptionsFrom(*graphBypassWaveshaperRelay)
            .withOptionsFrom(*graphBypassFilterRelay)
            .withOptionsFrom(*filterTypeRelay)
            .withOptionsFrom(*filterCutoffRelay)
            .withOptionsFrom(*filterResonanceRelay)
            .withOptionsFrom(*mixRelay)
            .withOptionsFrom(*outputLimiterStyleRelay)
            .withOptionsFrom(*inputGainRelay)
            .withOptionsFrom(*outputGainRelay)
            .withOptionsFrom(*graphBypassBitcrushRelay)
            .withOptionsFrom(*bitDepthRelay)
            .withOptionsFrom(*graphBypassGlitchRelay)
            .withOptionsFrom(*glitchModeRelay)
            .withOptionsFrom(*glitchBufferLengthRelay)
            .withOptionsFrom(*glitchProbabilityRelay)
            .withOptionsFrom(*sequencerEnabledRelay)
            .withOptionsFrom(*sequencerRateRelay)
            .withOptionsFrom(*sequencerStepsRelay)

            // --- Phase 5.2: Rhythmic Sequencer pattern-data bridge (custom
            // state, NOT an APVTS parameter - cannot use a Relay/Attachment
            // pair, see PluginEditor.h's top doc comment for the full design) ---
            .withNativeFunction("sequencerRequestPattern",
                [this](const juce::Array<juce::var>&, juce::WebBrowserComponent::NativeFunctionCompletion completion)
                {
                    emitSequencerPatternChanged();
                    completion(true);
                })
            .withNativeFunction("sequencerSetStep",
                [this](const juce::Array<juce::var>& args, juce::WebBrowserComponent::NativeFunctionCompletion completion)
                {
                    if (args.size() >= 3)
                    {
                        const int lane = static_cast<int>(args[0]);
                        const int step = static_cast<int>(args[1]);
                        const float value = static_cast<float>(args[2]);
                        // setStep() alone does NOT publish (RhythmicSequencer.h's own
                        // doc comment recommends batching many setStep() calls into one
                        // publishSnapshot() per gesture) - this bridge deliberately
                        // publishes on EVERY call instead, for immediate audible
                        // feedback while drag-editing a step. publishSnapshot() is
                        // real-time-safe by design (fixed-size struct copy + one
                        // atomic store, no allocation), and this call site is only
                        // ever reached from human mouse interaction (bounded to a few
                        // hundred Hz at most, nowhere near audio-thread rates), so the
                        // extra publish-per-cell cost is negligible.
                        processorRef.getRhythmicSequencer().setStep(lane, step, value);
                        processorRef.getRhythmicSequencer().publishSnapshot();
                    }
                    completion(true);
                })
            .withNativeFunction("sequencerPatternOp",
                [this](const juce::Array<juce::var>& args, juce::WebBrowserComponent::NativeFunctionCompletion completion)
                {
                    if (args.size() >= 1)
                    {
                        const juce::String op = args[0].toString();
                        const int numSteps = getCurrentSequencerNumSteps();
                        auto& seq = processorRef.getRhythmicSequencer();

                        // Only "random"/"mutate"/"reverse"/"mirror"/"shift"/"clear" have
                        // markup buttons in index.html's Sequencer card this phase
                        // (matching v5-ui.html's 6 action buttons) - "half"/"double"/
                        // "syncopate"/"humanize" are wired here anyway (all 10 of
                        // RhythmicSequencer's opXxx() operations, per its own API) so a
                        // future mockup iteration can add buttons for them with zero
                        // C++ changes, consistent with this phase's "extend, don't
                        // redesign" markup constraint.
                        if (op == "random")         seq.opRandom(numSteps);
                        else if (op == "mutate")    seq.opMutate(numSteps);
                        else if (op == "reverse")   seq.opReverse(numSteps);
                        else if (op == "mirror")    seq.opMirror(numSteps);
                        else if (op == "shift")     seq.opShift(numSteps, 1); // default +1 step; no shift-amount control exists in the mockup
                        else if (op == "half")      seq.opHalf(numSteps);
                        else if (op == "double")    seq.opDouble(numSteps);
                        else if (op == "syncopate") seq.opSyncopate(numSteps);
                        else if (op == "humanize")  seq.opHumanize(numSteps);
                        else if (op == "clear")     seq.opClear(numSteps);
                        // unrecognised op name: no-op (still falls through to the
                        // refresh emit below, harmless - JS will just re-render the
                        // pattern unchanged).
                    }
                    // Every opXxx() call above already published internally - this
                    // event tells the JS side (which cannot predict the resulting
                    // values) to re-fetch/re-render.
                    emitSequencerPatternChanged();
                    completion(true);
                })
    );

    // ------------------------------------------------------------------------
    // STEP 3: CREATE PARAMETER ATTACHMENTS (after WebView!) - 14 total
    // JUCE 8 requires the 3-parameter constructor (parameter, relay, undoManager)
    // ------------------------------------------------------------------------

    // --- Top Bar / Global ---
    qualityModeAttachment = std::make_unique<juce::WebComboBoxParameterAttachment>(
        *processorRef.getAPVTS().getParameter("qualityMode"), *qualityModeRelay, nullptr);

    // --- Distortion ---
    graphBypassSaturationAttachment = std::make_unique<juce::WebToggleButtonParameterAttachment>(
        *processorRef.getAPVTS().getParameter("graphBypassSaturation"), *graphBypassSaturationRelay, nullptr);
    distortionAlgorithmAttachment = std::make_unique<juce::WebComboBoxParameterAttachment>(
        *processorRef.getAPVTS().getParameter("distortionAlgorithm"), *distortionAlgorithmRelay, nullptr);
    driveAttachment = std::make_unique<juce::WebSliderParameterAttachment>(
        *processorRef.getAPVTS().getParameter("drive"), *driveRelay, nullptr);
    foldAttachment = std::make_unique<juce::WebSliderParameterAttachment>(
        *processorRef.getAPVTS().getParameter("fold"), *foldRelay, nullptr);
    graphBypassWaveshaperAttachment = std::make_unique<juce::WebToggleButtonParameterAttachment>(
        *processorRef.getAPVTS().getParameter("graphBypassWaveshaper"), *graphBypassWaveshaperRelay, nullptr);

    // --- Filter & EQ ---
    graphBypassFilterAttachment = std::make_unique<juce::WebToggleButtonParameterAttachment>(
        *processorRef.getAPVTS().getParameter("graphBypassFilter"), *graphBypassFilterRelay, nullptr);
    filterTypeAttachment = std::make_unique<juce::WebComboBoxParameterAttachment>(
        *processorRef.getAPVTS().getParameter("filterType"), *filterTypeRelay, nullptr);
    filterCutoffAttachment = std::make_unique<juce::WebSliderParameterAttachment>(
        *processorRef.getAPVTS().getParameter("filterCutoff"), *filterCutoffRelay, nullptr);
    filterResonanceAttachment = std::make_unique<juce::WebSliderParameterAttachment>(
        *processorRef.getAPVTS().getParameter("filterResonance"), *filterResonanceRelay, nullptr);

    // --- Center Master (Output/Global) ---
    mixAttachment = std::make_unique<juce::WebSliderParameterAttachment>(
        *processorRef.getAPVTS().getParameter("mix"), *mixRelay, nullptr);
    outputLimiterStyleAttachment = std::make_unique<juce::WebComboBoxParameterAttachment>(
        *processorRef.getAPVTS().getParameter("outputLimiterStyle"), *outputLimiterStyleRelay, nullptr);
    inputGainAttachment = std::make_unique<juce::WebSliderParameterAttachment>(
        *processorRef.getAPVTS().getParameter("inputGain"), *inputGainRelay, nullptr);
    outputGainAttachment = std::make_unique<juce::WebSliderParameterAttachment>(
        *processorRef.getAPVTS().getParameter("outputGain"), *outputGainRelay, nullptr);

    // --- Phase 5.2: Bitcrush ---
    graphBypassBitcrushAttachment = std::make_unique<juce::WebToggleButtonParameterAttachment>(
        *processorRef.getAPVTS().getParameter("graphBypassBitcrush"), *graphBypassBitcrushRelay, nullptr);
    bitDepthAttachment = std::make_unique<juce::WebSliderParameterAttachment>(
        *processorRef.getAPVTS().getParameter("bitDepth"), *bitDepthRelay, nullptr);

    // --- Phase 5.2: Glitch ---
    graphBypassGlitchAttachment = std::make_unique<juce::WebToggleButtonParameterAttachment>(
        *processorRef.getAPVTS().getParameter("graphBypassGlitch"), *graphBypassGlitchRelay, nullptr);
    glitchModeAttachment = std::make_unique<juce::WebComboBoxParameterAttachment>(
        *processorRef.getAPVTS().getParameter("glitchMode"), *glitchModeRelay, nullptr);
    glitchBufferLengthAttachment = std::make_unique<juce::WebComboBoxParameterAttachment>(
        *processorRef.getAPVTS().getParameter("glitchBufferLength"), *glitchBufferLengthRelay, nullptr);
    glitchProbabilityAttachment = std::make_unique<juce::WebSliderParameterAttachment>(
        *processorRef.getAPVTS().getParameter("glitchProbability"), *glitchProbabilityRelay, nullptr);

    // --- Phase 5.2: Rhythmic Sequencer ---
    sequencerEnabledAttachment = std::make_unique<juce::WebToggleButtonParameterAttachment>(
        *processorRef.getAPVTS().getParameter("sequencerEnabled"), *sequencerEnabledRelay, nullptr);
    sequencerRateAttachment = std::make_unique<juce::WebComboBoxParameterAttachment>(
        *processorRef.getAPVTS().getParameter("sequencerRate"), *sequencerRateRelay, nullptr);
    sequencerStepsAttachment = std::make_unique<juce::WebComboBoxParameterAttachment>(
        *processorRef.getAPVTS().getParameter("sequencerSteps"), *sequencerStepsRelay, nullptr);

    // ------------------------------------------------------------------------
    // WEBVIEW SETUP
    // ------------------------------------------------------------------------

    // Cache-bust every launch (Pattern #24) - avoids WKWebView serving a
    // stale snapshot of a previous load of this exact URL during active
    // development where HTML/JS content changes between builds.
    const juce::String cacheBuster = "?t=" + juce::String(juce::Time::getMillisecondCounterHiRes(), 0);
    webView->goToURL(juce::WebBrowserComponent::getResourceProviderRoot() + cacheBuster);

    addAndMakeVisible(*webView);

    // Window size from v5-ui.yaml: width 1200, viewport_height 800 (fixed
    // JUCE editor size). Total scrollable content is 1650px tall and scrolls
    // INTERNALLY within the WebView's .plugin-frame div - do NOT setSize(1200,1650).
    setSize(1200, 800);
    setResizable(false, false);
}

//==============================================================================
// Destructor
//==============================================================================

CORRUPTRAudioProcessorEditor::~CORRUPTRAudioProcessorEditor()
{
    // Members are automatically destroyed in reverse order of declaration:
    // Attachments -> WebView -> Relays. No manual cleanup needed.
}

//==============================================================================
// AudioProcessorEditor Overrides
//==============================================================================

void CORRUPTRAudioProcessorEditor::paint(juce::Graphics& g)
{
    // WebView fills the entire editor, so no custom painting needed.
    g.fillAll(juce::Colours::black);
}

void CORRUPTRAudioProcessorEditor::resized()
{
    if (webView)
        webView->setBounds(getLocalBounds());
}

//==============================================================================
// Resource Provider (JUCE 8 Required Pattern - explicit URL mapping, Pattern #8)
//==============================================================================

std::optional<juce::WebBrowserComponent::Resource> CORRUPTRAudioProcessorEditor::getResource(
    const juce::String& url)
{
    auto makeVector = [](const char* data, int size)
    {
        return std::vector<std::byte>(
            reinterpret_cast<const std::byte*>(data),
            reinterpret_cast<const std::byte*>(data) + size);
    };

    // Strip query string (cache-buster) before matching (Pattern #24)
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
            juce::String("application/javascript")
        };
    }

    if (path == "/js/juce/check_native_interop.js")
    {
        return juce::WebBrowserComponent::Resource {
            makeVector(BinaryData::check_native_interop_js, BinaryData::check_native_interop_jsSize),
            juce::String("application/javascript")
        };
    }

    return std::nullopt;
}

//==============================================================================
// Phase 5.2: Rhythmic Sequencer pattern-data bridge helpers
// (see PluginEditor.h's "PHASE 5.2 ADDITIONS" doc comment for the full design)
//==============================================================================

juce::var CORRUPTRAudioProcessorEditor::buildSequencerPatternVar() const
{
    auto* obj = new juce::DynamicObject();
    auto& seq = processorRef.getRhythmicSequencer();

    juce::Array<juce::var> lanesArray;
    for (int lane = 0; lane < RhythmicSequencer::kNumLanes; ++lane)
    {
        juce::Array<juce::var> laneArray;
        for (int step = 0; step < RhythmicSequencer::kMaxSteps; ++step)
            laneArray.add(seq.getStep(lane, step));
        lanesArray.add(juce::var(laneArray));
    }
    obj->setProperty("lanes", lanesArray);

    return juce::var(obj);
}

void CORRUPTRAudioProcessorEditor::emitSequencerPatternChanged()
{
    // Defensive null check only - by the time any JS-triggered native
    // function callback can run, the page has already loaded, meaning the
    // constructor (which assigns webView) has long since returned.
    if (webView)
        webView->emitEventIfBrowserIsVisible("sequencerPatternChanged", buildSequencerPatternVar());
}

int CORRUPTRAudioProcessorEditor::getCurrentSequencerNumSteps() const
{
    // Same formula as CORRUPTRAudioProcessor::updateSequencerStepAndContributions()'s
    // own sequencerNumSteps resolution (PluginProcessor.cpp): AudioParameterChoice
    // raw values are stored as their integer index (0="16"/1="32"), NOT a
    // normalised 0-1 value.
    if (auto* p = processorRef.getAPVTS().getRawParameterValue("sequencerSteps"))
        return (static_cast<int>(p->load()) == 1) ? 32 : 16;
    return 16;
}
