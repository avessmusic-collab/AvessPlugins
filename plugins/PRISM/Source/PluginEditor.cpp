// ============================================================================
// PRISM - WebView plugin editor (TEMPLATE generated from finalized mockup v8)
// See v8-PluginEditor.h for the full JS <-> C++ contract.
// Assumes the processor exposes:   juce::AudioProcessorValueTreeState apvts;
// (rename processorRef.apvts below if the Stage 1 processor names it differently)
// ============================================================================

#include "PluginEditor.h"
#include "BinaryData.h"
#include "UiStateUtil.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <chrono>

namespace
{
    //==========================================================================
    // Parameter table - ids EXACTLY as in parameter-spec.md / v8-ui.yaml, in spec order.
    //==========================================================================
    enum class Kind { Float, Bool, Choice };
    struct ParamDef { const char* id; Kind kind; };

    constexpr std::array<ParamDef, 17> kParams
    {{
        { "fft_size",          Kind::Choice },
        { "window",            Kind::Choice },
        { "smoothing",         Kind::Float  },
        { "spectrum_tilt",     Kind::Float  },
        { "display_range",     Kind::Choice },
        { "spectrogram_speed", Kind::Float  },
        { "peak_decay",        Kind::Float  },
        { "freeze",            Kind::Bool   },
        { "analysis_mode",     Kind::Choice },
        { "color_theme",       Kind::Choice },
        { "preset",            Kind::Choice },
        { "reduced_motion",    Kind::Bool   },
        { "glow_intensity",    Kind::Float  },
        { "src1_visible",      Kind::Bool   },
        { "src1_solo",         Kind::Bool   },
        { "src2_visible",      Kind::Bool   },
        { "src2_solo",         Kind::Bool   }
    }};

    using namespace prism::uistate;   // limits + int-list helpers shared with PluginProcessor.cpp

    // ValueTree (non-automatable) state entries, persisted inside apvts.state (ids from prism::ids: single source of truth)
    constexpr const char* kSrcNameIds[2]  = { prism::ids::src1Name, prism::ids::src2Name };
    constexpr const char* kSrcNameDefs[2] = { "Ch1", "Ch2" };

    static_assert (kParams.size() == 17, "gestureOpen in the header is sized for the 17 locked APVTS parameters");

    // Layout/size sanitising is shared with the processor (PRISMAudioProcessor::sanitizeState), so the
    // editor and setStateInformation apply identical clamps.
    juce::ValueTree sanitizedUiState (const juce::ValueTree& st)
    {
        auto copy = st.createCopy();
        PRISMAudioProcessor::sanitizeState (copy);
        return copy;
    }

    int findParamIndex (const juce::String& id)
    {
        for (size_t i = 0; i < kParams.size(); ++i)
            if (id == kParams[i].id)
                return (int) i;
        return -1;
    }

    //==========================================================================
    // Value conversion  (JS value <-> normalised parameter)
    //==========================================================================
    juce::var paramToVar (const juce::RangedAudioParameter& p, Kind kind)
    {
        switch (kind)
        {
            case Kind::Float:  return juce::var ((double) p.convertFrom0to1 (p.getValue()));
            case Kind::Bool:   return juce::var (p.getValue() >= 0.5f);
            case Kind::Choice:
            {
                const auto name = p.getCurrentValueAsText();
                // numeric choices (fft_size 4096, display_range -90) travel as numbers, like the mockup
                if (name.isNotEmpty() && name.containsOnly ("-0123456789"))
                    return juce::var (name.getIntValue());
                return juce::var (name);
            }
        }
        return {};
    }

    // wrapGesture=false when the page already opened a gesture for this parameter (drag in progress)
    void setParamFromVar (juce::RangedAudioParameter& p, Kind kind, const juce::var& v, bool wrapGesture)
    {
        float normalised = p.getValue();

        switch (kind)
        {
            case Kind::Float:
                normalised = p.convertTo0to1 ((float) (double) v);
                break;

            case Kind::Bool:
                normalised = ((double) v >= 0.5) ? 1.0f : 0.0f;
                break;

            case Kind::Choice:
            {
                const auto choices = p.getAllValueStrings();
                int index = choices.indexOf (v.toString());

                if (index < 0 && (v.isInt() || v.isInt64() || v.isDouble()))   // numeric match fallback
                    for (int i = 0; i < choices.size(); ++i)
                        if (choices[i].containsOnly ("-0123456789.") && juce::approximatelyEqual (choices[i].getDoubleValue(), (double) v))
                            index = i;

                if (index < 0)
                    return;   // unknown choice: ignore rather than corrupt state

                normalised = p.convertTo0to1 ((float) index);
                break;
            }
        }

        if (wrapGesture) p.beginChangeGesture();
        p.setValueNotifyingHost (normalised);
        if (wrapGesture) p.endChangeGesture();
    }

