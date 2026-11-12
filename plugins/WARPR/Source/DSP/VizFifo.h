#pragma once
/*
  VizFifo.h - lock-free audio-thread -> UI data path (SPSC, wait-free).

  Moved verbatim from the v9 mockup editor header.  One producer (the audio thread, one push
  per ~33 ms) and one consumer (the editor's 30 Hz timer, message thread).  push() never
  blocks, never allocates and drops the frame when the UI is behind.
*/

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <cstdint>

struct WarprVizFrame
{
    static constexpr int maxVoices = 32;
    static constexpr int numLfos   = 10;

    // organism drivers (0..1 unless noted) - computed in the audio thread from the POST-WARP stereo bus
    float low = 0, high = 0, rms = 0;       // band energies / overall level (smoothed envelope followers)
    float theta = 0;                        // stereo phase angle, radians (atan2(side, mid) of block energy)
    float r = 0;                            // stereo vector length 0..1
    float transient = 0;                    // strength of the latest detected transient (0 = none this frame)
    float tang = 0;                         // radians: angle on the organism where the swell appears (spectral centroid -> 0..2pi)
    uint32_t tseq = 0;                      // increments once per detected transient (UI diffs this)

    // meters (linear peak since the previous frame, post master gain)
    float peakL = 0, peakR = 0;

    // per-voice sample read positions for the waveform playheads
    int numVoices = 0;
    struct Voice { float pos = 0, level = 0; };            // pos 0..1 absolute in the sample
    std::array<Voice, maxVoices> voices{};

    // live grain read positions for the waveform grain cloud (GRANULAR engine, phase D8 publishes
    // these; until then numGrains stays 0 and the UI draws nothing).  One entry per sounding grain
    // across all voices, pos 0..1 absolute in the sample, level = that grain's window amplitude.
    static constexpr int maxGrains = 64;
    int numGrains = 0;
    struct Grain { float pos = 0, level = 0; };
    std::array<Grain, maxGrains> grains{};

    // LFO phases 0..1 of the most recent voice (-1 = LFO idle / unused)
    std::array<float, numLfos> lfoPhase{ -1,-1,-1,-1,-1,-1,-1,-1,-1,-1 };
};

class WarprVizFifo
{
public:
    // AUDIO THREAD: wait-free, never allocates.  Drops the frame if the UI is behind.
    void push (const WarprVizFrame& f) noexcept
    {
        int s1, n1, s2, n2;
        fifo.prepareToWrite (1, s1, n1, s2, n2);
        if (n1 > 0) { slots[(size_t) s1] = f; fifo.finishedWrite (1); }
    }

    // UI THREAD: drains everything, returns the newest frame (if any)
    bool popLatest (WarprVizFrame& out) noexcept
    {
        bool got = false;
        while (fifo.getNumReady() > 0)
        {
            int s1, n1, s2, n2;
            fifo.prepareToRead (1, s1, n1, s2, n2);
            if (n1 > 0) { out = slots[(size_t) s1]; got = true; }
            fifo.finishedRead (n1 + n2);
        }
        return got;
    }

private:
    static constexpr int capacity = 64;
    juce::AbstractFifo fifo { capacity };
    std::array<WarprVizFrame, (size_t) capacity> slots{};
};
