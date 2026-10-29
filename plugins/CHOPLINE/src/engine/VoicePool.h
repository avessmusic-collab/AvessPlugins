// CHOPLINE — VoicePool (spec §3, §6)
// Fixed pool of 16 preallocated ChopVoices plus the StretcherPool they draw
// from. A trigger takes a free voice, or steals the oldest one (the voice
// prerenders a 2 ms fade tail, so stealing never clicks). Choke groups
// (spec §6, default: all chops in group 1): a new trigger fast-fades every
// other ringing voice in its group. Everything audio-thread safe: no
// allocation, plain std::arrays.

#pragma once

#include "ChopVoice.h"

#include <array>

namespace chopline::engine
{

class VoicePool final
{
public:
    static constexpr int numVoices = 16;

    /// Message thread (prepareToPlay). Configures + warms the stretchers.
    /// `cheaperStretch` = Eco quality (Signalsmith presetCheaper, Phase 5).
    void prepare (double sampleRate, int maxBlockSize = 512, bool cheaperStretch = false)
    {
        stretchers.prepare (sampleRate, maxBlockSize, cheaperStretch);
        int index = 0;
        for (auto& v : voices)
            v.prepare (sampleRate, maxBlockSize, stretchers, index++);
        nextStamp = 1;
    }

    void reset() noexcept
    {
        for (auto& v : voices)
            v.reset();
    }

    // ---- audio thread -------------------------------------------------------
    void trigger (const SampleData* sample, const SliceMapSnapshot::Chop& chop,
                  int chopIndex, float gain, int tag = ChopVoice::tagMidi,
                  juce::int64 gateSamples = ChopVoice::gateNatural,
                  int midiNote = -1, double startNorm = 0.0,
                  const TriggerMods& mods = {}) noexcept
    {
        ChopVoice* chosen = nullptr;
        for (auto& v : voices)
        {
            if (! v.isActive())
            {
                chosen = &v;
                break;
            }
        }
        if (chosen == nullptr) // steal the oldest (its prerendered tail declicks)
        {
            chosen = &voices[0];
            for (auto& v : voices)
                if (v.getStamp() < chosen->getStamp())
                    chosen = &v;
        }

        // Choke (spec §6): fast-fade every OTHER ringing voice in the group
        // (the chosen voice's own retrigger is declicked by its steal tail).
        if (chop.chokeGroup > 0)
            for (auto& v : voices)
                if (&v != chosen && v.isActive() && v.getChokeGroup() == chop.chokeGroup)
                    v.fastRelease();

        chosen->trigger (sample, chop, chopIndex, gain, nextStamp++, tag,
                         gateSamples, midiNote, startNorm, mods);
    }

    /// The OLDEST Gate/Loop voice still holding this MIDI note enters its
    /// release stage (overlapping same-note retriggers keep the newer
    /// instance ringing — one note-off closes one note-on).
    void noteOff (int midiNote) noexcept
    {
        ChopVoice* oldest = nullptr;
        for (auto& v : voices)
            if (v.isNoteHeld (midiNote)
                && (oldest == nullptr || v.getStamp() < oldest->getStamp()))
                oldest = &v;
        if (oldest != nullptr)
            oldest->noteOff (midiNote);
    }

    void renderAdd (juce::AudioBuffer<float>& out, int startSample, int numSamples,
                    juce::AudioBuffer<float>* delayBus = nullptr,
                    juce::AudioBuffer<float>* verbBus = nullptr) noexcept
    {
        for (auto& v : voices)
            v.renderAdd (out, startSample, numSamples, delayBus, verbBus);
    }

    void releaseAll() noexcept
    {
        for (auto& v : voices)
            v.fastRelease();
    }

    /// Fast-fades only the voices triggered with the given source tag
    /// (transport stop / loop jump releases pattern voices, spec §8).
    void releaseTagged (int tag) noexcept
    {
        for (auto& v : voices)
            if (v.isActive() && v.getSourceTag() == tag)
                v.fastRelease();
    }

    // ---- message thread -------------------------------------------------------
    int countActive() const noexcept
    {
        int n = 0;
        for (auto& v : voices)
            if (v.isActive())
                ++n;
        return n;
    }

    bool usesSample (const SampleData* s) const noexcept
    {
        for (auto& v : voices)
            if (v.usesSample (s))
                return true;
        return false;
    }

    const ChopVoice& getVoice (int index) const noexcept
    {
        return voices[static_cast<size_t> (juce::jlimit (0, numVoices - 1, index))];
    }

private:
    std::array<ChopVoice, numVoices> voices;
    StretcherPool stretchers;
    std::uint64_t nextStamp = 1;
};

} // namespace chopline::engine