    juce::var numberOrNull (float v)
    {
        return std::isfinite (v) ? juce::var ((double) v) : juce::var();
    }

    juce::String packFloats (const float* data, int count)
    {
        // little-endian float32 -> base64 (JS: f32() in v8-ui.html). Compact vs. JSON number arrays.
        return count > 0 ? juce::Base64::toBase64 (data, (size_t) count * sizeof (float)) : juce::String();
    }

    std::vector<std::byte> toBytes (const juce::String& s)
    {
        const auto* p = reinterpret_cast<const std::byte*> (s.toRawUTF8());
        return std::vector<std::byte> (p, p + s.getNumBytesAsUTF8());
    }

    std::vector<std::byte> toBytes (const char* data, int size)
    {
        const auto* p = reinterpret_cast<const std::byte*> (data);
        return std::vector<std::byte> (p, p + size);
    }
} // namespace

//==============================================================================
// ParamBridge: listens to the APVTS parameters and the ValueTree state and only
// raises atomic dirty flags (these callbacks can fire on the audio thread for
// host automation). The editor's 60 Hz timer drains them on the message thread.
//==============================================================================
class PRISMAudioProcessorEditor::ParamBridge : private juce::AudioProcessorValueTreeState::Listener,
                                               private juce::ValueTree::Listener
{
public:
    explicit ParamBridge (juce::AudioProcessorValueTreeState& s) : apvts (s)
    {
        for (auto& d : kParams)
            apvts.addParameterListener (d.id, this);
        apvts.state.addListener (this);
        for (auto& f : dirty) f.store (false);
    }

    ~ParamBridge() override
    {
        apvts.state.removeListener (this);
        for (auto& d : kParams)
            apvts.removeParameterListener (d.id, this);
    }

    bool consumeDirty (size_t index)  { return dirty[index].exchange (false); }
    bool consumeNamesDirty()          { return namesDirty.exchange (false); }
    bool consumeRedirected()          { return redirected.exchange (false); }
    void markAllDirty()               { for (auto& f : dirty) f.store (true); namesDirty.store (true); }

private:
    void parameterChanged (const juce::String& id, float) override
    {
        const int i = findParamIndex (id);
        if (i >= 0) dirty[(size_t) i].store (true);
    }

    // setStateInformation / replaceState swaps the tree under us: everything must be re-sent to an open editor
    void valueTreeRedirected (juce::ValueTree&) override
    {
        markAllDirty();
        redirected.store (true);
    }

    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier& property) override
    {
        for (auto* nameId : kSrcNameIds)
            if (property == juce::Identifier (nameId))
                namesDirty.store (true);
    }

    juce::AudioProcessorValueTreeState& apvts;
    std::array<std::atomic<bool>, kParams.size()> dirty;
    std::atomic<bool> namesDirty { false };
    std::atomic<bool> redirected { false };
};

//==============================================================================
// KioskHost: borderless top-level component covering one display. TRUE FULLSCREEN works by reparenting the editor's existing WebBrowserComponent
// into this component and asking JUCE for kiosk mode (menu bar / Dock hidden). It is the SAME WebView instance (no second browser, no page reload:
// canvases, histories and JS state survive), and it always starts OFF: ui_fullscreen only restores the page's in-window presentation state on load,
// a plugin never seizes the screen by itself. Esc / F / the corner button leave it (the page asks C++); the key listener below is a backup for Esc.
//==============================================================================
class PRISMAudioProcessorEditor::KioskHost : public juce::Component, private juce::KeyListener
{
public:
    explicit KioskHost (PRISMAudioProcessorEditor& o) : owner (o)
    {
        setOpaque (true);
        setWantsKeyboardFocus (true);
        addKeyListener (this);
    }

    ~KioskHost() override { removeKeyListener (this); }

    void paint (juce::Graphics& g) override { g.fillAll (juce::Colour (0xff0a0c0e)); }   // the page background: letterbox bars look intentional

    void resized() override
    {
        if (getNumChildComponents() > 0)
            getChildComponent (0)->setBounds (getLocalBounds());
    }

    bool keyPressed (const juce::KeyPress& k) override { return handle (k); }

private:
    bool keyPressed (const juce::KeyPress& k, juce::Component*) override { return handle (k); }

    bool handle (const juce::KeyPress& k)
    {
        if (k == juce::KeyPress::escapeKey)
        {
            juce::Component::SafePointer<PRISMAudioProcessorEditor> sp (&owner);
            juce::MessageManager::callAsync ([sp] { if (sp != nullptr) sp->exitKiosk(); });
            return true;
        }
        return false;
    }

