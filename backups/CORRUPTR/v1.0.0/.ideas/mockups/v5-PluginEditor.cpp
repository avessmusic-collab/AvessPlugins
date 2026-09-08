#include "PluginEditor.h"

//==============================================================================
// Constructor - CRITICAL: Initialize in correct order
//==============================================================================

CORRUPTRAudioProcessorEditor::CORRUPTRAudioProcessorEditor(CORRUPTRAudioProcessor& p)
    : AudioProcessorEditor(&p), audioProcessor(p)
{
    // ========================================================================
    // INITIALIZATION SEQUENCE (CRITICAL ORDER)
    // ========================================================================
    //
    // 1. Create relays FIRST (before WebView construction)
    // 2. Create WebView with relay options
    // 3. Create parameter attachments LAST (after WebView construction)
    //
    // This matches the member declaration order and ensures safe destruction.
    // ========================================================================

    // ------------------------------------------------------------------------
    // STEP 1: CREATE RELAYS (before WebView!) - 56 total, module-grouped
    // ------------------------------------------------------------------------
    //
    // Each relay bridges a C++ parameter to JavaScript state. Relay constructor
    // takes the parameter ID (must match APVTS / parameter-spec.md exactly).
    //
    // --- Top Bar / Global ---
    qualityModeRelay = std::make_unique<juce::WebComboBoxRelay>("qualityMode");

    // --- Distortion ---
    graphBypassSaturationRelay = std::make_unique<juce::WebToggleButtonRelay>("graphBypassSaturation");
    distortionAlgorithmRelay = std::make_unique<juce::WebComboBoxRelay>("distortionAlgorithm");
    driveRelay = std::make_unique<juce::WebSliderRelay>("drive");
    foldRelay = std::make_unique<juce::WebSliderRelay>("fold");
    graphBypassWaveshaperRelay = std::make_unique<juce::WebToggleButtonRelay>("graphBypassWaveshaper");

    // --- Bitcrush ---
    graphBypassBitcrushRelay = std::make_unique<juce::WebToggleButtonRelay>("graphBypassBitcrush");
    bitDepthRelay = std::make_unique<juce::WebSliderRelay>("bitDepth");

    // --- Glitch ---
    graphBypassGlitchRelay = std::make_unique<juce::WebToggleButtonRelay>("graphBypassGlitch");
    glitchModeRelay = std::make_unique<juce::WebComboBoxRelay>("glitchMode");
    glitchBufferLengthRelay = std::make_unique<juce::WebComboBoxRelay>("glitchBufferLength");
    glitchProbabilityRelay = std::make_unique<juce::WebSliderRelay>("glitchProbability");

    // --- Center Master ---
    mixRelay = std::make_unique<juce::WebSliderRelay>("mix");
    outputLimiterStyleRelay = std::make_unique<juce::WebComboBoxRelay>("outputLimiterStyle");
    inputGainRelay = std::make_unique<juce::WebSliderRelay>("inputGain");
    outputGainRelay = std::make_unique<juce::WebSliderRelay>("outputGain");

    // --- Filter & EQ ---
    graphBypassFilterRelay = std::make_unique<juce::WebToggleButtonRelay>("graphBypassFilter");
    filterTypeRelay = std::make_unique<juce::WebComboBoxRelay>("filterType");
    filterCutoffRelay = std::make_unique<juce::WebSliderRelay>("filterCutoff");
    filterResonanceRelay = std::make_unique<juce::WebSliderRelay>("filterResonance");

    // --- Feedback + Delay ---
    feedbackAmountRelay = std::make_unique<juce::WebSliderRelay>("feedbackAmount");
    feedbackDampingRelay = std::make_unique<juce::WebSliderRelay>("feedbackDamping");
    microDelayTimeRelay = std::make_unique<juce::WebSliderRelay>("microDelayTime");

    // --- Macros ---
    macroDamageRelay = std::make_unique<juce::WebSliderRelay>("macroDamage");
    macroCrushRelay = std::make_unique<juce::WebSliderRelay>("macroCrush");
    macroGlitchRelay = std::make_unique<juce::WebSliderRelay>("macroGlitch");
    macroChaosRelay = std::make_unique<juce::WebSliderRelay>("macroChaos");
    macroRhythmRelay = std::make_unique<juce::WebSliderRelay>("macroRhythm");
    macroMovementRelay = std::make_unique<juce::WebSliderRelay>("macroMovement");
    macroWidthRelay = std::make_unique<juce::WebSliderRelay>("macroWidth");
    macroMixRelay = std::make_unique<juce::WebSliderRelay>("macroMix");

    // --- Performance Triggers ---
    performanceKillRelay = std::make_unique<juce::WebToggleButtonRelay>("performanceKill");
    performanceGlitchRelay = std::make_unique<juce::WebToggleButtonRelay>("performanceGlitch");
    performanceDestroyRelay = std::make_unique<juce::WebToggleButtonRelay>("performanceDestroy");
    performanceFreezeRelay = std::make_unique<juce::WebToggleButtonRelay>("performanceFreeze");
    performanceReverseRelay = std::make_unique<juce::WebToggleButtonRelay>("performanceReverse");
    performanceStutterRelay = std::make_unique<juce::WebToggleButtonRelay>("performanceStutter");
    performanceChaosRelay = std::make_unique<juce::WebToggleButtonRelay>("performanceChaos");

    // --- XY Performance Pad ---
    xyPadXRelay = std::make_unique<juce::WebSliderRelay>("xyPadX");
    xyPadYRelay = std::make_unique<juce::WebSliderRelay>("xyPadY");

    // --- Rhythmic Sequencer ---
    sequencerEnabledRelay = std::make_unique<juce::WebToggleButtonRelay>("sequencerEnabled");
    sequencerRateRelay = std::make_unique<juce::WebComboBoxRelay>("sequencerRate");
    sequencerStepsRelay = std::make_unique<juce::WebComboBoxRelay>("sequencerSteps");

    // --- Modulation Matrix (4 LFOs) ---
    modMatrixEnabledRelay = std::make_unique<juce::WebToggleButtonRelay>("modMatrixEnabled");
    lfo1RateRelay = std::make_unique<juce::WebSliderRelay>("lfo1Rate");
    lfo1ShapeRelay = std::make_unique<juce::WebComboBoxRelay>("lfo1Shape");
    lfo1SyncRelay = std::make_unique<juce::WebToggleButtonRelay>("lfo1Sync");
    lfo2RateRelay = std::make_unique<juce::WebSliderRelay>("lfo2Rate");
    lfo2ShapeRelay = std::make_unique<juce::WebComboBoxRelay>("lfo2Shape");
    lfo2SyncRelay = std::make_unique<juce::WebToggleButtonRelay>("lfo2Sync");
    lfo3RateRelay = std::make_unique<juce::WebSliderRelay>("lfo3Rate");
    lfo3ShapeRelay = std::make_unique<juce::WebComboBoxRelay>("lfo3Shape");
    lfo3SyncRelay = std::make_unique<juce::WebToggleButtonRelay>("lfo3Sync");
    lfo4RateRelay = std::make_unique<juce::WebSliderRelay>("lfo4Rate");
    lfo4ShapeRelay = std::make_unique<juce::WebComboBoxRelay>("lfo4Shape");
    lfo4SyncRelay = std::make_unique<juce::WebToggleButtonRelay>("lfo4Sync");

    // ------------------------------------------------------------------------
    // STEP 2: CREATE WEBVIEW (with relay options)
    // ------------------------------------------------------------------------
    //
    // WebView creation with all necessary options:
    // - withNativeIntegrationEnabled() - REQUIRED for JUCE parameter binding
    // - withResourceProvider() - REQUIRED for JUCE 8 (serves embedded files)
    // - withOptionsFrom(*relay) - REQUIRED for each of the 56 parameter relays
    // - withKeepPageLoadedWhenBrowserIsHidden() - OPTIONAL (FL Studio fix)
    //
    webView = std::make_unique<juce::WebBrowserComponent>(
        juce::WebBrowserComponent::Options{}
            // REQUIRED: Enable JUCE frontend library
            .withNativeIntegrationEnabled()

            // REQUIRED: Resource provider for embedded files
            .withResourceProvider([this](const auto& url) {
                return getResource(url);
            })

            // OPTIONAL: FL Studio fix (prevents blank screen on focus loss)
            .withKeepPageLoadedWhenBrowserIsHidden()

            // REQUIRED: Register each relay with WebView
            .withOptionsFrom(*qualityModeRelay)
            .withOptionsFrom(*graphBypassSaturationRelay)
            .withOptionsFrom(*distortionAlgorithmRelay)
            .withOptionsFrom(*driveRelay)
            .withOptionsFrom(*foldRelay)
            .withOptionsFrom(*graphBypassWaveshaperRelay)
            .withOptionsFrom(*graphBypassBitcrushRelay)
            .withOptionsFrom(*bitDepthRelay)
            .withOptionsFrom(*graphBypassGlitchRelay)
            .withOptionsFrom(*glitchModeRelay)
            .withOptionsFrom(*glitchBufferLengthRelay)
            .withOptionsFrom(*glitchProbabilityRelay)
            .withOptionsFrom(*mixRelay)
            .withOptionsFrom(*outputLimiterStyleRelay)
            .withOptionsFrom(*inputGainRelay)
            .withOptionsFrom(*outputGainRelay)
            .withOptionsFrom(*graphBypassFilterRelay)
            .withOptionsFrom(*filterTypeRelay)
            .withOptionsFrom(*filterCutoffRelay)
            .withOptionsFrom(*filterResonanceRelay)
            .withOptionsFrom(*feedbackAmountRelay)
            .withOptionsFrom(*feedbackDampingRelay)
            .withOptionsFrom(*microDelayTimeRelay)
            .withOptionsFrom(*macroDamageRelay)
            .withOptionsFrom(*macroCrushRelay)
            .withOptionsFrom(*macroGlitchRelay)
            .withOptionsFrom(*macroChaosRelay)
            .withOptionsFrom(*macroRhythmRelay)
            .withOptionsFrom(*macroMovementRelay)
            .withOptionsFrom(*macroWidthRelay)
            .withOptionsFrom(*macroMixRelay)
            .withOptionsFrom(*performanceKillRelay)
            .withOptionsFrom(*performanceGlitchRelay)
            .withOptionsFrom(*performanceDestroyRelay)
            .withOptionsFrom(*performanceFreezeRelay)
            .withOptionsFrom(*performanceReverseRelay)
            .withOptionsFrom(*performanceStutterRelay)
            .withOptionsFrom(*performanceChaosRelay)
            .withOptionsFrom(*xyPadXRelay)
            .withOptionsFrom(*xyPadYRelay)
            .withOptionsFrom(*sequencerEnabledRelay)
            .withOptionsFrom(*sequencerRateRelay)
            .withOptionsFrom(*sequencerStepsRelay)
            .withOptionsFrom(*modMatrixEnabledRelay)
            .withOptionsFrom(*lfo1RateRelay)
            .withOptionsFrom(*lfo1ShapeRelay)
            .withOptionsFrom(*lfo1SyncRelay)
            .withOptionsFrom(*lfo2RateRelay)
            .withOptionsFrom(*lfo2ShapeRelay)
            .withOptionsFrom(*lfo2SyncRelay)
            .withOptionsFrom(*lfo3RateRelay)
            .withOptionsFrom(*lfo3ShapeRelay)
            .withOptionsFrom(*lfo3SyncRelay)
            .withOptionsFrom(*lfo4RateRelay)
            .withOptionsFrom(*lfo4ShapeRelay)
            .withOptionsFrom(*lfo4SyncRelay)
    );

    // ------------------------------------------------------------------------
    // STEP 3: CREATE PARAMETER ATTACHMENTS (after WebView!) - 56 total
    // ------------------------------------------------------------------------
    //
    // Attachments synchronize APVTS parameters with relay state.
    // Constructor: (parameter, relay, undoManager). JUCE 8 requires the third
    // argument (undoManager, typically nullptr).
    //
    // NOTE: filterCutoff and glitchProbability are each bound to TWO controls
    // in v5-ui.html (base + ADV fine-adjust mirror) but get exactly ONE
    // attachment here - the JUCE frontend library's getSliderState() caches
    // state objects per parameter name, so both DOM controls automatically
    // share this single relay/attachment. Do NOT create a second attachment.
    //
    // --- Top Bar / Global ---
    qualityModeAttachment = std::make_unique<juce::WebComboBoxParameterAttachment>(
        *audioProcessor.apvts.getParameter("qualityMode"),
        *qualityModeRelay,
        nullptr
    );

    // --- Distortion ---
    graphBypassSaturationAttachment = std::make_unique<juce::WebToggleButtonParameterAttachment>(
        *audioProcessor.apvts.getParameter("graphBypassSaturation"),
        *graphBypassSaturationRelay,
        nullptr
    );
    distortionAlgorithmAttachment = std::make_unique<juce::WebComboBoxParameterAttachment>(
        *audioProcessor.apvts.getParameter("distortionAlgorithm"),
        *distortionAlgorithmRelay,
        nullptr
    );
    driveAttachment = std::make_unique<juce::WebSliderParameterAttachment>(
        *audioProcessor.apvts.getParameter("drive"),
        *driveRelay,
        nullptr
    );
    foldAttachment = std::make_unique<juce::WebSliderParameterAttachment>(
        *audioProcessor.apvts.getParameter("fold"),
        *foldRelay,
        nullptr
    );
    graphBypassWaveshaperAttachment = std::make_unique<juce::WebToggleButtonParameterAttachment>(
        *audioProcessor.apvts.getParameter("graphBypassWaveshaper"),
        *graphBypassWaveshaperRelay,
        nullptr
    );

    // --- Bitcrush ---
    graphBypassBitcrushAttachment = std::make_unique<juce::WebToggleButtonParameterAttachment>(
        *audioProcessor.apvts.getParameter("graphBypassBitcrush"),
        *graphBypassBitcrushRelay,
        nullptr
    );
    bitDepthAttachment = std::make_unique<juce::WebSliderParameterAttachment>(
        *audioProcessor.apvts.getParameter("bitDepth"),
        *bitDepthRelay,
        nullptr
    );

    // --- Glitch ---
    graphBypassGlitchAttachment = std::make_unique<juce::WebToggleButtonParameterAttachment>(
        *audioProcessor.apvts.getParameter("graphBypassGlitch"),
        *graphBypassGlitchRelay,
        nullptr
    );
    glitchModeAttachment = std::make_unique<juce::WebComboBoxParameterAttachment>(
        *audioProcessor.apvts.getParameter("glitchMode"),
        *glitchModeRelay,
        nullptr
    );
    glitchBufferLengthAttachment = std::make_unique<juce::WebComboBoxParameterAttachment>(
        *audioProcessor.apvts.getParameter("glitchBufferLength"),
        *glitchBufferLengthRelay,
        nullptr
    );
    glitchProbabilityAttachment = std::make_unique<juce::WebSliderParameterAttachment>(
        *audioProcessor.apvts.getParameter("glitchProbability"),
        *glitchProbabilityRelay,
        nullptr
    );

    // --- Center Master ---
    mixAttachment = std::make_unique<juce::WebSliderParameterAttachment>(
        *audioProcessor.apvts.getParameter("mix"),
        *mixRelay,
        nullptr
    );
    outputLimiterStyleAttachment = std::make_unique<juce::WebComboBoxParameterAttachment>(
        *audioProcessor.apvts.getParameter("outputLimiterStyle"),
        *outputLimiterStyleRelay,
        nullptr
    );
    inputGainAttachment = std::make_unique<juce::WebSliderParameterAttachment>(
        *audioProcessor.apvts.getParameter("inputGain"),
        *inputGainRelay,
        nullptr
    );
    outputGainAttachment = std::make_unique<juce::WebSliderParameterAttachment>(
        *audioProcessor.apvts.getParameter("outputGain"),
        *outputGainRelay,
        nullptr
    );

    // --- Filter & EQ ---
    graphBypassFilterAttachment = std::make_unique<juce::WebToggleButtonParameterAttachment>(
        *audioProcessor.apvts.getParameter("graphBypassFilter"),
        *graphBypassFilterRelay,
        nullptr
    );
    filterTypeAttachment = std::make_unique<juce::WebComboBoxParameterAttachment>(
        *audioProcessor.apvts.getParameter("filterType"),
        *filterTypeRelay,
        nullptr
    );
    filterCutoffAttachment = std::make_unique<juce::WebSliderParameterAttachment>(
        *audioProcessor.apvts.getParameter("filterCutoff"),
        *filterCutoffRelay,
        nullptr
    );
    filterResonanceAttachment = std::make_unique<juce::WebSliderParameterAttachment>(
        *audioProcessor.apvts.getParameter("filterResonance"),
        *filterResonanceRelay,
        nullptr
    );

    // --- Feedback + Delay ---
    feedbackAmountAttachment = std::make_unique<juce::WebSliderParameterAttachment>(
        *audioProcessor.apvts.getParameter("feedbackAmount"),
        *feedbackAmountRelay,
        nullptr
    );
    feedbackDampingAttachment = std::make_unique<juce::WebSliderParameterAttachment>(
        *audioProcessor.apvts.getParameter("feedbackDamping"),
        *feedbackDampingRelay,
        nullptr
    );
    microDelayTimeAttachment = std::make_unique<juce::WebSliderParameterAttachment>(
        *audioProcessor.apvts.getParameter("microDelayTime"),
        *microDelayTimeRelay,
        nullptr
    );

    // --- Macros ---
    macroDamageAttachment = std::make_unique<juce::WebSliderParameterAttachment>(
        *audioProcessor.apvts.getParameter("macroDamage"),
        *macroDamageRelay,
        nullptr
    );
    macroCrushAttachment = std::make_unique<juce::WebSliderParameterAttachment>(
        *audioProcessor.apvts.getParameter("macroCrush"),
        *macroCrushRelay,
        nullptr
    );
    macroGlitchAttachment = std::make_unique<juce::WebSliderParameterAttachment>(
        *audioProcessor.apvts.getParameter("macroGlitch"),
        *macroGlitchRelay,
        nullptr
    );
    macroChaosAttachment = std::make_unique<juce::WebSliderParameterAttachment>(
        *audioProcessor.apvts.getParameter("macroChaos"),
        *macroChaosRelay,
        nullptr
    );
    macroRhythmAttachment = std::make_unique<juce::WebSliderParameterAttachment>(
        *audioProcessor.apvts.getParameter("macroRhythm"),
        *macroRhythmRelay,
        nullptr
    );
    macroMovementAttachment = std::make_unique<juce::WebSliderParameterAttachment>(
        *audioProcessor.apvts.getParameter("macroMovement"),
        *macroMovementRelay,
        nullptr
    );
    macroWidthAttachment = std::make_unique<juce::WebSliderParameterAttachment>(
        *audioProcessor.apvts.getParameter("macroWidth"),
        *macroWidthRelay,
        nullptr
    );
    macroMixAttachment = std::make_unique<juce::WebSliderParameterAttachment>(
        *audioProcessor.apvts.getParameter("macroMix"),
        *macroMixRelay,
        nullptr
    );

    // --- Performance Triggers ---
    performanceKillAttachment = std::make_unique<juce::WebToggleButtonParameterAttachment>(
        *audioProcessor.apvts.getParameter("performanceKill"),
        *performanceKillRelay,
        nullptr
    );
    performanceGlitchAttachment = std::make_unique<juce::WebToggleButtonParameterAttachment>(
        *audioProcessor.apvts.getParameter("performanceGlitch"),
        *performanceGlitchRelay,
        nullptr
    );
    performanceDestroyAttachment = std::make_unique<juce::WebToggleButtonParameterAttachment>(
        *audioProcessor.apvts.getParameter("performanceDestroy"),
        *performanceDestroyRelay,
        nullptr
    );
    performanceFreezeAttachment = std::make_unique<juce::WebToggleButtonParameterAttachment>(
        *audioProcessor.apvts.getParameter("performanceFreeze"),
        *performanceFreezeRelay,
        nullptr
    );
    performanceReverseAttachment = std::make_unique<juce::WebToggleButtonParameterAttachment>(
        *audioProcessor.apvts.getParameter("performanceReverse"),
        *performanceReverseRelay,
        nullptr
    );
    performanceStutterAttachment = std::make_unique<juce::WebToggleButtonParameterAttachment>(
        *audioProcessor.apvts.getParameter("performanceStutter"),
        *performanceStutterRelay,
        nullptr
    );
    performanceChaosAttachment = std::make_unique<juce::WebToggleButtonParameterAttachment>(
        *audioProcessor.apvts.getParameter("performanceChaos"),
        *performanceChaosRelay,
        nullptr
    );

    // --- XY Performance Pad ---
    xyPadXAttachment = std::make_unique<juce::WebSliderParameterAttachment>(
        *audioProcessor.apvts.getParameter("xyPadX"),
        *xyPadXRelay,
        nullptr
    );
    xyPadYAttachment = std::make_unique<juce::WebSliderParameterAttachment>(
        *audioProcessor.apvts.getParameter("xyPadY"),
        *xyPadYRelay,
        nullptr
    );

    // --- Rhythmic Sequencer ---
    sequencerEnabledAttachment = std::make_unique<juce::WebToggleButtonParameterAttachment>(
        *audioProcessor.apvts.getParameter("sequencerEnabled"),
        *sequencerEnabledRelay,
        nullptr
    );
    sequencerRateAttachment = std::make_unique<juce::WebComboBoxParameterAttachment>(
        *audioProcessor.apvts.getParameter("sequencerRate"),
        *sequencerRateRelay,
        nullptr
    );
    sequencerStepsAttachment = std::make_unique<juce::WebComboBoxParameterAttachment>(
        *audioProcessor.apvts.getParameter("sequencerSteps"),
        *sequencerStepsRelay,
        nullptr
    );

    // --- Modulation Matrix (4 LFOs) ---
    modMatrixEnabledAttachment = std::make_unique<juce::WebToggleButtonParameterAttachment>(
        *audioProcessor.apvts.getParameter("modMatrixEnabled"),
        *modMatrixEnabledRelay,
        nullptr
    );
    lfo1RateAttachment = std::make_unique<juce::WebSliderParameterAttachment>(
        *audioProcessor.apvts.getParameter("lfo1Rate"),
        *lfo1RateRelay,
        nullptr
    );
    lfo1ShapeAttachment = std::make_unique<juce::WebComboBoxParameterAttachment>(
        *audioProcessor.apvts.getParameter("lfo1Shape"),
        *lfo1ShapeRelay,
        nullptr
    );
    lfo1SyncAttachment = std::make_unique<juce::WebToggleButtonParameterAttachment>(
        *audioProcessor.apvts.getParameter("lfo1Sync"),
        *lfo1SyncRelay,
        nullptr
    );
    lfo2RateAttachment = std::make_unique<juce::WebSliderParameterAttachment>(
        *audioProcessor.apvts.getParameter("lfo2Rate"),
        *lfo2RateRelay,
        nullptr
    );
    lfo2ShapeAttachment = std::make_unique<juce::WebComboBoxParameterAttachment>(
        *audioProcessor.apvts.getParameter("lfo2Shape"),
        *lfo2ShapeRelay,
        nullptr
    );
    lfo2SyncAttachment = std::make_unique<juce::WebToggleButtonParameterAttachment>(
        *audioProcessor.apvts.getParameter("lfo2Sync"),
        *lfo2SyncRelay,
        nullptr
    );
    lfo3RateAttachment = std::make_unique<juce::WebSliderParameterAttachment>(
        *audioProcessor.apvts.getParameter("lfo3Rate"),
        *lfo3RateRelay,
        nullptr
    );
    lfo3ShapeAttachment = std::make_unique<juce::WebComboBoxParameterAttachment>(
        *audioProcessor.apvts.getParameter("lfo3Shape"),
        *lfo3ShapeRelay,
        nullptr
    );
    lfo3SyncAttachment = std::make_unique<juce::WebToggleButtonParameterAttachment>(
        *audioProcessor.apvts.getParameter("lfo3Sync"),
        *lfo3SyncRelay,
        nullptr
    );
    lfo4RateAttachment = std::make_unique<juce::WebSliderParameterAttachment>(
        *audioProcessor.apvts.getParameter("lfo4Rate"),
        *lfo4RateRelay,
        nullptr
    );
    lfo4ShapeAttachment = std::make_unique<juce::WebComboBoxParameterAttachment>(
        *audioProcessor.apvts.getParameter("lfo4Shape"),
        *lfo4ShapeRelay,
        nullptr
    );
    lfo4SyncAttachment = std::make_unique<juce::WebToggleButtonParameterAttachment>(
        *audioProcessor.apvts.getParameter("lfo4Sync"),
        *lfo4SyncRelay,
        nullptr
    );

    // ------------------------------------------------------------------------
    // WEBVIEW SETUP
    // ------------------------------------------------------------------------

    // Navigate to root (loads index.html via resource provider)
    webView->goToURL(juce::WebBrowserComponent::getResourceProviderRoot());

    // Make WebView visible
    addAndMakeVisible(*webView);

    // ------------------------------------------------------------------------
    // WINDOW SIZE (from v5-ui.yaml: width 1200, viewport_height 800 = the fixed
    // JUCE editor size; total scrollable content height is 1650 and scrolls
    // INTERNALLY within the WebView/.plugin-frame div - do not setSize(1200,1650))
    // ------------------------------------------------------------------------

    setSize(1200, 800);
    setResizable(false, false);
}

