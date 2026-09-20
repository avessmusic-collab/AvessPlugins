#pragma once
#include <juce_dsp/juce_dsp.h>
#include "MusicalClock.h"
#include "PitchModule.h" // GranularGrainEngine - reused for the PitchVariation mode

/**
 * StutterModule - buffer-repeat stutter effect.
 *
 * See plugins/DROP/.ideas/architecture.md > Core Components > StutterModule
 * and plugins/DROP/.ideas/implementation-spec.md > STUTTERMODULE.
 *
 * Preallocated circular capture buffer, continuously written every sample
 * with the dry input. A dual-voice equal-power crossfade looper reads back
 * a FROZEN snapshot of the most-recently-captured window (length quantized
 * from MusicalClock, 1/4 through 1/64 per implementation-spec.md, shortening
 * as `progress` rises) and repeats
 * it; whenever a voice's content is about to loop, the OTHER voice is
 * (re)triggered with a fresh snapshot and the two are equal-power
 * crossfaded across a few-ms window, guaranteeing click-free loop-point
 * joins regardless of mode (architecture.md: "Avoid discontinuities and
 * clicks").
 *
 * Modes: Repeat (rate 1.0), Reverse (content read back-to-front), Half
 * (rate 0.5 - slower/darker), Double (rate 2.0 - faster/glitchier),
 * PitchVariation (Repeat-rate looping additionally routed through a
 * GranularGrainEngine instance - architecture.md: "reuses PitchModule's
 * grain engine").
 *
 * RT-safe: capture buffer + grain engine preallocated in prepare(), no
 * allocation in process().
 */
namespace drop
{

enum class StutterMode
{
    Repeat = 0,
    Reverse,
    Half,
    Double,
    PitchVariation
};

class StutterModule
{
public:
    void prepare(const juce::dsp::ProcessSpec& spec);
    void reset();

    void setMode(StutterMode newMode) noexcept { mode = newMode; }
    StutterMode getMode() const noexcept { return mode; }

    /** `clock` supplies the current tempo for musical-division length quantization. */
    void process(juce::dsp::AudioBlock<float> block, float weight, float progress, const MusicalClock& clock) noexcept;

private:
    struct Voice
    {
        bool active = false;
        double contentPos = 0.0;      // 0..contentLength, position within the frozen snapshot
        double contentLength = 1.0;   // snapshot length in samples, latched at trigger time
        int64_t sourceStart = 0;      // captureBuffer absolute write-position marking the start of the frozen window
    };

    float readCaptureInterpolated(int channel, double absolutePosition) const noexcept;
    void triggerVoice(Voice& voice, double contentLength) noexcept;
    double computeStutterLengthSamples(float progress, const MusicalClock& clock) const noexcept;

    juce::AudioBuffer<float> captureBuffer;
    int captureBufferLength = 0;
    int64_t writePosition = 0;

    Voice voices[2];
    int activeVoice = 0;
    double crossfadePos = -1.0; // -1 == not currently crossfading

    GranularGrainEngine pitchVariationEngine;

    juce::SmoothedValue<float> weightSmoothed;
    double sampleRate = 44100.0;
    StutterMode mode = StutterMode::Repeat;

    static constexpr float kFadeMs = 3.0f; // architecture.md: "2-5ms equal-power"
    static constexpr float kPitchVariationSemitones = 7.0f;
};

} // namespace drop
