// CHOPLINE — PluginEditor implementation. See header.

#include "PluginEditor.h"

#include "ui/SegControl.h"

namespace chopline
{

using LnF = ui::ChoplineLookAndFeel;

// ---- Phase 5 drag-out backend (spec §10) ---------------------------------------------

PatternDragExporter::PatternDragExporter (ChoplineAudioProcessor& p)
    : juce::Thread ("CHOPLINE Render"),
      processor (p),
      tempDir (juce::File::getSpecialLocation (juce::File::tempDirectory)
                   .getChildFile ("CHOPLINE")
                   .getChildFile (juce::Uuid().toString()))
{
}

PatternDragExporter::~PatternDragExporter()
{
    stopThread (4000);
    tempDir.deleteRecursively(); // temp files cleaned up on editor destruction
}

juce::File PatternDragExporter::writeMidiFile()
{
    auto& engine = processor.getEngine();
    auto& model = engine.getPatternModel();
    const auto slot = model.getActiveSlot();

    engine::Engine::BlockContext ctx;
    processor.fillContextFromParams (ctx);

    const auto midi = state::buildPatternMidi (model, slot, ctx.midiBaseNote,
                                               engine.getUiBpm());
    if (! tempDir.createDirectory())
        return {};
    const auto file = tempDir.getChildFile (
        "CHOPLINE Pattern " + juce::String::charToString (
            static_cast<juce::juce_wchar> ('A' + slot)) + ".mid");
    return state::writeMidiFile (midi, file) ? file : juce::File();
}

void PatternDragExporter::startRender()
{
    if (isThreadRunning())
        return; // already rendering (the progress hairline shows)

    ready = false;
    progress = 0.0f;

    auto& engine = processor.getEngine();
    capturedState = processor.buildStateTree();
    capturedSpec = {};
    capturedSpec.sampleRate = engine.getSampleRate() > 0.0 ? engine.getSampleRate() : 48000.0;
    capturedSpec.bpm = engine.getUiBpm();
    capturedSpec.slot = engine.getPatternModel().getActiveSlot();
    processor.fillContextFromParams (capturedSpec.context);
    capturedSpec.onProgress = [this] (float f)
    {
        progress.store (f, std::memory_order_relaxed);
    };

    wavFile = tempDir.getChildFile (
        "CHOPLINE Pattern " + juce::String::charToString (
            static_cast<juce::juce_wchar> ('A' + capturedSpec.slot)) + ".wav");

    startThread();
}

juce::File PatternDragExporter::getRenderedFile() const
{
    return ready.load (std::memory_order_acquire) ? wavFile : juce::File();
}

float PatternDragExporter::getProgress() const
{
    return isThreadRunning() ? progress.load (std::memory_order_relaxed) : -1.0f;
}

void PatternDragExporter::run()
{
    juce::String error;
    const auto rendered = state::renderPatternOffline (capturedState, capturedSpec, error);
    auto ok = error.isEmpty() && rendered.getNumSamples() > 0 && tempDir.createDirectory();
    if (ok)
        ok = state::writeWavFile (rendered, capturedSpec.sampleRate, wavFile);
    progress.store (-1.0f, std::memory_order_relaxed);
    ready.store (ok, std::memory_order_release);
}

// ---- header widgets ---------------------------------------------------------------

void ChoplineAudioProcessorEditor::MidiActivityDot::tick()
{
    const auto count = engine.getMidiFlashCount();
    const auto now = juce::Time::getMillisecondCounter();
    if (count != lastCount)
    {
        lastCount = count;
        flashStartMs = now;
    }
    const auto shouldBeLit = now - flashStartMs < 220 && flashStartMs != 0;
    if (shouldBeLit != lit)
    {
        lit = shouldBeLit;
        repaint();
    }
}

void ChoplineAudioProcessorEditor::MidiActivityDot::paint (juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat();
    const auto dot = juce::Rectangle<float> (7.0f, 7.0f)
                         .withCentre ({ 6.0f, b.getCentreY() });
    if (lit)
    {
        g.setColour (LnF::accent.withAlpha (0.25f)); // soft glow
        g.fillEllipse (dot.expanded (3.0f));
    }
    g.setColour (lit ? LnF::accent : juce::Colour (0xffc7c7cc));
    g.fillEllipse (dot);

    g.setColour (LnF::textTertiary);
    g.setFont (LnF::monoFont (9.0f));
    g.drawText ("MIDI", getLocalBounds().withTrimmedLeft (16),
                juce::Justification::centredLeft, false);
}

void ChoplineAudioProcessorEditor::PageSeg::paint (juce::Graphics& g)
{
    auto b = getLocalBounds().toFloat().reduced (0.5f);
    g.setColour (LnF::cell);
    g.fillRoundedRectangle (b, 7.0f);
    g.setColour (LnF::hairline());
    g.drawRoundedRectangle (b, 7.0f, 1.0f);

    const auto current = getPage != nullptr ? getPage() : 0;
    const char* names[] = { "MAIN", "FX" };
    const auto segW = b.getWidth() * 0.5f;
    for (int i = 0; i < 2; ++i)
    {
        auto seg = juce::Rectangle<float> (b.getX() + segW * static_cast<float> (i),
                                           b.getY(), segW, b.getHeight()).reduced (2.0f);
        if (i == current)
        {
            g.setColour (LnF::ink);
            g.fillRoundedRectangle (seg, 5.0f);
        }
        g.setFont (LnF::labelFont (8.0f));
        g.setColour (i == current ? juce::Colours::white : LnF::label);
        g.drawText (names[i], seg.toNearestInt(), juce::Justification::centred, false);
    }
}

void ChoplineAudioProcessorEditor::PageSeg::mouseDown (const juce::MouseEvent& e)
{
    if (setPage != nullptr && getWidth() > 0)
        setPage (e.position.x < static_cast<float> (getWidth()) * 0.5f ? 0 : 1);
}

void ChoplineAudioProcessorEditor::GearButton::paintButton (juce::Graphics& g,
                                                            bool highlighted, bool down)
{
    auto b = getLocalBounds().toFloat().reduced (0.5f);
    g.setColour (down ? juce::Colour (0xffededf0)
                      : highlighted ? juce::Colour (0xfff2f2f4) : LnF::cell);
    g.fillRoundedRectangle (b, 7.0f);
    g.setColour (LnF::hairline());
    g.drawRoundedRectangle (b, 7.0f, 1.0f);

    // Minimal gear glyph: ring + 6 teeth + hub.
    const auto c = b.getCentre();
    const auto r = 5.0f;
    g.setColour (LnF::label);
    g.drawEllipse (juce::Rectangle<float> (r * 2.0f, r * 2.0f).withCentre (c), 1.4f);
    for (int i = 0; i < 6; ++i)
    {
        const auto a = static_cast<float> (i) * juce::MathConstants<float>::twoPi / 6.0f;
        const juce::Point<float> p1 (c.x + (r + 0.5f) * std::sin (a), c.y - (r + 0.5f) * std::cos (a));
        const juce::Point<float> p2 (c.x + (r + 2.5f) * std::sin (a), c.y - (r + 2.5f) * std::cos (a));
        g.drawLine ({ p1, p2 }, 1.4f);
    }
    g.fillEllipse (juce::Rectangle<float> (2.4f, 2.4f).withCentre (c));
}

void ChoplineAudioProcessorEditor::HoldBadge::paint (juce::Graphics& g)
{
    // White pill, 1 px red outline, "REPEAT HOLD" (v17 beat_repeat_hold).
    const auto b = getLocalBounds().toFloat().reduced (0.5f);
    g.setColour (juce::Colours::white);
    g.fillRoundedRectangle (b, b.getHeight() * 0.5f);
    g.setColour (LnF::repeat);
    g.drawRoundedRectangle (b, b.getHeight() * 0.5f, 1.0f);
    g.setFont (LnF::labelFont (8.0f));
    g.drawText ("REPEAT HOLD", getLocalBounds(), juce::Justification::centred, false);
}

// ---- settings flyout (gear): MIDI base note + embed-sample toggle --------------------

namespace
{
    class SettingsFlyout final : public juce::Component
    {
    public:
        SettingsFlyout (juce::AudioProcessorValueTreeState& apvts, engine::Engine& eng)
            : engine (eng)
        {
            title.setText ("SETTINGS", juce::dontSendNotification);
            title.setFont (LnF::labelFont (8.5f));
            title.setColour (juce::Label::textColourId, LnF::label);
            addAndMakeVisible (title);

            // Phase 5 (spec §8 / v17 fl_set): Eco/HQ quality seg. The actual
            // stretcher reconfiguration happens on the processor's timer
            // (suspendProcessing wraps it).
            qualityParam = apvts.getParameter (params::id::quality);
            qualitySeg.options = { "Eco", "HQ" };
            qualitySeg.getValue = [this]
            {
                return qualityParam != nullptr
                    ? static_cast<int> (std::lround (
                          qualityParam->convertFrom0to1 (qualityParam->getValue())))
                    : 1;
            };
            qualitySeg.setValue = [this] (int v)
            {
                if (qualityParam != nullptr)
                    qualityParam->setValueNotifyingHost (
                        qualityParam->convertTo0to1 (static_cast<float> (v)));
            };
            qualitySeg.setTooltip ("Eco: cheaper time-stretch, capped granular density, "
                                   "less oversampling. HQ: full quality.");
            addAndMakeVisible (qualitySeg);

            baseNote.setName ("Base note");
            baseNote.addItemList ({ "C0", "C1", "C2", "C3" }, 1);
            addAndMakeVisible (baseNote);
            if (auto* param = dynamic_cast<juce::RangedAudioParameter*> (
                    apvts.getParameter (params::id::midiBaseNote)))
                baseNoteAttachment = std::make_unique<juce::ComboBoxParameterAttachment> (*param, baseNote);

            embed.setButtonText ("Embed sample in preset");
            embed.setToggleState (engine.getEmbedAudioInState(), juce::dontSendNotification);
            embed.onClick = [this] { engine.setEmbedAudioInState (embed.getToggleState()); };
            addAndMakeVisible (embed);

            // Standalone free-run BPM (spec §8: no host transport → free-run
            // at a manual BPM; v17 moves the BPM readout to the Phase 4
            // MASTER bar — until then it lives here).
            bpmCaption.setText ("FREE-RUN BPM", juce::dontSendNotification);
            bpmCaption.setFont (LnF::labelFont (8.5f));
            bpmCaption.setColour (juce::Label::textColourId, LnF::label);
            addAndMakeVisible (bpmCaption);

            bpmValue.setEditable (true);
            bpmValue.setFont (LnF::monoFont (10.0f));
            bpmValue.setColour (juce::Label::textColourId, LnF::ink);
            bpmValue.setJustificationType (juce::Justification::centredRight);
            bpmValue.setText (juce::String (engine.getPatternModel().getFreeBpm(), 1),
                              juce::dontSendNotification);
            bpmValue.onTextChange = [this]
            {
                const auto v = bpmValue.getText().getDoubleValue();
                if (v > 1.0)
                    engine.getPatternModel().setFreeBpm (v);
                bpmValue.setText (juce::String (engine.getPatternModel().getFreeBpm(), 1),
                                  juce::dontSendNotification);
            };
            addAndMakeVisible (bpmValue);

            // Phase 5 (spec §10): export all chops as individual WAVs.
            exportChops.setButtonText (juce::String (juce::CharPointer_UTF8 (
                "Export chops\xe2\x80\xa6")));
            exportChops.setTooltip ("Writes every chop's trimmed region as a 24-bit WAV");
            exportChops.onClick = [&engRef = engine]
            {
                auto chooser = std::make_shared<juce::FileChooser> (
                    "Export chops as WAVs",
                    juce::File::getSpecialLocation (juce::File::userMusicDirectory));
                chooser->launchAsync (juce::FileBrowserComponent::openMode
                                          | juce::FileBrowserComponent::canSelectDirectories,
                                      [chooser, &engRef] (const juce::FileChooser& fc)
                                      {
                                          const auto dir = fc.getResult();
                                          if (dir == juce::File())
                                              return;
                                          juce::String error;
                                          state::exportChopWavs (engRef, dir, error);
                                      });
            };
            addAndMakeVisible (exportChops);

            setSize (246, 208);
        }

