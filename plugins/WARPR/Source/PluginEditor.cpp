/*
  ==============================================================================
   WARPR - PluginEditor.cpp

   Adapted from .ideas/mockups/v9-PluginEditor.cpp.  Hosts Source/ui/public/index.html in a
   WebBrowserComponent.  Stage 1: all 135 relays + attachments and the 14 native functions are
   registered against the stub backend; the page opens blank-safe.  Stage 3 (G1) verifies and
   completes the wiring.

   Wire protocol with the page (names are the contract with the page):

     native functions (JS -> C++, all MESSAGE thread, all return via completion):
       warpr_getUiState()                    -> JSON string
       warpr_setLfoCount(n)
       warpr_setLfoShape(index, json)
       warpr_setModMatrix(json)
       warpr_setShaperCurve(curveId, json)
       warpr_shaperReset(curveId)            warpr_shaperNormalize(curveId)
       warpr_loadSample()                    -> opens an async file chooser
       warpr_requestSample()                 -> re-emits "warprSample"
       warpr_requestFilterResponse()         -> re-emits "warprFilterResponse"
       warpr_presetStep(+1|-1)  warpr_presetSave()  warpr_presetLoad()
       warpr_openSettings()

     events (C++ -> JS, emitted from the editor Timer, 30 Hz):
       "warprViz"            organism / meters / playheads / lfo phases (lock-free FIFO)
       "warprSample"         name + peaks, on revision change or request
       "warprFilterResponse" {db:[256]} real filter magnitude response
       "warprUiState"        full non-parameter state JSON (string)
       "warprShaperCurve"    {id, curve} (preset / reset / normalize / state load)
       "warprPreset"         {name}
  ==============================================================================
*/

#include "PluginEditor.h"
#include "BinaryData.h"

namespace
{
    // Parameter lookup that fails loudly (debug) if the table and the APVTS diverge.
    juce::RangedAudioParameter& param (PluginProcessor& p, const juce::String& id)
    {
        auto* prm = p.apvts.getParameter (id);
        jassert (prm != nullptr);                    // id missing from the APVTS layout -> fix the layout, not this file
        return *prm;
    }

    std::vector<std::byte> toBytes (const char* data, int size)
    {
        return std::vector<std::byte> (reinterpret_cast<const std::byte*> (data),
                                       reinterpret_cast<const std::byte*> (data) + size);
    }
}

