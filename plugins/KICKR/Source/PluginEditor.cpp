#include "PluginProcessor.h"
#include "PluginEditor.h"

#include "Parameters/ParameterIDs.h"
#include "Parameters/ParameterDescriptions.h"

namespace pal = kickr::palette;
using KSize = kickr::KickrKnob::Size;

//==============================================================================
KICKRAudioProcessorEditor::KICKRAudioProcessorEditor (KICKRAudioProcessor& p)
    : AudioProcessorEditor (&p), proc (p)
{
    setLookAndFeel (&lnf);
    // Delay itself is set on the `tooltip` member's constructor (PluginEditor.h) — this
    // redundant, matching call is kept only so the two can't silently drift apart again
    // the way they once did (this used to say 450, quietly overriding the header's 3000
    // the whole time the 3-second hover request was "done"). Both now read the same
    // constant: 1.5 s (2026-09-01 user request, down from 3 s).
    tooltip.setMillisecondsBeforeTipAppears (kTooltipDelayMs);

    // ---------------------------------------------------------------- analyzers
    // Added FIRST so the WAVE / SPECTRUM mode buttons (added below) sit on top.
    // Both fill the scope panel; `waveDisplay` visible by default.
    addChildComponent (waveDisplay);
    addChildComponent (spectrumDisplay);
    waveDisplay.setVisible (true);

    // ---------------------------------------------------------------- header
    wordmark.setText ("KICKR", juce::dontSendNotification);
    wordmark.setJustificationType (juce::Justification::centredLeft);
    wordmark.setColour (juce::Label::textColourId, pal::inkHi);
    // 2026-09-02 (user request): the wordmark is a button now — click -> about / licences page.
    wordmark.setInterceptsMouseClicks (true, false);
    wordmark.setMouseCursor (juce::MouseCursor::PointingHandCursor);
    wordmark.setTooltip ("About KICKR - licences and credits");
    wordmark.addMouseListener (this, false);
    addAndMakeVisible (wordmark);
    addChildComponent (aboutPage);   // hidden until the wordmark is clicked

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
                     &scopeWaveButton, &scopeSpectrumButton, &scopeFilterButton, &samplePrev, &sampleNext })
        addAndMakeVisible (b);

    auto afterAction = [this]
    {
        updateUndoRedoState();
        syncPresetName();
        updateSampleLabel();
    };

    undoButton.onClick   = [this, afterAction] { proc.getUndoManager().undo(); afterAction(); };
    redoButton.onClick   = [this, afterAction] { proc.getUndoManager().redo(); afterAction(); };
    saveButton.onClick   = [this] { showSaveDialog(); };
    randomButton.onClick = [this] { doRandomize(); };
    mutateButton.onClick = [this] { doMutate(); };
    abButton.onClick     = [this] { doAB(); };
    presetPrev.onClick   = [this]
    {
        const int i = juce::jmax (0, presetList.indexOf (proc.getPresetManager().getCurrentPresetName()));
        loadPresetAt ((i - 1 + juce::jmax (1, presetList.size())) % juce::jmax (1, presetList.size()));
    };
    presetNext.onClick   = [this]
    {
        const int i = juce::jmax (0, presetList.indexOf (proc.getPresetManager().getCurrentPresetName()));
        loadPresetAt ((i + 1) % juce::jmax (1, presetList.size()));
    };

    undoButton.setTooltip   ("Undo the last preset load / Randomize / Mutate / parameter change");
    redoButton.setTooltip   ("Redo");
    saveButton.setTooltip   ("Save the current patch as a user preset");
    randomButton.setTooltip ("Randomise the synth parameters (tuning / output / macros / sample untouched)");
    mutateButton.setTooltip ("Nudge every synth parameter a little - keeps the character");
    abButton.setTooltip     ("Stash / compare two states");
    presetPrev.setTooltip   ("Previous preset");
    presetNext.setTooltip   ("Next preset");
    // 2026-09-02 (user request): identical mirrored vector arrows (see KickrLookAndFeel::drawButtonText).
    presetPrev.getProperties().set ("arrowDir", -1);
    presetNext.getProperties().set ("arrowDir",  1);

    presetNameLabel.setInterceptsMouseClicks (true, false);
    presetNameLabel.addMouseListener (this, false);

    noticeLabel.setJustificationType (juce::Justification::centred);
    noticeLabel.setColour (juce::Label::textColourId, pal::inkHi);
    noticeLabel.setInterceptsMouseClicks (false, false);
    noticeLabel.setFont (kickr::KickrLookAndFeel::titleFont (11.0f));
    addChildComponent (noticeLabel);

    scopeWaveButton.setClickingTogglesState (true);
    scopeSpectrumButton.setClickingTogglesState (true);
    scopeFilterButton.setClickingTogglesState (true);     // 2026-09-02
    scopeWaveButton.setRadioGroupId (0x5c09e);
    scopeSpectrumButton.setRadioGroupId (0x5c09e);
    scopeFilterButton.setRadioGroupId (0x5c09e);
    scopeFilterButton.setTooltip ("Master filter - low / high pass with a draggable response");
    scopeWaveButton.setToggleState (true, juce::dontSendNotification);
    scopeWaveButton.setTooltip ("Kick waveform - one capture per trigger");
    scopeSpectrumButton.setTooltip ("Live FFT spectrum");

    // 2026-09-02: three radio pages — WAVE / SPECTRUM / FILTER.
    auto syncScopePages = [this]
    {
        waveDisplay.setVisible     (scopeWaveButton.getToggleState());
        spectrumDisplay.setVisible (scopeSpectrumButton.getToggleState());
        filterDisplay.setVisible   (scopeFilterButton.getToggleState());
    };
    scopeWaveButton.onClick     = syncScopePages;
    scopeSpectrumButton.onClick = syncScopePages;
    scopeFilterButton.onClick   = syncScopePages;
    addChildComponent (filterDisplay);
    filterDisplay.setVisible (false);
    for (auto* b : { &scopeWaveButton, &scopeSpectrumButton, &scopeFilterButton })
        b->toFront (false);   // the page components sit under the mode buttons

    // 2026-09-01 (user request) — Morph: a big knob in the scope panel's carved-out left
    // 1/6 that morphs the body oscillator's waveform (sine -> triangle -> saw -> square).
    // Default 0.0 (pure sine) so existing presets/factory sounds are unaffected unless
    // this knob is moved. Approved from an unbound layout mockup before wiring to DSP.
    morphKnob = &addKnob (kickr::id::morph, "MORPH", pal::violet, false, KSize::Large);

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
    addAndMakeVisible (sampleWaveform);
    // // Phase 3.3: full FileDragAndDropTarget + empty-bank / missing-file UI states + waveform

    samplePrev.setTooltip ("Previous sample in the bank");
    sampleNext.setTooltip ("Next sample in the bank");

    // 2026-08-31: browse the COMBINED bank (50 shipped factory kicks + your own imports)
    // via prevTotal()/nextTotal(), not the disk-only prev()/next().
    samplePrev.onClick = [this]
    {
        const auto n = proc.getSampleLibrary().prevTotal();
        if (n.isNotEmpty()) proc.loadSampleByName (n);
        updateSampleLabel();
    };
    sampleNext.onClick = [this]
    {
        const auto n = proc.getSampleLibrary().nextTotal();
        if (n.isNotEmpty()) proc.loadSampleByName (n);
        updateSampleLabel();
    };

    auto& synthTog  = addToggle (kickr::id::synthEnable,  "SYNTH",  pal::violet);
    auto& sampleTog = addToggle (kickr::id::sampleEnable, "SAMPLE", pal::red);
    sampleToggles = { &synthTog, &sampleTog };

    // All the sample-layer tweak controls share the SAMPLE toggle's red accent — one
    // consistent colour identifying "this knob belongs to the sample engine".
    sampleTweaks = {
        &addKnob   (kickr::id::sampleLevel,     "LEVEL",  pal::red, false, KSize::Small),
        &addKnob   (kickr::id::sampleStart,     "START",  pal::red, false, KSize::Small),
        &addKnob   (kickr::id::sampleEnd,       "END",    pal::red, false, KSize::Small),
        &addToggle (kickr::id::sampleReverse,   "REV",    pal::red),
        &addKnob   (kickr::id::sampleTune,      "TUNE",   pal::red, true,  KSize::Small),
        &addKnob   (kickr::id::sampleFine,      "FINE",   pal::red, true,  KSize::Small),
        &addToggle (kickr::id::sampleMidiTrack, "TRK",    pal::red),
        &addKnob   (kickr::id::sampleAttack,    "ATTACK", pal::red, false, KSize::Small),
        &addKnob   (kickr::id::sampleDecay,     "DECAY",  pal::red, false, KSize::Small),
        &addKnob   (kickr::id::sampleHP,        "HP",     pal::red, false, KSize::Small),
        &addKnob   (kickr::id::sampleLP,        "LP",     pal::red, false, KSize::Small),
        &addKnob   (kickr::id::sampleCrush,     "CRUSH",  pal::red, false, KSize::Small),
    };

    // 2026-09-01 (user request): "when sample is disabled the sampler controlling
    // knobs turn gray until you turn sampler on again." sampleEnable itself (and the
    // synth toggle) stay always interactive; only the tweak controls grey out.
    if (auto* sampleEnableParam = proc.getValueTreeState().getParameter (kickr::id::sampleEnable))
    {
        sampleEnableWatcher = std::make_unique<juce::ParameterAttachment> (
            *sampleEnableParam,
            [this] (float value)
            {
                const bool on = value >= 0.5f;
                for (auto* c : sampleTweaks)
                    if (c != nullptr) c->setEnabled (on);
            });
        sampleEnableWatcher->sendInitialUpdate();
    }

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

    // 2026-09-02 (user request): "remove the tuning section, we will add something there
    // later. leave the midi pitch mode on always." Engine cell 5 is intentionally EMPTY —
    // no title, combo, or knobs — reserved for a future module. The four parameters
    // (`tuneMode`, `tune`, `fineTune`, `velSensitivity`) stay in the APVTS untouched so
    // old sessions / presets still load; `tuneMode` is pinned to MIDI Pitch by
    // enforceMidiPitchMode() at every editor refresh point.
    // 2026-09-02 (user request, same day): "change the limiter to an identical to abletons
    // colour limiter" — the reserved cell becomes LIMITER: on/off + the Color-Limiter
    // control set in a 3 x 2 grid. OUTPUT keeps oversampling / GAIN / MIX.
    // 2026-09-02 (user request, later the same day): "remove the output section give limiter
    // the space and place the oversampling with the limiter" — LIMITER now spans the last
    // TWO engine cells (the 7th is gone), the oversampling selector sits beside the on/off
    // pill, and the six knobs are Medium size. `output` (GAIN) and `mix` stay in the APVTS
    // at their defaults (0 dB / 1.0) but have no control any more — LOUDNESS is the gain now.
    engineTitles[5] = &addSectionLabel ("LIMITER", pal::red);
    engineCombos[5] = &addCombo  (kickr::id::oversampling, { "1x", "2x", "4x", "8x" }, pal::red);
    engineExtra[5]  = &addToggle (kickr::id::limiter, "LIMITER", pal::red);
    engineCells[5] = {
        &addKnob (kickr::id::limLoudness,   "LOUD",  pal::red,     false, KSize::Medium),
        &addKnob (kickr::id::limCeiling,    "CEIL",  pal::red,     false, KSize::Medium),
        &addKnob (kickr::id::limLookahead,  "LOOK",  pal::lowfam,  false, KSize::Medium),
        &addKnob (kickr::id::limRelease,    "REL",   pal::lowfam,  false, KSize::Medium),
        &addKnob (kickr::id::limSaturation, "SAT",   pal::magenta, false, KSize::Medium),
        &addKnob (kickr::id::limColor,      "COLOR", pal::magenta, false, KSize::Medium),
    };

    // OUTPUT cell removed 2026-09-02 (see LIMITER above) — slot 6 is folded into slot 5.
    engineTitles[6] = nullptr;
    engineCombos[6] = nullptr;
    engineExtra[6]  = nullptr;
    engineCells[6].clear();

    // ---------------------------------------------------------------- wiring
    proc.getUndoManager().addChangeListener (this);
    rebuildPresetList();
    syncPresetName();
    updateUndoRedoState();
    updateSampleLabel();

    jassert (boundParamCount == 60);   // 2026-09-01: 59 + morph; 2026-09-02: - 4 TUNING controls, + 6 Color-Limiter knobs, - GAIN / MIX (OUTPUT cell removed)

    // 2026-09-01: reverted the 2026-08-31 fixed-size trial — resizable again, with a
    // bottom-right corner drag handle. Aspect-locked to the mockup's 1600:1170 ratio so
    // the layout (which scales entirely from getWidth()/1600 in resized()) never distorts.
    setResizable (true, true);
    setResizeLimits (900, 660, 2000, 1470);
    getConstrainer()->setFixedAspectRatio (1600.0 / 1170.0);
    setSize (1120, 819);
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
        const auto& desc = kickr::paramDescription (id);
        box->setTooltip (desc.isNotEmpty() ? desc : prm->getName (64));
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