    PRISMAudioProcessorEditor& owner;
};

//==============================================================================
PRISMAudioProcessorEditor::PRISMAudioProcessorEditor (PRISMAudioProcessor& p)
    : AudioProcessorEditor (&p), processorRef (p)
{
    // ------------------------------------------------------------------------
    // WebView (JUCE 8): native integration + resource provider + event listeners
    // ------------------------------------------------------------------------
    auto options = juce::WebBrowserComponent::Options{}
        .withNativeIntegrationEnabled()
        .withResourceProvider ([this] (const auto& url) { return getResource (url); })
        .withKeepPageLoadedWhenBrowserIsHidden()
        .withEventListener ("prismParam",   [this] (juce::var v) { handleParamEvent (v); })
        .withEventListener ("prismUiState", [this] (juce::var v) { handleUiStateEvent (v); })
        .withEventListener ("prismReady",   [this] (juce::var)   { handlePageReady(); })
        .withEventListener ("prismFullscreen", [this] (juce::var v) { handleFullscreenEvent (v); });
    // reset_all / reset_integrated arrive ONLY through prismParam {id:"reset_all"|"reset_integrated"}

   #if JUCE_WINDOWS
    options = options
        .withBackend (juce::WebBrowserComponent::Options::Backend::webview2)
        .withWinWebView2Options (juce::WebBrowserComponent::Options::WinWebView2{}
            .withUserDataFolder (juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("PRISM")));
   #endif

    webView = std::make_unique<juce::WebBrowserComponent> (options);

    // Bridge is created AFTER the webView (destroyed before it).
    paramBridge = std::make_unique<ParamBridge> (processorRef.apvts);

    // Cache-busting query (see Transitionist stale-cache note); getResource() strips it.
    const juce::String cacheBuster = "?t=" + juce::String (juce::Time::getMillisecondCounterHiRes(), 0);
    webView->goToURL (juce::WebBrowserComponent::getResourceProviderRoot() + cacheBuster);
    addAndMakeVisible (*webView);

    // ------------------------------------------------------------------------
    // Window: resizable, fixed 55:36 aspect, min 770x504, size restored from state
    // ------------------------------------------------------------------------
    constrainer.setMinimumSize (kMinWidth, kMinHeight);
    constrainer.setFixedAspectRatio ((double) kDefaultWidth / (double) kDefaultHeight);   // 1100:720 = 55:36
    setConstrainer (&constrainer);
    setResizable (true, true);

    const auto st = sanitizedUiState (processorRef.apvts.state);
    setSize ((int) st.getProperty (prism::ids::uiWidth, kDefaultWidth), (int) st.getProperty (prism::ids::uiHeight, kDefaultHeight));

    setEngineActive (true);
    startTimerHz (60);   // analysis frames + parameter sync
}

PRISMAudioProcessorEditor::~PRISMAudioProcessorEditor()
{
    stopTimer();
    exitKiosk (false);     // host closed the window / project while in true fullscreen: reparent the WebView back and drop the kiosk cleanly
    setEngineActive (false);
    closeOpenGestures();   // never leave the host with an unbalanced beginChangeGesture
    // Members destroyed in reverse: paramBridge (stops touching the WebView) -> webView.
}

void PRISMAudioProcessorEditor::setEngineActive (bool active)
{
    if (active == engineActive) return;
    engineActive = active;
    processorRef.setEditorActive (active);
}

void PRISMAudioProcessorEditor::visibilityChanged()
{
    // Hidden editor -> the engine drops to idle work; shown again -> full rate (the timer re-confirms via isShowing()).
    setEngineActive (isVisible());
}

void PRISMAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff101215));   // matches the page background until the WebView paints
}

void PRISMAudioProcessorEditor::resized()
{
    if (webView != nullptr && kiosk == nullptr)    // while in true fullscreen the WebView belongs to the kiosk window
        webView->setBounds (getLocalBounds());

    // serialize window size with the plugin state
    if (getWidth() >= kMinWidth)
    {
        auto& st = processorRef.apvts.state;
        st.setProperty (prism::ids::uiWidth,  getWidth(),  nullptr);
        st.setProperty (prism::ids::uiHeight, getHeight(), nullptr);
    }
}

