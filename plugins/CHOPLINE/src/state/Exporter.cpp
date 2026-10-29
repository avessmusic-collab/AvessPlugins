// CHOPLINE — Exporter implementation. See header for the contracts.

#include "Exporter.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <cmath>

namespace chopline::state
{

namespace
{
    constexpr int midiPpq = 960;
    constexpr double quartersPerBar = 4.0; // the grid's 4/4 box model
} // namespace

juce::MidiFile buildPatternMidi (const PatternModel& model, int slot,
                                 int baseNote, double bpm)
{
    juce::MidiFile midi;
    midi.setTicksPerQuarterNote (midiPpq);

    juce::MidiMessageSequence track;
    track.addEvent (juce::MidiMessage::tempoMetaEvent (
        static_cast<int> (std::llround (60.0e6 / juce::jlimit (20.0, 999.0, bpm)))));

    const auto n = model.getNumEvents (slot);
    for (int i = 0; i < n; ++i)
    {
        const auto ev = model.getEvent (slot, i);
        const auto note = juce::jlimit (0, 127, baseNote + ev.chop);
        const auto ratchet = ev.lenBars > 0.0
            ? engine::clampRatchet (juce::jlimit (1, 8, ev.ratchet))
            : 1;
        const auto subLen = ev.lenBars / static_cast<double> (ratchet);

        auto velocity = static_cast<float> (juce::jlimit (0, 127, ev.velocity)) / 127.0f;
        for (int k = 0; k < ratchet; ++k)
        {
            const auto startQ = (ev.startBar + static_cast<double> (k) * subLen)
                              * quartersPerBar;
            const auto lenQ = juce::jmax (1.0 / midiPpq,
                                          (ratchet > 1 ? subLen : ev.lenBars) * quartersPerBar);
            const auto vel = static_cast<juce::uint8> (
                juce::jlimit (1, 127, juce::roundToInt (velocity * 127.0f)));
            track.addEvent (juce::MidiMessage::noteOn (1, note, vel),
                            startQ * midiPpq);
            track.addEvent (juce::MidiMessage::noteOff (1, note),
                            (startQ + lenQ) * midiPpq);
            velocity *= engine::ratchetDecay;
        }
    }

    track.updateMatchedPairs();
    track.sort();
    midi.addTrack (track);
    return midi;
}

bool writeMidiFile (const juce::MidiFile& midi, const juce::File& file)
{
    file.deleteFile();
    if (auto stream = file.createOutputStream())
        return midi.writeTo (*stream, 1);
    return false;
}

juce::AudioBuffer<float> renderPatternOffline (const juce::ValueTree& fullState,
                                               const OfflineRenderSpec& spec,
                                               juce::String& error)
{
    error.clear();

    const auto rate = spec.sampleRate > 0.0 ? spec.sampleRate : 48000.0;
    const auto blockSize = juce::jlimit (16, 4096, spec.blockSize);
    const auto bpm = juce::jlimit (20.0, 999.0, spec.bpm);

    engine::Engine eng;
    eng.prepare (rate, blockSize, 2);
    eng.restoreFrom (fullState, true); // synchronous: embedded FLAC / path load

    const auto slot = juce::jlimit (0, PatternModel::numSlots - 1, spec.slot);
    auto& pm = eng.getPatternModel();
    if (eng.currentSample() == nullptr)
    {
        error = "No sample loaded";
        return {};
    }
    if (pm.getNumEvents (slot) <= 0)
    {
        error = "The pattern is empty";
        return {};
    }

    const auto bars = pm.getPatternBars (slot);
    const auto samplesPerBar = rate * 60.0 / bpm * quartersPerBar;
    const auto cycleLen = static_cast<int> (std::llround (samplesPerBar * bars));
    const auto cycles = spec.keepSecondCycle ? 2 : 1;
    const auto totalLen = cycleLen * cycles;
    if (cycleLen <= 0)
    {
        error = "Zero-length pattern";
        return {};
    }

    auto ctx = spec.context;
    ctx.hasHostPosition = true;
    ctx.hostPlaying = true;
    ctx.hasPpq = true;
    ctx.hasBpm = true;
    ctx.bpm = bpm;
    ctx.timeSigNumerator = 4;
    ctx.timeSigDenominator = 4;
    ctx.patternSlot = slot;
    ctx.eco = false; // spec §8: offline renders run HQ
    ctx.master.ecoNoOversample = false;
    ctx.freeRunGate = true;

    juce::AudioBuffer<float> full (2, totalLen);
    full.clear();
    juce::AudioBuffer<float> block (2, blockSize);
    const juce::MidiBuffer noMidi;

    double ppq = 0.0;
    for (int pos = 0; pos < totalLen; pos += blockSize)
    {
        const auto n = juce::jmin (blockSize, totalLen - pos);
        block.setSize (2, n, false, false, true);
        ctx.ppqPosition = ppq;
        eng.process (block, noMidi, ctx);
        for (int ch = 0; ch < 2; ++ch)
            full.copyFrom (ch, pos, block, ch, 0, n);
        ppq += static_cast<double> (n) / rate * bpm / 60.0;
        if (spec.onProgress != nullptr)
            spec.onProgress (static_cast<float> (pos + n) / static_cast<float> (totalLen));
    }

    if (! spec.keepSecondCycle)
        return full;

    juce::AudioBuffer<float> out (2, cycleLen);
    for (int ch = 0; ch < 2; ++ch)
        out.copyFrom (ch, 0, full, ch, cycleLen, cycleLen);
    return out;
}

bool writeWavFile (const juce::AudioBuffer<float>& buffer, double sampleRate,
                   const juce::File& file)
{
    if (buffer.getNumSamples() <= 0)
        return false;
    file.deleteFile();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> stream = file.createOutputStream();
    if (stream == nullptr)
        return false;
    auto writer = wav.createWriterFor (stream,
                                       juce::AudioFormatWriterOptions{}
                                           .withSampleRate (sampleRate)
                                           .withNumChannels (buffer.getNumChannels())
                                           .withBitsPerSample (24));
    if (writer == nullptr)
        return false;
    return writer->writeFromAudioSampleBuffer (buffer, 0, buffer.getNumSamples());
}

int exportChopWavs (engine::Engine& eng, const juce::File& dir, juce::String& error)
{
    error.clear();
    const auto sample = eng.currentSample();
    const auto& markers = eng.getMarkers();
    if (sample == nullptr || sample->numFrames() <= 0 || markers.empty())
    {
        error = "No sample loaded";
        return 0;
    }
    if (! dir.createDirectory())
    {
        error = "Cannot create " + dir.getFullPathName();
        return 0;
    }

    const auto rate = sample->sessionRate;
    const auto total = sample->numFrames();
    const auto lengthSec = sample->lengthSeconds();
    const auto baseName = sample->fileName.isNotEmpty()
        ? juce::File::createLegalFileName (sample->fileName.upToLastOccurrenceOf (".", false, false))
        : juce::String ("chop");

    int written = 0;
    const auto numChops = eng.getNumChops();
    for (int i = 0; i < numChops; ++i)
    {
        const auto s = eng.getChopSettings (i);
        const auto baseStart = markers[static_cast<size_t> (i)].timeSec;
        const auto baseEnd = i + 1 < static_cast<int> (markers.size())
            ? markers[static_cast<size_t> (i) + 1].timeSec
            : lengthSec;
        const auto startSec = juce::jlimit (0.0, lengthSec, baseStart + s.startOffsetSec);
        const auto endSec = juce::jlimit (startSec, lengthSec, baseEnd + s.endOffsetSec);

        const auto start = juce::jlimit<juce::int64> (0, total,
            static_cast<juce::int64> (std::llround (startSec * rate)));
        const auto end = juce::jlimit<juce::int64> (start, total,
            static_cast<juce::int64> (std::llround (endSec * rate)));
        const auto len = static_cast<int> (end - start);
        if (len < 2)
            continue;

        juce::AudioBuffer<float> chop (sample->audio.getNumChannels(), len);
        for (int ch = 0; ch < chop.getNumChannels(); ++ch)
            chop.copyFrom (ch, 0, sample->audio, ch, static_cast<int> (start), len);

        const auto file = dir.getChildFile (
            juce::String::formatted ("%02d ", i + 1) + baseName + ".wav");
        if (writeWavFile (chop, rate, file))
            ++written;
    }

    if (written == 0)
        error = "No chops written";
    return written;
}

} // namespace chopline::state
