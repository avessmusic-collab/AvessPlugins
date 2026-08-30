#include "PluginProcessor.h"
#include "PluginEditor.h"

#include "Parameters/ParameterIDs.h"

namespace pal = kickr::palette;
using KSize = kickr::KickrKnob::Size;

//==============================================================================
KICKRAudioProcessorEditor::KICKRAudioProcessorEditor (KICKRAudioProcessor& p)
    : AudioProcessorEditor (&p), proc (p)
{
    setLookAndFeel (&lnf);
    tooltip.setMillisecondsBeforeTipAppears (450);

    // ---------------------------------------------------------------- header
    wordmark.setText ("KICKR", juce::dontSendNotification);
    wordmark.setJustificationType (juce::Justification::centredLeft);
    wordmark.setColour (juce::Label::textColourId, pal::inkHi);
    wordmark.setInterceptsMouseClicks (false, false);
    addAndMakeVisible (wordmark);

    versionLabel.setText ("v1.0", juce::dontSendNotification);
    versionLabel.setJustificationType (juce::Justification::centredLeft);
    versionLabel.setColour (juce::Label::textColourId, pal::inkDim);
    versionLabel.setInterceptsMouseClicks (false, false);
    addAndMakeVisible (versionLabel);

    presetNameLabel.setText ("Default", juce::dontSendNotification);
    presetNameLabel.setJustificationType (juce::Justification::centred);
    presetNameLabel.setColour (juce::Label::textColourId, pal::magenta);
    presetNameLabel.setInterceptsMouseClicks (false, false);
    addAndMakeVisible (presetNameLabel);

    for (auto* b : { &undoButton, &redoButton, &saveButton, &randomButton, &mutateButton,
                     &abButton, &presetPrev, &presetNext,
                     &scopeWaveButton, &scopeSpectrumButton, &samplePrev, &sampleNext })
        addAndMakeVisible (b);

    undoButton.onClick = [this] { proc.getUndoManager().undo(); updateUndoRedoState(); };
    redoButton.onClick = [this] { proc.getUndoManager().redo(); updateUndoRedoState(); };

    for (auto* b : { &saveButton, &randomButton, &mutateButton, &abButton, &presetPrev, &presetNext })
    {
        b->setEnabled (false);
        b->setTooltip ("Presets / Randomize / Mutate arrive in Phase 3.3");
    }

    scopeWaveButton.setClickingTogglesState (true);
    scopeSpectrumButton.setClickingTogglesState (true);
    scopeWaveButton.setRadioGroupId (0x5c09e);
    scopeSpectrumButton.setRadioGroupId (0x5c09e);
    scopeWaveButton.setToggleState (true, juce::dontSendNotification);
    scopeWaveButton.setTooltip ("Waveform / spectrum analyzer arrives in Phase 3.2");
    scopeSpectrumButton.setTooltip ("Waveform / spectrum analyzer arrives in Phase 3.2");

    // ---------------------------------------------------------------- sample strip
    sampleNameLabel.setJustificationType (juce::Justification::centredLeft);
    sampleNameLabel.setColour (juce::Label::textColourId, pal::lowfam);
    sampleNameLabel.setInterceptsMouseClicks (false, false);
    addAndMakeVisible (sampleNameLabel);

    sampleDropHint.setText ("DROP AUDIO", juce::dontSendNotification);
    sampleDropHint.setJustificationType (juce::Justification::centredRight);
    sampleDropHint.setColour (juce::Label::textColourId, pal::inkDim);
    sampleDropHint.setInterceptsMouseClicks (false, false);
    addAndMakeVisible (sampleDropHint);
    // // Phase 3.3: full FileDragAndDropTarget + empty-bank / missing-file UI states + waveform

    samplePrev.onClick = [this]
    {
        const auto n = proc.getSampleLibrary().prev();
        if (n.isNotEmpty()) proc.loadSampleByName (n);
        updateSampleLabel();
    };
    sampleNext.onClick = [this]
    {
        const auto n = proc.getSampleLibrary().next();
        if (n.isNotEmpty()) proc.loadSampleByName (n);
        updateSampleLabel();
    };

    auto& synthTog  = addToggle (kickr::id::synthEnable,  "SYNTH",  pal::violet);
    auto& sampleTog = addToggle (kickr::id::sampleEnable, "SAMPLE", pal::red);
    sampleToggles = { &synthTog, &sampleTog };

    sampleTweaks = {
        &addKnob   (kickr::id::sampleLevel,     "LEVEL",  pal::red,     false, KSize::Small),
        &addKnob   (kickr::id::sampleStart,     "START",  pal::lowfam,  false, KSize::Small),
        &addKnob   (kickr::id::sampleEnd,       "END",    pal::lowfam,  false, KSize::Small),
        &addToggle (kickr::id::sampleReverse,   "REV",    pal::lowfam),
        &addKnob   (kickr::id::sampleTune,      "TUNE",   pal::lowfam,  true,  KSize::Small),
        &addKnob   (kickr::id::sampleFine,      "FINE",   pal::lowfam,  true,  KSize::Small),
        &addToggle (kickr::id::sampleMidiTrack, "TRK",    pal::lowfam),
        &addKnob   (kickr::id::sampleAttack,    "ATTACK", pal::magenta, false, KSize::Small),
        &addKnob   (kickr::id::sampleDecay,     "DECAY",  pal::magenta, false, KSize::Small),
        &addKnob   (kickr::id::sampleHP,        "HP",     pal::violet,  false, KSize::Small),
        &addKnob   (kickr::id::sampleLP,        "LP",     pal::violet,  false, KSize::Small),
        &addKnob   (kickr::id::sampleCrush,     "CRUSH",  pal::red,     false, KSize::Small),
    };

    // ---------------------------------------------------------------- macro modules
    heroTitles[0] = &addSectionLabel ("PUNCH", pal::red);
    heroBig[0]    = &addKnob (kickr::id::macroPunch, "MACRO", pal::red, false, KSize::Large);
    heroMacro[0]  = nullptr;   // PUNCH's macro IS its hero knob
    punchFooter = {
        &addKnob (kickr::id::transientAttack,  "T ATK", pal::red, true, KSize::Small),
        &addKnob (kickr::id::transientSustain, "T SUS", pal::red, true, KSize::Small),
    };

    heroTitles[1] = &addSectionLabel ("BODY", pal::lowfam);
    heroBig[1]    = &addKnob (kickr::id::fundamental, "NOTE",  pal::lowfam, false, KSize::Large);
    heroMacro[1]  = &addKnob (kickr::id::macroBody,   "MACRO", pal::lowfam, false, KSize::Small);
    bodyFooter = {
        &addKnob (kickr::id::bodyLevel,     "LEVEL",   pal::violet, false, KSize::Small),
        &addKnob (kickr::id::bodyDecay,     "DECAY",   pal::lowfam, false, KSize::Small),
        &addKnob (kickr::id::bodyHarmonics, "HARM",    pal::violet, false, KSize::Small),
        &addKnob (kickr::id::pitchStart,    "P START", pal::lowfam, false, KSize::Small),
        &addKnob (kickr::id::pitchTime,     "P TIME",  pal::lowfam, false, KSize::Small),
        &addKnob (kickr::id::pitchCurve,    "P CURVE", pal::lowfam, false, KSize::Small),
    };

    heroTitles[2] = &addSectionLabel ("CRUSH", pal::magenta);
    heroBig[2]    = &addKnob (kickr::id::drive,      "DRIVE", pal::magenta, false, KSize::Large);
    heroMacro[2]  = &addKnob (kickr::id::macroCrush, "MACRO", pal::magenta, false, KSize::Small);
    // // Phase 3.4: `character` style label (Tanh -> Cubic -> ... -> Bitcrush)
    crushFooter = {
        &addKnob (kickr::id::character, "CHARACTR", pal::magenta, false, KSize::Small),
        &addKnob (kickr::id::driveMix,  "MIX",      pal::magenta, false, KSize::Small),
    };

    heroTitles[3] = &addSectionLabel ("TAIL", pal::lowfam);
    heroBig[3]    = &addKnob (kickr::id::tailLength, "LENGTH", pal::lowfam, false, KSize::Large);
    heroMacro[3]  = &addKnob (kickr::id::macroTail,  "MACRO",  pal::lowfam, false, KSize::Small);
    tailFooter = {
        &addKnob (kickr::id::tailLevel, "LEVEL", pal::lowfam, false, KSize::Small),
        &addKnob (kickr::id::tailTone,  "TONE",  pal::lowfam, false, KSize::Small),
        &addKnob (kickr::id::tailDrive, "DRIVE", pal::red,    false, KSize::Small),
    };

    // ---------------------------------------------------------------- engine strip
    engineTitles[0] = &addSectionLabel ("CLICK", pal::magenta);
    engineCells[0] = {
        &addKnob (kickr::id::clickLevel, "LEVEL", pal::magenta, false, KSize::Small),
        &addKnob (kickr::id::clickTone,  "TONE",  pal::magenta, false, KSize::Small),
        &addKnob (kickr::id::clickTime,  "TIME",  pal::magenta, false, KSize::Small),
        &addKnob (kickr::id::clickPitch, "PITCH", pal::magenta, false, KSize::Small),
    };

    engineTitles[1] = &addSectionLabel ("SUB", pal::lowfam);
    engineCells[1] = {
        &addKnob (kickr::id::subLevel, "LEVEL", pal::lowfam, false, KSize::Small),
        &addKnob (kickr::id::subFreq,  "FREQ",  pal::lowfam, false, KSize::Small),
        &addKnob (kickr::id::subDecay, "DECAY", pal::lowfam, false, KSize::Small),
    };

    engineTitles[2] = &addSectionLabel ("NOISE", pal::magenta);
    engineCombos[2] = &addCombo (kickr::id::noiseType, { "White", "Pink", "Filtered" }, pal::magenta);
    engineCells[2] = {
        &addKnob (kickr::id::noiseLevel, "LEVEL", pal::magenta, false, KSize::Small),
        &addKnob (kickr::id::noiseDecay, "DECAY", pal::magenta, false, KSize::Small),
        &addKnob (kickr::id::noiseTone,  "TONE",  pal::magenta, false, KSize::Small),
    };

    engineTitles[3] = &addSectionLabel ("TONE", pal::violet);
    engineCells[3] = {
        &addKnob (kickr::id::low,  "LOW",  pal::violet, true, KSize::Small),
        &addKnob (kickr::id::mid,  "MID",  pal::violet, true, KSize::Small),
        &addKnob (kickr::id::high, "HIGH", pal::violet, true, KSize::Small),
    };

    engineTitles[4] = &addSectionLabel ("STEREO", pal::violet);
    engineCells[4] = {
        &addKnob (kickr::id::bodyWidth,   "BODY",  pal::violet,  false, KSize::Small),
        &addKnob (kickr::id::clickWidth,  "CLICK", pal::magenta, false, KSize::Small),
        &addKnob (kickr::id::outputWidth, "OUT",   pal::violet,  false, KSize::Small),
    };

    engineTitles[5] = &addSectionLabel ("TUNING", pal::lowfam);
    engineCombos[5] = &addCombo (kickr::id::tuneMode, { "MIDI Pitch", "Fixed Frequency" }, pal::lowfam);
    engineCells[5] = {
        &addKnob (kickr::id::tune,           "TUNE", pal::lowfam, true,  KSize::Small),
        &addKnob (kickr::id::fineTune,       "FINE", pal::lowfam, true,  KSize::Small),
        &addKnob (kickr::id::velSensitivity, "VEL",  pal::lowfam, false, KSize::Small),
    };

    engineTitles[6] = &addSectionLabel ("OUTPUT", pal::red);
    engineCombos[6] = &addCombo  (kickr::id::oversampling, { "1x", "2x", "4x", "8x" }, pal::red);
    engineExtra[6]  = &addToggle (kickr::id::limiter, "LIMITER", pal::red);
    engineCells[6] = {
        &addKnob (kickr::id::output, "GAIN", pal::violet, false, KSize::Small),
        &addKnob (kickr::id::mix,    "MIX",  pal::violet, false, KSize::Small),
    };

    // ---------------------------------------------------------------- wiring
    proc.getUndoManager().addChangeListener (this);
    updateUndoRedoState();
    updateSampleLabel();

    jassert (boundParamCount == 59);

    setResizable (true, true);
    setResizeLimits (900, 660, 2000, 1470);
    if (auto* c = getConstrainer())
        c->setFixedAspectRatio (1600.0 / 1170.0);

    setSize (1280, 936);
}