//============================================================ Phase 3.3 — presets
void KICKRAudioProcessorEditor::rebuildPresetList()
{
    auto& pm = proc.getPresetManager();
    presetList = pm.getFactoryNames();
    numFactoryInList = presetList.size();
    presetList.addArray (pm.getUserNames());
}

void KICKRAudioProcessorEditor::syncPresetName()
{
    presetNameLabel.setText (proc.getPresetManager().getCurrentPresetName(),
                             juce::dontSendNotification);
}

void KICKRAudioProcessorEditor::loadPresetAt (int combinedIndex)
{
    rebuildPresetList();
    if (presetList.isEmpty())
        return;

    const int idx = juce::jlimit (0, presetList.size() - 1, combinedIndex);
    const auto name = presetList[idx];

    const bool ok = (idx < numFactoryInList)
                        ? (proc.getPresetManager().loadFactory (idx), true)
                        : proc.getPresetManager().loadUser (name);

    updateUndoRedoState();
    syncPresetName();
    updateSampleLabel();
    showNotice (ok ? "Loaded  " + name : "Preset not found - kept current");
}

void KICKRAudioProcessorEditor::showPresetMenu()
{
    rebuildPresetList();

    juce::PopupMenu menu, factory, user;
    for (int i = 0; i < numFactoryInList; ++i)
        factory.addItem (i + 1, presetList[i]);
    for (int i = numFactoryInList; i < presetList.size(); ++i)
        user.addItem (i + 1, presetList[i]);

    menu.addSubMenu ("Factory", factory);
    if (user.getNumItems() > 0)
        menu.addSubMenu ("User", user);
    menu.addSeparator();
    menu.addItem (1000, "Save As...");

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (presetNameLabel),
                        [this] (int r)
                        {
                            if (r == 0) return;
                            if (r == 1000) { showSaveDialog(); return; }
                            loadPresetAt (r - 1);
                        });
}