//==============================================================================
// JS -> C++
//==============================================================================
void PRISMAudioProcessorEditor::handleParamEvent (const juce::var& payload)
{
    const auto id    = payload.getProperty ("id", {}).toString();
    const auto value = payload.getProperty ("value", {});

    // momentary actions
    if (id == "reset_all")        { requestResetAll();        return; }
    if (id == "reset_integrated") { requestResetIntegrated(); return; }

    // ValueTree string state: src1_name / src2_name (max 10 chars)
    for (int i = 0; i < 2; ++i)
    {
        if (id == kSrcNameIds[i])
        {
            auto name = value.toString().trim().substring (0, kMaxNameLength).trim();
            if (name.isEmpty()) name = kSrcNameDefs[i];
            processorRef.apvts.state.setProperty (kSrcNameIds[i], name, nullptr);
            return;
        }
    }

    // APVTS parameters
    const int idx = findParamIndex (id);
    if (idx < 0)
    {
        DBG ("PRISM: unknown prismParam id '" << id << "'");
        return;
    }

    auto* param = processorRef.apvts.getParameter (kParams[(size_t) idx].id);
    if (param == nullptr)
        return;

    // gesture protocol: {id, gesture:'begin'|'end'} brackets a drag; value events inside it use setValueNotifyingHost only
    const auto gesture = payload.getProperty ("gesture", {}).toString();
    bool& open = gestureOpen[(size_t) idx];

    if (gesture == "begin") { if (! open) { param->beginChangeGesture(); open = true; } return; }   // repeated begin ignored
    if (gesture == "end")   { if (open)   { param->endChangeGesture();   open = false; } return; }   // stray end ignored

    setParamFromVar (*param, kParams[(size_t) idx].kind, value, ! open);   // no open gesture -> one-shot begin/value/end
}

void PRISMAudioProcessorEditor::handleUiStateEvent (const juce::var& payload)
{
    using namespace prism::ids;

    // Run the page's values through the processor's sanitizer before they enter the APVTS state.
    juce::ValueTree tmp ("PRISM_UI");
    tmp.setProperty (uiRowSplits,      joinInts (intsFromVar (payload.getProperty ("row_splits", {}))),       nullptr);
    tmp.setProperty (uiLowerColSplits, joinInts (intsFromVar (payload.getProperty ("lower_col_splits", {}))), nullptr);
    tmp.setProperty (uiSgOrientation,  payload.getProperty ("spectrogram_orientation", "v").toString(),      nullptr);
    tmp.setProperty (uiFullscreen,     (bool) payload.getProperty ("fullscreen", false),                      nullptr);
    tmp.setProperty (uiWaveformStyle,  payload.getProperty ("waveform_style", "rgb").toString(),                       nullptr);
    tmp.setProperty (uiGonioSource,    (int) payload.getProperty ("gonio_source", 0),                         nullptr);
    tmp.setProperty (uiScopeSource,    (int) payload.getProperty ("scope_source", 0),                         nullptr);
    tmp.setProperty (uiWaveformSpan,   (double) payload.getProperty ("waveform_span", 8.0),                       nullptr);
    PRISMAudioProcessor::sanitizeState (tmp);

    auto& st = processorRef.apvts.state;
    for (auto* id : { uiRowSplits, uiLowerColSplits, uiSgOrientation, uiFullscreen, uiWaveformSpan, uiWaveformStyle, uiGonioSource, uiScopeSource })
        st.setProperty (id, tmp.getProperty (id), nullptr);
}

void PRISMAudioProcessorEditor::closeOpenGestures()
{
    for (size_t i = 0; i < kParams.size(); ++i)
    {
        if (! gestureOpen[i]) continue;
        if (auto* p = processorRef.apvts.getParameter (kParams[i].id)) p->endChangeGesture();
        gestureOpen[i] = false;
    }
}

void PRISMAudioProcessorEditor::handleFullscreenEvent (const juce::var& payload)
{
    // Deferred: never reparent the native WebView from inside its own script-message callback.
    const bool on = (bool) payload.getProperty ("on", false);
    juce::Component::SafePointer<PRISMAudioProcessorEditor> sp (this);
    juce::MessageManager::callAsync ([sp, on] { if (sp != nullptr) { if (on) sp->enterKiosk(); else sp->exitKiosk(); } });
}

