// CHOPLINE — BeatRepeat (spec §7)
// Captures the plugin's own post-voice mix into a pre-allocated ring buffer
// and repeats a grid-sized slice of it in musical time.
//
// Algorithm
//   The chop-bus signal is written continuously into a ring sized for 4 bars
//   at 30 BPM (32 s) at the session rate — at 192 kHz that is the spec §7
//   figure. A repeat TRIGGER (Auto: probability roll at every interval(+offset)
//   boundary; Manual: HOLD latch / H key / MIDI performance note, quantised to
//   the next grid boundary) pins a slice at the trigger position. Repeat slot
//   0 is the capture pass (the reader tracks the write head → bit-identical to
//   dry); every following slot re-reads the slice:
//     gain        (1 − decay)^k                 (volume drop per repeat)
//     rate        2^(−k · pitchDecay / 12)      (pitch decay, varispeed read)
//     direction   reverse per Off / Alternate / All (slot 0 is always forward —
//                 its audio does not exist yet)
//     length      grid · 2^(−3 · accel · progress) (stutter roll / build-up)
//   Every slot boundary crossfades old → new reader (3 ms); a stop fades the
//   repeat tail out over the same window. An optional LP/HP sweep (state-
//   variable filter) runs over the gate on the repeat path only. Mix modes:
//   Mix (dry + repeats k ≥ 1), Insert (dry muted while repeating), Gate
//   (repeats only, dry never passes). Freeze pins the current slice/slot and
//   suspends the gate until released.
//
// Timing is derived from the host bar position every block (same discipline
// as the PatternScheduler): the engine passes a continuous bar phase and the
// current samples-per-bar; all boundaries are computed in rounded sample
// space, so repeats land PPQ-aligned at any block size.
//
// RT safety: the ring and every state live in prepare()-allocated storage;
// process() never allocates, locks or logs.

#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <atomic>
#include <vector>

namespace chopline::engine
{

class BeatRepeat final
{
public:
    /// All values map 1:1 to the APVTS parameters (filled by the processor;
    /// defaults keep the stage fully transparent for engine tests).
    struct Params
    {
        int mode = 2;             // 0 Auto, 1 Manual, 2 Off (test default: Off)
        bool hold = false;        // HOLD latch | H momentary | MIDI perf note
        int intervalIndex = 1;    // 1/4, 1/2, 1, 2, 4 bars
        int offsetSteps = 0;      // 0–15 sixteenths of the interval
        float chancePct = 60.0f;  // Auto probability per interval
        int gridIndex = 3;        // 1/2 … 1/64 + 1/8T 1/16T 1/32T (default 1/16)
        int gateIndex = 3;        // 1/16 … 4 bars (default 1/2)
        float decayPct = 25.0f;
        float pitchDecaySt = 0.0f; // 0–12 st down per repeat
        float accelPct = 0.0f;
        int reverseMode = 0;      // 0 Off, 1 Alternate, 2 All
        int filterMode = 0;       // 0 Off, 1 LP, 2 HP (sweep over the gate)
        float sweepPct = 50.0f;
        int mixMode = 0;          // 0 Mix, 1 Insert, 2 Gate
        bool freeze = false;
        bool rndGrid = false;     // random grid per repeat (advanced)
    };

    BeatRepeat() = default;

    static constexpr int numGridChoices = 9;
    static constexpr double ringSeconds = 32.0; // 4 bars at 30 BPM (spec §7)

    /// Grid slice size in bars (4/4): 1/2 … 1/64 straight, 1/8T 1/16T 1/32T.
    static double gridBars (int index) noexcept
    {
        static constexpr double table[numGridChoices] = {
            1.0 / 2.0, 1.0 / 4.0, 1.0 / 8.0, 1.0 / 16.0, 1.0 / 32.0, 1.0 / 64.0,
            1.0 / 12.0, 1.0 / 24.0, 1.0 / 48.0,
        };
        return table[juce::jlimit (0, numGridChoices - 1, index)];
    }

