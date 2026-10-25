// CHOPLINE tests — Phase 3 (spec §6/§12): playback modes (Gate/Loop/Fit/
// One-shot incl. note-off behaviour), varispeed + Signalsmith Stretch pitch
// accuracy (+7 st within 2 cents), Fit length within 1 ms, formant preserve
// on/off, loop crossfade with no clicks, choke groups, unison, AHDSR shape,
// settings-edit safety for ringing voices, and the per-chop state round trip
// (including Phase 1/2-shaped states without the new fields).

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "TestHelpers.h"

#include <cmath>

using Catch::Approx;

using namespace chopline;
using namespace chopline::test;

namespace
{
    constexpr double rate = 48000.0;

    /// 2 s buffer → Equal mode, 2 chops of ~1 s each.
    void loadTwoChops (engine::Engine& eng, const juce::AudioBuffer<float>& buf)
    {
        eng.prepare (rate, 512, 2);
        REQUIRE (eng.loadBufferSync (buf, rate).isEmpty());
        eng.setZeroCrossSnap (false);
        eng.setSliceMode (engine::SliceMode::equal);
        eng.setSensitivity (0.0f); // → 2 slices
        REQUIRE (eng.getNumChops() == 2);
    }

    engine::ChopSettings chop0 (engine::Engine& eng) { return eng.getChopSettings (0); }

    /// Renders `length` samples in 512-blocks; optional note-on at `onAt` and
    /// note-off at `offAt` (absolute sample positions, −1 = none).
    juce::AudioBuffer<float> renderMidi (engine::Engine& eng, int length,
                                         int note = 36, int onAt = 0, int offAt = -1,
                                         juce::uint8 velocity = 127)
    {
        juce::AudioBuffer<float> full (2, length);
        full.clear();
        juce::AudioBuffer<float> block (2, 512);
        const engine::Engine::BlockContext ctx { 36 };
        for (int pos = 0; pos < length; pos += 512)
        {
            const auto n = juce::jmin (512, length - pos);
            block.setSize (2, n, false, false, true);
            juce::MidiBuffer midi;
            if (onAt >= pos && onAt < pos + n)
                midi.addEvent (juce::MidiMessage::noteOn (1, note, velocity), onAt - pos);
            if (offAt >= pos && offAt < pos + n)
                midi.addEvent (juce::MidiMessage::noteOff (1, note), offAt - pos);
            eng.process (block, midi, ctx);
            for (int ch = 0; ch < 2; ++ch)
                full.copyFrom (ch, pos, block, ch, 0, n);
        }
        return full;
    }

    /// Frequency estimate from interpolated positive-going zero crossings.
    double estimateFreq (const juce::AudioBuffer<float>& buf, int start, int end)
    {
        const auto* d = buf.getReadPointer (0);
        double first = -1.0, last = -1.0;
        int cycles = 0;
        for (int i = start + 1; i < juce::jmin (end, buf.getNumSamples()); ++i)
        {
            if (d[i - 1] < 0.0f && d[i] >= 0.0f)
            {
                const auto frac = static_cast<double> (d[i - 1])
                                / (static_cast<double> (d[i - 1]) - static_cast<double> (d[i]));
                const auto t = static_cast<double> (i - 1) + frac;
                if (first < 0.0)
                    first = t;
                else
                {
                    last = t;
                    ++cycles;
                }
            }
        }
        return cycles > 0 && last > first ? rate * cycles / (last - first) : 0.0;
    }

    double centsOff (double freq, double target)
    {
        return 1200.0 * std::log2 (freq / target);
    }

    int lastAudibleSample (const juce::AudioBuffer<float>& buf, float threshold = 1.0e-4f)
    {
        const auto* d = buf.getReadPointer (0);
        for (int i = buf.getNumSamples() - 1; i >= 0; --i)
            if (std::abs (d[i]) > threshold)
                return i;
        return -1;
    }

