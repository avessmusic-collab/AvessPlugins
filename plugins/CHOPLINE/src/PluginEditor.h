// CHOPLINE — PluginEditor (Phase 1)
// Native-JUCE editor on the locked v17 wireframe-light design. MAIN page top
// region is live (header + Strip A sampler); the grid and bottom bar areas
// are quiet placeholders until Phases 2/4. The whole UI is laid out on a
// fixed 1100x720 canvas that scales 75%–200% with the window (fixed aspect).

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>

#include "PluginProcessor.h"
#include "ui/GridStrip.h"
#include "ui/LookAndFeel.h"
#include "ui/SamplerStrip.h"

namespace chopline
{

class ChoplineAudioProcessorEditor final : public juce::AudioProcessorEditor,
                                           public juce::DragAndDropContainer
{
public:
    explicit ChoplineAudioProcessorEditor (ChoplineAudioProcessor&);
    ~ChoplineAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override; // Cmd/Ctrl-Z undo, Shift-Cmd-Z redo

private:
    // ---- small header widgets ------------------------------------------------
    class MidiActivityDot final : public juce::Component
    {
    public:
        explicit MidiActivityDot (engine::Engine& e) : engine (e) {}
        void tick(); // from the VBlank callback
        void paint (juce::Graphics&) override;

    private:
        engine::Engine& engine;
        std::uint32_t lastCount = 0;
        juce::uint32 flashStartMs = 0;
        bool lit = false;
    };

    class PageSeg final : public juce::Component // static MAIN | FX (FX = later phase)
    {
    public:
        void paint (juce::Graphics&) override;
    };

    class GearButton final : public juce::Button
    {
    public:
        GearButton() : juce::Button ("Settings") {}
        void paintButton (juce::Graphics&, bool, bool) override;
    };

    class QuietPanel final : public juce::Component // placeholder cards
    {
    public:
        explicit QuietPanel (juce::String titleToUse) : title (std::move (titleToUse)) {}
        void paint (juce::Graphics&) override;

    private:
        juce::String title;
    };

    void openSettingsFlyout();

    static constexpr int designWidth = 1100;
    static constexpr int designHeight = 720;

    ChoplineAudioProcessor& processorRef;
    ui::ChoplineLookAndFeel lookAndFeel;

    juce::Component canvas; // fixed 1100x720, scaled by transform

    // Header.
    juce::Label wordmark;
    juce::TextButton presetPrev { juce::CharPointer_UTF8 ("\xe2\x80\xb9") };  // ‹
    juce::TextButton presetNext { juce::CharPointer_UTF8 ("\xe2\x80\xba") };  // ›
    juce::ComboBox presetBox;
    MidiActivityDot midiDot;
    PageSeg pageSeg;
    GearButton gearButton;

    // Strips.
    ui::SamplerStrip samplerStrip;
    ui::GridStrip gridStrip;
    QuietPanel beatRepeatPlaceholder { juce::String (juce::CharPointer_UTF8 ("BEAT REPEAT \xe2\x80\x94 PHASE 4")) };
    QuietPanel masterPlaceholder { juce::String (juce::CharPointer_UTF8 ("MASTER \xe2\x80\x94 PHASE 4")) };

    juce::VBlankAttachment vblank;
    int gcCountdown = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChoplineAudioProcessorEditor)
};

} // namespace chopline