void KICKRAudioProcessorEditor::showSaveDialog()
{
    saveDialog = std::make_unique<juce::AlertWindow> ("Save preset",
                     "Name this patch:", juce::MessageBoxIconType::NoIcon, this);
    saveDialog->addTextEditor ("name", proc.getPresetManager().getCurrentPresetName()
                                           .replace ("*", "").trim());
    saveDialog->addButton ("Save",   1, juce::KeyPress (juce::KeyPress::returnKey));
    saveDialog->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    saveDialog->enterModalState (true, juce::ModalCallbackFunction::create (
        [this] (int r)
        {
            if (r == 1)
            {
                const auto name = saveDialog->getTextEditorContents ("name").trim();
                const bool ok   = proc.getPresetManager().saveUser (name);
                syncPresetName();
                showNotice (ok ? "Saved  " + name : "Could not save preset");
            }
            saveDialog.reset();
        }), false);
}

void KICKRAudioProcessorEditor::doRandomize()
{
    proc.getPresetManager().randomize();
    updateUndoRedoState();
    syncPresetName();
    showNotice ("Randomised");
}

void KICKRAudioProcessorEditor::doMutate()
{
    proc.getPresetManager().mutate();
    updateUndoRedoState();
    syncPresetName();
    showNotice ("Mutated");
}