KICKRAudioProcessorEditor::~KICKRAudioProcessorEditor()
{
    proc.getUndoManager().removeChangeListener (this);
    setLookAndFeel (nullptr);
}

//==============================================================================
int KICKRAudioProcessorEditor::scaled (int referencePx) const noexcept
{
    return juce::roundToInt ((float) referencePx * scaleFactor);
}

kickr::KickrKnob& KICKRAudioProcessorEditor::addKnob (juce::StringRef id, const juce::String& caption,
                                                      juce::Colour accent, bool bipolar,
                                                      kickr::KickrKnob::Size size)
{
    auto k = std::make_unique<kickr::KickrKnob> (proc.getValueTreeState(), id, caption, accent, bipolar, size);
    addAndMakeVisible (*k);
    auto& ref = *k;
    knobs.push_back (std::move (k));
    ++boundParamCount;
    return ref;
}

kickr::KickrToggle& KICKRAudioProcessorEditor::addToggle (juce::StringRef id, const juce::String& text,
                                                          juce::Colour accent)
{
    auto t = std::make_unique<kickr::KickrToggle> (proc.getValueTreeState(), id, text, accent);
    addAndMakeVisible (*t);
    auto& ref = *t;
    toggles.push_back (std::move (t));
    ++boundParamCount;
    return ref;
}