void PRISMAudioProcessorEditor::enterKiosk()
{
    if (kiosk != nullptr || webView == nullptr)
        return;

    const auto& displays = juce::Desktop::getInstance().getDisplays();
    const auto* d = displays.getDisplayForRect (getScreenBounds());   // the display that contains the editor
    if (d == nullptr) d = displays.getPrimaryDisplay();
    if (d == nullptr) return;

    kiosk = std::make_unique<KioskHost> (*this);
    kiosk->setBounds (d->totalArea);
    kiosk->addToDesktop (0);                      // borderless top-level window
    kiosk->setVisible (true);
    juce::Desktop::getInstance().setKioskModeComponent (kiosk.get(), false);   // true fullscreen: menu bar / Dock hidden
    kiosk->toFront (true);
    kiosk->addAndMakeVisible (*webView);          // reparents the SAME WebView out of the editor (after the window is final)
    kiosk->resized();
    webView->setVisible (false);                  // nudge the native WKWebView so it re-evaluates its window visibility
    webView->setVisible (true);
    webView->grabKeyboardFocus();

    juce::Component::SafePointer<PRISMAudioProcessorEditor> sp (this);
    juce::Timer::callAfterDelay (150, [sp]
    {
        if (sp != nullptr && sp->webView != nullptr)
        {
            auto* o = new juce::DynamicObject();
            o->setProperty ("on", true);
            sp->webView->emitEventIfBrowserIsVisible ("prismKiosk", juce::var (o));
        }
    });
}

void PRISMAudioProcessorEditor::exitKiosk (bool notifyPage)
{
    if (kiosk == nullptr)
        return;

    juce::Desktop::getInstance().setKioskModeComponent (nullptr);   // leave OS fullscreen first
    if (webView != nullptr)
        addAndMakeVisible (*webView);                                // reparent back into the editor (removed from the kiosk)
    kiosk.reset();
    resized();                                                       // editor layout restored: WebView fills the editor again
    if (webView != nullptr)
        webView->grabKeyboardFocus();

    if (notifyPage && webView != nullptr)
    {
        juce::Component::SafePointer<PRISMAudioProcessorEditor> sp (this);
        juce::Timer::callAfterDelay (100, [sp]
        {
            if (sp != nullptr && sp->webView != nullptr)
            {
                auto* o = new juce::DynamicObject();
                o->setProperty ("on", false);
                sp->webView->emitEventIfBrowserIsVisible ("prismKiosk", juce::var (o));
            }
        });
    }
}

void PRISMAudioProcessorEditor::handlePageReady()
{
    closeOpenGestures();   // a (re)loaded page cannot still be mid-drag
   #if PRISM_DEBUG_CONTROL
    { static int readyCount = 0; std::fprintf (stderr, "PRISM_READY n=%d\n", ++readyCount); std::fflush (stderr); }
   #endif
    pageReady = true;
    lastSequence = 0;
    wasHidden = (webView == nullptr) || ! webView->isShowing();   // hidden -> the timer pushes the full init once visible
    if (! wasHidden)
        pushFullInit();
}

//==============================================================================
// C++ -> JS
//==============================================================================
juce::var PRISMAudioProcessorEditor::buildInitParams() const
{
    auto* o = new juce::DynamicObject();

    for (auto& d : kParams)
        if (auto* p = processorRef.apvts.getParameter (d.id))
            o->setProperty (d.id, paramToVar (*p, d.kind));

    for (int i = 0; i < 2; ++i)
        o->setProperty (kSrcNameIds[i], processorRef.apvts.state.getProperty (kSrcNameIds[i], kSrcNameDefs[i]).toString());

    return juce::var (o);
}

juce::var PRISMAudioProcessorEditor::buildInitUi() const
{
    const auto st = sanitizedUiState (processorRef.apvts.state);   // corrupt / out-of-range saved state never reaches the page
    auto* o = new juce::DynamicObject();
    o->setProperty ("row_splits",       intsToVar (parseInts (st.getProperty (prism::ids::uiRowSplits, "").toString())));
    o->setProperty ("lower_col_splits", intsToVar (parseInts (st.getProperty (prism::ids::uiLowerColSplits, "").toString())));
    o->setProperty ("fullscreen",       (bool) st.getProperty (prism::ids::uiFullscreen, false));
    o->setProperty ("waveform_span",    (double) st.getProperty (prism::ids::uiWaveformSpan, 8.0));
    o->setProperty ("waveform_style",   st.getProperty (prism::ids::uiWaveformStyle, "rgb").toString());
    o->setProperty ("gonio_source",     (int) st.getProperty (prism::ids::uiGonioSource, 0));
    o->setProperty ("scope_source",     (int) st.getProperty (prism::ids::uiScopeSource, 0));
    o->setProperty ("spectrogram_orientation", st.getProperty (prism::ids::uiSgOrientation, "v").toString() == "h" ? "h" : "v");
    return juce::var (o);
}

void PRISMAudioProcessorEditor::pushFullInit()
{
    if (webView == nullptr) return;

    auto* o = new juce::DynamicObject();
    o->setProperty ("params", buildInitParams());
    o->setProperty ("ui",     buildInitUi());
    webView->emitEventIfBrowserIsVisible ("prismInit", juce::var (o));

    if (paramBridge != nullptr)   // everything was just sent: drop any stale dirty flags
    {
        for (size_t i = 0; i < kParams.size(); ++i) paramBridge->consumeDirty (i);
        paramBridge->consumeNamesDirty();
    }
}

