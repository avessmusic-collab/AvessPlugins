#include "PluginEditor.h"
#include "BinaryData.h"

TransitionistAudioProcessorEditor::TransitionistAudioProcessorEditor(TransitionistAudioProcessor& p)
    : AudioProcessorEditor(&p), processorRef(p)
{
    // STEP 1: Relays (before WebView)
    transitionRelay = std::make_unique<juce::WebSliderRelay>("transition");
    reverbRelay = std::make_unique<juce::WebSliderRelay>("reverb");
    delayRelay = std::make_unique<juce::WebSliderRelay>("delay");
    delaySyncRelay = std::make_unique<juce::WebComboBoxRelay>("delaySync");
    dryWetRelay = std::make_unique<juce::WebSliderRelay>("dryWet");
    inputGainRelay = std::make_unique<juce::WebSliderRelay>("inputGain");
    outputGainRelay = std::make_unique<juce::WebSliderRelay>("outputGain");

    // STEP 2: WebView (with relay options)
    webView = std::make_unique<juce::WebBrowserComponent>(
        juce::WebBrowserComponent::Options{}
            .withNativeIntegrationEnabled()
            .withResourceProvider([this](const auto& url) { return getResource(url); })
            .withKeepPageLoadedWhenBrowserIsHidden()
            .withOptionsFrom(*transitionRelay)
            .withOptionsFrom(*reverbRelay)
            .withOptionsFrom(*delayRelay)
            .withOptionsFrom(*delaySyncRelay)
            .withOptionsFrom(*dryWetRelay)
            .withOptionsFrom(*inputGainRelay)
            .withOptionsFrom(*outputGainRelay)
    );

    // STEP 3: Attachments (after WebView)
    transitionAttachment = std::make_unique<juce::WebSliderParameterAttachment>(
        *processorRef.parameters.getParameter("transition"), *transitionRelay, nullptr);
    reverbAttachment = std::make_unique<juce::WebSliderParameterAttachment>(
        *processorRef.parameters.getParameter("reverb"), *reverbRelay, nullptr);
    delayAttachment = std::make_unique<juce::WebSliderParameterAttachment>(
        *processorRef.parameters.getParameter("delay"), *delayRelay, nullptr);
    delaySyncAttachment = std::make_unique<juce::WebComboBoxParameterAttachment>(
        *processorRef.parameters.getParameter("delaySync"), *delaySyncRelay, nullptr);
    dryWetAttachment = std::make_unique<juce::WebSliderParameterAttachment>(
        *processorRef.parameters.getParameter("dryWet"), *dryWetRelay, nullptr);
    inputGainAttachment = std::make_unique<juce::WebSliderParameterAttachment>(
        *processorRef.parameters.getParameter("inputGain"), *inputGainRelay, nullptr);
    outputGainAttachment = std::make_unique<juce::WebSliderParameterAttachment>(
        *processorRef.parameters.getParameter("outputGain"), *outputGainRelay, nullptr);

    // Cache-busting: append a per-instance unique query string so WebKit
    // cannot restore a stale cached/back-forward-cache snapshot of this
    // exact URL from a previous app launch (observed during development:
    // some Standalone relaunches were rendering a stale DOM snapshot from
    // an earlier build instead of re-executing the current index.html,
    // even though the underlying compiled resource had genuinely changed).
    // getResource() below strips this query string before matching, so it
    // has no effect on which resource is actually served.
    const juce::String cacheBuster = "?t=" + juce::String(juce::Time::getMillisecondCounterHiRes(), 0);
    webView->goToURL(juce::WebBrowserComponent::getResourceProviderRoot() + cacheBuster);
    addAndMakeVisible(*webView);

    setSize(1100, 440);
    setResizable(false, false);

    // 30Hz meter update loop (juce8-critical-patterns.md Pattern #20) -
    // pushes the 2 UI-only level meters to the WebView. Not parameter-bound.
    startTimerHz(30);
}

TransitionistAudioProcessorEditor::~TransitionistAudioProcessorEditor()
{
    stopTimer();
}

void TransitionistAudioProcessorEditor::timerCallback()
{
    if (webView == nullptr)
        return;

    const float inputDb = processorRef.inputLevelDb.load(std::memory_order_relaxed);
    const float outputDb = processorRef.outputLevelDb.load(std::memory_order_relaxed);

    webView->emitEventIfBrowserIsVisible("updateInputMeter", inputDb);
    webView->emitEventIfBrowserIsVisible("updateOutputMeter", outputDb);
}

void TransitionistAudioProcessorEditor::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colours::black);
}

void TransitionistAudioProcessorEditor::resized()
{
    if (webView)
        webView->setBounds(getLocalBounds());
}

std::optional<juce::WebBrowserComponent::Resource> TransitionistAudioProcessorEditor::getResource(
    const juce::String& url
)
{
    auto makeVector = [](const char* data, int size) {
        return std::vector<std::byte>(
            reinterpret_cast<const std::byte*>(data),
            reinterpret_cast<const std::byte*>(data) + size
        );
    };

    // Strip any query string (e.g. the cache-busting "?t=..." appended in
    // the constructor's goToURL call) before matching - it exists purely to
    // give WebKit a unique URL per launch, not to select a different resource.
    const juce::String path = url.upToFirstOccurrenceOf("?", false, false);

    if (path == "/" || path == "/index.html") {
        return juce::WebBrowserComponent::Resource {
            makeVector(BinaryData::index_html, BinaryData::index_htmlSize),
            juce::String("text/html")
        };
    }

    if (path == "/js/juce/index.js") {
        return juce::WebBrowserComponent::Resource {
            makeVector(BinaryData::index_js, BinaryData::index_jsSize),
            juce::String("application/javascript")
        };
    }

    if (path == "/js/juce/check_native_interop.js") {
        return juce::WebBrowserComponent::Resource {
            makeVector(BinaryData::check_native_interop_js, BinaryData::check_native_interop_jsSize),
            juce::String("application/javascript")
        };
    }

    return std::nullopt;
}