void KICKRAudioProcessorEditor::doAB()
{
    // Stash the current state into the *inactive* slot, then apply the other slot.
    juce::MemoryBlock cur;
    proc.getStateInformation (cur);
    abSlot[abActive] = cur;

    const int other = 1 - abActive;
    if (abSlot[other].getSize() > 0)
    {
        proc.setStateInformation (abSlot[other].getData(), (int) abSlot[other].getSize());
        abActive = other;
        updateUndoRedoState();
        syncPresetName();
        updateSampleLabel();
        showNotice (abActive == 0 ? "A" : "B");
    }
    else
    {
        showNotice (juce::String (abActive == 0 ? "A" : "B") + " stored - switch again to compare");
        abActive = other;
    }
}

//======================================================= Phase 3.3 — sample drag-drop
bool KICKRAudioProcessorEditor::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (const auto& f : files)
    {
        const auto ext = juce::File (f).getFileExtension().toLowerCase();
        if (ext == ".wav" || ext == ".aif" || ext == ".aiff" || ext == ".flac" || ext == ".caf")
            return true;
    }
    return false;
}

void KICKRAudioProcessorEditor::fileDragEnter (const juce::StringArray&, int, int)
{
    fileDragActive = true;  repaint();
}

void KICKRAudioProcessorEditor::fileDragExit (const juce::StringArray&)
{
    fileDragActive = false; repaint();
}