    /// Rich harmonic test tone (for formant tests): sum of decaying harmonics.
    juce::AudioBuffer<float> makeHarmonicTone (double f0, double lengthSec)
    {
        juce::AudioBuffer<float> buf (1, static_cast<int> (lengthSec * rate));
        for (int i = 0; i < buf.getNumSamples(); ++i)
        {
            double v = 0.0;
            for (int h = 1; h <= 10; ++h)
                v += std::sin (juce::MathConstants<double>::twoPi * f0 * h * i / rate) / h;
            buf.setSample (0, i, static_cast<float> (0.3 * v));
        }
        return buf;
    }
} // namespace

// ---- pitch accuracy (spec §12) ----------------------------------------------------

TEST_CASE ("Varispeed: 440 Hz shifted +7 st lands within 2 cents", "[warp][pitch]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    engine::Engine eng;
    loadTwoChops (eng, makeSine (440.0, 2.0, rate));

    auto s = chop0 (eng);
    s.warp = engine::WarpMode::varispeed;
    s.mode = engine::PlaybackMode::oneShot;
    s.pitchSemitones = 7;
    eng.setChopSettings (0, s);

    const auto out = renderMidi (eng, 32000);
    const auto target = 440.0 * std::pow (2.0, 7.0 / 12.0);
    const auto freq = estimateFreq (out, 4800, 28000);
    INFO ("varispeed freq " << freq);
    REQUIRE (std::abs (centsOff (freq, target)) < 2.0);
}

TEST_CASE ("Stretch: 440 Hz shifted +7 st lands within 2 cents, length preserved",
           "[warp][stretch][pitch]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    engine::Engine eng;
    loadTwoChops (eng, makeSine (440.0, 2.0, rate));

    auto s = chop0 (eng);
    s.warp = engine::WarpMode::stretch;
    s.mode = engine::PlaybackMode::oneShot;
    s.pitchSemitones = 7;
    eng.setChopSettings (0, s);

    const auto chopLen = static_cast<int> (std::llround (eng.getMarkers()[1].timeSec * rate));
    const auto out = renderMidi (eng, chopLen + 9600);

    const auto target = 440.0 * std::pow (2.0, 7.0 / 12.0);
    const auto freq = estimateFreq (out, 9600, chopLen - 9600);
    INFO ("stretch freq " << freq);
    REQUIRE (std::abs (centsOff (freq, target)) < 2.0);

    // Independent time: pitch +7 must NOT shorten the chop (within 1 ms).
    const auto last = lastAudibleSample (out);
    INFO ("stretch length " << last << " vs chop " << chopLen);
    REQUIRE (std::abs (last - chopLen) <= 48);
}

TEST_CASE ("Stretch starts on time (pre-rolled, spec §6)", "[warp][stretch][timing]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    engine::Engine eng;
    loadTwoChops (eng, makeSine (440.0, 2.0, rate));

    auto s = chop0 (eng);
    s.warp = engine::WarpMode::stretch;
    s.mode = engine::PlaybackMode::oneShot;
    s.pitchSemitones = -5;
    eng.setChopSettings (0, s);

    const auto out = renderMidi (eng, 9600, 36, 100);
    const auto first = firstAudibleSample (out);
    INFO ("first audible " << first);
    REQUIRE (first >= 100);
    REQUIRE (first <= 100 + 128); // within the 2 ms declick fade-in
}

// ---- Fit step (spec §6/§12) ---------------------------------------------------------

