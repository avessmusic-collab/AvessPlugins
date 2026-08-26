#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include <array>

/**
 * Transitionist - Stage 2 DSP, Phase 4.3 (Advanced Features) - FINAL DSP PHASE
 *
 * Pure audio effect (stereo in -> stereo out), no MIDI, no file I/O.
 * Implements all 9 of architecture.md's Core Components in strict serial
 * order: Tempo-Synced Delay Line (hand-built feedback loop w/ throw-scaled
 * tanh saturator + FirstOrderTPTFilter damping) -> juce::dsp::Reverb with
 * smoothstep-ramped freezeMode + explicit input-mute gate -> Reverb
 * Modulation (hand-rolled ~0.15Hz sine LFO modulating a short
 * juce::dsp::DelayLine, +/-3ms around a 5ms center) -> Bipolar DJ Filter
 * (dual juce::dsp::LadderFilter crossfade) -> Output Glue
 * (juce::dsp::WaveShaper tanh soft-clip + juce::dsp::Limiter) -> Stereo
 * Width (manual M/S, wet-only, fixed 1.35x side gain) -> equal-power
 * Dry/Wet Mixer.
 */
class TransitionistAudioProcessor : public juce::AudioProcessor
{
public:
    TransitionistAudioProcessor();
    ~TransitionistAudioProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Transitionist"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    // Public access to parameters for editor (Stage 3 GUI binds via this member,
    // following this codebase's established convention - see AngelGrain/GainKnob).
    juce::AudioProcessorValueTreeState parameters;

private:
    // Parameter layout creation - implements the locked 3-parameter contract
    // from parameter-spec.md: throw, space, sweep (in this exact order).
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    // ------------------------------------------------------------------
    // DSP Components (Stage 2, Phases 4.1-4.3 - full 9-component chain)
    // ------------------------------------------------------------------

    double currentSampleRate = 44100.0;
    int maxDelaySamples = 0;

    // Component 1: Tempo-Synced Delay Line (manual popSample/pushSample -
    // the feedback path routes through the saturator + damping filter
    // below before being written back, so DelayLine::process() is NOT used).
    juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Lagrange3rd> delayLine;
    juce::SmoothedValue<float> smoothedDelaySamples;
    float lastTargetDelaySamples = -1.0f;

    // Components 2-3: Feedback Saturator (raw std::tanh, applied inline) +
    // Feedback Damping Filter (one instance per channel, fixed 8kHz lowpass).
    std::array<juce::dsp::FirstOrderTPTFilter<float>, 2> feedbackFilter;

    // Component 4: Reverb Engine, with Phase 4.2's smoothstep-ramped
    // freezeMode + explicit input-mute gate (computed per-block in
    // processBlock(), no additional member state needed here).
    juce::dsp::Reverb reverb;

    // Component 5 (Phase 4.3): Reverb Modulation - hand-rolled phase
    // accumulator (sine LFO, fixed ~0.15Hz) modulating a short, dedicated
    // juce::dsp::DelayLine (NOT the main tempo-synced delay line - this is
    // a small, independently-sized buffer per architecture.md Component 5).
    // Applies unconditionally (including during reverb freeze) so a frozen
    // tail stays "alive" rather than reading as a static drone.
    juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Lagrange3rd> reverbModDelayLine;
    float reverbModPhase = 0.0f;
    float reverbModPhaseIncrement = 0.0f; // set in prepareToPlay from the fixed 0.15Hz rate

    // Component 6: Bipolar DJ Filter - two persistent, always-running
    // LadderFilter instances (never mode-switched at runtime) + a
    // dead-zone/smoothstep crossfade against a true dry-bypass tap.
    juce::dsp::LadderFilter<float> lpfFilter; // pinned Mode::LPF24
    juce::dsp::LadderFilter<float> hpfFilter; // pinned Mode::HPF24

    // Component 7 (Phase 4.3): Output Glue - tanh soft-clip (WaveShaper)
    // followed by a fast safety Limiter. Wet-only, fixed internal constants
    // (no automatable parameters), applied after the Bipolar DJ Filter so
    // it also catches filter-resonance peaks, not just feedback/reverb ones.
    juce::dsp::WaveShaper<float> softClip;
    juce::dsp::Limiter<float> limiter;

    // Preallocated scratch buffers (real-time safety - no allocation in
    // processBlock()): dry tap for Component 9's final mix, and per-filter
    // copies so both LadderFilter instances can process every sample
    // regardless of which one is currently audible via the crossfade gain.
    juce::AudioBuffer<float> dryBuffer;
    juce::AudioBuffer<float> lpfBuffer;
    juce::AudioBuffer<float> hpfBuffer;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TransitionistAudioProcessor)
};