void KICKRAudioProcessorEditor::filesDropped (const juce::StringArray& files, int, int)
{
    fileDragActive = false; repaint();
    for (const auto& f : files)
    {
        const juce::File file (f);
        const auto ext = file.getFileExtension().toLowerCase();
        if (ext == ".wav" || ext == ".aif" || ext == ".aiff" || ext == ".flac" || ext == ".caf")
        {
            importAudioFile (file);
            return;   // first accepted file only
        }
    }
}

void KICKRAudioProcessorEditor::importAudioFile (const juce::File& file)
{
    const auto name = proc.getSampleLibrary().importFile (file);
    if (name.isEmpty())
    {
        showNotice ("Couldn't import - needs WAV/AIFF/FLAC/CAF, <= 5 s, <= 2 ch");
        return;
    }

    proc.loadSampleByName (name);

    if (auto* en = proc.getValueTreeState().getParameter (kickr::id::sampleEnable))
        en->setValueNotifyingHost (1.0f);   // drop -> sample layer on

    updateSampleLabel();
    showNotice ("Added  " + name);
}

//================================================================ Phase 3.3 — notice
void KICKRAudioProcessorEditor::showNotice (const juce::String& text)
{
    noticeLabel.setText (text, juce::dontSendNotification);
    noticeLabel.setVisible (true);
    noticeLabel.toFront (false);

    juce::Timer::callAfterDelay (3800, [safe = juce::Component::SafePointer<KICKRAudioProcessorEditor> (this)]
    {
        if (safe != nullptr) safe->clearNotice();
    });
}

void KICKRAudioProcessorEditor::clearNotice()
{
    noticeLabel.setVisible (false);
}

void KICKRAudioProcessorEditor::mouseDown (const juce::MouseEvent& e)
{
    if (e.eventComponent == &presetNameLabel)
        showPresetMenu();
    else if (e.eventComponent == &wordmark)   // 2026-09-02
        aboutPage.show();
}

void KICKRAudioProcessorEditor::enforceMidiPitchMode()
{
    // 2026-09-02 — the TUNING cell is gone; the mode has no UI any more and must always
    // read MIDI Pitch (index 0). A preset / session / undo step that carried "Fixed
    // Frequency" is corrected here (message thread, at the same refresh points as the
    // sample label). Headless use (tests) is unaffected — this is editor-only.
    if (auto* p = proc.getValueTreeState().getParameter (kickr::id::tuneMode))
        if (p->getValue() > 0.0f)
            p->setValueNotifyingHost (0.0f);
}

