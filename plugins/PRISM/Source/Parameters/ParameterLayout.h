#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <cmath>
#include "ParameterIDs.h"

namespace prism
{
    // Builds the 17-parameter layout exactly as specified in parameter-spec.md.
    // Order, ids, choice strings, ranges and defaults are a locked contract.
    inline juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout()
    {
        using juce::AudioParameterBool;
        using juce::AudioParameterChoice;
        using juce::AudioParameterFloat;
        using juce::AudioParameterFloatAttributes;
        using juce::NormalisableRange;
        using juce::ParameterID;
        using juce::StringArray;

        juce::AudioProcessorValueTreeState::ParameterLayout layout;

        // fft_size - Choice, default 4096 (index 2)
        layout.add(std::make_unique<AudioParameterChoice>(
            ParameterID { ids::fftSize, 1 }, "FFT Size",
            StringArray { "1024", "2048", "4096", "8192", "16384" }, 2));

        // window - Choice, default hann (index 0)
        layout.add(std::make_unique<AudioParameterChoice>(
            ParameterID { ids::window, 1 }, "Window",
            StringArray { "hann", "blackmanHarris" }, 0));

        // smoothing - Float 0..100 %, default 35, linear
        layout.add(std::make_unique<AudioParameterFloat>(
            ParameterID { ids::smoothing, 1 }, "Smoothing",
            NormalisableRange<float>(0.0f, 100.0f, 0.01f), 35.0f,
            AudioParameterFloatAttributes().withLabel("%")));

        // spectrum_tilt - Float 0..4.5 dB/oct, default 0 (flat; user reverted the Pro-Q
        // 4.5 default on 2026-10-16 - slope stays available on the TILT knob), linear
        layout.add(std::make_unique<AudioParameterFloat>(
            ParameterID { ids::spectrumTilt, 1 }, "Spectrum Tilt",
            NormalisableRange<float>(0.0f, 4.5f, 0.1f), 0.0f,
            AudioParameterFloatAttributes().withLabel("dB/oct")));

        // display_range - Choice (index 0..3), default "-90" (index 3)
        layout.add(std::make_unique<AudioParameterChoice>(
            ParameterID { ids::displayRange, 1 }, "Display Range",
            StringArray { "-48", "-60", "-72", "-90" }, 3));

        // spectrogram_speed - Float 0.25..4.0 x, LOGARITHMIC (geometric midpoint = 1.0), default 1.0
        {
            constexpr float lo = 0.25f, hi = 4.0f;
            NormalisableRange<float> range(
                lo, hi,
                [](float start, float end, float norm)
                {
                    return start * std::pow(end / start, juce::jlimit(0.0f, 1.0f, norm));
                },
                [](float start, float end, float value)
                {
                    return juce::jlimit(0.0f, 1.0f, std::log(juce::jlimit(start, end, value) / start)
                                                       / std::log(end / start));
                },
                [](float start, float end, float value)
                {
                    // 0.01 interval
                    return juce::jlimit(start, end, std::round(value * 100.0f) / 100.0f);
                });

            layout.add(std::make_unique<AudioParameterFloat>(
                ParameterID { ids::spectrogramSpeed, 1 }, "Spectrogram Speed",
                range, 1.0f,
                AudioParameterFloatAttributes().withLabel("x")));
        }

        // peak_decay - Float 3..60 dB/s, default 12, linear
        layout.add(std::make_unique<AudioParameterFloat>(
            ParameterID { ids::peakDecay, 1 }, "Peak Decay",
            NormalisableRange<float>(3.0f, 60.0f, 0.01f), 12.0f,
            AudioParameterFloatAttributes().withLabel("dB/s")));

        // freeze - Bool, default off
        layout.add(std::make_unique<AudioParameterBool>(
            ParameterID { ids::freeze, 1 }, "Freeze", false));

        // analysis_mode - Choice, default combined (index 2)
        layout.add(std::make_unique<AudioParameterChoice>(
            ParameterID { ids::analysisMode, 1 }, "Analysis Mode",
            StringArray { "leftRight", "midSide", "combined" }, 2));

        // color_theme - Choice (7), default "Coral/Mint" (index 0); strings verbatim incl. slash
        layout.add(std::make_unique<AudioParameterChoice>(
            ParameterID { ids::colorTheme, 1 }, "Color Theme",
            StringArray { "Coral/Mint", "Cyan/Violet", "Amber/Cyan", "Magenta/Blue",
                          "Green/Ice", "Gold/Violet", "Ice/Steel" }, 0));

        // preset - Choice (6), default Studio (index 0). No engine effect; bundle fan-out is UI-side.
        layout.add(std::make_unique<AudioParameterChoice>(
            ParameterID { ids::preset, 1 }, "Preset",
            StringArray { "Studio", "Minimal", "Neon", "Ambient", "Scientific", "Performance" }, 0));

        // reduced_motion - Bool, default off
        layout.add(std::make_unique<AudioParameterBool>(
            ParameterID { ids::reducedMotion, 1 }, "Reduced Motion", false));

        // glow_intensity - Float 0..100 %, default 55, linear
        layout.add(std::make_unique<AudioParameterFloat>(
            ParameterID { ids::glowIntensity, 1 }, "Glow Intensity",
            NormalisableRange<float>(0.0f, 100.0f, 0.01f), 55.0f,
            AudioParameterFloatAttributes().withLabel("%")));

        // Sources
        layout.add(std::make_unique<AudioParameterBool>(
            ParameterID { ids::src1Visible, 1 }, "Ch1 Visible", true));
        layout.add(std::make_unique<AudioParameterBool>(
            ParameterID { ids::src1Solo, 1 }, "Ch1 Solo", false));
        layout.add(std::make_unique<AudioParameterBool>(
            ParameterID { ids::src2Visible, 1 }, "Ch2 Visible", true));
        layout.add(std::make_unique<AudioParameterBool>(
            ParameterID { ids::src2Solo, 1 }, "Ch2 Solo", false));

        return layout;
    }
} // namespace prism