juce::ComboBox& KICKRAudioProcessorEditor::addCombo (juce::StringRef id, const juce::StringArray& items,
                                                     juce::Colour accent)
{
    auto box = std::make_unique<juce::ComboBox>();
    box->addItemList (items, 1);
    box->setJustificationType (juce::Justification::centred);
    box->setColour (juce::ComboBox::textColourId, accent);
    box->setColour (juce::ComboBox::arrowColourId, accent);
    addAndMakeVisible (*box);

    if (auto* prm = proc.getValueTreeState().getParameter (id))
    {
        box->setTooltip (prm->getName (64));
        comboAtts.push_back (std::make_unique<juce::ComboBoxParameterAttachment> (
            *prm, *box, proc.getValueTreeState().undoManager));
    }

    auto& ref = *box;
    combos.push_back (std::move (box));
    ++boundParamCount;
    return ref;
}

juce::Label& KICKRAudioProcessorEditor::addSectionLabel (const juce::String& text, juce::Colour accent)
{
    auto lbl = std::make_unique<juce::Label>();
    lbl->setText (text, juce::dontSendNotification);
    lbl->setJustificationType (juce::Justification::centredLeft);
    lbl->setColour (juce::Label::textColourId, accent.brighter (0.35f));
    lbl->setInterceptsMouseClicks (false, false);
    addAndMakeVisible (*lbl);
    auto& ref = *lbl;
    sectionLabels.push_back (std::move (lbl));
    return ref;
}