void PRISMAudioProcessorEditor::flushParameterChanges()
{
    for (size_t i = 0; i < kParams.size(); ++i)
    {
        if (! paramBridge->consumeDirty (i)) continue;

        if (auto* p = processorRef.apvts.getParameter (kParams[i].id))
        {
            auto* o = new juce::DynamicObject();
            o->setProperty ("id",    kParams[i].id);
            o->setProperty ("value", paramToVar (*p, kParams[i].kind));
            webView->emitEventIfBrowserIsVisible ("prismParamSync", juce::var (o));
        }
    }

    if (paramBridge->consumeNamesDirty())
    {
        for (int i = 0; i < 2; ++i)
        {
            auto* o = new juce::DynamicObject();
            o->setProperty ("id",    kSrcNameIds[i]);
            o->setProperty ("value", processorRef.apvts.state.getProperty (kSrcNameIds[i], kSrcNameDefs[i]).toString());
            webView->emitEventIfBrowserIsVisible ("prismParamSync", juce::var (o));
        }
    }
}

void PRISMAudioProcessorEditor::timerCallback()
{
   #if PRISM_DEBUG_CONTROL
    if (webView != nullptr)
        if (const char* path = std::getenv ("PRISM_DEBUG_JS_FILE"))
        {
            juce::File f (path);
            if (f.existsAsFile())
            {
                const auto js = f.loadFileAsString();
                f.deleteFile();
               #if PRISM_DEBUG_TONE
                if (js.startsWith ("//BURST"))   // "//BURST <ms>": exactly that many ms of tone (sample-accurate), then silence
                {
                    const double fs = processorRef.getSampleRate() > 0.0 ? processorRef.getSampleRate() : 48000.0;
                    processorRef.toneStartMs.store (0);
                    processorRef.toneLeft.store ((int) std::lround (fs * js.fromFirstOccurrenceOf ("//BURST", false, false).getDoubleValue() / 1000.0));
                    std::fprintf (stderr, "PRISM_BURST armed epoch_ms=%lld\n", (long long) std::chrono::duration_cast<std::chrono::milliseconds> (std::chrono::system_clock::now().time_since_epoch()).count());
                    std::fflush (stderr);
                }
                else if (js.startsWith ("//TONEON"))  { processorRef.toneLeft.store (-1); }
                else if (js.startsWith ("//TONEOFF")) { processorRef.toneLeft.store (0); }
                else
               #endif
                if (js.startsWith ("//DUMP"))   // print host-side truth: parameter values, ui state, latest snapshot meters
                {
                    const auto snap = processorRef.getLatestAnalysisSnapshot();
                    std::fprintf (stderr, "PRISM_DUMP toneStart=%lld params=%s ui=%s snap=%s\n",
                       #if PRISM_DEBUG_TONE
                        (long long) processorRef.toneStartMs.load(),
                       #else
                        0LL,
                       #endif
                        juce::JSON::toString (buildInitParams(), true).replaceCharacters ("\n\r\t", "   ").toRawUTF8(),
                        juce::JSON::toString (buildInitUi(), true).replaceCharacters ("\n\r\t", "   ").toRawUTF8(),
                        snap == nullptr ? "null" : (juce::String ("seq=") + juce::String ((juce::int64) snap->sequence)
                            + " M=" + juce::String (snap->momentaryLufs) + " I=" + juce::String (snap->integratedLufs)
                            + " LRA=" + juce::String (snap->loudnessRange)
                            + " TPmax=" + juce::String (snap->sources[0].truePeakMaxDb) + " SPmax=" + juce::String (snap->sources[0].samplePeakMaxDb)).toRawUTF8());
                    std::fflush (stderr);
                }
                else if (js.isNotEmpty())
                    webView->evaluateJavascript (js, [] (juce::WebBrowserComponent::EvaluationResult r)
                    {
                        if (auto* v = r.getResult()) std::fprintf (stderr, "PRISM_JS_RESULT %s\n", juce::JSON::toString (*v, true).toRawUTF8());
                        else                         std::fprintf (stderr, "PRISM_JS_ERROR\n");
                        std::fflush (stderr);
                    });
            }
        }
   #endif

    if (! pageReady || webView == nullptr)
        return;   // JS has not registered listeners yet; dirty flags are kept for the prismInit resync

    // emitEventIfBrowserIsVisible() silently drops events while the view is hidden. Nothing is consumed
    // (no dirty flag cleared) while hidden; when visibility returns the whole state is re-pushed.
    if (! webView->isShowing())
    {
        wasHidden = true;
        setEngineActive (false);
        return;
    }
    setEngineActive (true);

    ++tickCount;
    // reduced_motion: 30 Hz data push (parameter sync stays at 60 Hz)
    static constexpr bool kHalveRateOnReducedMotion = true;
    const bool reduced = kHalveRateOnReducedMotion && processorRef.apvts.getRawParameterValue ("reduced_motion")->load() >= 0.5f;

    if (paramBridge->consumeRedirected())   // setStateInformation / replaceState happened
    {
        const auto st = sanitizedUiState (processorRef.apvts.state);
        const int w = (int) st.getProperty (prism::ids::uiWidth,  kDefaultWidth);
        const int h = (int) st.getProperty (prism::ids::uiHeight, kDefaultHeight);
        if (w != getWidth() || h != getHeight()) setSize (w, h);
        wasHidden = true;   // force the full resync below
    }

    if (wasHidden)
    {
        wasHidden = false;
        pushFullInit();     // params + names + splits + orientation + fullscreen; also clears dirty flags
    }

    flushParameterChanges();
    if (! reduced || (tickCount & 1u) == 0u)
        pushAnalysisFrame();
}