TEST_CASE ("Fit step fills the event span within 1 ms (compress and stretch)",
           "[warp][fit]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    const auto spanBars = GENERATE_COPY (0.25, 0.75); // 24000 / 72000 samples @120 BPM
    DYNAMIC_SECTION ("span " << spanBars << " bars")
    {
        engine::Engine eng;
        loadTwoChops (eng, makeSine (440.0, 2.0, rate));

        auto s = chop0 (eng);
        s.mode = engine::PlaybackMode::fitStep;
        s.warp = engine::WarpMode::varispeed; // Fit forces the stretch engine regardless
        eng.setChopSettings (0, s);

        eng.getPatternModel().addEvent (0, 0.0, spanBars, 0, 127);

        SimHost host;
        const auto span = static_cast<int> (std::llround (spanBars * host.samplesPerBar()));
        const auto out = renderWithHost (eng, host, 90000, 512);

        REQUIRE (firstAudibleSample (out) <= 128);
        const auto last = lastAudibleSample (out);
        INFO ("fit output ends at " << last << " target " << span);
        REQUIRE (std::abs (last - span) <= 48); // within 1 ms

        // Pitch preserved while time changes.
        const auto freq = estimateFreq (out, span / 4, span * 3 / 4);
        INFO ("fit freq " << freq);
        REQUIRE (std::abs (centsOff (freq, 440.0)) < 4.0);
    }
}

TEST_CASE ("Fit without a span (plain MIDI) behaves like One-shot", "[warp][fit]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    engine::Engine eng;
    loadTwoChops (eng, makeSine (440.0, 2.0, rate));

    auto s = chop0 (eng);
    s.mode = engine::PlaybackMode::fitStep;
    eng.setChopSettings (0, s);

    const auto chopLen = static_cast<int> (std::llround (eng.getMarkers()[1].timeSec * rate));
    const auto out = renderMidi (eng, chopLen + 9600, 36, 0, chopLen / 2); // note-off ignored
    const auto last = lastAudibleSample (out);
    REQUIRE (last > chopLen - 480); // played to its natural end
    REQUIRE (std::abs (last - chopLen) <= 96);
}

// ---- playback modes (spec §6) ----------------------------------------------------------

TEST_CASE ("Gate releases at the event end through the RELEASE time", "[modes][gate]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    engine::Engine eng;
    loadTwoChops (eng, makeConstant (0.5f, 2.0, rate));

    auto s = chop0 (eng);
    s.mode = engine::PlaybackMode::gate;
    s.releaseMs = 50.0f; // 2400 samples
    eng.setChopSettings (0, s);

    eng.getPatternModel().addEvent (0, 0.0, 0.25, 0, 127); // span 24000 @120 BPM

    SimHost host;
    const auto out = renderWithHost (eng, host, 48000, 512);

    // Full level inside the span, silent after span + release (+ margin).
    REQUIRE (std::abs (out.getSample (0, 23000) - 0.5f) < 0.01f);
    const auto last = lastAudibleSample (out);
    INFO ("gate ends at " << last);
    REQUIRE (last >= 24000);
    REQUIRE (last <= 24000 + 2400 + 96);
    // The release is a fade, not a cut.
    REQUIRE (maxSampleJump (out, 0, 1, 30000) < 0.06f);
}

TEST_CASE ("Gate releases at MIDI note-off; One-shot ignores note-off", "[modes][noteoff]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    SECTION ("gate")
    {
        engine::Engine eng;
        loadTwoChops (eng, makeConstant (0.5f, 2.0, rate));
        auto s = chop0 (eng);
        s.mode = engine::PlaybackMode::gate;
        s.releaseMs = 50.0f;
        eng.setChopSettings (0, s);

        const auto out = renderMidi (eng, 32000, 36, 0, 12000);
        REQUIRE (std::abs (out.getSample (0, 11000) - 0.5f) < 0.01f);
        const auto last = lastAudibleSample (out);
        REQUIRE (last >= 12000);
        REQUIRE (last <= 12000 + 2400 + 600); // note-off handled within the block
    }

    SECTION ("one-shot")
    {
        engine::Engine eng;
        loadTwoChops (eng, makeConstant (0.5f, 2.0, rate));
        auto s = chop0 (eng);
        s.mode = engine::PlaybackMode::oneShot;
        eng.setChopSettings (0, s);

        const auto chopLen = static_cast<int> (std::llround (eng.getMarkers()[1].timeSec * rate));
        const auto out = renderMidi (eng, chopLen + 4800, 36, 0, 12000);
        REQUIRE (std::abs (out.getSample (0, 20000) - 0.5f) < 0.01f); // still playing
        REQUIRE (lastAudibleSample (out) > chopLen - 480);            // to the natural end
    }
}