//==============================================================================
// Destructor
//==============================================================================

CORRUPTRAudioProcessorEditor::~CORRUPTRAudioProcessorEditor()
{
    // Members are automatically destroyed in reverse order of declaration:
    // 1. Attachments destroyed first (stop calling evaluateJavascript)
    // 2. WebView destroyed second (safe, attachments are gone)
    // 3. Relays destroyed last (safe, nothing using them)
    //
    // No manual cleanup needed if member order is correct!
}

//==============================================================================
// AudioProcessorEditor Overrides
//==============================================================================

void CORRUPTRAudioProcessorEditor::paint(juce::Graphics& g)
{
    // WebView fills the entire editor, so no custom painting needed
    g.fillAll(juce::Colours::black);
}

void CORRUPTRAudioProcessorEditor::resized()
{
    // Make WebView fill the entire editor bounds
    if (webView)
        webView->setBounds(getLocalBounds());
}

//==============================================================================
// Resource Provider (JUCE 8 Required Pattern)
//==============================================================================

std::optional<juce::WebBrowserComponent::Resource> CORRUPTRAudioProcessorEditor::getResource(
    const juce::String& url
)
{
    // ========================================================================
    // RESOURCE PROVIDER IMPLEMENTATION
    // ========================================================================
    //
    // Maps URLs to embedded binary data (from juce_add_binary_data).
    //
    // CRITICAL: Use explicit URL mapping (Pattern #8 from
    // juce8-critical-patterns.md). Generic loops break because BinaryData
    // flattens paths (e.g. "js/juce/index.js" -> "index_js").
    // ========================================================================

    // Helper to convert raw binary to std::vector<std::byte>
    auto makeVector = [](const char* data, int size) {
        return std::vector<std::byte>(
            reinterpret_cast<const std::byte*>(data),
            reinterpret_cast<const std::byte*>(data) + size
        );
    };

    // Handle root URL (redirect to index.html)
    if (url == "/" || url == "/index.html") {
        return juce::WebBrowserComponent::Resource {
            makeVector(BinaryData::index_html, BinaryData::index_htmlSize),
            juce::String("text/html")
        };
    }

    // JUCE frontend library
    if (url == "/js/juce/index.js") {
        return juce::WebBrowserComponent::Resource {
            makeVector(BinaryData::index_js, BinaryData::index_jsSize),
            juce::String("application/javascript")  // CRITICAL: Correct MIME type
        };
    }

    // JUCE interop checker (required - see juce8-critical-patterns.md Pattern #13)
    if (url == "/js/juce/check_native_interop.js") {
        return juce::WebBrowserComponent::Resource {
            makeVector(BinaryData::check_native_interop_js, BinaryData::check_native_interop_jsSize),
            juce::String("application/javascript")
        };
    }

    // 404 - Resource not found
    return std::nullopt;
}