void KICKRAudioProcessorEditor::updateSampleLabel()
{
    enforceMidiPitchMode();   // 2026-09-02 — shares every state-refresh point

    auto& lib = proc.getSampleLibrary();
    const auto n = proc.getCurrentSampleName();

    juce::String text;
    juce::Colour col = pal::lowfam;

    // 2026-08-31: "missing" / "N in the bank" now consider the COMBINED bank (50 shipped
    // factory kicks + your own imports) — indexOfNameTotal(), not the disk-only
    // indexOfName(), so recalling a factory sample never wrongly reads as "missing".
    if (n.isNotEmpty() && lib.indexOfNameTotal (n) < 0)
    {
        text = "sample missing - " + n;   // recall, file gone
        col  = pal::red;
    }
    else if (n.isNotEmpty())
    {
        text = n;
    }
    else if (lib.getTotalCount() == 0)
    {
        // Defensive only — can't actually happen with the factory bank linked in, kept
        // in case BinaryData ever fails to link (would mean 0 factory + 0 user samples).
        text = "- drop a kick here -";
        col  = pal::inkDim;
    }
    else
    {
        text = juce::String::fromUTF8 ("\xe2\x97\x84 \xe2\x96\xba  ") + juce::String (lib.getTotalCount())
             + (lib.getTotalCount() == 1 ? " kick in the bank" : " kicks in the bank");
        col  = pal::inkDim;
    }

    sampleNameLabel.setText (text, juce::dontSendNotification);
    sampleNameLabel.setColour (juce::Label::textColourId, col);

    sampleWaveform.setSample (lib, n);
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
    aboutPage.setBounds (getLocalBounds());   // 2026-09-02 — overlay covers the whole editor
    aboutPage.setScale (scaleFactor);

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

    // 2026-09-01 (user request) — carve ~1/6 off the scope panel's left edge for the
    // Morph knob, without changing the panel's own outer size.
    auto morphSlot = scope.removeFromLeft (scope.getWidth() / 6);
    scope.removeFromLeft (gap);
    layoutMorph (morphSlot);

    scopeBounds  = scope;
    sampleBounds = sample;
    engineBounds = engine;

    layoutHeader (header);
    layoutScope  (scope);
    layoutSample (sample);
    layoutHeroes (heroes);
    layoutEngine (engine);

    // Notice strip — centred over the top of the scope panel.
    noticeLabel.setBounds (scopeBounds.withHeight (scaled (26)).reduced (scaled (8), scaled (4)));
    noticeLabel.setFont (kickr::KickrLookAndFeel::titleFont ((float) scaled (11)));
}

void KICKRAudioProcessorEditor::layoutHeader (juce::Rectangle<int> a)
{
    // 2026-09-02 (user request: "make the KICKR logo bigger"): 20 -> 30 px, 104 -> 150 wide.
    wordmark.setFont (kickr::KickrLookAndFeel::titleFont ((float) scaled (30)));
    wordmark.setBounds (a.removeFromLeft (scaled (150)));

    versionLabel.setFont (kickr::KickrLookAndFeel::microFont ((float) scaled (10)));
    versionLabel.setBounds (a.removeFromLeft (scaled (44)));

    auto btns = a.removeFromRight (scaled (600));
    const int bw = btns.getWidth() / 6;
    for (auto* b : { &saveButton, &undoButton, &redoButton, &randomButton, &mutateButton, &abButton })
        b->setBounds (btns.removeFromLeft (bw).reduced (scaled (3)));

    // 2026-09-02 (user request: "move them a bit closer to each other"): the two arrows
    // used to sit at the far ends of the whole header gap; now they hug a fixed-width
    // name area centred in it.
    // Follow-up (same day, "now the arrows are way too close move them half of what you
    // moved them before"): the group is now the midpoint between the old far-ends layout
    // (the full pill width) and the tight 200 px name area.
    auto pill = a.reduced (scaled (14), scaled (6));
    const int arrowW = scaled (26), nameW = scaled (200), gap = scaled (6);
    const int tightW = nameW + 2 * (arrowW + gap);
    const int groupW = juce::jmin (pill.getWidth(), (pill.getWidth() + tightW) / 2);
    auto group = pill.withSizeKeepingCentre (groupW, pill.getHeight());
    presetPrev.setBounds (group.removeFromLeft (arrowW));
    presetNext.setBounds (group.removeFromRight (arrowW));
    presetNameLabel.setBounds (group.reduced (gap, 0));
    presetNameLabel.setFont (kickr::KickrLookAndFeel::titleFont ((float) scaled (11)));
}