TEST_CASE ("Loop wraps crossfaded with no click and keeps playing while held",
           "[modes][loop]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    engine::Engine eng;
    loadTwoChops (eng, makeConstant (0.5f, 2.0, rate));

    auto s = chop0 (eng);
    s.mode = engine::PlaybackMode::loop;
    s.loopStart = 0.25;
    s.loopEnd = 0.5;
    s.loopXfadeMs = 10.0f;
    eng.setChopSettings (0, s);

    const auto chopLen = static_cast<int> (std::llround (eng.getMarkers()[1].timeSec * rate));
    const auto out = renderMidi (eng, 96000, 36, 0, -1); // held for 2 s

    // Still sounding far past the chop's natural end = it loops.
    REQUIRE (std::abs (out.getSample (0, 90000) - 0.5f) < 0.02f);
    REQUIRE (90000 > chopLen);
    // DC through a LINEAR loop crossfade stays flat: any jump is a click.
    REQUIRE (maxSampleJump (out, 0, 200, 90000) < 0.06f);
}

// ---- choke groups (spec §6) ----------------------------------------------------------------

TEST_CASE ("Choke: a new trigger fades the ringing chop with no click", "[choke]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    engine::Engine eng;
    loadTwoChops (eng, makeStaircase (2, 2.0, rate)); // chop 0 = 0.1, chop 1 = 0.2

    // Default: both chops in choke group 1 (spec §6) — no setup needed.
    REQUIRE (chop0 (eng).chokeGroup == 1);
    REQUIRE (eng.getChopSettings (1).chokeGroup == 1);

    juce::AudioBuffer<float> block (2, 512);
    const engine::Engine::BlockContext ctx { 36 };

    juce::MidiBuffer on0;
    on0.addEvent (juce::MidiMessage::noteOn (1, 36, static_cast<juce::uint8> (127)), 0);
    eng.process (block, on0, ctx);
    REQUIRE (std::abs (block.getSample (0, 400) - 0.1f) < 0.01f);

    juce::MidiBuffer on1;
    on1.addEvent (juce::MidiMessage::noteOn (1, 37, static_cast<juce::uint8> (127)), 0);
    juce::AudioBuffer<float> block2 (2, 512);
    eng.process (block2, on1, ctx);

    // After the 2 ms choke fade (96 samples) only chop 1 remains — no overlap.
    REQUIRE (std::abs (block2.getSample (0, 400) - 0.2f) < 0.015f);
    // And the choke itself is a fade, not a click.
    REQUIRE (maxSampleJump (block2, 0) < 0.06f);
}

// ---- unison (spec §6) -------------------------------------------------------------------------