        void resized() override
        {
            auto b = getLocalBounds();
            title.setBounds (b.removeFromTop (14));
            b.removeFromTop (6);
            qualitySeg.setBounds (b.removeFromTop (28));
            b.removeFromTop (8);
            baseNote.setBounds (b.removeFromTop (28));
            b.removeFromTop (8);
            embed.setBounds (b.removeFromTop (24));
            b.removeFromTop (8);
            auto row = b.removeFromTop (20);
            bpmCaption.setBounds (row.removeFromLeft (130));
            bpmValue.setBounds (row);
            b.removeFromTop (8);
            exportChops.setBounds (b.removeFromTop (26));
        }

    private:
        engine::Engine& engine;
        juce::Label title;
        ui::SegControl qualitySeg;
        juce::RangedAudioParameter* qualityParam = nullptr;
        juce::ComboBox baseNote;
        std::unique_ptr<juce::ComboBoxParameterAttachment> baseNoteAttachment;
        juce::ToggleButton embed;
        juce::Label bpmCaption, bpmValue;
        juce::TextButton exportChops;
    };

    // ---- Phase 5 save-preset dialog (spec §10; v17-style minimal) ---------------------
    class SavePresetDialog final : public juce::Component
    {
    public:
        std::function<juce::String (const juce::String& name, const juce::String& category,
                                    bool kitOnly)> onSave; // returns an error or {}

