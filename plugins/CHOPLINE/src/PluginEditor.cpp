// CHOPLINE — PluginEditor implementation. See header.

#include "PluginEditor.h"

namespace chopline
{

using LnF = ui::ChoplineLookAndFeel;

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

void ChoplineAudioProcessorEditor::QuietPanel::paint (juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat();
    g.setColour (LnF::paper);
    g.fillRoundedRectangle (b, 16.0f);
    g.setColour (LnF::hairline());
    g.drawRoundedRectangle (b.reduced (0.5f), 16.0f, 1.0f);
    g.setColour (LnF::textTertiary);
    g.setFont (LnF::labelFont (8.5f));
    g.drawText (title, getLocalBounds(), juce::Justification::centred, false);
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

            setSize (246, 132);
        }

        void resized() override
        {
            auto b = getLocalBounds();
            title.setBounds (b.removeFromTop (14));
            b.removeFromTop (6);
            baseNote.setBounds (b.removeFromTop (28));
            b.removeFromTop (8);
            embed.setBounds (b.removeFromTop (24));
            b.removeFromTop (8);
            auto row = b.removeFromTop (20);
            bpmCaption.setBounds (row.removeFromLeft (130));
            bpmValue.setBounds (row);
        }

    private:
        engine::Engine& engine;
        juce::Label title;
        juce::ComboBox baseNote;
        std::unique_ptr<juce::ComboBoxParameterAttachment> baseNoteAttachment;
        juce::ToggleButton embed;
        juce::Label bpmCaption, bpmValue;
    };
} // namespace

// ---- editor ----------------------------------------------------------------------------

ChoplineAudioProcessorEditor::ChoplineAudioProcessorEditor (ChoplineAudioProcessor& p)
    : AudioProcessorEditor (p),
      processorRef (p),
      midiDot (p.getEngine()),
      samplerStrip (p.getEngine()),
      gridStrip (p.getEngine(), p.getState()),
      fxPage (p.getEngine()),
      chopOverlay (p.getEngine()),
      vblank (this, [this]
      {
          samplerStrip.animationTick();
          gridStrip.animationTick();
          chopOverlay.animationTick();
          midiDot.tick();
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

    presetPrev.setEnabled (false); // preset browser lands with the preset system (Phase 5)
    presetNext.setEnabled (false);
    presetBox.setTextWhenNothingSelected ("Init");
    presetBox.setEnabled (false);
    canvas.addAndMakeVisible (presetPrev);
    canvas.addAndMakeVisible (presetBox);
    canvas.addAndMakeVisible (presetNext);

    canvas.addAndMakeVisible (midiDot);

    pageSeg.getPage = [this] { return page; };
    pageSeg.setPage = [this] (int newPage) { setPage (newPage); };
    canvas.addAndMakeVisible (pageSeg);

    gearButton.onClick = [this] { openSettingsFlyout(); };
    canvas.addAndMakeVisible (gearButton);

    canvas.addAndMakeVisible (samplerStrip);
    canvas.addAndMakeVisible (gridStrip);
    canvas.addAndMakeVisible (beatRepeatPlaceholder);
    canvas.addAndMakeVisible (masterPlaceholder);

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
                int chop = 0;
                for (auto cell : cellsToFill)
                {
                    if (cell >= steps)
                        break;
                    model.addEvent (slot, step * cell,
                                    cell == 4 ? step * 2.0 : step, // one spanned event
                                    chop % juce::jmax (1, engine.getNumChops()));
                    ++chop;
                }
                safe->setSelectedChop (1);
            });
        }

        // Dev-only: CHOPLINE_UI_VIEW=overlay|fx switches the view before the
        // snapshot fires (Phase 3 screenshots).
        const auto uiView = juce::SystemStats::getEnvironmentVariable ("CHOPLINE_UI_VIEW", {});
        if (uiView == "overlay" || uiView == "fx")
        {
            juce::Timer::callAfterDelay (5500,
                [safe = juce::Component::SafePointer<ChoplineAudioProcessorEditor> (this), uiView]
            {
                if (safe == nullptr)
                    return;
                if (uiView == "fx")
                    safe->setPage (1);
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
    setLookAndFeel (nullptr);
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
    beatRepeatPlaceholder.setVisible (mainVisible);
    masterPlaceholder.setVisible (mainVisible);
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
    juce::CallOutBox::launchAsynchronously (std::move (content),
                                            gearButton.getScreenBounds(), nullptr);
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

    samplerStrip.setBounds (15, 60, 1070, 138);
    gridStrip.setBounds (15, 206, 1070, 379);
    beatRepeatPlaceholder.setBounds (15, 593, 500, 112);
    masterPlaceholder.setBounds (523, 593, 562, 112);

    fxPage.setBounds (0, 0, designWidth, designHeight);     // children use canvas coords
    chopOverlay.setBounds (0, 0, designWidth, designHeight); // scrim covers everything
}

} // namespace chopline