//==============================================================================
PluginEditor::PluginEditor (PluginProcessor& p)
    : AudioProcessorEditor (&p), audioProcessor (p)
{
    // ------------------------------------------------------------------------
    // STEP 1: RELAYS (before the WebView)
    // ------------------------------------------------------------------------
   #define X(id) id##Relay = std::make_unique<juce::WebSliderRelay> (#id);
    WARPR_SLIDER_PARAMS (X)
   #undef X
   #define X(id) id##Relay = std::make_unique<juce::WebToggleButtonRelay> (#id);
    WARPR_TOGGLE_PARAMS (X)
   #undef X
   #define X(id) id##Relay = std::make_unique<juce::WebComboBoxRelay> (#id);
    WARPR_COMBO_PARAMS (X)
   #undef X

    for (size_t i = 0; i < (size_t) numLfos; ++i)
    {
        const juce::String n ((int) i + 1);
        auto& r = lfoRelays[i];
        r.mode   = std::make_unique<juce::WebComboBoxRelay> ("lfo" + n + "Mode");
        r.dir    = std::make_unique<juce::WebComboBoxRelay> ("lfo" + n + "Dir");
        r.sync   = std::make_unique<juce::WebComboBoxRelay> ("lfo" + n + "Sync");
        r.gridX  = std::make_unique<juce::WebComboBoxRelay> ("lfo" + n + "GridX");
        r.gridY  = std::make_unique<juce::WebComboBoxRelay> ("lfo" + n + "GridY");
        r.rate   = std::make_unique<juce::WebSliderRelay>   ("lfo" + n + "Rate");
        r.phase  = std::make_unique<juce::WebSliderRelay>   ("lfo" + n + "Phase");
        r.smooth = std::make_unique<juce::WebSliderRelay>   ("lfo" + n + "Smooth");
    }

    // ------------------------------------------------------------------------
    // STEP 2: WEBVIEW (relay options + native functions)
    // ------------------------------------------------------------------------
    auto options = juce::WebBrowserComponent::Options{}
        .withNativeIntegrationEnabled()
       #if JUCE_WINDOWS
        // WebView2 must be selected explicitly or JUCE falls back to the legacy IE control
        .withBackend (juce::WebBrowserComponent::Options::Backend::webview2)
       #endif
        .withResourceProvider ([this] (const juce::String& url) { return getResource (url); })
        .withKeepPageLoadedWhenBrowserIsHidden()
       #if JUCE_WINDOWS
        // writable user-data folder (Program Files next to the DAW exe is read-only)
        .withWinWebView2Options (juce::WebBrowserComponent::Options::WinWebView2{}
            .withUserDataFolder (juce::File::getSpecialLocation (juce::File::tempDirectory)
                                     .getChildFile (warpr::identity::webView2DataDir)))
       #endif
        ;

    registerNativeFunctions (options);

   #define X(id) options = options.withOptionsFrom (*id##Relay);
    WARPR_SLIDER_PARAMS (X)
    WARPR_TOGGLE_PARAMS (X)
    WARPR_COMBO_PARAMS (X)
   #undef X
    for (auto& r : lfoRelays)
    {
        options = options.withOptionsFrom (*r.mode).withOptionsFrom (*r.dir).withOptionsFrom (*r.sync)
                         .withOptionsFrom (*r.gridX).withOptionsFrom (*r.gridY)
                         .withOptionsFrom (*r.rate).withOptionsFrom (*r.phase).withOptionsFrom (*r.smooth);
    }

    webView = std::make_unique<juce::WebBrowserComponent> (options);

    // ------------------------------------------------------------------------
    // STEP 3: ATTACHMENTS (after the WebView) - JUCE 8 needs the 3rd (undo) argument
    // ------------------------------------------------------------------------
   #define X(id) id##Attachment = std::make_unique<juce::WebSliderParameterAttachment> ( \
                     param (audioProcessor, #id), *id##Relay, nullptr);
    WARPR_SLIDER_PARAMS (X)
   #undef X
   #define X(id) id##Attachment = std::make_unique<juce::WebToggleButtonParameterAttachment> ( \
                     param (audioProcessor, #id), *id##Relay, nullptr);
    WARPR_TOGGLE_PARAMS (X)
   #undef X
   #define X(id) id##Attachment = std::make_unique<juce::WebComboBoxParameterAttachment> ( \
                     param (audioProcessor, #id), *id##Relay, nullptr);
    WARPR_COMBO_PARAMS (X)
   #undef X

    for (size_t i = 0; i < (size_t) numLfos; ++i)
    {
        const juce::String n ((int) i + 1);
        auto& r = lfoRelays[i];
        auto& a = lfoAttachments[i];
        a.mode   = std::make_unique<juce::WebComboBoxParameterAttachment> (param (audioProcessor, "lfo" + n + "Mode"),   *r.mode,   nullptr);
        a.dir    = std::make_unique<juce::WebComboBoxParameterAttachment> (param (audioProcessor, "lfo" + n + "Dir"),    *r.dir,    nullptr);
        a.sync   = std::make_unique<juce::WebComboBoxParameterAttachment> (param (audioProcessor, "lfo" + n + "Sync"),   *r.sync,   nullptr);
        a.gridX  = std::make_unique<juce::WebComboBoxParameterAttachment> (param (audioProcessor, "lfo" + n + "GridX"),  *r.gridX,  nullptr);
        a.gridY  = std::make_unique<juce::WebComboBoxParameterAttachment> (param (audioProcessor, "lfo" + n + "GridY"),  *r.gridY,  nullptr);
        a.rate   = std::make_unique<juce::WebSliderParameterAttachment>   (param (audioProcessor, "lfo" + n + "Rate"),   *r.rate,   nullptr);
        a.phase  = std::make_unique<juce::WebSliderParameterAttachment>   (param (audioProcessor, "lfo" + n + "Phase"),  *r.phase,  nullptr);
        a.smooth = std::make_unique<juce::WebSliderParameterAttachment>   (param (audioProcessor, "lfo" + n + "Smooth"), *r.smooth, nullptr);
    }

    // ------------------------------------------------------------------------
    // PAGE LOAD (cache-busted: pattern #24)
    // ------------------------------------------------------------------------
    addAndMakeVisible (*webView);
    webView->goToURL (juce::WebBrowserComponent::getResourceProviderRoot()
                      + "?t=" + juce::String (juce::Time::getMillisecondCounterHiRes(), 0));

    // ------------------------------------------------------------------------
    // WINDOW: 1200 x 820 design size; the page scales uniformly, so the aspect ratio is
    // locked.  Spec minimum 1000x700 -> effective 1025x700 at this ratio.
    // ------------------------------------------------------------------------
    setResizeLimits (1025, 700, 2400, 1640);
    getConstrainer()->setFixedAspectRatio ((double) designWidth / (double) designHeight);
    setSize (designWidth, designHeight);

    startTimerHz (30);
}

PluginEditor::~PluginEditor()
{
    stopTimer();
    // Members destruct in reverse order: chooser etc -> attachments -> webView -> relays.
}

void PluginEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colours::black);       // visible only before the page paints
}

void PluginEditor::resized()
{
    if (webView != nullptr)
        webView->setBounds (getLocalBounds());
}

//==============================================================================
// Native functions (JS -> C++)
//==============================================================================
void PluginEditor::registerNativeFunctions (juce::WebBrowserComponent::Options& options)
{
    using Args = juce::Array<juce::var>;
    using Done = juce::WebBrowserComponent::NativeFunctionCompletion;

    auto be = [this]() -> WarprUiBackend& { return audioProcessor.getUiBackend(); };

    options = options
        .withNativeFunction ("warpr_getUiState", [be] (const Args&, Done done)
        {
            done (juce::var (be().getUiStateJson()));
        })
        .withNativeFunction ("warpr_setLfoCount", [be] (const Args& a, Done done)
        {
            if (a.size() > 0) be().setLfoCount (juce::jlimit (1, numLfos, (int) a[0]));
            done ({});
        })
        .withNativeFunction ("warpr_setLfoShape", [be] (const Args& a, Done done)
        {
            if (a.size() > 1) be().setLfoShapeJson (juce::jlimit (0, numLfos - 1, (int) a[0]), a[1].toString());
            done ({});
        })
        .withNativeFunction ("warpr_setModMatrix", [be] (const Args& a, Done done)
        {
            if (a.size() > 0) be().setModMatrixJson (a[0].toString());
            done ({});
        })
        .withNativeFunction ("warpr_setShaperCurve", [be] (const Args& a, Done done)
        {
            if (a.size() > 1) be().setShaperCurveJson (a[0].toString(), a[1].toString());
            done ({});
        })
        .withNativeFunction ("warpr_shaperReset", [be] (const Args& a, Done done)
        {
            if (a.size() > 0) be().shaperReset (a[0].toString());
            done ({});
        })
        .withNativeFunction ("warpr_shaperNormalize", [be] (const Args& a, Done done)
        {
            if (a.size() > 0) be().shaperNormalize (a[0].toString());
            done ({});
        })
        .withNativeFunction ("warpr_loadSample", [this] (const Args&, Done done)
        {
            chooser = std::make_unique<juce::FileChooser> ("Load sample", juce::File{}, "*.wav;*.aif;*.aiff");
            chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                [safe = juce::Component::SafePointer<PluginEditor> (this)] (const juce::FileChooser& fc)
                {
                    const auto f = fc.getResult();
                    if (safe != nullptr && f.existsAsFile())
                        safe->audioProcessor.getUiBackend().loadSampleFile (f);   // decode off-thread inside; bumps Revision::sample
                });
            done ({});
        })
        .withNativeFunction ("warpr_requestSample", [this] (const Args&, Done done)
        {
            sampleRequested = true; done ({});
        })
        .withNativeFunction ("warpr_requestFilterResponse", [this] (const Args&, Done done)
        {
            filterRequested = true; done ({});
        })
        .withNativeFunction ("warpr_setSliceMap", [be] (const Args& a, Done done)
        {
            // slice mode: the UI's slice document (stored verbatim in uiState, published to the engine)
            if (a.size() > 0)
                be().setSliceMapJson (a[0].toString());

            done ({});
        })
        .withNativeFunction ("warpr_sliceEdges", [be] (const Args& a, Done done)
        {
            // auto-slice: (count, byTransients) -> [e0..eN] over the current sample region
            juce::Array<juce::var> out;

            if (a.size() >= 2)
            {
                std::vector<double> edges;

                if (be().computeSliceEdges ((int) a[0], (bool) ((int) a[1] != 0), edges))
                {
                    out.ensureStorageAllocated ((int) edges.size());

                    for (double e : edges)
                        out.add (e);
                }
            }

            done (juce::var (out));
        })
        .withNativeFunction ("warpr_processedPeaks", [be] (const Args& a, Done done)
        {
            // processed-waveform preview: (startNorm, endNorm, pairCount) -> [min, max, ...] of the
            // region run through the LIVE warp + shaper settings (real kernels, message thread)
            juce::Array<juce::var> out;

            if (a.size() >= 3)
            {
                const int pairs = juce::jlimit (1, 2048, (int) a[2]);
                std::vector<float> buf ((size_t) pairs * 2, 0.0f);
                const int got = be().computeProcessedPeaks (buf.data(), pairs, (double) a[0], (double) a[1]);
                out.ensureStorageAllocated (got * 2);

                for (int i = 0; i < got * 2; ++i)
                    out.add ((double) buf[(size_t) i]);
            }

            done (juce::var (out));
        })
        .withNativeFunction ("warpr_rangePeaks", [be] (const Args& a, Done done)
        {
            // waveform zoom: (startNorm, endNorm, pairCount) -> [min, max, ...] for that sub-range
            juce::Array<juce::var> out;

            if (a.size() >= 3)
            {
                const auto pk = be().getRangePeaks ((double) a[0], (double) a[1], (int) a[2]);
                out.ensureStorageAllocated ((int) pk.size());

                for (float v : pk)
                    out.add ((double) v);
            }

            done (juce::var (out));
        })
        .withNativeFunction ("warpr_presetStep", [be] (const Args& a, Done done)
        {
            if (a.size() > 0) be().presetStep ((int) a[0] < 0 ? -1 : 1);
            done ({});
        })
        .withNativeFunction ("warpr_presetSave",   [be] (const Args&, Done done) { be().presetSaveDialog(); done ({}); })
        .withNativeFunction ("warpr_presetLoad",   [be] (const Args&, Done done) { be().presetLoadDialog(); done ({}); })
        .withNativeFunction ("warpr_openSettings", [be] (const Args&, Done done) { be().openSettings();     done ({}); })
        .withNativeFunction ("warpr_midiNote", [this] (const Args& a, Done done)
        {
            // QWERTY keyboard: (note, velocity, on) -> wait-free queue -> processBlock's MIDI stream
            if (a.size() >= 3)
                audioProcessor.queueKeyboardNote ((int) a[0], (int) a[1], (bool) a[2]);
            done ({});
        })
        .withNativeFunction ("warpr_dropBegin", [this] (const Args& a, Done done)
        {
            dropName = a.size() > 0 ? a[0].toString() : juce::String();
            dropStream = std::make_unique<juce::MemoryOutputStream>();
            done ({});
        })
        .withNativeFunction ("warpr_dropChunk", [this] (const Args& a, Done done)
        {
            if (dropStream != nullptr && a.size() > 0)
                juce::Base64::convertFromBase64 (*dropStream, a[0].toString());
            done ({});
        })
        .withNativeFunction ("warpr_dropEnd", [this, be] (const Args&, Done done)
        {
            if (dropStream != nullptr && dropStream->getDataSize() > 0)
            {
                const auto name = juce::File::createLegalFileName (dropName.isEmpty() ? "dropped.wav" : dropName);
                auto file = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                .getChildFile ("WARPR").getChildFile (name);
                file.getParentDirectory().createDirectory();
                file.deleteFile();

                if (auto out = file.createOutputStream())
                {
                    out->write (dropStream->getData(), dropStream->getDataSize());
                    out->flush();
                    be().loadSampleFile (file);
                }
            }

            dropStream = nullptr;
            done ({});
        });
}

