#include "PluginEditor.h"

//==============================================================================
// Constructor - CRITICAL: Initialize in correct order
//==============================================================================

TransitionistAudioProcessorEditor::TransitionistAudioProcessorEditor(TransitionistAudioProcessor& p)
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
    // STEP 1: CREATE RELAYS (before WebView!)
    // ------------------------------------------------------------------------
    //
    // Each relay bridges a C++ parameter to JavaScript state.
    // Relay constructor takes the parameter ID (must match APVTS).
    //
    // NOTE: "sweep" uses the exact same WebSliderRelay type as "throw" and
    // "space" - the relay/attachment layer has no concept of unipolar vs.
    // bipolar. The -100..+100 bipolar range lives entirely in the APVTS
    // parameter's NormalisableRange (declared in Stage 2/Shell - see
    // parameter-spec.md) and in the HTML/JS bipolar arc-fill renderer.
    //
    throwRelay = std::make_unique<juce::WebSliderRelay>("throw");
    spaceRelay = std::make_unique<juce::WebSliderRelay>("space");
    sweepRelay = std::make_unique<juce::WebSliderRelay>("sweep");

    // ------------------------------------------------------------------------
    // STEP 2: CREATE WEBVIEW (with relay options)
    // ------------------------------------------------------------------------
    //
    // WebView creation with all necessary options:
    // - withNativeIntegrationEnabled() - REQUIRED for JUCE parameter binding
    // - withResourceProvider() - REQUIRED for JUCE 8 (serves embedded files)
    // - withOptionsFrom(*relay) - REQUIRED for each parameter relay
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
            .withOptionsFrom(*throwRelay)
            .withOptionsFrom(*spaceRelay)
            .withOptionsFrom(*sweepRelay)
    );

    // ------------------------------------------------------------------------
    // STEP 3: CREATE PARAMETER ATTACHMENTS (after WebView!)
    // ------------------------------------------------------------------------
    //
    // Attachments synchronize APVTS parameters with relay state.
    // Constructor: (parameter, relay, undoManager)
    //
    // Parameter must be retrieved from APVTS:
    //   audioProcessor.apvts.getParameter("PARAM_ID")
    //
    // JUCE 8 requires the third parameter (undoManager, typically nullptr).
    //
    throwAttachment = std::make_unique<juce::WebSliderParameterAttachment>(
        *audioProcessor.apvts.getParameter("throw"),
        *throwRelay,
        nullptr  // No undo manager
    );

    spaceAttachment = std::make_unique<juce::WebSliderParameterAttachment>(
        *audioProcessor.apvts.getParameter("space"),
        *spaceRelay,
        nullptr
    );

    // "sweep" attachment is created identically to the unipolar knobs above -
    // the attachment reads/writes the parameter's NORMALISED (0-1) value
    // regardless of the underlying NormalisableRange's min/max, so the
    // -100..+100 bipolar range requires no special handling here. See
    // parameter-spec.md for the APVTS NormalisableRange declaration that
    // makes normalised 0.5 correspond to 0% (center detent).
    sweepAttachment = std::make_unique<juce::WebSliderParameterAttachment>(
        *audioProcessor.apvts.getParameter("sweep"),
        *sweepRelay,
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
    // WINDOW SIZE (from v4-ui.yaml: 980x420, non-resizable)
    // ------------------------------------------------------------------------

    setSize(980, 420);
    setResizable(false, false);
}

//==============================================================================
// Destructor
//==============================================================================

TransitionistAudioProcessorEditor::~TransitionistAudioProcessorEditor()
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

void TransitionistAudioProcessorEditor::paint(juce::Graphics& g)
{
    // WebView fills the entire editor, so no custom painting needed
    g.fillAll(juce::Colours::black);
}

void TransitionistAudioProcessorEditor::resized()
{
    // Make WebView fill the entire editor bounds
    if (webView)
        webView->setBounds(getLocalBounds());
}

//==============================================================================
// Resource Provider (JUCE 8 Required Pattern)
//==============================================================================

std::optional<juce::WebBrowserComponent::Resource> TransitionistAudioProcessorEditor::getResource(
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