void PRISMAudioProcessorEditor::pushAnalysisFrame()
{
   #if PRISM_FRAME_TIMING
    const auto t0 = juce::Time::getHighResolutionTicks();
    if (! pushAnalysisFrameImpl())
        return;   // no new snapshot: not a real push, excluded from the statistics
    frameTimesMs.push_back ((float) (juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - t0) * 1000.0));
    if (frameTimesMs.size() >= 1200)
    {
        auto v = frameTimesMs; std::sort (v.begin(), v.end());
        float sum = 0; for (auto x : v) sum += x;
        std::fprintf (stderr, "PRISM_FRAME_TIMING n=%zu mean=%.3fms p50=%.3fms p99=%.3fms max=%.3fms\n", v.size(),
                      sum / (float) v.size(), v[v.size() / 2], v[v.size() * 99 / 100], v.back());
        std::fflush (stderr);
        frameTimesMs.clear();
    }
   #else
    pushAnalysisFrameImpl();
   #endif
}

bool PRISMAudioProcessorEditor::pushAnalysisFrameImpl()
{
    const auto snap = fetchLatestSnapshot();
    if (snap == nullptr || snap->sequence == lastSequence)
        return false;   // nothing new: the UI keeps animating from its last targets, then falls idle after 1 s

    lastSequence = snap->sequence;

    juce::Array<juce::var> spectrum, waveform, gonio, scope, peaks, stereo;

    for (const auto& s : snap->sources)
    {
        spectrum.add (s.hasSpectrum ? juce::var (packFloats (s.spectrumDb.data(), prism::kSpectrumPoints)) : juce::var());

        // waveform columns are flattened [bass, mids, highs, flag, onsetFrac] x n (frame field waveformStride = 5; older pages assume 4)
        std::vector<float> flat;
        flat.reserve ((size_t) s.waveformColumnCount * 5);
        for (int c = 0; c < s.waveformColumnCount; ++c)
            flat.insert (flat.end(), s.waveformColumns[(size_t) c].begin(), s.waveformColumns[(size_t) c].end());
        waveform.add (juce::var (packFloats (flat.data(), (int) flat.size())));

        gonio.add (s.gonioPairCount > 0 ? juce::var (packFloats (s.gonio.data(), s.gonioPairCount * 2)) : juce::var());
        scope.add (s.scopeCount     > 0 ? juce::var (packFloats (s.scope.data(), s.scopeCount))         : juce::var());

        auto* pk = new juce::DynamicObject();
        pk->setProperty ("tp", numberOrNull (s.truePeakMaxDb));
        pk->setProperty ("sp", numberOrNull (s.samplePeakMaxDb));
        peaks.add (juce::var (pk));

        if (s.hasStereo)
        {
            auto* st = new juce::DynamicObject();
            st->setProperty ("corr",  numberOrNull (s.correlation));
            st->setProperty ("width", numberOrNull (s.widthPercent));
            st->setProperty ("bal",   numberOrNull (s.balanceDb));
            stereo.add (juce::var (st));
        }
        else
        {
            stereo.add (juce::var());
        }
    }

    auto* loud = new juce::DynamicObject();
    loud->setProperty ("M",   numberOrNull (snap->momentaryLufs));
    loud->setProperty ("S",   numberOrNull (snap->shortTermLufs));
    loud->setProperty ("I",   numberOrNull (snap->integratedLufs));
    loud->setProperty ("LRA", numberOrNull (snap->loudnessRange));

    auto* meters = new juce::DynamicObject();
    meters->setProperty ("loudness", juce::var (loud));
    meters->setProperty ("peaks",    peaks);
    meters->setProperty ("stereo",   stereo);

    auto* frame = new juce::DynamicObject();
    frame->setProperty ("sampleRate", snap->sampleRate);
    frame->setProperty ("live",       snap->signalPresent);
    frame->setProperty ("bpm",        numberOrNull (snap->bpm));
    frame->setProperty ("waveformStride", 5);   // floats per waveform column: bass, mids, highs, flag, onsetFrac (older pages assume 4 and ignore the rest)
    frame->setProperty ("transport", snap->transportPlaying);   // additive: -1 unknown (Standalone / no playhead), 0 paused, 1 playing
    if (snap->barAnchorStreamFrame >= 0)   // additive + optional: the grid anchor (stream frame of the latest downbeat; exact in a double up to 2^53)
        frame->setProperty ("barAnchor", (double) snap->barAnchorStreamFrame);
    {
        juce::Array<juce::var> live;   // in-progress column per source: [index, bass, mids, highs] (index -1 = none); additive + optional
        for (const auto& s : snap->sources)
            live.add (juce::Array<juce::var> { (double) s.liveColumnIndex, (double) s.liveColumn[0], (double) s.liveColumn[1], (double) s.liveColumn[2] });
        frame->setProperty ("waveformLive", live);
    }
    {
        juce::Array<juce::var> first;   // absolute index of the first column in each source's waveform array (-1 = none): place by index, detect gaps
        for (const auto& s : snap->sources) first.add ((double) s.waveformFirstColumn);
        frame->setProperty ("waveformFirst", first);
    }
    if (snap->timeSigNumerator > 0 && snap->timeSigDenominator > 0)   // additive + optional: older pages ignore it
        frame->setProperty ("timeSig", juce::Array<juce::var> { snap->timeSigNumerator, snap->timeSigDenominator });
    frame->setProperty ("spectrum",   spectrum);
    frame->setProperty ("waveform",   waveform);
    // Decimation hook (risk R2 lever): gonio/scope on every 2nd pushed frame only. OFF: the page treats a missing
    // field as "no data" for that module, so enabling this needs the page to hold the last arrays (Phase 3.4).
    static constexpr bool kDecimateGonioScopeTo30Hz = false;
    if (! kDecimateGonioScopeTo30Hz || (tickCount & 1u) == 0u)
    {
        frame->setProperty ("gonio",  gonio);
        frame->setProperty ("scope",  scope);
    }
    frame->setProperty ("meters",     juce::var (meters));

    webView->emitEventIfBrowserIsVisible ("prismFrame", juce::var (frame));
    return true;
}

