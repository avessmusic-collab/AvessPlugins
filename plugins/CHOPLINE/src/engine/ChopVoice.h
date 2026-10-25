// CHOPLINE — ChopVoice (spec §3, §6; Phase 3 warp engine)
// One playback voice: plays a chop's EFFECTIVE sample range (trim offsets
// applied in the snapshot) through the per-chop chain —
//
//   read source (forward/reverse, crossfaded loop, declick fades)
//     → warp (Varispeed: fractional-rate catmull-rom playback, pitch & speed
//              coupled ±24 st; Stretch / Fit-step: one Signalsmith Stretch
//              instance per unison stream, independent pitch ±24 st, formant
//              preserve, Fit input-rate clamped 0.25×–4×)
//     → unison 1–4 (fixed cents + stereo spread, 1/√N gain)
//     → AHDSR amp envelope (gate from the event span, MIDI note hold, or the
//        chop's natural length; One-shot/Fit ignore note-off)
//     → declick / steal handling.
//
// Playback modes (spec §6): One-shot, Gate (releases via the chop RELEASE =
// AHDSR release), Loop (forward, crossfaded by loopXfade; keeps looping
// through the release), Fit step (time-stretched to the event span, pitch
// preserved; a span-less trigger falls back to One-shot). Granular/Freeze are
// Phase 5 — they currently play as Varispeed.
//
// Stealing / choking never clicks: a retrigger (or choke) renders the running
// playback's next 2 ms into a small preallocated tail buffer immediately,
// with a linear fade-out, and frees its stretchers for the new note. The tail
// is then mixed over the following blocks. fastRelease() (all-notes-off,
// transport stop) fades the LIVE playback over 2 ms instead.
//
// Everything is preallocated in prepare(): stretchers live in StretcherPool
// (dedicated per voice + a shared unison pool), scratch buffers are sized for
// the worst-case block and Fit rate. No allocation on the audio thread.
//
// Snapshot safety: the playback publishes the SampleData* it reads through an
// atomic, so the message-thread GC never frees a sample snapshot a voice is
// still playing. Chop settings are copied BY VALUE at trigger time, so a
// settings edit never glitches a ringing voice.

#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include "SampleStore.h"
#include "SliceMap.h"
#include "StretcherPool.h"

#include <atomic>

namespace chopline::engine
{

class ChopVoice final
{
public:
    static constexpr double fastFadeSeconds = 0.002; // 2 ms steal/choke fade
    static constexpr int maxUnison = 4;

    // Trigger source tags (Phase 2: transport stop / loop jumps release only
    // pattern voices; MIDI and audition voices keep ringing — spec §8).
    static constexpr int tagMidi = 0;
    static constexpr int tagPattern = 1;

    // gateSamples semantics for trigger().
    static constexpr juce::int64 gateHeld = -1;    // MIDI: until noteOff()
    static constexpr juce::int64 gateNatural = -2; // audition: one chop's worth

    /// Message thread (prepareToPlay). The StretcherPool must outlive the voice.
    void prepare (double sampleRate, int maxBlockSize, StretcherPool& stretchers,
                  int voiceIndex);
    void reset() noexcept;

    // ---- audio thread -------------------------------------------------------
    void trigger (const SampleData* sample, const SliceMapSnapshot::Chop& chop,
                  int chopIndex, float gain, std::uint64_t newStamp,
                  int tag = tagMidi, juce::int64 gateSamples = gateNatural,
                  int midiNote = -1, double startNorm = 0.0) noexcept;
    void noteOff (int midiNote) noexcept;  // Gate/Loop enter release; others ignore
    /// True while a Gate/Loop playback is still held open by this MIDI note.
    bool isNoteHeld (int midiNote) const noexcept
    {
        return pb.active && pb.midiNote == midiNote && pb.gateApplies && pb.gateRemaining == -1;
    }
    void fastRelease() noexcept;           // steal / stop / all-notes-off: 2 ms fade
    void renderAdd (juce::AudioBuffer<float>& out, int startSample, int numSamples) noexcept;

    bool isActive() const noexcept { return pb.active || tailRemaining > 0; }
    bool isHolding() const noexcept { return pb.active && ! pb.fastFading; }
    std::uint64_t getStamp() const noexcept { return stamp; }
    int getSourceTag() const noexcept { return sourceTag; }
    /// Choke group of the LIVE playback (0 when inactive or choke off).
    int getChokeGroup() const noexcept { return pb.active ? pb.chop.chokeGroup : 0; }

