// ============================================================================
// PRISM - WebView plugin editor (TEMPLATE generated from finalized mockup v8)
// See v8-PluginEditor.h for the full JS <-> C++ contract.
// Assumes the processor exposes:   juce::AudioProcessorValueTreeState apvts;
// (rename processorRef.apvts below if the Stage 1 processor names it differently)
// ============================================================================

#include "PluginEditor.h"
#include "BinaryData.h"

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

    // ValueTree (non-automatable) state entries, persisted inside apvts.state
    constexpr const char* kSrcNameIds[2]  = { "src1_name", "src2_name" };
    constexpr const char* kSrcNameDefs[2] = { "Ch1", "Ch2" };
    constexpr int kMaxNameLength = 10;

    // Serialized UI state (layout splits / fullscreen / window size), also inside apvts.state
    constexpr const char* kRowSplitsId = "ui_row_splits";        // "226,70,302"
    constexpr const char* kColSplitsId = "ui_lower_col_splits";  // "296,266,272"
    constexpr const char* kFullscreenId = "ui_fullscreen";
    constexpr const char* kSgOrientId   = "ui_spectrogram_orientation";   // "v" | "h" (UI state, NOT a parameter)
    constexpr const char* kWidthId  = "ui_width";
    constexpr const char* kHeightId = "ui_height";
    constexpr int kDefaultWidth = 1100, kDefaultHeight = 720, kMinWidth = 770, kMinHeight = 504;

    static_assert (kParams.size() == 17, "gestureOpen in the header is sized for the 17 locked APVTS parameters");

    // Layout/size sanitising is shared with the processor (PRISMAudioProcessor::sanitizeState), so the
    // editor and setStateInformation apply identical clamps.
    juce::ValueTree sanitizedUiState (const juce::ValueTree& st)
    {
        auto copy = st.createCopy();
        PRISMAudioProcessor::sanitizeState (copy);
        return copy;
    }

    juce::Array<int> parseInts (const juce::String& csv)
    {
        juce::Array<int> out;
        for (auto& t : juce::StringArray::fromTokens (csv, ",", ""))
            out.add (t.getIntValue());
        return out;
    }

    juce::Array<int> intsFromVar (const juce::var& arr)
    {
        juce::Array<int> out;
        if (auto* a = arr.getArray())
            for (auto& e : *a) out.add ((int) e);
        return out;
    }

    juce::String joinInts (const juce::Array<int>& v)
    {
        juce::StringArray parts;
        for (int e : v) parts.add (juce::String (e));
        return parts.joinIntoString (",");
    }

    juce::var intsToVar (const juce::Array<int>& v)
    {
        juce::Array<juce::var> out;
        for (int e : v) out.add (e);
        return out;
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
        .withEventListener ("prismReady",   [this] (juce::var)   { handlePageReady(); });
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
    setSize ((int) st.getProperty (kWidthId, kDefaultWidth), (int) st.getProperty (kHeightId, kDefaultHeight));

    setEngineActive (true);
    startTimerHz (60);   // analysis frames + parameter sync
}

PRISMAudioProcessorEditor::~PRISMAudioProcessorEditor()
{
    stopTimer();
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
    if (webView != nullptr)
        webView->setBounds (getLocalBounds());

    // serialize window size with the plugin state
    if (getWidth() >= kMinWidth)
    {
        auto& st = processorRef.apvts.state;
        st.setProperty (kWidthId,  getWidth(),  nullptr);
        st.setProperty (kHeightId, getHeight(), nullptr);
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
    tmp.setProperty (uiWaveformSpan,   (int) payload.getProperty ("waveform_span", 12),                       nullptr);
    PRISMAudioProcessor::sanitizeState (tmp);

    auto& st = processorRef.apvts.state;
    for (auto* id : { uiRowSplits, uiLowerColSplits, uiSgOrientation, uiFullscreen, uiWaveformSpan })
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

void PRISMAudioProcessorEditor::handlePageReady()
{
    closeOpenGestures();   // a (re)loaded page cannot still be mid-drag
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
    o->setProperty ("row_splits",       intsToVar (parseInts (st.getProperty (kRowSplitsId, "").toString())));
    o->setProperty ("lower_col_splits", intsToVar (parseInts (st.getProperty (kColSplitsId, "").toString())));
    o->setProperty ("fullscreen",       (bool) st.getProperty (kFullscreenId, false));
    o->setProperty ("waveform_span",    (int) st.getProperty (prism::ids::uiWaveformSpan, 12));
    o->setProperty ("spectrogram_orientation", st.getProperty (kSgOrientId, "v").toString() == "h" ? "h" : "v");
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
        const int w = (int) st.getProperty (kWidthId,  kDefaultWidth);
        const int h = (int) st.getProperty (kHeightId, kDefaultHeight);
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

        // waveform columns are flattened [bass, mids, highs, flag] x n
        std::vector<float> flat;
        flat.reserve ((size_t) s.waveformColumnCount * 4);
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
    processorRef.requestResetAll();   // integrated loudness, LRA, max true/sample peaks
    lastSequence = 0;
}

void PRISMAudioProcessorEditor::requestResetIntegrated()
{
    processorRef.requestResetIntegrated();   // integrated LUFS + LRA only
    lastSequence = 0;
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
