// CHOPLINE — Parameters implementation.

#include "Parameters.h"

namespace chopline::params
{

namespace
{
    juce::ParameterID pid (const char* idString)
    {
        return { idString, kParameterVersion };
    }

    std::unique_ptr<juce::AudioParameterFloat> percent (const char* idString,
                                                        const juce::String& name,
                                                        float defaultValue,
                                                        float maxValue = 100.0f)
    {
        return std::make_unique<juce::AudioParameterFloat> (
            pid (idString), name, juce::NormalisableRange<float> (0.0f, maxValue, 0.1f),
            defaultValue, juce::AudioParameterFloatAttributes().withLabel ("%"));
    }

    std::unique_ptr<juce::AudioParameterBool> toggle (const char* idString,
                                                      const juce::String& name,
                                                      bool defaultValue)
    {
        return std::make_unique<juce::AudioParameterBool> (pid (idString), name, defaultValue);
    }

    std::unique_ptr<juce::AudioParameterChoice> choice (const char* idString,
                                                        const juce::String& name,
                                                        const juce::StringArray& options,
                                                        int defaultIndex)
    {
        return std::make_unique<juce::AudioParameterChoice> (pid (idString), name,
                                                             options, defaultIndex);
    }
} // namespace

juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    layout.add (choice (id::midiBaseNote, "MIDI Base Note",
                        { "C0", "C1", "C2", "C3" }, 1)); // default C1 = 36 (chop 1)

    // Phase 2 (spec §5): swing 0–75 % on even steps — trigger TIME only, so a
    // per-block atomic read is enough (no audio-rate smoothing needed).
    layout.add (std::make_unique<juce::AudioParameterFloat> (
        pid (id::swing), "Swing",
        juce::NormalisableRange<float> (0.0f, 75.0f, 0.1f), 0.0f,
        juce::AudioParameterFloatAttributes().withLabel ("%")));

    // ---- Beat Repeat (Phase 4, spec §7; defaults = locked v17) -----------------
    layout.add (choice (id::brMode, "Repeat Mode", { "Auto", "Manual", "Off" }, 1));
    layout.add (toggle (id::brHold, "Repeat Hold", false));
    layout.add (choice (id::brInterval, "Repeat Interval",
                        { "1/4", "1/2", "1 bar", "2 bars", "4 bars" }, 1));
    {
        juce::StringArray offsets;
        for (int i = 0; i < 16; ++i)
            offsets.add (juce::String (i));
        layout.add (choice (id::brOffset, "Repeat Offset", offsets, 0));
    }
    layout.add (percent (id::brChance, "Repeat Chance", 60.0f));
    layout.add (choice (id::brGrid, "Repeat Grid",
                        { "1/2", "1/4", "1/8", "1/16", "1/32", "1/64",
                          "1/8T", "1/16T", "1/32T" }, 3));
    layout.add (choice (id::brGate, "Repeat Gate",
                        { "1/16", "1/8", "1/4", "1/2", "1 bar", "2 bars", "4 bars" }, 3));
    layout.add (percent (id::brDecay, "Repeat Decay", 25.0f));
    layout.add (std::make_unique<juce::AudioParameterFloat> (
        pid (id::brPitchDecay), "Repeat Pitch Decay",
        juce::NormalisableRange<float> (0.0f, 12.0f, 0.5f), 0.0f,
        juce::AudioParameterFloatAttributes().withLabel ("st")));
    layout.add (percent (id::brAccel, "Repeat Accel", 0.0f));
    layout.add (choice (id::brReverse, "Reverse Repeats", { "Off", "Alternate", "All" }, 0));
    layout.add (choice (id::brFilterMode, "Repeat Filter", { "Off", "LP", "HP" }, 0));
    layout.add (percent (id::brSweep, "Repeat Filter Sweep", 50.0f));
    layout.add (choice (id::brMix, "Repeat Mix Mode", { "Mix", "Insert", "Gate" }, 0));
    layout.add (toggle (id::brFreeze, "Repeat Freeze", false));
    layout.add (toggle (id::brRndGrid, "Repeat Random Grid", false));
    layout.add (choice (id::brMidiBase, "Repeat MIDI Base", { "C2", "C3", "C4", "C5" }, 1));

    // ---- master chain (Phase 4; chain order per the 2026-10-25 user decision:
    // Beat Repeat → Filter → Distortion → Delay/Reverb returns → Width →
    // Output gain → safety limiter) ---------------------------------------------
    layout.add (toggle (id::masterFilterOn, "Master Filter On", true));
    layout.add (choice (id::masterFilterType, "Master Filter Type", { "LP", "BP", "HP" }, 0));
    {
        juce::NormalisableRange<float> cutoff (20.0f, 20000.0f);
        cutoff.setSkewForCentre (632.0f); // log-ish: sqrt(20 * 20000)
        layout.add (std::make_unique<juce::AudioParameterFloat> (
            pid (id::masterFilterCutoff), "Master Filter Cutoff", cutoff, 20000.0f,
            juce::AudioParameterFloatAttributes().withLabel ("Hz")));
    }
    layout.add (percent (id::masterFilterReso, "Master Filter Reso", 10.0f));

    layout.add (toggle (id::masterDistOn, "Master Distortion On", true));
    layout.add (choice (id::masterDistType, "Master Distortion Type",
                        { "Soft clip", "Hard clip", "Tape", "Fold",
                          "Bitcrush", "Downsample" }, 2));
    layout.add (percent (id::masterDistDrive, "Master Drive", 10.0f));
    layout.add (percent (id::masterDistMix, "Master Distortion Mix", 100.0f));
    layout.add (toggle (id::masterDistAutoGain, "Master Auto Gain", true));

    layout.add (toggle (id::masterWidthOn, "Master Width On", true));
    layout.add (percent (id::masterWidth, "Master Width", 100.0f, 200.0f));
    layout.add (std::make_unique<juce::AudioParameterFloat> (
        pid (id::masterMono), "Mono Below",
        juce::NormalisableRange<float> (20.0f, 300.0f, 1.0f), 120.0f,
        juce::AudioParameterFloatAttributes().withLabel ("Hz")));

    layout.add (toggle (id::masterLimiterOn, "Output / Limiter On", true));
    layout.add (std::make_unique<juce::AudioParameterFloat> (
        pid (id::masterOutput), "Output Gain",
        juce::NormalisableRange<float> (-24.0f, 12.0f, 0.1f), 0.0f,
        juce::AudioParameterFloatAttributes().withLabel ("dB")));
    layout.add (std::make_unique<juce::AudioParameterFloat> (
        pid (id::masterCeiling), "Limiter Ceiling",
        juce::NormalisableRange<float> (-12.0f, 0.0f, 0.1f), -0.3f,
        juce::AudioParameterFloatAttributes().withLabel ("dB")));

    layout.add (toggle (id::masterDelayOn, "Master Delay On", true));
    layout.add (choice (id::masterDelayTime, "Delay Time",
                        { "1/16", "1/8", "1/8.", "1/4", "1/4.", "1/2", "1/8T", "1/4T" }, 2));
    layout.add (percent (id::masterDelayFb, "Delay Feedback", 35.0f, 95.0f));
    layout.add (percent (id::masterDelayRtn, "Delay Return", 0.0f));

    layout.add (toggle (id::masterVerbOn, "Master Reverb On", true));
    layout.add (percent (id::masterVerbDecay, "Reverb Decay", 40.0f));
    layout.add (percent (id::masterVerbDamp, "Reverb Damp", 50.0f));
    layout.add (percent (id::masterVerbRtn, "Reverb Return", 0.0f));

    return layout;
}

} // namespace chopline::params