//==============================================================================
void KICKRAudioProcessorEditor::changeListenerCallback (juce::ChangeBroadcaster*)
{
    updateUndoRedoState();
}

void KICKRAudioProcessorEditor::updateUndoRedoState()
{
    undoButton.setEnabled (proc.getUndoManager().canUndo());
    redoButton.setEnabled (proc.getUndoManager().canRedo());
}

void KICKRAudioProcessorEditor::updateSampleLabel()
{
    const auto n = proc.getCurrentSampleName();
    sampleNameLabel.setText (n.isEmpty() ? juce::String::fromUTF8 ("\xe2\x80\x94 drop a kick \xe2\x80\x94")
                                         : n,
                             juce::dontSendNotification);
}

//==============================================================================
void KICKRAudioProcessorEditor::placeRow (juce::Rectangle<int> area,
                                          const std::vector<juce::Component*>& items, int gap)
{
    const int n = (int) items.size();
    if (n <= 0) return;

    const int w = area.getWidth() / n;
    for (int i = 0; i < n; ++i)
    {
        juce::Rectangle<int> cell (area.getX() + i * w, area.getY(), w, area.getHeight());
        if (items[(size_t) i] != nullptr)
            items[(size_t) i]->setBounds (cell.reduced (gap, gap));
    }
}