//==============================================================================
// Timer: lock-free audio data + revision-driven pushes (C++ -> JS)
//==============================================================================
void PluginEditor::timerCallback()
{
    audioProcessor.drainRetired();          // objects retired by the audio thread are freed HERE, never there

    if (webView == nullptr)
        return;

    auto& be = audioProcessor.getUiBackend();

    // Playheads: wait-free snapshot of the sounding voices' read positions (written by the audio
    // thread).  D2 will additionally push full frames through the FIFO; a FIFO frame that already
    // carries voices wins, otherwise the snapshot is merged in.  An idle, silent session emits nothing.
    WarprVizFrame frame;
    const bool gotFrame = be.vizFifo().popLatest (frame);
    if (frame.numVoices == 0)
        frame.numVoices = be.getPlayheads (frame.voices.data(), WarprVizFrame::maxVoices);

    if (gotFrame || frame.numVoices > 0 || frame.numGrains > 0 || playheadsWereShown)
    {
        emitViz (frame);
        // one trailing empty frame clears the playheads / grain cloud
        playheadsWereShown = frame.numVoices > 0 || frame.numGrains > 0;
    }

    auto changed = [&be] (WarprUiBackend::Revision r, uint32_t& seen)
    {
        const auto v = be.revision (r);
        if (v == seen) return false;
        seen = v; return true;
    };

    const bool uiState = changed (WarprUiBackend::Revision::uiState, seenUiState);
    const bool sample  = changed (WarprUiBackend::Revision::sample, seenSample);
    const bool shaper  = changed (WarprUiBackend::Revision::shaperCurves, seenShaper);
    const bool filter  = changed (WarprUiBackend::Revision::filterResponse, seenFilter);
    const bool preset  = changed (WarprUiBackend::Revision::preset, seenPreset);

    if (uiState) emitUiState();
    if (sample  || sampleRequested.exchange (false))  emitSample();
    if (shaper  || uiState)                           emitShaperCurves();
    if (filter  || filterRequested.exchange (false) || uiState) emitFilterResponse();
    if (preset)  emitPreset();
}