        SavePresetDialog()
        {
            title.setText ("SAVE PRESET", juce::dontSendNotification);
            title.setFont (LnF::labelFont (8.5f));
            title.setColour (juce::Label::textColourId, LnF::label);
            addAndMakeVisible (title);

            name.setFont (LnF::monoFont (11.0f));
            name.setTextToShowWhenEmpty ("Preset name", LnF::textTertiary);
            name.setColour (juce::TextEditor::backgroundColourId, juce::Colours::white);
            name.setColour (juce::TextEditor::outlineColourId, LnF::hairline());
            name.setColour (juce::TextEditor::focusedOutlineColourId, LnF::accent);
            name.setColour (juce::TextEditor::textColourId, LnF::ink);
            name.setColour (juce::TextEditor::highlightedTextColourId, LnF::ink);
            name.setColour (juce::CaretComponent::caretColourId, LnF::ink);
            addAndMakeVisible (name);

            category.addItemList (state::PresetManager::factoryCategories(), 1);
            category.setSelectedId (6, juce::dontSendNotification); // User
            addAndMakeVisible (category);

            full.setButtonText ("Include sample (full preset)");
            full.setToggleState (true, juce::dontSendNotification);
            full.setTooltip ("Off = kit only: patterns + chops + settings, no sample");
            addAndMakeVisible (full);

            saveButton.setButtonText ("Save");
            saveButton.onClick = [this]
            {
                if (onSave == nullptr)
                    return;
                const auto error = onSave (name.getText(), category.getText(),
                                           ! full.getToggleState());
                if (error.isEmpty())
                {
                    if (auto* box = findParentComponentOfClass<juce::CallOutBox>())
                        box->dismiss();
                }
                else
                {
                    info.setText (error, juce::dontSendNotification);
                }
            };
            addAndMakeVisible (saveButton);

            info.setFont (LnF::labelFont (8.0f));
            info.setColour (juce::Label::textColourId, LnF::repeat);
            addAndMakeVisible (info);

            setSize (250, 162);
        }