void KICKRAudioProcessorEditor::placeGrid (juce::Rectangle<int> area,
                                           const std::vector<juce::Component*>& items,
                                           int cols, int gap)
{
    const int n = (int) items.size();
    if (n <= 0) return;

    cols = juce::jlimit (1, n, cols);
    const int rows = (n + cols - 1) / cols;
    const int cw = area.getWidth() / cols;
    const int ch = area.getHeight() / rows;

    for (int i = 0; i < n; ++i)
    {
        const int col = i % cols;
        const int row = i / cols;
        juce::Rectangle<int> cell (area.getX() + col * cw, area.getY() + row * ch, cw, ch);
        if (items[(size_t) i] != nullptr)
            items[(size_t) i]->setBounds (cell.reduced (gap, gap));
    }
}

//==============================================================================
void KICKRAudioProcessorEditor::resized()
{
    scaleFactor = (float) getWidth() / 1600.0f;

    auto face = getLocalBounds().reduced (scaled (10));
    faceplateBounds = face.reduced (scaled (24));
    auto content = faceplateBounds.reduced (scaled (16));

    const int gap = scaled (14);

    auto header = content.removeFromTop (scaled (46));
    content.removeFromTop (gap);
    auto scope = content.removeFromTop (scaled (206));
    content.removeFromTop (gap);
    auto sample = content.removeFromTop (scaled (150));
    content.removeFromTop (gap);
    auto engine = content.removeFromBottom (scaled (296));
    content.removeFromBottom (gap);
    auto heroes = content;

    scopeBounds  = scope;
    sampleBounds = sample;
    engineBounds = engine;

    layoutHeader (header);
    layoutScope  (scope);
    layoutSample (sample);
    layoutHeroes (heroes);
    layoutEngine (engine);
}

void KICKRAudioProcessorEditor::layoutHeader (juce::Rectangle<int> a)
{
    wordmark.setFont (kickr::KickrLookAndFeel::titleFont ((float) scaled (20)));
    wordmark.setBounds (a.removeFromLeft (scaled (104)));

    versionLabel.setFont (kickr::KickrLookAndFeel::microFont ((float) scaled (10)));
    versionLabel.setBounds (a.removeFromLeft (scaled (44)));

    auto btns = a.removeFromRight (scaled (600));
    const int bw = btns.getWidth() / 6;
    for (auto* b : { &saveButton, &undoButton, &redoButton, &randomButton, &mutateButton, &abButton })
        b->setBounds (btns.removeFromLeft (bw).reduced (scaled (3)));

    auto pill = a.reduced (scaled (14), scaled (6));
    presetPrev.setBounds (pill.removeFromLeft (scaled (26)));
    presetNext.setBounds (pill.removeFromRight (scaled (26)));
    presetNameLabel.setBounds (pill);
    presetNameLabel.setFont (kickr::KickrLookAndFeel::titleFont ((float) scaled (11)));
}

void KICKRAudioProcessorEditor::layoutScope (juce::Rectangle<int> a)
{
    auto modes = a.reduced (scaled (14)).removeFromTop (scaled (22)).removeFromRight (scaled (150));
    scopeSpectrumButton.setBounds (modes.removeFromRight (scaled (78)));
    modes.removeFromRight (scaled (4));
    scopeWaveButton.setBounds (modes);
}