void PluginEditor::emitViz (const WarprVizFrame& f)
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("low", (double) f.low);          o->setProperty ("high", (double) f.high);
    o->setProperty ("rms", (double) f.rms);          o->setProperty ("theta", (double) f.theta);
    o->setProperty ("r", (double) f.r);              o->setProperty ("transient", (double) f.transient);
    o->setProperty ("tang", (double) f.tang);        o->setProperty ("tseq", (juce::int64) f.tseq);
    o->setProperty ("dom", (double) f.dom);          o->setProperty ("sharp", (double) f.sharp);
    o->setProperty ("peakL", (double) f.peakL);      o->setProperty ("peakR", (double) f.peakR);

    juce::Array<juce::var> voices;
    for (int i = 0; i < juce::jmin (f.numVoices, WarprVizFrame::maxVoices); ++i)
    {
        auto* v = new juce::DynamicObject();
        v->setProperty ("pos",   (double) f.voices[(size_t) i].pos);
        v->setProperty ("level", (double) f.voices[(size_t) i].level);
        voices.add (juce::var (v));
    }
    o->setProperty ("voices", voices);

    juce::Array<juce::var> grains;
    for (int i = 0; i < juce::jmin (f.numGrains, WarprVizFrame::maxGrains); ++i)
    {
        auto* g = new juce::DynamicObject();
        g->setProperty ("pos",   (double) f.grains[(size_t) i].pos);
        g->setProperty ("level", (double) f.grains[(size_t) i].level);
        grains.add (juce::var (g));
    }
    o->setProperty ("grains", grains);

    juce::Array<juce::var> lfo;
    for (float ph : f.lfoPhase) lfo.add ((double) ph);
    o->setProperty ("lfo", lfo);

    webView->emitEventIfBrowserIsVisible (juce::Identifier ("warprViz"), juce::var (o));
}