TEST_CASE ("Unison spreads copies across the stereo field", "[unison]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    auto render = [] (int unison, engine::WarpMode warp)
    {
        engine::Engine eng;
        loadTwoChops (eng, makeSine (440.0, 2.0, rate));
        auto s = eng.getChopSettings (0);
        s.unison = unison;
        s.warp = warp;
        s.mode = engine::PlaybackMode::oneShot;
        eng.setChopSettings (0, s);
        return renderMidi (eng, 24000);
    };

    SECTION ("varispeed")
    {
        const auto mono = render (1, engine::WarpMode::varispeed);
        float lrDiff = 0.0f;
        for (int i = 0; i < mono.getNumSamples(); ++i)
            lrDiff = juce::jmax (lrDiff, std::abs (mono.getSample (0, i) - mono.getSample (1, i)));
        REQUIRE (lrDiff < 1.0e-6f); // unison 1: perfectly centred

        const auto wide = render (4, engine::WarpMode::varispeed);
        float wideDiff = 0.0f, peak = 0.0f;
        for (int i = 4800; i < wide.getNumSamples(); ++i)
        {
            wideDiff = juce::jmax (wideDiff, std::abs (wide.getSample (0, i) - wide.getSample (1, i)));
            peak = juce::jmax (peak, std::abs (wide.getSample (0, i)));
        }
        REQUIRE (wideDiff > 0.05f); // detuned + panned copies decorrelate L/R
        REQUIRE (peak < 2.0f);      // 1/√N compensation keeps the level sane
    }

    SECTION ("stretch")
    {
        const auto wide = render (3, engine::WarpMode::stretch);
        float wideDiff = 0.0f;
        for (int i = 4800; i < wide.getNumSamples(); ++i)
            wideDiff = juce::jmax (wideDiff, std::abs (wide.getSample (0, i) - wide.getSample (1, i)));
        REQUIRE (wideDiff > 0.05f);
    }
}

// ---- formants (spec §6/§12) ---------------------------------------------------------------------

TEST_CASE ("Formant preserve on/off produce audibly different spectra", "[warp][formant]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    auto render = [] (bool preserve)
    {
        engine::Engine eng;
        loadTwoChops (eng, makeHarmonicTone (110.0, 2.0));
        auto s = eng.getChopSettings (0);
        s.warp = engine::WarpMode::stretch;
        s.mode = engine::PlaybackMode::oneShot;
        s.pitchSemitones = 7;
        s.formantPreserve = preserve;
        eng.setChopSettings (0, s);
        return renderMidi (eng, 32000);
    };

    const auto on = render (true);
    const auto off = render (false);

    double rmsOn = 0.0, rmsDiff = 0.0;
    for (int i = 4800; i < 28000; ++i)
    {
        const auto a = static_cast<double> (on.getSample (0, i));
        const auto b = static_cast<double> (off.getSample (0, i));
        rmsOn += a * a;
        rmsDiff += (a - b) * (a - b);
    }
    REQUIRE (rmsOn > 0.0);
    const auto rel = std::sqrt (rmsDiff / rmsOn);
    INFO ("relative difference " << rel);
    REQUIRE (rel > 0.1); // clearly different renders
}

// ---- envelope (v17 ENVELOPE card) ---------------------------------------------------------------

TEST_CASE ("AHDSR shape: attack, hold, decay to sustain", "[env]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    engine::Engine eng;
    loadTwoChops (eng, makeConstant (0.5f, 2.0, rate));

    auto s = chop0 (eng);
    s.mode = engine::PlaybackMode::oneShot;
    s.attackMs = 10.0f;   // 480
    s.holdMs = 20.0f;     // 960
    s.decayMs = 100.0f;   // 4800
    s.sustainPct = 50.0f;
    eng.setChopSettings (0, s);

    eng.audition (0, 1.0f);
    juce::AudioBuffer<float> out (2, 16384);
    out.clear();
    renderBlocks (eng, out, 1); // one 16384-sample block (voice chunks internally)

    // Attack peak at ~480 (× the 2 ms declick fade already done by then).
    REQUIRE (std::abs (out.getSample (0, 500) - 0.5f) < 0.02f);
    // Hold keeps it at 1.0 × DC until ~1440.
    REQUIRE (std::abs (out.getSample (0, 1400) - 0.5f) < 0.02f);
    // After the decay (~1440 + 4800): sustain 50% → 0.25.
    REQUIRE (std::abs (out.getSample (0, 7000) - 0.25f) < 0.02f);
    // Monotonically decreasing through the decay.
    float prev = out.getSample (0, 1500);
    for (int i = 1600; i < 6200; i += 100)
    {
        const auto v = out.getSample (0, i);
        REQUIRE (v <= prev + 1.0e-4f);
        prev = v;
    }
}

