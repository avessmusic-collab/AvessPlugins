// CHOPLINE — FxPage implementation. See header.

#include "FxPage.h"

#include "../engine/MasterFX.h"
#include "../state/Parameters.h"

#include <cmath>

namespace chopline::ui
{

using LnF = ChoplineLookAndFeel;

FxPage::FxPage (engine::Engine& engineToUse, juce::AudioProcessorValueTreeState& apvts)
    : eng (engineToUse), state (apvts)
{
    // PLAYBACK card — mirrors of the overlay controls (v17 mirror_of).
    modeBox.setName ("Mode");
    modeBox.addItem ("One-shot", 1);
    modeBox.addItem ("Gate", 2);
    modeBox.addItem ("Loop", 3);
    modeBox.addItem ("Fit step", 4);
    modeBox.onChange = [this]
    {
        if (refreshing || modeBox.getSelectedId() == 0)
            return;
        auto s = settings();
        s.mode = static_cast<engine::PlaybackMode> (modeBox.getSelectedId() - 1);
        setSettings (s);
    };
    addAndMakeVisible (modeBox);

    reverseToggle.onClick = [this]
    {
        if (refreshing)
            return;
        auto s = settings();
        s.reverse = reverseToggle.getToggleState();
        setSettings (s);
    };
    addAndMakeVisible (reverseToggle);

    warpSeg.options = { "Varispeed", "Stretch", "Granular", "Freeze" };
    warpSeg.getValue = [this] { return static_cast<int> (settings().warp); };
    warpSeg.setValue = [this] (int v)
    {
        auto s = settings();
        s.warp = static_cast<engine::WarpMode> (v);
        setSettings (s);
    };
    addAndMakeVisible (warpSeg);

    prevButton.onClick = [this]
    {
        if (setChop != nullptr && getChop != nullptr)
            setChop (juce::jmax (0, getChop() - 1));
    };
    nextButton.onClick = [this]
    {
        if (setChop != nullptr && getChop != nullptr)
            setChop (juce::jmin (juce::jmax (0, eng.getNumChops() - 1), getChop() + 1));
    };
    addAndMakeVisible (prevButton);
    addAndMakeVisible (nextButton);

    addAndMakeVisible (envDisplay);

    auto setupKnob = [this] (CompactKnob& k, double lo, double hi, double step, double resetTo,
                             std::function<void (engine::ChopSettings&, double)> apply)
    {
        k.getSlider().setRange (lo, hi, step);
        k.getSlider().setDoubleClickReturnValue (true, resetTo);
        k.onValueChange = [this, fn = std::move (apply)] (double v)
        {
            if (refreshing)
                return;
            auto s = settings();
            fn (s, v);
            setSettings (s);
        };
        addAndMakeVisible (k);
    };

    setupKnob (attackKnob, 0.0, 500.0, 1.0, 2.0,
               [] (engine::ChopSettings& s, double v) { s.attackMs = static_cast<float> (v); });
    attackKnob.valueSuffix = " ms";
    setupKnob (holdKnob, 0.0, 500.0, 1.0, 0.0,
               [] (engine::ChopSettings& s, double v) { s.holdMs = static_cast<float> (v); });
    holdKnob.valueSuffix = " ms";
    setupKnob (decayKnob, 5.0, 1000.0, 1.0, 160.0,
               [] (engine::ChopSettings& s, double v) { s.decayMs = static_cast<float> (v); });
    decayKnob.valueSuffix = " ms";
    setupKnob (sustainKnob, 0.0, 100.0, 1.0, 100.0,
               [] (engine::ChopSettings& s, double v) { s.sustainPct = static_cast<float> (v); });
    sustainKnob.valueSuffix = "%";
    setupKnob (releaseKnob, 5.0, 500.0, 1.0, 80.0,
               [] (engine::ChopSettings& s, double v) { s.releaseMs = static_cast<float> (v); });
    releaseKnob.valueSuffix = " ms";

    buildMasterColumn();

    // Tooltips (spec §9, Phase 5 polish).
    attackKnob.setTooltip ("Amp envelope attack");
    holdKnob.setTooltip ("Amp envelope hold");
    decayKnob.setTooltip ("Amp envelope decay");
    sustainKnob.setTooltip ("Amp envelope sustain");
    releaseKnob.setTooltip ("Amp envelope release (= chop RELEASE)");
    fxCutoff.setTooltip ("Master filter cutoff");
    fxReso.setTooltip ("Master filter resonance");
    fxDrive.setTooltip ("Master distortion drive");
    fxDistMix.setTooltip ("Master distortion dry/wet");
    fxWidth.setTooltip ("Stereo width (mid/side)");
    fxMono.setTooltip ("Mono below this frequency");
    fxOut.setTooltip ("Output gain into the safety limiter");
    fxCeiling.setTooltip ("Limiter ceiling");
    fxDelayFb.setTooltip ("Delay feedback");
    fxDelayRtn.setTooltip ("Delay return level (fed by chop DELAY sends)");
    fxVerbDecay.setTooltip ("Reverb decay");
    fxVerbDamp.setTooltip ("Reverb damping");
    fxVerbRtn.setTooltip ("Reverb return level (fed by chop VERB sends)");

    eng.addChangeListener (this);
    refresh();
}

FxPage::~FxPage()
{
    eng.removeChangeListener (this);
}

// ---- MASTER column (Phase 4) ---------------------------------------------------------

void FxPage::buildMasterColumn()
{
    namespace pid = params::id;

    const auto attachKnob = [this] (CompactKnob& knob, const char* paramId)
    {
        addAndMakeVisible (knob);
        if (auto* p = dynamic_cast<juce::RangedAudioParameter*> (state.getParameter (paramId)))
            knobAttachments.push_back (
                std::make_unique<juce::SliderParameterAttachment> (*p, knob.getSlider()));
    };

    // Sections: card rects + titles + bypass params (v17 geometry).
    const struct { juce::Rectangle<int> card; const char* title; const char* onId;
                   juce::Rectangle<int> toggle; } defs[6] = {
        { { 458, 92, 560, 108 },  "FILTER",     pid::masterFilterOn,  { 980, 97, 28, 20 } },
        { { 458, 215, 560, 108 }, "DISTORTION", pid::masterDistOn,    { 980, 220, 28, 20 } },
        { { 458, 338, 274, 108 }, "WIDTH",      pid::masterWidthOn,   { 694, 343, 28, 20 } },
        { { 744, 338, 274, 108 }, "OUTPUT",     pid::masterLimiterOn, { 980, 343, 28, 20 } },
        { { 458, 461, 560, 108 }, "DELAY",      pid::masterDelayOn,   { 980, 466, 28, 20 } },
        { { 458, 584, 560, 108 }, "REVERB",     pid::masterVerbOn,    { 980, 589, 28, 20 } },
    };
    for (size_t i = 0; i < 6; ++i)
    {
        auto& s = sections[i];
        s.card = defs[i].card;
        s.title = defs[i].title;
        s.onParam = state.getRawParameterValue (defs[i].onId);
        s.bypass.setButtonText ({});
        s.bypass.setTooltip (juce::String (defs[i].title).toLowerCase() + " on/off");
        s.bypass.setBounds (defs[i].toggle);
        addAndMakeVisible (s.bypass);
        if (auto* p = state.getParameter (defs[i].onId))
            s.attachment = std::make_unique<juce::ButtonParameterAttachment> (*p, s.bypass);
    }

    // FILTER — type seg + cutoff/reso + response curve (12 dB/oct, master-only).
    filterTypeSeg.options = { "LP", "BP", "HP" };
    if (auto* p = state.getParameter (pid::masterFilterType))
    {
        filterTypeAttachment = std::make_unique<juce::ParameterAttachment> (
            *p, [this] (float) { filterTypeSeg.repaint(); });
        filterTypeSeg.getValue = [p]
        {
            return static_cast<int> (std::lround (p->convertFrom0to1 (p->getValue())));
        };
        filterTypeSeg.setValue = [this] (int v)
        {
            filterTypeAttachment->setValueAsCompleteGesture (static_cast<float> (v));
        };
    }
    addAndMakeVisible (filterTypeSeg);
    attachKnob (fxCutoff, pid::masterFilterCutoff);
    fxCutoff.formatValue = [] (double v)
    {
        return v >= 1000.0 ? juce::String (v / 1000.0, 1) + "k"
                           : juce::String (static_cast<int> (v));
    };
    attachKnob (fxReso, pid::masterFilterReso);
    fxReso.valueSuffix = "%";
    filterCurve.type = state.getRawParameterValue (pid::masterFilterType);
    filterCurve.cutoff = state.getRawParameterValue (pid::masterFilterCutoff);
    filterCurve.reso = state.getRawParameterValue (pid::masterFilterReso);
    addAndMakeVisible (filterCurve);
    sections[0].content = { &filterTypeSeg, &fxCutoff, &fxReso, &filterCurve };

    // DISTORTION — 6 types (Tape default = the absorbed v14 master saturation).
    distTypeBox.setName ("DIST");
    distTypeBox.addItemList ({ "Soft clip", "Hard clip", "Tape", "Fold",
                               "Bitcrush", "Downsample" }, 1);
    addAndMakeVisible (distTypeBox);
    if (auto* p = dynamic_cast<juce::RangedAudioParameter*> (
            state.getParameter (pid::masterDistType)))
        distTypeAttachment = std::make_unique<juce::ComboBoxParameterAttachment> (*p, distTypeBox);
    addAndMakeVisible (autoGainToggle);
    if (auto* p = state.getParameter (pid::masterDistAutoGain))
        autoGainAttachment = std::make_unique<juce::ButtonParameterAttachment> (*p, autoGainToggle);
    attachKnob (fxDrive, pid::masterDistDrive);
    fxDrive.valueSuffix = "%";
    attachKnob (fxDistMix, pid::masterDistMix);
    fxDistMix.valueSuffix = "%";
    distCurve.type = state.getRawParameterValue (pid::masterDistType);
    distCurve.drive = state.getRawParameterValue (pid::masterDistDrive);
    distCurve.mix = state.getRawParameterValue (pid::masterDistMix);
    distCurve.autoGain = state.getRawParameterValue (pid::masterDistAutoGain);
    addAndMakeVisible (distCurve);
    sections[1].content = { &distTypeBox, &autoGainToggle, &fxDrive, &fxDistMix, &distCurve };

    // WIDTH — width + mono-below + stereo field.
    attachKnob (fxWidth, pid::masterWidth);
    fxWidth.valueSuffix = "%";
    attachKnob (fxMono, pid::masterMono);
    fxMono.valueSuffix = " Hz";
    widthScope.width = state.getRawParameterValue (pid::masterWidth);
    widthScope.mono = state.getRawParameterValue (pid::masterMono);
    addAndMakeVisible (widthScope);
    sections[2].content = { &fxWidth, &fxMono, &widthScope };

    // OUTPUT — gain + ceiling + out/GR meter.
    attachKnob (fxOut, pid::masterOutput);
    fxOut.valueSuffix = " dB";
    attachKnob (fxCeiling, pid::masterCeiling);
    fxCeiling.valueSuffix = " dB";
    addAndMakeVisible (outMeter);
    sections[3].content = { &fxOut, &fxCeiling, &outMeter };

    // DELAY — tempo-synced time + feedback + return + taps.
    delayTimeBox.addItemList ({ "1/16", "1/8", "1/8.", "1/4", "1/4.", "1/2",
                                "1/8T", "1/4T" }, 1);
    addAndMakeVisible (delayTimeBox);
    if (auto* p = dynamic_cast<juce::RangedAudioParameter*> (
            state.getParameter (pid::masterDelayTime)))
        delayTimeAttachment = std::make_unique<juce::ComboBoxParameterAttachment> (*p, delayTimeBox);
    attachKnob (fxDelayFb, pid::masterDelayFb);
    fxDelayFb.valueSuffix = "%";
    attachKnob (fxDelayRtn, pid::masterDelayRtn);
    fxDelayRtn.valueSuffix = "%";
    delayTaps.time = state.getRawParameterValue (pid::masterDelayTime);
    delayTaps.feedback = state.getRawParameterValue (pid::masterDelayFb);
    addAndMakeVisible (delayTaps);
    sections[4].content = { &delayTimeBox, &fxDelayFb, &fxDelayRtn, &delayTaps };

    // REVERB — small plate: decay + damp + return + tail.
    attachKnob (fxVerbDecay, pid::masterVerbDecay);
    fxVerbDecay.valueSuffix = "%";
    attachKnob (fxVerbDamp, pid::masterVerbDamp);
    fxVerbDamp.valueSuffix = "%";
    attachKnob (fxVerbRtn, pid::masterVerbRtn);
    fxVerbRtn.valueSuffix = "%";
    verbTail.decay = state.getRawParameterValue (pid::masterVerbDecay);
    verbTail.damp = state.getRawParameterValue (pid::masterVerbDamp);
    addAndMakeVisible (verbTail);
    sections[5].content = { &fxVerbDecay, &fxVerbDamp, &fxVerbRtn, &verbTail };
}

void FxPage::animationTick()
{
    if (! isVisible())
        return;

    // Bypass dimming (v17: bypassed module dims to 45 %).
    for (auto& s : sections)
    {
        const auto off = s.onParam != nullptr && s.onParam->load (std::memory_order_relaxed) < 0.5f;
        if (off != s.dimmed)
        {
            s.dimmed = off;
            for (auto* c : s.content)
                c->setAlpha (off ? 0.45f : 1.0f);
            repaint (s.card);
        }
    }

    // Displays repaint only when their underlying values change.
    filterCurve.refreshIfChanged();
    distCurve.refreshIfChanged();
    widthScope.refreshIfChanged();
    delayTaps.bpm = eng.getUiBpm();
    delayTaps.refreshIfChanged();
    verbTail.refreshIfChanged();

    outMeter.peak = eng.getMasterOutPeak();
    outMeter.grDb = eng.getMasterGainReductionDb();
    outMeter.refreshIfChanged();

    const auto bpm = eng.getUiBpm();
    if (std::abs (bpm - shownBpm) > 0.049)
    {
        shownBpm = bpm;
        repaint (900, 60, 124, 26);
    }
}

void FxPage::changeListenerCallback (juce::ChangeBroadcaster*)
{
    if (isVisible()) // hidden page re-syncs when shown (setPage calls refresh)
        refresh();
}

void FxPage::refresh()
{
    refreshing = true;
    const auto s = settings();
    modeBox.setSelectedId (static_cast<int> (s.mode) + 1, juce::dontSendNotification);
    reverseToggle.setToggleState (s.reverse, juce::dontSendNotification);
    attackKnob.setValueQuiet (s.attackMs);
    holdKnob.setValueQuiet (s.holdMs);
    decayKnob.setValueQuiet (s.decayMs);
    sustainKnob.setValueQuiet (s.sustainPct);
    releaseKnob.setValueQuiet (s.releaseMs);
    warpSeg.repaint();
    envDisplay.repaint();
    refreshing = false;
    repaint();
}

void FxPage::resized()
{
    // v17 canvas-absolute geometry (the page fills the 1100x720 canvas).
    modeBox.setBounds (96, 128, 158, 28);
    reverseToggle.setBounds (270, 128, 158, 28);
    warpSeg.setBounds (96, 166, 332, 28);
    prevButton.setBounds (382, 62, 24, 20);
    nextButton.setBounds (418, 62, 24, 20);
    envDisplay.setBounds (96, 253, 332, 176);
    attackKnob.setBounds (96, 441, 50, 54);
    holdKnob.setBounds (167, 441, 50, 54);
    decayKnob.setBounds (237, 441, 50, 54);
    sustainKnob.setBounds (308, 441, 50, 54);
    releaseKnob.setBounds (378, 441, 50, 54);

    // MASTER column (bypass toggles were positioned in buildMasterColumn).
    filterTypeSeg.setBounds (472, 145, 120, 28);
    fxCutoff.setBounds (608, 132, 50, 54);
    fxReso.setBounds (670, 132, 50, 54);
    filterCurve.setBounds (732, 132, 272, 54);

    distTypeBox.setBounds (472, 253, 150, 24);
    autoGainToggle.setBounds (472, 287, 150, 24);
    fxDrive.setBounds (638, 255, 50, 54);
    fxDistMix.setBounds (700, 255, 50, 54);
    distCurve.setBounds (762, 253, 242, 58);

    fxWidth.setBounds (472, 378, 50, 54);
    fxMono.setBounds (534, 378, 50, 54);
    widthScope.setBounds (596, 378, 122, 54);

    fxOut.setBounds (758, 378, 50, 54);
    fxCeiling.setBounds (820, 378, 50, 54);
    outMeter.setBounds (882, 378, 122, 54);

    delayTimeBox.setBounds (472, 514, 120, 28);
    fxDelayFb.setBounds (608, 501, 50, 54);
    fxDelayRtn.setBounds (670, 501, 50, 54);
    delayTaps.setBounds (732, 501, 272, 54);

    fxVerbDecay.setBounds (472, 624, 50, 54);
    fxVerbDamp.setBounds (534, 624, 50, 54);
    fxVerbRtn.setBounds (596, 624, 50, 54);
    verbTail.setBounds (658, 624, 346, 54);
}

void FxPage::paint (juce::Graphics& g)
{
    // Column titles (transparent containers — v17 layout_fx).
    g.setColour (LnF::label);
    g.setFont (LnF::labelFont (8.5f));
    g.drawText ("CHOP", juce::Rectangle<int> (82, 64, 80, 14),
                juce::Justification::centredLeft, false);
    g.drawText ("MASTER", juce::Rectangle<int> (458, 64, 80, 14),
                juce::Justification::centredLeft, false);

    // Chip: swatch + CHOP nn (v17 fx_chop_chip at 254,63).
    {
        const auto chop = chopIndex();
        const auto shade = LnF::chopShade (chop);
        const auto swatch = juce::Rectangle<float> (254.0f, 67.0f, 10.0f, 10.0f);
        g.setColour (shade);
        g.fillRoundedRectangle (swatch, 2.5f);
        g.setColour (LnF::accent);
        g.drawRoundedRectangle (swatch.expanded (1.5f), 3.5f, 1.0f);
        g.setColour (LnF::label);
        g.setFont (LnF::labelFont (8.5f));
        g.drawText ("CHOP " + juce::String (chop + 1),
                    juce::Rectangle<int> (272, 63, 98, 18),
                    juce::Justification::centredLeft, false);
    }

    // BPM readout mirror (v17 fx_bpm at 906,65).
    {
        const auto readout = juce::Rectangle<int> (906, 65, 112, 18);
        g.setColour (LnF::display);
        g.fillRoundedRectangle (readout.toFloat(), 6.0f);
        g.setColour (LnF::hairline());
        g.drawRoundedRectangle (readout.toFloat().reduced (0.5f), 6.0f, 1.0f);
        g.setColour (LnF::textTertiary);
        g.setFont (LnF::labelFont (6.5f));
        g.drawText ("BPM", readout.reduced (8, 0), juce::Justification::centredLeft, false);
        g.setColour (LnF::ink);
        g.setFont (LnF::monoFont (9.0f));
        g.drawText (juce::String (shownBpm, 1), readout.reduced (8, 0),
                    juce::Justification::centredRight, false);
    }

    const auto card = [&g] (juce::Rectangle<float> r, const juce::String& title, float alpha)
    {
        g.setColour (LnF::cell.withMultipliedAlpha (alpha));
        g.fillRoundedRectangle (r, 12.0f);
        g.setColour (LnF::hairline().withMultipliedAlpha (alpha));
        g.drawRoundedRectangle (r.reduced (0.5f), 12.0f, 1.0f);
        g.setColour (LnF::label.withMultipliedAlpha (alpha));
        g.setFont (LnF::labelFont (8.5f));
        g.drawText (title, juce::Rectangle<int> (static_cast<int> (r.getX()) + 10,
                                                 static_cast<int> (r.getY()) + 8, 200, 12),
                    juce::Justification::centredLeft, false);
    };

    card ({ 82.0f, 92.0f, 360.0f, 114.0f }, "PLAYBACK", 1.0f);
    card ({ 82.0f, 221.0f, 360.0f, 288.0f }, "ENVELOPE", 1.0f);

    for (const auto& s : sections)
        card (s.card.toFloat(), s.title, s.dimmed ? 0.45f : 1.0f);
}

// ---- master displays ---------------------------------------------------------------------

void FxPage::MasterDisplay::paint (juce::Graphics& g)
{
    const auto area = getLocalBounds().toFloat();
    g.setColour (LnF::display);
    g.fillRoundedRectangle (area, 10.0f);
    {
        juce::Graphics::ScopedSaveState save (g);
        juce::Path clip;
        clip.addRoundedRectangle (area, 10.0f);
        g.reduceClipRegion (clip);

        g.setColour (LnF::gridLine());
        for (auto gx = 22.0f; gx < area.getWidth(); gx += 22.0f)
            g.drawVerticalLine (static_cast<int> (gx), 0.0f, area.getHeight());
        for (auto gy = 22.0f; gy < area.getHeight(); gy += 22.0f)
            g.drawHorizontalLine (static_cast<int> (gy), 0.0f, area.getWidth());

        drawContent (g, area.reduced (6.0f, 6.0f));
    }
    g.setColour (LnF::hairline());
    g.drawRoundedRectangle (area.reduced (0.5f), 10.0f, 1.0f);
}

bool FxPage::MasterDisplay::refreshIfChanged()
{
    const auto sig = signature();
    if (juce::exactlyEqual (sig, lastSignature))
        return false;
    lastSignature = sig;
    repaint();
    return true;
}

namespace
{
    inline float atomicValue (std::atomic<float>* a, float fallback = 0.0f)
    {
        return a != nullptr ? a->load (std::memory_order_relaxed) : fallback;
    }
} // namespace

void FxPage::FilterCurve::drawContent (juce::Graphics& g, juce::Rectangle<float> r)
{
    const auto t = static_cast<int> (atomicValue (type));
    const auto fc = juce::jlimit (20.0f, 20000.0f, atomicValue (cutoff, 20000.0f));
    const auto q = 0.707f + 7.3f * atomicValue (reso, 10.0f) / 100.0f;

    juce::Path p;
    const auto n = 64;
    for (int i = 0; i <= n; ++i)
    {
        const auto f = 20.0f * std::pow (1000.0f, static_cast<float> (i) / n); // 20..20k log
        const auto w = f / fc;
        const auto w2 = w * w;
        float mag;
        if (t == 0)      mag = 1.0f / std::sqrt (juce::square (1.0f - w2) + w2 / (q * q));
        else if (t == 1) mag = (w / q) / std::sqrt (juce::square (1.0f - w2) + w2 / (q * q));
        else             mag = w2 / std::sqrt (juce::square (1.0f - w2) + w2 / (q * q));
        const auto db = juce::jlimit (-30.0f, 18.0f, juce::Decibels::gainToDecibels (mag, -60.0f));
        const auto x = r.getX() + r.getWidth() * static_cast<float> (i) / n;
        const auto y = juce::jmap (db, -30.0f, 18.0f, r.getBottom(), r.getY());
        if (i == 0)
            p.startNewSubPath (x, y);
        else
            p.lineTo (x, y);
    }
    g.setColour (LnF::ink);
    g.strokePath (p, juce::PathStrokeType (1.7f));

    // Cutoff marker (thin blue accent, v17).
    const auto cx = r.getX() + r.getWidth()
                        * (std::log (fc / 20.0f) / std::log (1000.0f));
    g.setColour (LnF::accent);
    g.drawLine (cx, r.getY(), cx, r.getBottom(), 1.0f);
}

float FxPage::FilterCurve::signature() const
{
    return atomicValue (type) * 10.0f + atomicValue (cutoff) * 0.001f + atomicValue (reso);
}

void FxPage::DistCurve::drawContent (juce::Graphics& g, juce::Rectangle<float> r)
{
    const auto t = static_cast<int> (atomicValue (type));
    const auto d01 = atomicValue (drive, 10.0f) / 100.0f;
    const auto m = atomicValue (mix, 100.0f) / 100.0f;
    const auto ag = atomicValue (autoGain, 1.0f) >= 0.5f
                        ? engine::MasterFX::autoGainFor (t, d01) : 1.0f;

    juce::Path p;
    const auto n = 96;
    for (int i = 0; i <= n; ++i)
    {
        const auto x01 = static_cast<float> (i) / n * 2.0f - 1.0f;
        auto y01 = t == 5 ? x01 // Downsample: identity transfer (rate reduction)
                          : engine::MasterFX::shapeSample (t, x01, d01) * ag;
        y01 = x01 * (1.0f - m) + y01 * m;
        y01 = juce::jlimit (-1.2f, 1.2f, y01);
        const auto x = r.getX() + r.getWidth() * static_cast<float> (i) / n;
        const auto y = juce::jmap (y01, -1.2f, 1.2f, r.getBottom(), r.getY());
        if (i == 0)
            p.startNewSubPath (x, y);
        else
            p.lineTo (x, y);
    }
    g.setColour (LnF::ink);
    g.strokePath (p, juce::PathStrokeType (1.7f));
}

float FxPage::DistCurve::signature() const
{
    return atomicValue (type) * 1000.0f + atomicValue (drive) + atomicValue (mix) * 0.001f
         + atomicValue (autoGain) * 7777.0f;
}

void FxPage::WidthScope::drawContent (juce::Graphics& g, juce::Rectangle<float> r)
{
    // Stereo field: mid axis + an ellipse whose side radius follows WIDTH.
    const auto w01 = atomicValue (width, 100.0f) / 200.0f; // 0..1
    const auto c = r.getCentre();
    const auto midR = juce::jmin (r.getWidth(), r.getHeight()) * 0.42f;
    const auto sideR = midR * juce::jmax (0.02f, w01);

    juce::Path ellipse;
    ellipse.addEllipse (c.x - sideR, c.y - midR * 0.8f, sideR * 2.0f, midR * 1.6f);
    ellipse.applyTransform (juce::AffineTransform::rotation (
        -juce::MathConstants<float>::pi / 4.0f, c.x, c.y));
    g.setColour (LnF::ink);
    g.strokePath (ellipse, juce::PathStrokeType (1.5f));

    // Mono-below caption (small, faint).
    g.setColour (LnF::textTertiary);
    g.setFont (LnF::monoFont (8.0f));
    g.drawText ("<" + juce::String (static_cast<int> (atomicValue (mono, 120.0f))) + "Hz mono",
                r.toNearestInt().removeFromBottom (10), juce::Justification::centredRight, false);
}

float FxPage::WidthScope::signature() const
{
    return atomicValue (width) + atomicValue (mono) * 0.001f;
}

void FxPage::OutMeter::drawContent (juce::Graphics& g, juce::Rectangle<float> r)
{
    // Output peak bar (ink) + gain-reduction bar (blue, v17 faders_meters).
    auto bars = r.reduced (10.0f, 4.0f);
    auto peakBar = bars.removeFromLeft (bars.getWidth() * 0.5f - 6.0f);
    bars.removeFromLeft (12.0f);
    auto grBar = bars;

    const auto track = [&g] (juce::Rectangle<float> b)
    {
        g.setColour (juce::Colours::black.withAlpha (0.16f));
        g.fillRoundedRectangle (b.withSizeKeepingCentre (4.0f, b.getHeight()), 2.0f);
    };
    track (peakBar);
    track (grBar);

    const auto peakDb = juce::Decibels::gainToDecibels (juce::jmax (1.0e-4f, peak), -60.0f);
    const auto peakNorm = juce::jlimit (0.0f, 1.0f, juce::jmap (peakDb, -60.0f, 6.0f, 0.0f, 1.0f));
    auto pFill = peakBar.withSizeKeepingCentre (4.0f, peakBar.getHeight());
    pFill = pFill.removeFromBottom (pFill.getHeight() * peakNorm);
    g.setColour (LnF::ink);
    g.fillRoundedRectangle (pFill, 2.0f);

    const auto grNorm = juce::jlimit (0.0f, 1.0f, -grDb / 12.0f);
    auto gFill = grBar.withSizeKeepingCentre (4.0f, grBar.getHeight());
    gFill = gFill.removeFromTop (gFill.getHeight() * grNorm);
    g.setColour (LnF::accent); // blue while limiting
    g.fillRoundedRectangle (gFill, 2.0f);

    g.setColour (LnF::textTertiary);
    g.setFont (LnF::labelFont (6.5f));
    g.drawText ("OUT", peakBar.toNearestInt().removeFromBottom (9).translated (0, 6),
                juce::Justification::centred, false);
    g.drawText ("GR", grBar.toNearestInt().removeFromBottom (9).translated (0, 6),
                juce::Justification::centred, false);
}

float FxPage::OutMeter::signature() const
{
    return peak * 100.0f + grDb;
}

void FxPage::DelayTaps::drawContent (juce::Graphics& g, juce::Rectangle<float> r)
{
    const auto idx = static_cast<int> (atomicValue (time, 2.0f));
    const auto fb01 = atomicValue (feedback, 35.0f) / 100.0f;
    const auto seconds = params::delayTimeBeats (idx) * 60.0
                             / juce::jlimit (20.0, 999.0, bpm > 1.0 ? bpm : 120.0);
    const auto span = 3.0; // display window in seconds

    g.setColour (LnF::ink);
    auto level = 1.0f;
    for (int k = 1; k <= 8; ++k)
    {
        const auto t = seconds * k;
        if (t > span)
            break;
        const auto x = r.getX() + r.getWidth() * static_cast<float> (t / span);
        const auto h = r.getHeight() * juce::jmax (0.06f, level);
        g.drawLine (x, r.getBottom() - h, x, r.getBottom(), 2.0f);
        level *= fb01;
    }
}

float FxPage::DelayTaps::signature() const
{
    return atomicValue (time) * 100.0f + atomicValue (feedback)
         + static_cast<float> (bpm) * 0.01f;
}

void FxPage::VerbTail::drawContent (juce::Graphics& g, juce::Rectangle<float> r)
{
    const auto decay01 = atomicValue (decay, 40.0f) / 100.0f;
    const auto damp01 = atomicValue (damp, 50.0f) / 100.0f;

    juce::Path p;
    const auto n = 64;
    for (int i = 0; i <= n; ++i)
    {
        const auto t01 = static_cast<float> (i) / n;
        // Tail envelope: longer with DECAY; DAMP curves it down faster early.
        const auto tail = std::exp (-t01 * (6.0f - 5.0f * decay01) * (1.0f + damp01 * t01));
        const auto x = r.getX() + r.getWidth() * t01;
        const auto y = r.getBottom() - r.getHeight() * tail * 0.9f;
        if (i == 0)
            p.startNewSubPath (x, y);
        else
            p.lineTo (x, y);
    }
    g.setColour (LnF::ink);
    g.strokePath (p, juce::PathStrokeType (1.7f));
}

float FxPage::VerbTail::signature() const
{
    return atomicValue (decay) + atomicValue (damp) * 0.001f;
}

// ================================ EnvDisplay =============================================

FxPage::EnvDisplay::Nodes FxPage::EnvDisplay::computeNodes (const engine::ChopSettings& s) const
{
    Nodes n;
    n.area = getLocalBounds().toFloat().reduced (10.0f, 14.0f);
    const auto w = n.area.getWidth();
    const auto top = n.area.getY();
    const auto bottom = n.area.getBottom();

    // Segment widths: each stage gets a max share of the width, scaled by its
    // value relative to its range (A 22%, H 16%, D 22%, S fixed 16%, R 24%).
    const auto xA = n.area.getX() + w * 0.22f * (s.attackMs / 500.0f);
    const auto xH = xA + w * 0.16f * (s.holdMs / 500.0f);
    const auto xD = xH + w * 0.22f * (s.decayMs / 1000.0f);
    const auto xS = xD + w * 0.16f;
    const auto xR = xS + w * 0.24f * (s.releaseMs / 500.0f);
    const auto ySus = bottom - (bottom - top) * (s.sustainPct / 100.0f);

    n.a = { xA, top };
    n.h = { xH, top };
    n.d = { xD, ySus };
    n.s = { xS, ySus };
    n.r = { xR, bottom };
    return n;
}

void FxPage::EnvDisplay::paint (juce::Graphics& g)
{
    const auto area = getLocalBounds().toFloat();
    g.setColour (LnF::display);
    g.fillRoundedRectangle (area, 10.0f);

    {
        juce::Graphics::ScopedSaveState save (g);
        juce::Path clip;
        clip.addRoundedRectangle (area, 10.0f);
        g.reduceClipRegion (clip);

        g.setColour (LnF::gridLine());
        for (auto gx = 22.0f; gx < area.getWidth(); gx += 22.0f)
            g.drawVerticalLine (static_cast<int> (gx), 0.0f, area.getHeight());
        for (auto gy = 22.0f; gy < area.getHeight(); gy += 22.0f)
            g.drawHorizontalLine (static_cast<int> (gy), 0.0f, area.getWidth());

        const auto s = owner.settings();
        const auto n = computeNodes (s);

        // Envelope polyline (dark stroke, v17 display tokens).
        juce::Path env;
        env.startNewSubPath (n.area.getX(), n.area.getBottom());
        env.lineTo (n.a);
        env.lineTo (n.h);
        env.lineTo (n.d);
        env.lineTo (n.s);
        env.lineTo (n.r);
        g.setColour (LnF::ink);
        g.strokePath (env, juce::PathStrokeType (1.8f));

        // Soft fill under the curve.
        auto fill = env;
        fill.lineTo (n.r.x, n.area.getBottom());
        fill.closeSubPath();
        g.setColour (LnF::ink.withAlpha (0.05f));
        g.fillPath (fill);

        // Draggable nodes: ring circles (white fill, dark stroke; blue when hot).
        struct NodeView { juce::Point<float> p; Handle handle; };
        const NodeView nodes[] = {
            { n.a, Handle::attack },
            { n.h, Handle::hold },
            { n.d, Handle::decaySustain },
            { n.r, Handle::release },
        };
        for (const auto& node : nodes)
        {
            const bool hot = node.handle == hover || node.handle == drag;
            g.setColour (juce::Colours::white);
            g.fillEllipse (node.p.x - 4.5f, node.p.y - 4.5f, 9.0f, 9.0f);
            g.setColour (hot ? LnF::accent : LnF::ink);
            g.drawEllipse (node.p.x - 4.5f, node.p.y - 4.5f, 9.0f, 9.0f, hot ? 1.8f : 1.4f);
        }

        // Value of the hovered / dragged handle only (v17 env_display).
        const auto active = drag != Handle::none ? drag : hover;
        if (active != Handle::none)
        {
            juce::String text;
            juce::Point<float> at;
            switch (active)
            {
                case Handle::attack:       text = juce::String (s.attackMs, 0) + " ms";  at = n.a; break;
                case Handle::hold:         text = juce::String (s.holdMs, 0) + " ms";    at = n.h; break;
                case Handle::decaySustain: text = juce::String (s.decayMs, 0) + " ms / "
                                                  + juce::String (s.sustainPct, 0) + "%"; at = n.d; break;
                case Handle::release:      text = juce::String (s.releaseMs, 0) + " ms"; at = n.r; break;
                case Handle::none:         break;
            }
            g.setFont (LnF::monoFont (9.0f));
            const auto tw = static_cast<float> (
                juce::GlyphArrangement::getStringWidthInt (g.getCurrentFont(), text) + 10);
            auto pill = juce::Rectangle<float> (tw, 14.0f)
                            .withCentre ({ juce::jlimit (area.getX() + tw * 0.5f,
                                                         area.getRight() - tw * 0.5f, at.x),
                                           juce::jmax (12.0f, at.y - 14.0f) });
            g.setColour (juce::Colours::white);
            g.fillRoundedRectangle (pill, 7.0f);
            g.setColour (LnF::hairline());
            g.drawRoundedRectangle (pill, 7.0f, 1.0f);
            g.setColour (LnF::ink);
            g.drawText (text, pill.toNearestInt(), juce::Justification::centred, false);
        }
    }

    g.setColour (LnF::hairline());
    g.drawRoundedRectangle (area.reduced (0.5f), 10.0f, 1.0f);
}

FxPage::EnvDisplay::Handle FxPage::EnvDisplay::handleAt (juce::Point<float> p,
                                                         const Nodes& n) const
{
    struct Candidate { juce::Point<float> at; Handle handle; };
    const Candidate candidates[] = {
        { n.a, Handle::attack },
        { n.h, Handle::hold },
        { n.d, Handle::decaySustain },
        { n.r, Handle::release },
    };
    auto best = Handle::none;
    auto bestDist = 12.0f;
    for (const auto& c : candidates)
    {
        const auto d = p.getDistanceFrom (c.at);
        if (d < bestDist)
        {
            bestDist = d;
            best = c.handle;
        }
    }
    return best;
}

void FxPage::EnvDisplay::mouseMove (const juce::MouseEvent& e)
{
    const auto n = computeNodes (owner.settings());
    const auto newHover = handleAt (e.position, n);
    if (newHover != hover)
    {
        hover = newHover;
        repaint();
    }
    setMouseCursor (newHover == Handle::none ? juce::MouseCursor::NormalCursor
                                             : juce::MouseCursor::DraggingHandCursor);
}

void FxPage::EnvDisplay::mouseExit (const juce::MouseEvent&)
{
    hover = Handle::none;
    repaint();
}

void FxPage::EnvDisplay::mouseDown (const juce::MouseEvent& e)
{
    drag = handleAt (e.position, computeNodes (owner.settings()));
    repaint();
}

void FxPage::EnvDisplay::applyDrag (const juce::MouseEvent& e)
{
    auto s = owner.settings();
    const auto n = computeNodes (s);
    const auto w = n.area.getWidth();

    switch (drag)
    {
        case Handle::attack:
            s.attackMs = static_cast<float> (juce::jlimit (0.0, 500.0,
                             (e.position.x - n.area.getX()) / (w * 0.22) * 500.0));
            break;
        case Handle::hold:
            s.holdMs = static_cast<float> (juce::jlimit (0.0, 500.0,
                           (e.position.x - n.a.x) / (w * 0.16) * 500.0));
            break;
        case Handle::decaySustain:
            s.decayMs = static_cast<float> (juce::jlimit (5.0, 1000.0,
                            (e.position.x - n.h.x) / (w * 0.22) * 1000.0));
            s.sustainPct = static_cast<float> (juce::jlimit (0.0, 100.0,
                               (n.area.getBottom() - e.position.y) / n.area.getHeight() * 100.0));
            break;
        case Handle::release:
            s.releaseMs = static_cast<float> (juce::jlimit (5.0, 500.0,
                              (e.position.x - n.s.x) / (w * 0.24) * 500.0));
            break;
        case Handle::none:
            return;
    }
    owner.setSettings (s);
}

void FxPage::EnvDisplay::mouseDrag (const juce::MouseEvent& e)
{
    if (drag != Handle::none)
        applyDrag (e);
}

void FxPage::EnvDisplay::mouseUp (const juce::MouseEvent&)
{
    drag = Handle::none;
    repaint();
}

} // namespace chopline::ui
