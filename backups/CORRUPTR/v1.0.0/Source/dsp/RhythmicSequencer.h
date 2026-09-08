#pragma once

#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>
#include <array>
#include <atomic>
#include <cmath>
#include <utility>

//==============================================================================
// Stage 2 Phase 3.7: Rhythmic Sequencer — pattern data + double-buffered
// audio-thread snapshot.
//
// See architecture.md component #5 ("Rhythmic Sequencer") and plan.md's
// "Phase 3.7: Rhythmic Sequencer". Owns the 10-lane x up-to-32-step pattern
// data as custom (non-APVTS) state — architecture.md's explicit Stage 0
// decision: "Sequencer pattern data (16-32 steps x 10 lanes) is custom
// juce::ValueTree state, NOT individual APVTS parameters" (320 values would
// explode the automatable-parameter count and isn't how comparable products
// like Stepic/Effectrix expose step data).
//
// DESIGN RESOLUTION (flagged, architecture.md does not pin this down): the
// LIVE, message-thread-editable representation of the pattern held here
// (`workingPattern`) is a plain fixed-size std::array-based struct
// (PatternSnapshot), NOT a live juce::ValueTree tree of per-step nodes.
// architecture.md's "custom juce::ValueTree state" language is satisfied at
// the SERIALIZATION boundary instead — see toValueTree()/fromValueTree(),
// called only from CORRUPTRAudioProcessor::getStateInformation()/
// setStateInformation() — which is where architecture.md's State
// Persistence section actually specifies the ValueTree requirement ("nested
// juce::ValueTree merged into the same state blob... following the
// DrumRoulette pattern"). A plain fixed-size array is simpler to mutate
// correctly for 10 pattern operations (no ValueTree property-lookup
// overhead on every edit/read) and is trivially, cheaply copyable for the
// double-buffered audio-thread snapshot mechanism below, while still
// producing a genuine juce::ValueTree for the actual persisted-state
// contract.
//
// Thread safety (architecture.md Thread Boundaries: "Sequencer pattern:
// double-buffered ValueTree snapshot, pointer swap (lock-free)"): the
// message thread owns `workingPattern` — every pattern-editing method here
// (setStep, all ten opXxx operations, fromValueTree) runs on the message
// thread only and mutates `workingPattern` directly. publishSnapshot()
// copies workingPattern into whichever of the two preallocated buffers
// (bufferA/bufferB) is NOT currently active, then atomically swaps the
// "active" pointer — the exact "double-buffered snapshot, pointer swap,
// same pattern as DrumRoulette's sample-buffer swap" mechanism
// architecture.md specifies. The audio thread calls ONLY
// getActiveSnapshot() (a single relaxed atomic load — real-time-safe,
// lock-free, no allocation) and reads through the returned pointer; it
// never touches workingPattern, the ValueTree serialization methods, or
// `editRandom` directly.
//
// Message-thread RNG (`editRandom`): used exclusively by the randomized
// pattern operations (opRandom/opMutate/opSyncopate/opHumanize). This is a
// SEPARATE juce::Random instance from PluginProcessor's audio-thread
// `glitchRandom` (Phase 3.5) — deliberately never shared, per this phase's
// explicit task instruction to preserve Phase 3.5's `glitchRandom`
// draw-sequence determinism invariant. Unlike `glitchRandom`, this RNG's
// seed is NOT persisted: what gets saved/restored is the RESULTING pattern
// data itself (via toValueTree()/fromValueTree()), so a reproducible RNG
// STREAM isn't needed here the way it is for Glitch's continuously-drawn,
// never-directly-saved chaos jitter — only the final step values matter for
// recall, and those round-trip exactly.
//==============================================================================
class RhythmicSequencer
{
public:
    static constexpr int kMaxSteps = 32;
    static constexpr int kNumLanes = 10;

    // Fixed lane order — matches architecture.md component #5's explicit
    // list ("Drive, Mix, Filter Cutoff, Bit Depth, Sample Rate, Glitch
    // Probability, Volume, Pan, Pitch, Gate") exactly, in the same order.
    enum Lane
    {
        laneDrive = 0,
        laneMix,
        laneFilterCutoff,
        laneBitDepth,
        laneSampleRate,
        laneGlitchProbability,
        laneVolume,
        lanePan,
        lanePitch,
        laneGate,
        laneCount // == kNumLanes
    };

    // Plain POD snapshot — fixed size, no allocation, trivially copyable.
    // Every lane is bipolar -1..+1 EXCEPT laneGate, which is unipolar 0..1
    // ("how open the gate is this step" — see PluginProcessor.cpp's Phase
    // 3.7 per-lane destination-mapping comments for the full consumption
    // rationale of each lane).
    struct PatternSnapshot
    {
        std::array<std::array<float, kMaxSteps>, kNumLanes> values {};
    };