//==============================================================================
// Stage 2 seams
//==============================================================================
std::shared_ptr<const prism::AnalysisSnapshot> PRISMAudioProcessorEditor::fetchLatestSnapshot() const
{
    // The processor records the fetch heartbeat and lastFetchedSequence (the waveform ack) inside this call.
    return processorRef.getLatestAnalysisSnapshot();
}

void PRISMAudioProcessorEditor::requestResetAll()
{
    processorRef.requestResetAll();   // integrated loudness, LRA, max true/sample peaks. lastSequence is NOT reset: delivered columns stay valid, and re-pushing the same snapshot would duplicate waveform columns
}

void PRISMAudioProcessorEditor::requestResetIntegrated()
{
    processorRef.requestResetIntegrated();   // integrated LUFS + LRA only (same: no forced re-push)
}

//==============================================================================
// Resource provider (BinaryData from juce_add_binary_data, plus the dynamic seed script)
//==============================================================================
std::optional<juce::WebBrowserComponent::Resource> PRISMAudioProcessorEditor::getResource (const juce::String& url)
{
    const juce::String path = url.upToFirstOccurrenceOf ("?", false, false);

    if (path == "/" || path == "/index.html")
        return juce::WebBrowserComponent::Resource {
            toBytes (BinaryData::index_html, BinaryData::index_htmlSize),
            juce::String ("text/html") };

    if (path == "/prism-init.js")
    {
        // Evaluated before the page script: layout splits/fullscreen + parameter values + source names.
        const auto script = "window.__PRISM_INIT_UI = "     + juce::JSON::toString (buildInitUi(),     true) + ";\n"
                            "window.__PRISM_INIT_PARAMS = " + juce::JSON::toString (buildInitParams(), true) + ";\n";
        return juce::WebBrowserComponent::Resource { toBytes (script), juce::String ("application/javascript") };
    }

    return std::nullopt;
}