    // ---- message thread (UI feedback / snapshot GC) --------------------------
    int getUiChopIndex() const noexcept { return uiChop.load (std::memory_order_relaxed); }
    float getUiNormalisedPos() const noexcept { return uiPos.load (std::memory_order_relaxed); }
    bool usesSample (const SampleData* s) const noexcept
    {
        return pb.sample.load (std::memory_order_relaxed) == s;
    }

private:
    enum class EnvStage : int { attack, hold, decay, sustain, release, idle };

    struct Stream
    {
        bool dead = true;
        bool useStretch = false;
        float panL = 1.0f, panR = 1.0f;
        // Varispeed.
        double pos = 0.0;  // fractional logical position
        double rate = 1.0; // logical frames per output sample
        // Stretch.
        Stretcher* st = nullptr;
        bool sharedStretcher = false;
        double inRate = 1.0;          // input frames per output sample
        juce::int64 inFed = 0;        // logical input fed (relative to x0)
        juce::int64 outDone = 0;      // output samples produced
        juce::int64 expectedOut = -1; // −1 = unbounded (loop)
    };

    struct Playback
    {
        std::atomic<const SampleData*> sample { nullptr };
        SliceMapSnapshot::Chop chop;
        bool active = false;
        bool looping = false;
        bool gateApplies = false;
        double chopLen = 0.0;             // logical frames (effective)
        juce::int64 loopA = 0, loopB = 0; // logical loop bounds
        int loopXfade = 0;
        double x0 = 0.0;                  // logical start (audition-from)
        juce::int64 expectedOut = -1;     // output samples, −1 = unbounded
        juce::int64 outPos = 0;
        float gain = 1.0f;
        int chopIndex = -1;
        int midiNote = -1;
        // Envelope.
        EnvStage stage = EnvStage::idle;
        double envLevel = 0.0;
        juce::int64 stagePos = 0;
        juce::int64 gateRemaining = -1; // −1 = held (noteOff closes)
        double releaseStep = 0.0;
        // Steal / stop fade.
        bool fastFading = false;
        float fastGain = 1.0f;

        int numStreams = 1;
        Stream streams[maxUnison];
    };

    void setupPlayback (const SampleData* sample, const SliceMapSnapshot::Chop& chop,
                        int chopIndex, float gain, juce::int64 gateSamples,
                        int midiNote, double startNorm) noexcept;
    void stopPlayback (Playback& p) noexcept;
    void releaseStreams (Playback& p) noexcept;
    void prerenderTail() noexcept;

    void renderPlayback (Playback& p, float* outL, float* outR, int numSamples) noexcept;
    void renderChunk (Playback& p, float* outL, float* outR, int numSamples) noexcept;
    void renderVarispeed (Playback& p, Stream& s, float* outL, float* outR, int n) noexcept;
    void renderStretch (Playback& p, Stream& s, float* outL, float* outR, int n) noexcept;

    void readFrame (const Playback& p, juce::int64 logical, float& l, float& r) const noexcept;
    void readInterp (const Playback& p, double logical, float& l, float& r) const noexcept;

    double advanceEnv (Playback& p) noexcept;
    void enterRelease (Playback& p) noexcept;
    void updateUiFeedback() noexcept;

    Playback pb;

    // Prerendered steal tail (PCM — holds no sample/stretcher references).
    int tailRemaining = 0, tailPos = 0;

    double sampleRate = 48000.0;
    int maxBlock = 512;
    int fastFadeSamples = 96;
    float fastFadeStep = 1.0f / 96.0f;
    int voiceIndex = 0;
    StretcherPool* pool = nullptr;

    // Preallocated scratch (prepare()).
    std::vector<float> envScratch;
    std::vector<float> inScratchL, inScratchR;
    std::vector<float> outScratchL, outScratchR;
    std::vector<float> tailL, tailR;

    std::uint64_t stamp = 0;
    int sourceTag = tagMidi; // audio thread only
    std::atomic<int> uiChop { -1 };
    std::atomic<float> uiPos { 0.0f };
};

} // namespace chopline::engine