void PluginEditor::emitSample()
{
    const auto d = audioProcessor.getUiBackend().getSampleDisplay();
    auto* o = new juce::DynamicObject();
    o->setProperty ("name", d.name);
    o->setProperty ("channels", d.numChannels);
    o->setProperty ("sampleRate", d.sampleRate);
    o->setProperty ("numSamples", d.numSamples);
    juce::Array<juce::var> peaks;
    peaks.ensureStorageAllocated ((int) d.peaksMinMax.size());
    for (float v : d.peaksMinMax) peaks.add ((double) v);
    o->setProperty ("peaks", peaks);
    webView->emitEventIfBrowserIsVisible (juce::Identifier ("warprSample"), juce::var (o));
}

void PluginEditor::emitFilterResponse()
{
    constexpr int n = 256;
    std::array<float, n> db{};
    audioProcessor.getUiBackend().computeFilterResponse (db.data(), n);   // real DSP magnitude response, message thread
    juce::Array<juce::var> arr;
    arr.ensureStorageAllocated (n);
    for (float v : db) arr.add ((double) v);
    auto* o = new juce::DynamicObject();
    o->setProperty ("db", arr);
    webView->emitEventIfBrowserIsVisible (juce::Identifier ("warprFilterResponse"), juce::var (o));
}