        void resized() override
        {
            auto b = getLocalBounds();
            title.setBounds (b.removeFromTop (14));
            b.removeFromTop (6);
            name.setBounds (b.removeFromTop (26));
            b.removeFromTop (8);
            category.setBounds (b.removeFromTop (28));
            b.removeFromTop (8);
            full.setBounds (b.removeFromTop (24));
            b.removeFromTop (8);
            auto row = b.removeFromTop (26);
            saveButton.setBounds (row.removeFromRight (74));
            info.setBounds (row.reduced (0, 4));
        }

        void visibilityChanged() override
        {
            if (isVisible())
                name.grabKeyboardFocus();
        }

    private:
        juce::Label title, info;
        juce::TextEditor name;
        juce::ComboBox category;
        juce::ToggleButton full;
        juce::TextButton saveButton;
    };
} // namespace

// ---- editor ----------------------------------------------------------------------------

ChoplineAudioProcessorEditor::ChoplineAudioProcessorEditor (ChoplineAudioProcessor& p)
    : AudioProcessorEditor (p),
      processorRef (p),
      midiDot (p.getEngine()),
      samplerStrip (p.getEngine()),
      gridStrip (p.getEngine(), p.getState()),
      beatRepeatPanel (p.getState(), p.getEngine()),
      masterPanel (p.getState(), p.getEngine()),
      fxPage (p.getEngine(), p.getState()),
      chopOverlay (p.getEngine()),
      dragExporter (p),
      vblank (this, [this]
      {
          samplerStrip.animationTick();
          gridStrip.animationTick();
          chopOverlay.animationTick();
          beatRepeatPanel.animationTick();
          masterPanel.animationTick();
          fxPage.animationTick();
          midiDot.tick();

          // REPEAT HOLD badge (both pages) + red accents follow the engine.
          const auto holding = processorRef.getEngine().isBeatRepeatActive();
          if (holding != holdBadge.isVisible())
              holdBadge.setVisible (holding);

          if (--gcCountdown <= 0)
          {
              gcCountdown = 30; // ~0.5 s: free retired snapshots
              processorRef.getEngine().collectGarbage();
          }
      })
{
    setLookAndFeel (&lookAndFeel);

    canvas.setSize (designWidth, designHeight);
    addAndMakeVisible (canvas);

    // Header (v17): wordmark | presets | MIDI dot | MAIN|FX | gear.
    wordmark.setText ("chopline", juce::dontSendNotification);
    wordmark.setFont (juce::Font (juce::FontOptions (17.0f).withStyle ("Bold")));
    wordmark.setColour (juce::Label::textColourId, LnF::ink);
    canvas.addAndMakeVisible (wordmark);

    // Phase 5 preset browser (spec §10; v17 header prev/combo/next).
    presetPrev.setTooltip ("Previous preset");
    presetNext.setTooltip ("Next preset");
    presetPrev.onClick = [this] { stepPreset (-1); };
    presetNext.onClick = [this] { stepPreset (1); };
    presetBox.setTextWhenNothingSelected ("Init");
    presetBox.setTooltip ("Presets");
    presetBox.onChange = [this]
    {
        const auto id = presetBox.getSelectedId();
        if (id == 0)
            return;
        if (id == 100000)
        {
            // "Save preset…": restore the display, then open the dialog.
            presetBox.setSelectedId (currentPresetIndex >= 0 ? currentPresetIndex + 1 : 0,
                                     juce::dontSendNotification);
            openSavePresetDialog();
            return;
        }
        loadPresetAtIndex (id - 1);
    };
    canvas.addAndMakeVisible (presetPrev);
    canvas.addAndMakeVisible (presetBox);
    canvas.addAndMakeVisible (presetNext);
    refreshPresetBox();

    canvas.addAndMakeVisible (midiDot);
    canvas.addChildComponent (holdBadge); // visible only while BR holds

    pageSeg.getPage = [this] { return page; };
    pageSeg.setPage = [this] (int newPage) { setPage (newPage); };
    canvas.addAndMakeVisible (pageSeg);

    gearButton.onClick = [this] { openSettingsFlyout(); };
    canvas.addAndMakeVisible (gearButton);

    canvas.addAndMakeVisible (samplerStrip);
    canvas.addAndMakeVisible (gridStrip);
    canvas.addAndMakeVisible (beatRepeatPanel);
    canvas.addAndMakeVisible (masterPanel);

    canvas.addChildComponent (fxPage); // FX page starts hidden (page 0 = MAIN)
    canvas.addChildComponent (chopOverlay); // topmost

    // Shared chop selection: grid paints Strip A's selection; double-clicking
    // a filled box selects that chop back in Strip A (spec §5) and opens the
    // chop editor (v17 chop_editing_without_panel).
    gridStrip.getSelectedChops = [this] { return samplerStrip.getWaveform().getSelectedChops(); };
    gridStrip.onSelectChop = [this] (int chop) { setSelectedChop (chop); };
    gridStrip.getEditChop = [this] { return selectedChop(); };
    gridStrip.onOpenChopEditor = [this] { openChopEditor (selectedChop()); };

    samplerStrip.getWaveform().onSelectionChanged = [this] { selectionChanged(); };
    samplerStrip.getWaveform().onOpenChopEditor = [this] { openChopEditor (selectedChop()); };

    // Phase 5 drag-out (spec §10): MIDI/WAV export from the grid header zone.
    gridStrip.createMidiDragFile = [this] { return dragExporter.writeMidiFile(); };
    gridStrip.startAudioRender = [this] { dragExporter.startRender(); };
    gridStrip.getRenderedAudioFile = [this] { return dragExporter.getRenderedFile(); };
    gridStrip.getAudioRenderProgress = [this] { return dragExporter.getProgress(); };

    fxPage.getChop = [this] { return selectedChop(); };
    fxPage.setChop = [this] (int chop) { setSelectedChop (chop); };

    chopOverlay.onClose = [this] { closeChopEditor(); };

    setWantsKeyboardFocus (true); // page keys + Cmd/Ctrl-Z fallback

    // Resizable 75%–200% of 1100x720, fixed aspect ratio (spec §9).
    setResizable (true, true);
    setResizeLimits (designWidth * 3 / 4, designHeight * 3 / 4,
                     designWidth * 2, designHeight * 2);
    if (auto* constrainer = getConstrainer())
        constrainer->setFixedAspectRatio (static_cast<double> (designWidth) / designHeight);

    setSize (designWidth, designHeight);

    // Dev-only (manual smoke testing): CHOPLINE_UI_SNAPSHOT=<path.png> renders
    // the editor to a PNG a few seconds after it opens (Standalone only; scoped
    // to this component — never a screen capture).
    if (processorRef.wrapperType == juce::AudioProcessor::wrapperType_Standalone)
    {
        // Dev-only: CHOPLINE_DEMO_PATTERN=1 paints the first chops onto the
        // grid ~5 s after open (after the autoload settles) so UI snapshots
        // show a populated pattern. No effect in hosts, never set in CI.
        if (juce::SystemStats::getEnvironmentVariable ("CHOPLINE_DEMO_PATTERN", {}) == "1")
        {
            juce::Timer::callAfterDelay (5000,
                [safe = juce::Component::SafePointer<ChoplineAudioProcessorEditor> (this)]
            {
                if (safe == nullptr)
                    return;
                auto& engine = safe->processorRef.getEngine();
                auto& model = engine.getPatternModel();
                const auto slot = model.getActiveSlot();
                if (model.getNumEvents (slot) > 0 || engine.getNumChops() <= 0)
                    return;
                const auto step = engine::noteLengthBars (model.getNoteLength (slot));
                const auto steps = engine::noteLengthStepsPerBar (model.getNoteLength (slot));
                model.beginTransaction ("Demo pattern");
                const int cellsToFill[] = { 0, 2, 4, 6, 7, 10, 12, 14 };
                const int demoVels[] = { 127, 84, 110, 64, 96, 120, 72, 104 };
                int chop = 0;
                for (auto cell : cellsToFill)
                {
                    if (cell >= steps)
                        break;
                    const auto id = model.addEvent (slot, step * cell,
                                    cell == 4 ? step * 2.0 : step, // one spanned event
                                    chop % juce::jmax (1, engine.getNumChops()),
                                    demoVels[chop % 8]); // varied velocity lane
                    if (chop == 1)
                        model.setEventPitch (slot, id, 7.0f);
                    if (chop == 3)
                        model.setEventRatchet (slot, id, 4);
                    if (chop == 5)
                        model.setEventProbability (slot, id, 60);
                    ++chop;
                }
                safe->setSelectedChop (1);
            });
        }

        // Dev-only: CHOPLINE_UI_VIEW=overlay|fx switches the view before the
        // snapshot fires (Phase 3 screenshots).
        const auto uiView = juce::SystemStats::getEnvironmentVariable ("CHOPLINE_UI_VIEW", {});
        if (uiView == "overlay" || uiView == "fx" || uiView == "presets")
        {
            juce::Timer::callAfterDelay (5500,
                [safe = juce::Component::SafePointer<ChoplineAudioProcessorEditor> (this), uiView]
            {
                if (safe == nullptr)
                    return;
                if (uiView == "fx")
                    safe->setPage (1);
                else if (uiView == "presets")
                    safe->openSavePresetDialog();
                else
                    safe->openChopEditor (safe->selectedChop());
            });
        }

        const auto snapshotPath =
            juce::SystemStats::getEnvironmentVariable ("CHOPLINE_UI_SNAPSHOT", {});
        if (snapshotPath.isNotEmpty() && juce::File::isAbsolutePath (snapshotPath))
        {
            juce::Timer::callAfterDelay (6000,
                [safe = juce::Component::SafePointer<ChoplineAudioProcessorEditor> (this),
                 snapshotPath]
            {
                if (safe == nullptr)
                    return;
                const auto image = safe->createComponentSnapshot (safe->getLocalBounds(), true, 2.0f);
                juce::File file (snapshotPath);
                file.deleteFile();
                if (auto stream = file.createOutputStream())
                {
                    juce::PNGImageFormat png;
                    png.writeImageToStream (image, *stream);
                }
            });
        }
    }
}

ChoplineAudioProcessorEditor::~ChoplineAudioProcessorEditor()
{
    // Never leave a momentary H hold / Space preview stuck on when the window
    // closes.
    if (momentaryHold)
        processorRef.getEngine().setMomentaryHold (false);
    if (previewHeld)
        processorRef.getEngine().setPreviewHeld (false);
    setLookAndFeel (nullptr);
}

void ChoplineAudioProcessorEditor::focusLost (FocusChangeType)
{
    // The key-up would be delivered elsewhere — release the momentary holds.
    if (momentaryHold)
    {
        momentaryHold = false;
        processorRef.getEngine().setMomentaryHold (false);
    }
    if (previewHeld)
    {
        previewHeld = false;
        processorRef.getEngine().setPreviewHeld (false);
    }
}

// ---- pages / selection / overlay ----------------------------------------------------

void ChoplineAudioProcessorEditor::setPage (int newPage)
{
    newPage = juce::jlimit (0, 1, newPage);
    if (newPage == page && chopOverlay.isVisible() == false)
    {
        pageSeg.repaint();
        return;
    }
    page = newPage;
    chopOverlay.setVisible (false); // switching closes the overlay (v17 persistence)

    const bool mainVisible = page == 0;
    samplerStrip.setVisible (mainVisible);
    gridStrip.setVisible (mainVisible);
    beatRepeatPanel.setVisible (mainVisible);
    masterPanel.setVisible (mainVisible);
    fxPage.setVisible (! mainVisible);
    if (! mainVisible)
        fxPage.refresh();
    pageSeg.repaint();
}

int ChoplineAudioProcessorEditor::selectedChop()
{
    const auto chops = samplerStrip.getWaveform().getSelectedChops();
    return chops.empty() ? 0 : chops.front();
}

void ChoplineAudioProcessorEditor::setSelectedChop (int chop)
{
    const auto numChops = processorRef.getEngine().getNumChops();
    if (numChops <= 0)
        return;
    samplerStrip.getWaveform().selectChop (juce::jlimit (0, numChops - 1, chop));
    // selectChop fires onSelectionChanged → selectionChanged().
}

void ChoplineAudioProcessorEditor::selectionChanged()
{
    gridStrip.repaint();
    fxPage.refresh();
    if (chopOverlay.isVisible())
        chopOverlay.setChop (selectedChop());
}

void ChoplineAudioProcessorEditor::openChopEditor (int chop)
{
    if (processorRef.getEngine().getNumChops() <= 0)
        return;
    setSelectedChop (chop);
    chopOverlay.setChop (selectedChop());
    chopOverlay.toFront (false);
    chopOverlay.setVisible (true);
}

void ChoplineAudioProcessorEditor::closeChopEditor()
{
    chopOverlay.setVisible (false);
    grabKeyboardFocus();
}

void ChoplineAudioProcessorEditor::openSettingsFlyout()
{
    auto content = std::make_unique<SettingsFlyout> (processorRef.getState(),
                                                     processorRef.getEngine());
    // Parented to the editor (not the desktop): the v17 flyouts sit inside
    // the plate, and component snapshots capture them.
    juce::CallOutBox::launchAsynchronously (std::move (content),
                                            getLocalArea (nullptr, gearButton.getScreenBounds()), this);
}

// ---- presets (Phase 5, spec §10) ------------------------------------------------------

void ChoplineAudioProcessorEditor::refreshPresetBox()
{
    presetBox.clear (juce::dontSendNotification);

    juce::String lastCategory;
    const auto& presets = presetManager.getPresets();
    for (size_t i = 0; i < presets.size(); ++i)
    {
        const auto& p = presets[i];
        if (p.category != lastCategory)
        {
            presetBox.addSectionHeading (p.category.toUpperCase());
            lastCategory = p.category;
        }
        presetBox.addItem (p.name, static_cast<int> (i) + 1);
    }

    if (! presets.empty())
        presetBox.addSeparator();
    presetBox.addItem (juce::String (juce::CharPointer_UTF8 ("Save preset\xe2\x80\xa6")),
                       100000);

    if (currentPresetIndex >= 0 && currentPresetIndex < static_cast<int> (presets.size()))
        presetBox.setSelectedId (currentPresetIndex + 1, juce::dontSendNotification);
}

void ChoplineAudioProcessorEditor::loadPresetAtIndex (int index)
{
    const auto& presets = presetManager.getPresets();
    if (index < 0 || index >= static_cast<int> (presets.size()))
        return;

    const auto tree = presetManager.load (presets[static_cast<size_t> (index)].file);
    if (! tree.isValid() || tree.getType() != processorRef.getState().state.getType())
    {
        // Unreadable / foreign file: leave the state alone AND roll the combo
        // display back to the preset that is actually loaded.
        presetBox.setSelectedId (currentPresetIndex >= 0 ? currentPresetIndex + 1 : 0,
                                 juce::dontSendNotification);
        return;
    }

    // Same glitch-safe path as a session restore (spec §10 discipline).
    processorRef.applyStateTree (tree);
    currentPresetIndex = index;
    presetBox.setSelectedId (index + 1, juce::dontSendNotification);
}

void ChoplineAudioProcessorEditor::stepPreset (int delta)
{
    const auto n = static_cast<int> (presetManager.getPresets().size());
    if (n <= 0)
        return;
    const auto base = currentPresetIndex >= 0 ? currentPresetIndex : (delta > 0 ? -1 : 0);
    loadPresetAtIndex (((base + delta) % n + n) % n);
}

void ChoplineAudioProcessorEditor::openSavePresetDialog()
{
    auto content = std::make_unique<SavePresetDialog>();
    content->onSave = [this] (const juce::String& name, const juce::String& category,
                              bool kitOnly) -> juce::String
    {
        juce::String error;
        const auto file = presetManager.save (processorRef.buildStateTree(), name,
                                              category, kitOnly, error);
        if (error.isNotEmpty())
            return error;
        currentPresetIndex = presetManager.indexOfFile (file);
        refreshPresetBox();
        return {};
    };
    juce::CallOutBox::launchAsynchronously (std::move (content),
                                            getLocalArea (nullptr, presetBox.getScreenBounds()), this);
}

bool ChoplineAudioProcessorEditor::keyStateChanged (bool)
{
    bool handled = false;

    // H = MOMENTARY Beat Repeat hold on either page (v17 hold_pad / keyboard.h;
    // the HOLD button stays the latching toggle).
    const auto down = juce::KeyPress::isKeyCurrentlyDown ('h')
                   || juce::KeyPress::isKeyCurrentlyDown ('H');
    if (down != momentaryHold)
    {
        momentaryHold = down;
        processorRef.getEngine().setMomentaryHold (down);
        handled = true;
    }

    // Space = MOMENTARY transport preview (v17 keyboard.space, Phase 5),
    // STANDALONE ONLY: in hosts Space must stay the DAW's play shortcut, so
    // the plugin neither consumes nor acts on it there.
    if (processorRef.wrapperType == juce::AudioProcessor::wrapperType_Standalone)
    {
        const auto space = juce::KeyPress::isKeyCurrentlyDown (juce::KeyPress::spaceKey);
        if (space != previewHeld)
        {
            previewHeld = space;
            processorRef.getEngine().setPreviewHeld (space);
            handled = true;
        }
    }

    return handled;
}

bool ChoplineAudioProcessorEditor::keyPressed (const juce::KeyPress& key)
{
    if (key.getModifiers().isCommandDown() && (key.isKeyCode ('z') || key.isKeyCode ('Z')))
    {
        auto& undo = processorRef.getEngine().getPatternModel().getUndoManager();
        if (key.getModifiers().isShiftDown())
            undo.redo();
        else
            undo.undo();
        return true;
    }

    if (key.getModifiers().isAnyModifierKeyDown())
        return false;

    // v17 keyboard map: Tab toggles the page, M/F jump, 1–8 select the chop,
    // Enter opens the chop editor, Esc closes it.
    if (key.isKeyCode (juce::KeyPress::tabKey))
    {
        setPage (1 - page);
        return true;
    }
    if (key.isKeyCode ('m') || key.isKeyCode ('M'))
    {
        setPage (0);
        return true;
    }
    if (key.isKeyCode ('f') || key.isKeyCode ('F'))
    {
        setPage (1);
        return true;
    }
    if ((key.isKeyCode ('h') || key.isKeyCode ('H'))
        && ! key.getModifiers().isAnyModifierKeyDown())
        return true; // momentary BR hold — handled by keyStateChanged
    if (key.isKeyCode (juce::KeyPress::spaceKey))
        // Momentary transport preview (keyStateChanged) — Standalone only; in
        // hosts the key falls through so the DAW's play shortcut keeps working.
        return processorRef.wrapperType == juce::AudioProcessor::wrapperType_Standalone;
    if (key.getKeyCode() >= '1' && key.getKeyCode() <= '8')
    {
        setSelectedChop (key.getKeyCode() - '1');
        return true;
    }
    if (key.isKeyCode (juce::KeyPress::returnKey))
    {
        openChopEditor (selectedChop());
        return true;
    }
    if (key.isKeyCode (juce::KeyPress::escapeKey) && chopOverlay.isVisible())
    {
        closeChopEditor();
        return true;
    }
    return false;
}

void ChoplineAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (LnF::page);
}

void ChoplineAudioProcessorEditor::resized()
{
    const auto scale = static_cast<float> (getWidth()) / static_cast<float> (designWidth);
    canvas.setTransform (juce::AffineTransform::scale (scale));
    canvas.setTopLeftPosition (0, 0);

    // Fixed v17 design coordinates on the 1100x720 canvas.
    wordmark.setBounds (15, 15, 110, 24);
    presetPrev.setBounds (140, 15, 24, 24);
    presetBox.setBounds (174, 15, 220, 24);
    presetNext.setBounds (404, 15, 24, 24);
    midiDot.setBounds (452, 15, 54, 24);
    pageSeg.setBounds (956, 15, 91, 24);
    gearButton.setBounds (1057, 15, 28, 24);

    holdBadge.setBounds (790, 15, 154, 24);

    samplerStrip.setBounds (15, 60, 1070, 138);
    gridStrip.setBounds (15, 206, 1070, 379);
    beatRepeatPanel.setBounds (15, 593, 500, 112);
    masterPanel.setBounds (523, 593, 562, 112);

    fxPage.setBounds (0, 0, designWidth, designHeight);     // children use canvas coords
    chopOverlay.setBounds (0, 0, designWidth, designHeight); // scrim covers everything
}

} // namespace chopline