    RhythmicSequencer()
    {
        clearPatternToDefault(workingPattern);
        bufferA = workingPattern;
        bufferB = workingPattern;
        activeSnapshot.store(&bufferA, std::memory_order_relaxed);
        inactiveIsA = false; // bufferB is the first inactive/staging target
    }

    //=========================================================================
    // Audio-thread API — real-time-safe (single relaxed atomic load, no
    // allocation, no locks). This is the ONLY method the audio thread may
    // call on this class.
    //=========================================================================
    const PatternSnapshot* getActiveSnapshot() const noexcept
    {
        return activeSnapshot.load(std::memory_order_relaxed);
    }

    //=========================================================================
    // Message-thread API (pattern editing). All methods below this point
    // are message-thread-only — never call from processBlock() or any
    // audio-thread context.
    //=========================================================================
    float getStep(int lane, int step) const noexcept
    {
        if (lane < 0 || lane >= kNumLanes || step < 0 || step >= kMaxSteps)
            return 0.0f;
        return workingPattern.values[(size_t) lane][(size_t) step];
    }

    // Single-cell edit (e.g. GUI step-grid drag). Does NOT auto-publish —
    // callers doing many setStep() calls in one gesture (e.g. a drag across
    // several cells) should batch them and call publishSnapshot() once at
    // the end, rather than publishing on every individual cell write.
    void setStep(int lane, int step, float value) noexcept
    {
        if (lane < 0 || lane >= kNumLanes || step < 0 || step >= kMaxSteps)
            return;
        workingPattern.values[(size_t) lane][(size_t) step] =
            juce::jlimit(laneMinValue(lane), laneMaxValue(lane), value);
    }

    // Publishes the current workingPattern to the audio thread: copies into
    // whichever of the two preallocated buffers is currently inactive, then
    // atomically swaps the active pointer. Real-time-safe on its own merits
    // (both buffers preallocated at construction, plain struct copy, no
    // heap traffic) but is intended for the message thread only — it exists
    // so pattern edits/operations become audible without requiring a full
    // prepareToPlay() cycle.
    void publishSnapshot() noexcept
    {
        PatternSnapshot& target = inactiveIsA ? bufferA : bufferB;
        target = workingPattern;
        activeSnapshot.store(&target, std::memory_order_release);
        inactiveIsA = ! inactiveIsA;
    }

    //=========================================================================
    // Pattern operations (message thread only). Each operation mutates
    // `workingPattern` in place AND publishes (calls publishSnapshot()
    // internally) — unlike setStep()'s batched-editing contract, these are
    // typically triggered by a single discrete user action (e.g. a button
    // press), so auto-publishing keeps the call site simple (one call =
    // one audible change) without requiring every caller to remember a
    // separate publish step.
    //
    // architecture.md does not numerically define most of these operations
    // (only names them) — the exact transformation each implements below
    // is this implementation's own resolution, flagged here per this
    // repo's established precedent for documenting such decisions.
    //=========================================================================

    void opClear(int numSteps)
    {
        const int n = juce::jlimit(1, kMaxSteps, numSteps);
        for (int lane = 0; lane < kNumLanes; ++lane)
            for (int step = 0; step < n; ++step)
                workingPattern.values[(size_t) lane][(size_t) step] = laneDefaultValue(lane);
        publishSnapshot();
    }

    void opRandom(int numSteps)
    {
        const int n = juce::jlimit(1, kMaxSteps, numSteps);
        for (int lane = 0; lane < kNumLanes; ++lane)
        {
            const float lo = laneMinValue(lane);
            const float hi = laneMaxValue(lane);
            for (int step = 0; step < n; ++step)
                workingPattern.values[(size_t) lane][(size_t) step] = lo + editRandom.nextFloat() * (hi - lo);
        }
        publishSnapshot();
    }

    // Gentler than opRandom() — nudges each step by a bounded random delta
    // (+/- amount * lane range) rather than fully re-randomizing.
    void opMutate(int numSteps, float amount = 0.25f)
    {
        const int n = juce::jlimit(1, kMaxSteps, numSteps);
        for (int lane = 0; lane < kNumLanes; ++lane)
        {
            const float lo = laneMinValue(lane);
            const float hi = laneMaxValue(lane);
            const float range = hi - lo;
            for (int step = 0; step < n; ++step)
            {
                float& v = workingPattern.values[(size_t) lane][(size_t) step];
                v = juce::jlimit(lo, hi, v + (editRandom.nextFloat() * 2.0f - 1.0f) * amount * range);
            }
        }
        publishSnapshot();
    }