void KICKRAudioProcessorEditor::layoutSample (juce::Rectangle<int> a)
{
    auto inner = a.reduced (scaled (14));

    auto bank = inner.removeFromLeft (scaled (330));
    inner.removeFromLeft (scaled (14));

    auto togRow = bank.removeFromTop (scaled (28));
    if (sampleToggles[0] != nullptr)
        sampleToggles[0]->setBounds (togRow.removeFromLeft (togRow.getWidth() / 2).reduced (scaled (3)));
    if (sampleToggles[1] != nullptr)
        sampleToggles[1]->setBounds (togRow.reduced (scaled (3)));
    bank.removeFromTop (scaled (8));

    auto slot = bank;
    auto slotTop = slot.removeFromTop (scaled (18));
    sampleNameLabel.setBounds (slotTop.removeFromLeft (slotTop.getWidth() * 2 / 3));
    sampleDropHint.setBounds (slotTop);
    sampleNameLabel.setFont (kickr::KickrLookAndFeel::microFont ((float) scaled (10)));
    sampleDropHint.setFont (kickr::KickrLookAndFeel::microFont ((float) scaled (8)));

    auto navRow = slot.removeFromBottom (scaled (20));
    samplePrev.setBounds (navRow.removeFromLeft (scaled (34)));
    navRow.removeFromLeft (scaled (4));
    sampleNext.setBounds (navRow.removeFromLeft (scaled (34)));

    placeRow (inner, sampleTweaks, scaled (5));
}

void KICKRAudioProcessorEditor::layoutHeroes (juce::Rectangle<int> a)
{
    const int gap = scaled (16);
    const int cw = (a.getWidth() - gap * 3) / 4;

    for (int i = 0; i < 4; ++i)
    {
        juce::Rectangle<int> m (a.getX() + i * (cw + gap), a.getY(), cw, a.getHeight());
        heroRects[(size_t) i] = m;

        auto inner = m.reduced (scaled (14));

        auto head = inner.removeFromTop (scaled (42));
        if (heroMacro[(size_t) i] != nullptr)
        {
            heroMacro[(size_t) i]->setBounds (head.removeFromRight (scaled (56)));
            head.removeFromRight (scaled (6));
        }
        if (heroTitles[(size_t) i] != nullptr)
        {
            heroTitles[(size_t) i]->setBounds (head);
            heroTitles[(size_t) i]->setFont (kickr::KickrLookAndFeel::titleFont ((float) scaled (15)));
        }

        auto hero = inner.removeFromTop (juce::jmin (scaled (108), inner.getHeight() * 5 / 9));
        if (heroBig[(size_t) i] != nullptr)
        {
            const int d = juce::jmin (hero.getWidth(), hero.getHeight());
            heroBig[(size_t) i]->setBounds (hero.withSizeKeepingCentre (juce::jmin (hero.getWidth(), d + scaled (34)), hero.getHeight()));
        }

        const std::vector<juce::Component*>& footer = i == 0 ? punchFooter
                                                    : i == 1 ? bodyFooter
                                                    : i == 2 ? crushFooter
                                                             : tailFooter;
        placeGrid (inner, footer, 3, scaled (4));
    }
}

void KICKRAudioProcessorEditor::layoutEngine (juce::Rectangle<int> a)
{
    const int cw = a.getWidth() / 7;

    for (int i = 0; i < 7; ++i)
    {
        juce::Rectangle<int> cell (a.getX() + i * cw, a.getY(), cw, a.getHeight());
        engineRects[(size_t) i] = cell;

        auto inner = cell.reduced (scaled (10));

        if (engineTitles[(size_t) i] != nullptr)
        {
            engineTitles[(size_t) i]->setBounds (inner.removeFromTop (scaled (18)));
            engineTitles[(size_t) i]->setFont (kickr::KickrLookAndFeel::titleFont ((float) scaled (11)));
        }
        inner.removeFromTop (scaled (4));

        if (engineCombos[(size_t) i] != nullptr)
        {
            engineCombos[(size_t) i]->setBounds (inner.removeFromTop (scaled (22)));
            inner.removeFromTop (scaled (6));
        }
        if (engineExtra[(size_t) i] != nullptr)
        {
            engineExtra[(size_t) i]->setBounds (inner.removeFromTop (scaled (22)));
            inner.removeFromTop (scaled (6));
        }

        placeGrid (inner, engineCells[(size_t) i], 2, scaled (4));
    }
}