// ---- edit safety (Phase 3 requirement) ----------------------------------------------------------

TEST_CASE ("A settings edit never glitches ringing voices", "[warp][edit]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    engine::Engine eng;
    loadTwoChops (eng, makeConstant (0.5f, 2.0, rate));

    auto s = chop0 (eng);
    s.mode = engine::PlaybackMode::oneShot;
    eng.setChopSettings (0, s);

    juce::AudioBuffer<float> full (2, 24000);
    full.clear();
    juce::AudioBuffer<float> block (2, 512);
    const engine::Engine::BlockContext ctx { 36 };
    for (int pos = 0; pos < 24000; pos += 512)
    {
        juce::MidiBuffer midi;
        if (pos == 0)
            midi.addEvent (juce::MidiMessage::noteOn (1, 36, static_cast<juce::uint8> (127)), 0);
        if (pos == 10240) // mid-ring edit: pitch + reverse + mode
        {
            auto edited = chop0 (eng);
            edited.pitchSemitones = 12;
            edited.reverse = true;
            edited.mode = engine::PlaybackMode::loop;
            eng.setChopSettings (0, edited);
        }
        eng.process (block, midi, ctx);
        for (int ch = 0; ch < 2; ++ch)
            full.copyFrom (ch, pos, block, ch, 0, 512);
    }

    // The ringing voice finished on its OLD settings: steady DC, no glitch.
    REQUIRE (std::abs (full.getSample (0, 20000) - 0.5f) < 0.01f);
    REQUIRE (maxSampleJump (full, 0, 200, 23000) < 0.06f);
}

// ---- state (spec §10/§12) ------------------------------------------------------------------------

TEST_CASE ("Per-chop settings round-trip through the plugin state", "[state][perchop]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    engine::Engine engA;
    loadTwoChops (engA, makeSine (330.0, 2.0, rate));

    engine::ChopSettings s;
    s.startOffsetSec = 0.05;
    s.endOffsetSec = -0.1;
    s.fadeInMs = 7.5f;
    s.fadeOutMs = 12.0f;
    s.loopStart = 0.3;
    s.loopEnd = 0.8;
    s.loopXfadeMs = 42.0f;
    s.mode = engine::PlaybackMode::loop;
    s.warp = engine::WarpMode::stretch;
    s.reverse = true;
    s.pitchSemitones = -13;
    s.releaseMs = 123.0f;
    s.delaySend = 35.0f;
    s.verbSend = 60.0f;
    s.unison = 3;
    s.attackMs = 44.0f;
    s.holdMs = 55.0f;
    s.decayMs = 666.0f;
    s.sustainPct = 73.0f;
    s.chokeGroup = 2;
    s.formantPreserve = false;
    engA.setChopSettings (0, s);

    juce::ValueTree root ("CHOPLINE");
    engA.serializeTo (root);

    engine::Engine engB;
    engB.prepare (rate, 512, 2);
    engB.restoreFrom (root, true);

    const auto r = engB.getChopSettings (0);
    REQUIRE (r.startOffsetSec == Catch::Approx (0.05));
    REQUIRE (r.endOffsetSec == Catch::Approx (-0.1));
    REQUIRE (r.fadeInMs == Catch::Approx (7.5f));
    REQUIRE (r.fadeOutMs == Catch::Approx (12.0f));
    REQUIRE (r.loopStart == Catch::Approx (0.3));
    REQUIRE (r.loopEnd == Catch::Approx (0.8));
    REQUIRE (r.loopXfadeMs == Catch::Approx (42.0f));
    REQUIRE (r.mode == engine::PlaybackMode::loop);
    REQUIRE (r.warp == engine::WarpMode::stretch);
    REQUIRE (r.reverse);
    REQUIRE (r.pitchSemitones == -13);
    REQUIRE (r.releaseMs == Catch::Approx (123.0f));
    REQUIRE (r.delaySend == Catch::Approx (35.0f));
    REQUIRE (r.verbSend == Catch::Approx (60.0f));
    REQUIRE (r.unison == 3);
    REQUIRE (r.attackMs == Catch::Approx (44.0f));
    REQUIRE (r.holdMs == Catch::Approx (55.0f));
    REQUIRE (r.decayMs == Catch::Approx (666.0f));
    REQUIRE (r.sustainPct == Catch::Approx (73.0f));
    REQUIRE (r.chokeGroup == 2);
    REQUIRE (! r.formantPreserve);

    // The untouched chop 1 keeps its defaults.
    const auto d = engB.getChopSettings (1);
    REQUIRE (d.mode == engine::PlaybackMode::gate);
    REQUIRE (d.chokeGroup == 1);
}