    // Reverses step order across all lanes (whole-pattern time-reversal).
    void opReverse(int numSteps)
    {
        const int n = juce::jlimit(1, kMaxSteps, numSteps);
        for (int lane = 0; lane < kNumLanes; ++lane)
        {
            auto& row = workingPattern.values[(size_t) lane];
            int a = 0, b = n - 1;
            while (a < b)
            {
                std::swap(row[(size_t) a], row[(size_t) b]);
                ++a;
                --b;
            }
        }
        publishSnapshot();
    }

    // Overwrites the second half of the active window with a mirrored
    // (palindrome) copy of the first half — distinct from opReverse(),
    // which reverses the WHOLE pattern in place rather than mirroring one
    // half onto the other. Odd numSteps: the middle step is left as-is.
    void opMirror(int numSteps)
    {
        const int n = juce::jlimit(1, kMaxSteps, numSteps);
        for (int lane = 0; lane < kNumLanes; ++lane)
        {
            auto& row = workingPattern.values[(size_t) lane];
            for (int step = n / 2; step < n; ++step)
                row[(size_t) step] = row[(size_t) (n - 1 - step)];
        }
        publishSnapshot();
    }

    // Circular shift by `amount` steps (positive = later, negative =
    // earlier), wrapping within the active numSteps window only.
    void opShift(int numSteps, int amount)
    {
        const int n = juce::jlimit(1, kMaxSteps, numSteps);
        for (int lane = 0; lane < kNumLanes; ++lane)
        {
            auto& row = workingPattern.values[(size_t) lane];
            std::array<float, kMaxSteps> shifted {};
            for (int step = 0; step < n; ++step)
            {
                const int src = ((step - amount) % n + n) % n;
                shifted[(size_t) step] = row[(size_t) src];
            }
            for (int step = 0; step < n; ++step)
                row[(size_t) step] = shifted[(size_t) step];
        }
        publishSnapshot();
    }

    // "Half": stretches the first numSteps/2 steps across the whole active
    // window (nearest-neighbour hold, each source step repeated twice) —
    // halves the pattern's perceived rhythmic resolution without changing
    // `sequencerSteps` itself. architecture.md doesn't define Half/Double
    // numerically; this is this implementation's own resolution.
    void opHalf(int numSteps)
    {
        const int n = juce::jlimit(1, kMaxSteps, numSteps);
        std::array<float, kMaxSteps> result {};
        for (int lane = 0; lane < kNumLanes; ++lane)
        {
            auto& row = workingPattern.values[(size_t) lane];
            for (int step = 0; step < n; ++step)
                result[(size_t) step] = row[(size_t) (step / 2)];
            for (int step = 0; step < n; ++step)
                row[(size_t) step] = result[(size_t) step];
        }
        publishSnapshot();
    }

    // "Double": the inverse of Half — samples every other step (even
    // indices) and spreads them across the full active window, doubling
    // perceived rhythmic resolution.
    void opDouble(int numSteps)
    {
        const int n = juce::jlimit(1, kMaxSteps, numSteps);
        std::array<float, kMaxSteps> result {};
        for (int lane = 0; lane < kNumLanes; ++lane)
        {
            auto& row = workingPattern.values[(size_t) lane];
            for (int step = 0; step < n; ++step)
                result[(size_t) step] = row[(size_t) juce::jmin(n - 1, step * 2)];
            for (int step = 0; step < n; ++step)
                row[(size_t) step] = result[(size_t) step];
        }
        publishSnapshot();
    }

    // Randomly swaps a subset of adjacent step pairs (per-lane, per-pair
    // Bernoulli trial) to introduce off-grid rhythmic accents.
    // architecture.md doesn't define Syncopate numerically; this
    // implementation's own resolution.
    void opSyncopate(int numSteps, float probability = 0.3f)
    {
        const int n = juce::jlimit(1, kMaxSteps, numSteps);
        for (int lane = 0; lane < kNumLanes; ++lane)
        {
            auto& row = workingPattern.values[(size_t) lane];
            for (int step = 0; step < n - 1; ++step)
                if (editRandom.nextFloat() < probability)
                    std::swap(row[(size_t) step], row[(size_t) (step + 1)]);
        }
        publishSnapshot();
    }

    // Subtle per-step random jitter (smaller default amount than
    // opMutate()) intended to emulate "human" imprecision rather than a
    // deliberate pattern variation. Reuses opMutate()'s math at a gentler
    // default depth.
    void opHumanize(int numSteps, float amount = 0.08f)
    {
        opMutate(numSteps, amount); // opMutate() already publishes
    }

