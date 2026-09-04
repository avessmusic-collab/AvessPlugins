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

    // ------------------------------------------------------------------------
    // STEP 2: CREATE WEBVIEW (with relay options)
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