void PluginEditor::emitShaperCurves()
{
    for (const char* id : { "A", "B" })
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("id", juce::String (id));
        o->setProperty ("curve", juce::JSON::parse (audioProcessor.getUiBackend().getShaperCurveJson (id)));
        webView->emitEventIfBrowserIsVisible (juce::Identifier ("warprShaperCurve"), juce::var (o));
    }
}

void PluginEditor::emitUiState()
{
    webView->emitEventIfBrowserIsVisible (juce::Identifier ("warprUiState"),
                                          juce::var (audioProcessor.getUiBackend().getUiStateJson()));
}

void PluginEditor::emitPreset()
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("name", audioProcessor.getUiBackend().getPresetName());
    webView->emitEventIfBrowserIsVisible (juce::Identifier ("warprPreset"), juce::var (o));
}

//==============================================================================
// Sample drag & drop
//==============================================================================
bool PluginEditor::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (auto& f : files)
        if (juce::File (f).hasFileExtension ("wav;aif;aiff"))
            return true;
    return false;
}

void PluginEditor::filesDropped (const juce::StringArray& files, int, int)
{
    for (auto& f : files)
        if (juce::File (f).hasFileExtension ("wav;aif;aiff"))
        {
            audioProcessor.getUiBackend().loadSampleFile (juce::File (f));
            return;
        }
}

//==============================================================================
// Resource provider (explicit URL mapping, pattern #8; query string stripped, pattern #24)
// BinaryData names are FILE names only: index.html -> index_html, js/juce/index.js -> index_js
//==============================================================================
std::optional<juce::WebBrowserComponent::Resource> PluginEditor::getResource (const juce::String& url)
{
    const auto path = url.upToFirstOccurrenceOf ("?", false, false);

    if (path == "/" || path == "/index.html")
        return juce::WebBrowserComponent::Resource { toBytes (BinaryData::index_html, BinaryData::index_htmlSize),
                                                     juce::String ("text/html") };

    if (path == "/js/juce/index.js")
        return juce::WebBrowserComponent::Resource { toBytes (BinaryData::index_js, BinaryData::index_jsSize),
                                                     juce::String ("text/javascript") };

    if (path == "/js/juce/check_native_interop.js")      // REQUIRED (pattern #13)
        return juce::WebBrowserComponent::Resource { toBytes (BinaryData::check_native_interop_js, BinaryData::check_native_interop_jsSize),
                                                     juce::String ("text/javascript") };

    juce::Logger::writeToLog (juce::String (warpr::identity::resourceLogPrefix) + ": resource not found: " + url);
    return std::nullopt;
}
