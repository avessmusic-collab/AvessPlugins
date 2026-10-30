#pragma once

#include <array>
#include <vector>

#include "CustomLookAndFeel.h"

// Radial pulse field behind the hero knob (v4 yaml "pulse_field"): 72 spikes,
// outward pulse rings, particles, halo, plus the backdrop and the accent
// hairline. It fills the whole editor (520x520) and is clipped to the 500x500
// inner window.
//
// PHASE 4.1 STATUS: drawing is complete; the data source is a settable State
// (tension / level / phase) pushed by the editor, and a basic 60 Hz juce::Timer
// advances the animation so the editor is not frozen. Phase 4.3 replaces the
// data source with the real lock-free telemetry (TensionTelemetry via
// processor.getTelemetry()) read on this same timer. The animation itself is
// UI-owned and independent of audio.
class Visualizer : public juce::Component, private juce::Timer
{
public:
    struct State
    {
        float tension = 0.0f;   // 0..1 (already smoothed upstream in 4.3)
        float level   = 0.0f;   // 0..1 output level (slight halo modulation)
        float phase   = 0.0f;   // radians, seeds spike phase (LFO phase in 4.3)
    };

    explicit Visualizer (const TensionUI::ThemeState& themeState);
    ~Visualizer() override;

    void setState (const State& s) noexcept { state = s; wake(); }
    void setRiseMode (int mode) noexcept { riseMode = mode; wake(); }

    void paint (juce::Graphics&) override;

private:
    struct Particle
    {
        float angle = 0, radius = 0, speed = 0, life = 1, age = 0, size = 1, drift = 0;
        bool alive = false;
    };
    struct Ring { float age = 0; };

    void timerCallback() override;
    void wake() { idleFrames = 0; }
    void step (float dt);
    void spawnParticle();

    const TensionUI::ThemeState& theme;
    State state;
    int riseMode = TensionUI::Cinematic;

    // UI-owned animation state
    float smoothedI = 0.0f;       // one-pole smoothed intensity (0.12 / frame)
    float smoothedTension = 0.0f;
    float gate = 0.0f;
    float spikePhase = 0.0f;
    float rotation = 0.0f;        // radians
    float breathPhase = 0.0f;
    float ringAccum = 0.0f;
    double lastTickMs = 0.0;
    int idleFrames = 0;

    std::vector<Ring> rings;
    std::array<Particle, 192> particles;
    juce::Random random;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Visualizer)
};