//==============================================================================
void KICKRAudioProcessorEditor::paint (juce::Graphics& g)
{
    const auto sf = [this] (int v) { return (float) v * scaleFactor; };

    g.fillAll (pal::voidBlack);

    // chassis
    {
        auto chassis = getLocalBounds().toFloat().reduced (sf (8));
        juce::ColourGradient cg (pal::chassisHi, chassis.getX(), chassis.getY(),
                                 pal::chassisLo, chassis.getRight(), chassis.getBottom(), false);
        cg.addColour (0.5, pal::chassisMid);
        g.setGradientFill (cg);
        g.fillRoundedRectangle (chassis, sf (42));
        g.setColour (juce::Colours::white.withAlpha (0.05f));
        g.drawRoundedRectangle (chassis.reduced (1.0f), sf (42), 1.0f);
    }

    // recessed faceplate
    {
        auto fp = faceplateBounds.toFloat();
        juce::ColourGradient fg (pal::faceplateHi, 0.0f, fp.getY(),
                                 pal::faceplateLo, 0.0f, fp.getBottom(), false);
        g.setGradientFill (fg);
        g.fillRoundedRectangle (fp, sf (26));
        g.setColour (juce::Colours::black.withAlpha (0.55f));
        g.drawRoundedRectangle (fp.reduced (0.5f), sf (26), 1.2f);
    }

    auto panel = [&] (juce::Rectangle<int> b)
    {
        if (b.isEmpty()) return;
        auto bf = b.toFloat();
        juce::ColourGradient pg (pal::panelHi, 0.0f, bf.getY(), pal::panelLo, 0.0f, bf.getBottom(), false);
        g.setGradientFill (pg);
        g.fillRoundedRectangle (bf, sf (20));
        g.setColour (pal::panelEdge.withAlpha (0.6f));
        g.drawRoundedRectangle (bf.reduced (0.5f), sf (20), 1.0f);
    };

    panel (sampleBounds);
    for (auto& hr : heroRects) panel (hr);
    panel (engineBounds);

    // engine cell dividers
    g.setColour (juce::Colours::black.withAlpha (0.35f));
    for (int i = 1; i < 7; ++i)
        g.drawVerticalLine (engineRects[(size_t) i].getX(),
                            (float) engineBounds.getY() + sf (10),
                            (float) engineBounds.getBottom() - sf (10));

    // ---- oscilloscope placeholder (Phase 3.2) ----
    {
        auto s = scopeBounds.toFloat();
        juce::ColourGradient sg (juce::Colour (0xff17111F), s.getCentreX(), s.getY() - s.getHeight() * 0.2f,
                                 juce::Colour (0xff060409), s.getCentreX(), s.getBottom(), true);
        sg.addColour (0.55, juce::Colour (0xff0A0710));
        g.setGradientFill (sg);
        g.fillRoundedRectangle (s, sf (18));
        g.setColour (juce::Colours::black);
        g.drawRoundedRectangle (s.reduced (0.5f), sf (18), 1.5f);

        g.setColour (pal::ink.withAlpha (0.10f));
        g.drawHorizontalLine (scopeBounds.getCentreY(), s.getX() + sf (14), s.getRight() - sf (14));
        for (int k = 1; k < 6; ++k)
        {
            const int x = juce::roundToInt (s.getX() + s.getWidth() * (float) k / 6.0f);
            g.drawVerticalLine (x, s.getY() + sf (12), s.getBottom() - sf (12));
        }

        const int axH = juce::roundToInt (sf (12));
        const int axW = juce::roundToInt (sf (46));
        const int axY = scopeBounds.getBottom() - juce::roundToInt (sf (16));
        g.setColour (pal::ink.withAlpha (0.32f));
        g.setFont (juce::Font (juce::FontOptions (sf (8))));
        g.drawText ("0 ms",   scopeBounds.getX() + juce::roundToInt (sf (10)), axY, axW, axH, juce::Justification::left,  false);
        g.drawText ("250",    scopeBounds.getCentreX() - axW / 2,              axY, axW, axH, juce::Justification::centred, false);
        g.drawText ("500",    scopeBounds.getRight() - axW - juce::roundToInt (sf (10)), axY, axW, axH, juce::Justification::right, false);

        g.setColour (pal::ink.withAlpha (0.12f));
        g.setFont (kickr::KickrLookAndFeel::titleFont (sf (11)));
        g.drawText (juce::String::fromUTF8 ("ANALYZER \xe2\x80\x94 PHASE 3.2"),
                    scopeBounds.reduced (juce::roundToInt (sf (20))),
                    juce::Justification::centred, false);
    }
}