void KICKRAudioProcessorEditor::layoutMorph (juce::Rectangle<int> a)
{
    if (morphKnob != nullptr)
        morphKnob->setBounds (a.reduced (scaled (8)));
}

void KICKRAudioProcessorEditor::layoutScope (juce::Rectangle<int> a)
{
    // Both analyzers fill the whole scope panel; they paint their own recessed
    // dark radial-gradient background. The mode buttons float on top, top-right.
    waveDisplay.setBounds (a);
    spectrumDisplay.setBounds (a);

    filterDisplay.setBounds (a);   // 2026-09-02 — third page

    auto modes = a.reduced (scaled (14)).removeFromTop (scaled (22)).removeFromRight (scaled (224));
    scopeFilterButton.setBounds (modes.removeFromRight (scaled (66)));
    modes.removeFromRight (scaled (4));
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

    // Small static waveform preview fills the gap left between the name row and the
    // prev/next nav row.
    slot.removeFromTop (scaled (4));
    slot.removeFromBottom (scaled (4));
    sampleWaveform.setBounds (slot);

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
        // 2026-09-02: LIMITER (slot 5) takes slots 5 + 6; slot 6 itself is empty.
        if (i == 6)
        {
            engineRects[6] = {};
            continue;
        }
        const int span = (i == 5) ? 2 : 1;
        juce::Rectangle<int> cell (a.getX() + i * cw, a.getY(), cw * span, a.getHeight());
        if (i == 5)
            cell.setRight (a.getRight());   // absorb the /7 rounding remainder
        engineRects[(size_t) i] = cell;

        auto inner = cell.reduced (scaled (10));

        if (engineTitles[(size_t) i] != nullptr)
        {
            engineTitles[(size_t) i]->setBounds (inner.removeFromTop (scaled (18)));
            engineTitles[(size_t) i]->setFont (kickr::KickrLookAndFeel::titleFont ((float) scaled (11)));
        }
        inner.removeFromTop (scaled (4));

        if (i == 5 && engineCombos[5] != nullptr && engineExtra[5] != nullptr)
        {
            // LIMITER: on/off pill (left) and oversampling selector (right) share one row.
            auto row = inner.removeFromTop (scaled (22));
            engineExtra[5]->setBounds (row.removeFromLeft (row.getWidth() / 2).withTrimmedRight (scaled (4)));
            engineCombos[5]->setBounds (row.withTrimmedLeft (scaled (4)));
            inner.removeFromTop (scaled (6));
        }
        else
        {
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
        }

        placeGrid (inner, engineCells[(size_t) i], i == 5 ? 3 : 2, scaled (4));   // LIMITER cell: 3 x 2
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
    for (int i = 1; i < 6; ++i)   // 2026-09-02: no divider inside the double-width LIMITER cell
        g.drawVerticalLine (engineRects[(size_t) i].getX(),
                            (float) engineBounds.getY() + sf (10),
                            (float) engineBounds.getBottom() - sf (10));

    // ---- oscilloscope (Phase 3.2) ----
    // The scope panel is now owned + painted by `waveDisplay` / `spectrumDisplay`
    // (dark radial-gradient background, grid, trace, ms/Hz axes, KICKR watermark).
    // The editor only reserves `scopeBounds` for them in resized().

    // ---- Phase 3.3: audio-file drag highlight over the whole editor ----
    if (fileDragActive)
    {
        auto hi = getLocalBounds().toFloat().reduced (sf (8));
        g.setColour (pal::lowfam.withAlpha (0.55f));
        g.drawRoundedRectangle (hi.reduced (2.0f), sf (42), sf (3));
        g.setColour (pal::lowfam.withAlpha (0.08f));
        g.fillRoundedRectangle (hi, sf (42));
    }
}