    //=========================================================================
    // Lane semantics — min/max/default value per lane. Every lane is
    // bipolar -1..+1 with a "no modulation" default of 0.0 EXCEPT laneGate,
    // which is unipolar 0..1 with a "fully open / no gating effect" default
    // of 1.0 (see PluginProcessor.cpp's Phase 3.7 comments for why: Gate's
    // destination-side base is "fully open," so a default step value of 1.0
    // yields zero net accumulator contribution, matching every other lane's
    // "default = no effect" semantics even though its numeric range
    // differs).
    //=========================================================================
    static constexpr float laneMinValue(int lane) noexcept { return lane == laneGate ? 0.0f : -1.0f; }
    static constexpr float laneMaxValue(int) noexcept { return 1.0f; }
    static constexpr float laneDefaultValue(int lane) noexcept { return lane == laneGate ? 1.0f : 0.0f; }

    //=========================================================================
    // Serialization (message thread only) — called exclusively from
    // CORRUPTRAudioProcessor::getStateInformation()/setStateInformation().
    // See this class's top doc comment for why the LIVE representation
    // above is a plain array while THIS is where the actual
    // juce::ValueTree architecture.md's State Persistence section
    // describes is materialized.
    //=========================================================================
    juce::ValueTree toValueTree() const
    {
        juce::ValueTree root("SequencerPattern");
        root.setProperty("version", 1, nullptr);
        for (int lane = 0; lane < kNumLanes; ++lane)
        {
            juce::ValueTree laneTree("Lane");
            laneTree.setProperty("index", lane, nullptr);
            juce::String csv;
            for (int step = 0; step < kMaxSteps; ++step)
            {
                if (step > 0)
                    csv << ",";
                csv << juce::String(workingPattern.values[(size_t) lane][(size_t) step], 6);
            }
            laneTree.setProperty("values", csv, nullptr);
            root.appendChild(laneTree, nullptr);
        }
        return root;
    }

    // Restores from a ValueTree produced by toValueTree(). Never crashes on
    // malformed input (architecture.md Restore Behavior: "fall back to a
    // default empty pattern... never crash on malformed preset data") — any
    // structural problem (wrong/missing tag, missing lane, unparsable CSV
    // token, non-finite value) falls back to the default value for that
    // lane/step individually rather than aborting the whole restore.
    // Automatically publishes the restored pattern to the audio thread.
    void fromValueTree(const juce::ValueTree& tree)
    {
        if (! tree.isValid() || tree.getType().toString() != "SequencerPattern")
        {
            clearPatternToDefault(workingPattern);
            publishSnapshot();
            return;
        }

        // Start from a known-good default, then overlay whatever validly
        // parses — a partially-corrupt preset still restores as much of
        // the real pattern as possible rather than discarding it entirely.
        clearPatternToDefault(workingPattern);

        for (int lane = 0; lane < kNumLanes; ++lane)
        {
            const juce::ValueTree laneTree = tree.getChildWithProperty("index", lane);
            if (! laneTree.isValid())
                continue; // missing lane -> stays at default for this lane only

            const juce::String csv = laneTree.getProperty("values", juce::String()).toString();
            const juce::StringArray tokens = juce::StringArray::fromTokens(csv, ",", "");
            for (int step = 0; step < kMaxSteps && step < tokens.size(); ++step)
            {
                const float parsed = tokens[step].getFloatValue();
                if (std::isfinite(parsed))
                    workingPattern.values[(size_t) lane][(size_t) step] =
                        juce::jlimit(laneMinValue(lane), laneMaxValue(lane), parsed);
            }
        }

        publishSnapshot();
    }

private:
    static void clearPatternToDefault(PatternSnapshot& p)
    {
        for (int lane = 0; lane < kNumLanes; ++lane)
            for (int step = 0; step < kMaxSteps; ++step)
                p.values[(size_t) lane][(size_t) step] = laneDefaultValue(lane);
    }

    PatternSnapshot workingPattern;              // message-thread-owned, authoritative live pattern
    PatternSnapshot bufferA, bufferB;             // preallocated, fixed-size — the two double-buffer slots
    std::atomic<PatternSnapshot*> activeSnapshot { nullptr };
    bool inactiveIsA = true;                      // tracks which of bufferA/bufferB is currently the staging target

    juce::Random editRandom { juce::Random::getSystemRandom().nextInt64() }; // message-thread-only; see top doc comment for why this is never shared with the audio-thread glitchRandom instance
};