    /// Decision interval in bars: 1/4, 1/2, 1, 2, 4.
    static double intervalBars (int index) noexcept
    {
        static constexpr double table[5] = { 0.25, 0.5, 1.0, 2.0, 4.0 };
        return table[juce::jlimit (0, 4, index)];
    }

    /// Gate (repeat window) in bars: 1/16 … 4 bars.
    static double gateBars (int index) noexcept
    {
        static constexpr double table[7] = {
            1.0 / 16.0, 1.0 / 8.0, 1.0 / 4.0, 1.0 / 2.0, 1.0, 2.0, 4.0,
        };
        return table[juce::jlimit (0, 6, index)];
    }

    /// MIDI performance note → grid index (spec §7: base = 1/4, base+1 = 1/8, …).
    /// Returns −1 when the note is outside base … base+7.
    static int gridIndexForPerformanceNote (int note, int baseNote) noexcept
    {
        const auto step = note - baseNote;
        return step >= 0 && step < 8 ? step + 1 : -1; // grid 0 (1/2) not performable
    }

    // ---- lifecycle (message thread) ----------------------------------------------
    void prepare (double sampleRate, int maxBlockSize);
    void reset() noexcept;

    /// Tests only (message thread, before processing): deterministic chance.
    void setRandomSeed (juce::int64 seed) noexcept { rng.setSeed (seed); }

    // ---- audio thread ---------------------------------------------------------------
    /// In-place. `barPhase` = musical position in bars at block start (host
    /// PPQ-derived or free-run), `samplesPerBar` from the current tempo.
    void process (juce::AudioBuffer<float>& buffer, const Params& params,
                  double barPhase, double samplesPerBar) noexcept;

    /// UI (message thread): true while a repeat is sounding or armed by hold.
    bool isActiveUi() const noexcept { return uiActive.load (std::memory_order_relaxed); }

private:
    struct Reader
    {
        bool active = false;
        double pos = 0.0;   // absolute (monotonic) sample position, fractional
        double rate = 1.0;
        bool reverse = false;
        float gain = 1.0f;
    };

    void startRepeat (const Params& p, double samplesPerBar) noexcept;
    void startSlot (const Params& p, bool advance) noexcept;
    void beginStop() noexcept;
    float readRing (const Reader& r, int channel) const noexcept;
    void updateFilterCoeffs (double fc) noexcept;

    // Ring (prepare()-allocated).
    std::vector<float> ringL, ringR;
    juce::int64 ringLen = 0;
    juce::int64 absPos = 0; // absolute sample counter (write head)

    double rate = 48000.0;
    int fadeLen = 144;      // 3 ms repeat-boundary crossfade (spec: 1–10 ms)

    // Repeat state (audio thread).
    bool repeating = false;
    bool holdWas = false;
    juce::int64 sliceStart = 0;
    double sliceLen = 0.0;        // captured slice length (input samples)
    double gridSamples = 0.0;     // grid length at trigger (output samples)
    int slotIndex = 0;
    juce::int64 slotRemaining = 0;
    juce::int64 gateRemaining = 0;
    juce::int64 gateTotal = 1;
    juce::int64 elapsed = 0;

    Reader cur, prev;
    juce::int64 fadePos = 0; // 0..fadeLen while crossfading cur in / prev out

    float dryGain = 1.0f, repGain = 0.0f; // ramped per sample toward targets

    // Sweep filter state (TPT SVF, stereo).
    double fIc1[2] {}, fIc2[2] {};
    double fA1 = 0.0, fA2 = 0.0, fA3 = 0.0, fK = 1.414;
    double lastFc = -1.0;

    juce::Random rng { 0x43484f50 }; // deterministic default seed ("CHOP")

    std::atomic<bool> uiActive { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BeatRepeat)
};

} // namespace chopline::engine