TEST_CASE ("Phase 1/2-shaped marker state (t/fi/fo only) loads with defaults",
           "[state][migration]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    engine::Engine engA;
    loadTwoChops (engA, makeSine (330.0, 2.0, rate));

    juce::ValueTree root ("CHOPLINE");
    engA.serializeTo (root);

    // Strip every Phase 3 property off the markers — the exact shape a
    // Phase 1/2 session saved.
    auto slices = root.getChildWithName (engine::Engine::slicesTag);
    REQUIRE (slices.isValid());
    for (auto marker : slices)
    {
        for (const auto* key : { "so", "eo", "ls", "le", "lx", "md", "wp", "rv", "pt",
                                 "rl", "dsn", "vsn", "un", "at", "hd", "dc", "su", "ck", "fp" })
            marker.removeProperty (key, nullptr);
        REQUIRE (marker.hasProperty ("t"));
        REQUIRE (marker.hasProperty ("fi"));
    }

    engine::Engine engB;
    engB.prepare (rate, 512, 2);
    engB.restoreFrom (root, true);

    REQUIRE (engB.getNumChops() == 2);
    const auto r = engB.getChopSettings (0);
    REQUIRE (r.mode == engine::PlaybackMode::gate); // v17 default
    REQUIRE (r.warp == engine::WarpMode::varispeed);
    REQUIRE (r.unison == 1);
    REQUIRE (r.chokeGroup == 1);
    REQUIRE (r.releaseMs == Catch::Approx (80.0f));
    REQUIRE (r.fadeInMs == Catch::Approx (2.0f));
}

// ---- robustness ------------------------------------------------------------------------------------

TEST_CASE ("Stretch + unison render safely at block sizes 1–4096", "[warp][blocks]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    engine::Engine eng;
    eng.prepare (rate, 4096, 2);
    REQUIRE (eng.loadBufferSync (makeSine (440.0, 2.0, rate), rate).isEmpty());
    eng.setZeroCrossSnap (false);
    eng.setSliceMode (engine::SliceMode::equal);
    eng.setSensitivity (0.0f);

    auto s = eng.getChopSettings (0);
    s.warp = engine::WarpMode::stretch;
    s.mode = engine::PlaybackMode::gate;
    s.unison = 4;
    s.pitchSemitones = 3;
    eng.setChopSettings (0, s);

    const engine::Engine::BlockContext ctx { 36 };
    for (const int blockSize : { 1, 3, 16, 333, 512, 4096 })
    {
        juce::AudioBuffer<float> out (2, blockSize);
        juce::MidiBuffer midi;
        midi.addEvent (juce::MidiMessage::noteOn (1, 36, static_cast<juce::uint8> (127)), 0);
        eng.process (out, midi, ctx);
        for (int i = 0; i < out.getNumSamples(); ++i)
        {
            REQUIRE (std::isfinite (out.getSample (0, i)));
            REQUIRE (std::isfinite (out.getSample (1, i)));
        }
    }
}
