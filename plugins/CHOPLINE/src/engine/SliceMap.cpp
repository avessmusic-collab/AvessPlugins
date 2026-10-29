// CHOPLINE — SliceMap implementation. See header for the model.

#include "SliceMap.h"

#include "TransientDetector.h"

#include <algorithm>
#include <cmath>

namespace chopline::engine
{

ChopSettings ChopSettings::clamped (ChopSettings s) noexcept
{
    s.fadeInMs = juce::jlimit (0.0f, SliceModel::maxFadeMs, s.fadeInMs);
    s.fadeOutMs = juce::jlimit (0.0f, SliceModel::maxFadeMs, s.fadeOutMs);
    s.loopStart = juce::jlimit (0.0, 0.95, s.loopStart);
    s.loopEnd = juce::jlimit (s.loopStart + 0.05, 1.0, s.loopEnd);
    s.loopXfadeMs = juce::jlimit (0.0f, 200.0f, s.loopXfadeMs);
    s.mode = static_cast<PlaybackMode> (juce::jlimit (0, 3, static_cast<int> (s.mode)));
    s.warp = static_cast<WarpMode> (juce::jlimit (0, 3, static_cast<int> (s.warp)));
    s.pitchSemitones = juce::jlimit (-24, 24, s.pitchSemitones);
    s.releaseMs = juce::jlimit (5.0f, 500.0f, s.releaseMs);
    s.delaySend = juce::jlimit (0.0f, 100.0f, s.delaySend);
    s.verbSend = juce::jlimit (0.0f, 100.0f, s.verbSend);
    s.unison = juce::jlimit (1, 4, s.unison);
    s.attackMs = juce::jlimit (0.0f, 500.0f, s.attackMs);
    s.holdMs = juce::jlimit (0.0f, 500.0f, s.holdMs);
    s.decayMs = juce::jlimit (5.0f, 1000.0f, s.decayMs);
    s.sustainPct = juce::jlimit (0.0f, 100.0f, s.sustainPct);
    s.chokeGroup = juce::jlimit (0, 16, s.chokeGroup);
    s.grainMs = juce::jlimit (5.0f, 200.0f, s.grainMs);
    s.grainDensity = juce::jlimit (1, 8, s.grainDensity);
    s.grainJitterMs = juce::jlimit (0.0f, 50.0f, s.grainJitterMs);
    s.grainPitchJitterCents = juce::jlimit (0.0f, 100.0f, s.grainPitchJitterCents);
    s.grainSprayMs = juce::jlimit (0.0f, 500.0f, s.grainSprayMs);
    return s;
}

int SliceModel::gridDivisionIndex (float sensitivity01) noexcept
{
    const auto s = juce::jlimit (0.0f, 1.0f, sensitivity01);
    return juce::jlimit (0, 3, static_cast<int> (s * 4.0f)); // 0..3 → 1/4..1/32
}

int SliceModel::equalSliceCount (float sensitivity01) noexcept
{
    const auto s = juce::jlimit (0.0f, 1.0f, sensitivity01);
    return juce::jlimit (2, 64, 2 + static_cast<int> (std::lround (s * 62.0f)));
}

int SliceModel::getNumChops (const SampleData* sample) const noexcept
{
    if (sample == nullptr || sample->numFrames() <= 0)
        return 0;
    return static_cast<int> (juce::jmin<size_t> (markers.size(), SliceMapSnapshot::maxChops));
}

double SliceModel::snapToZeroCross (const SampleData& sample, double timeSec, double windowSec)
{
    const auto rate = sample.sessionRate;
    const auto n = sample.numFrames();
    if (rate <= 0.0 || n < 2)
        return timeSec;

    const auto centre = juce::jlimit<juce::int64> (0, n - 1,
                                                   static_cast<juce::int64> (std::llround (timeSec * rate)));
    const auto radius = static_cast<juce::int64> (windowSec * rate);
    const auto lo = juce::jmax<juce::int64> (1, centre - radius);
    const auto hi = juce::jmin<juce::int64> (n - 1, centre + radius);

    const auto* l = sample.audio.getReadPointer (0);
    const auto* r = sample.audio.getNumChannels() > 1 ? sample.audio.getReadPointer (1) : l;
    auto monoAt = [&] (juce::int64 i) { return 0.5f * (l[i] + r[i]); };

    juce::int64 best = -1;
    for (juce::int64 i = lo; i <= hi; ++i)
    {
        const auto a = monoAt (i - 1);
        const auto b = monoAt (i);
        const bool crossing = (a <= 0.0f && b > 0.0f) || (a >= 0.0f && b < 0.0f)
                           || (a != 0.0f && b == 0.0f);
        if (! crossing)
            continue;
        if (best < 0 || std::llabs (i - centre) < std::llabs (best - centre))
            best = i;
    }

    return best >= 0 ? static_cast<double> (best) / rate : timeSec;
}

void SliceModel::assignFreshIds()
{
    for (auto& m : markers)
        m.id = nextId++;
}

void SliceModel::regenerate (const SampleData* sample)
{
    if (sample == nullptr || sample->numFrames() <= 0)
    {
        markers.clear();
        return;
    }

    const auto length = sample->lengthSeconds();
    const auto minGap = minGapSeconds();

    if (mode == SliceMode::manual)
    {
        if (markers.empty())
        {
            markers.push_back ({ nextId++, 0.0, {} });
        }
        return; // manual mode never regenerates what the user built
    }

    std::vector<double> times;
    times.push_back (0.0);

    switch (mode)
    {
        case SliceMode::transient:
        {
            const TransientDetector::Analysis analysis { sample->onsetEnvelope,
                                                         sample->onsetHopSeconds,
                                                         sample->onsetFrameSeconds };
            for (auto t : TransientDetector::pickOnsets (analysis, sensitivity, minGap))
                times.push_back (t);
            break;
        }
        case SliceMode::grid:
        {
            const auto bpm = sampleBpm > 1.0 ? sampleBpm : 120.0;
            static constexpr double divisionOfQuarter[] = { 1.0, 0.5, 0.25, 0.125 };
            const auto step = (60.0 / bpm) * divisionOfQuarter[gridDivisionIndex (sensitivity)];
            for (auto t = step; t < length - 1.0e-6; t += step)
                times.push_back (t);
            break;
        }
        case SliceMode::equal:
        {
            const auto count = equalSliceCount (sensitivity);
            for (int i = 1; i < count; ++i)
                times.push_back (length * static_cast<double> (i) / count);
            break;
        }
        case SliceMode::manual:
            break; // handled above
    }

    std::sort (times.begin(), times.end());

    markers.clear();
    for (auto t : times)
    {
        auto snapped = (zeroCrossSnap && t > 0.0) ? snapToZeroCross (*sample, t) : t;
        snapped = juce::jlimit (0.0, length, snapped);

        // Enforce the minimum slice length sequentially (keep the earlier marker;
        // the 0 s marker always wins).
        if (! markers.empty() && snapped - markers.back().timeSec < minGap)
            continue;
        // The final chop must also respect the minimum length.
        if (snapped > 0.0 && length - snapped < minGap)
            continue;

        markers.push_back ({ 0, snapped, {} });
        if (markers.size() >= SliceMapSnapshot::maxChops)
            break;
    }

    assignFreshIds();
}

bool SliceModel::moveMarker (int id, double newTimeSec, const SampleData* sample)
{
    if (sample == nullptr)
        return false;

    const auto it = std::find_if (markers.begin(), markers.end(),
                                  [id] (const SliceMarker& m) { return m.id == id; });
    if (it == markers.end() || it == markers.begin()) // the 0 s marker is fixed
        return false;

    const auto index = static_cast<size_t> (std::distance (markers.begin(), it));
    const auto length = sample->lengthSeconds();
    const auto minGap = minGapSeconds();

    const auto lo = markers[index - 1].timeSec + minGap;
    const auto hi = (index + 1 < markers.size() ? markers[index + 1].timeSec : length) - minGap;
    if (lo > hi)
        return false;

    auto t = juce::jlimit (lo, hi, newTimeSec);
    if (zeroCrossSnap)
        t = juce::jlimit (lo, hi, snapToZeroCross (*sample, t));

    if (juce::exactlyEqual (t, it->timeSec))
        return false;

    it->timeSec = t;
    return true;
}

int SliceModel::addMarker (double timeSec, const SampleData* sample)
{
    if (sample == nullptr || markers.size() >= SliceMapSnapshot::maxChops)
        return -1;

    const auto length = sample->lengthSeconds();
    const auto minGap = minGapSeconds();

    auto t = juce::jlimit (0.0, length, timeSec);
    if (zeroCrossSnap)
        t = snapToZeroCross (*sample, t);

    if (markers.empty())
        markers.push_back ({ nextId++, 0.0, {} });

    // Find neighbours.
    auto next = std::find_if (markers.begin(), markers.end(),
                              [t] (const SliceMarker& m) { return m.timeSec > t; });
    const auto prevTime = next == markers.begin() ? 0.0 : std::prev (next)->timeSec;
    const auto nextTime = next == markers.end() ? length : next->timeSec;

    if (t - prevTime < minGap || nextTime - t < minGap)
        return -1; // too close to a neighbour — no-op

    const auto id = nextId++;
    markers.insert (next, { id, t, {} });
    return id;
}

// ---- per-chop settings (Phase 3) -----------------------------------------------

ChopSettings SliceModel::getChopSettings (int index) const noexcept
{
    if (index < 0 || index >= static_cast<int> (markers.size()))
        return {};
    return markers[static_cast<size_t> (index)].settings;
}

bool SliceModel::setChopSettings (int index, const ChopSettings& s) noexcept
{
    if (index < 0 || index >= static_cast<int> (markers.size()))
        return false;
    markers[static_cast<size_t> (index)].settings = ChopSettings::clamped (s);
    return true;
}

bool SliceModel::removeMarker (int id)
{
    const auto it = std::find_if (markers.begin(), markers.end(),
                                  [id] (const SliceMarker& m) { return m.id == id; });
    if (it == markers.end() || it == markers.begin()) // never delete the 0 s marker
        return false;
    markers.erase (it);
    return true;
}

void SliceModel::setMarkersFromState (std::vector<SliceMarker> restored)
{
    std::sort (restored.begin(), restored.end(),
               [] (const SliceMarker& a, const SliceMarker& b) { return a.timeSec < b.timeSec; });

    markers.clear();
    for (auto& m : restored)
    {
        if (markers.size() >= SliceMapSnapshot::maxChops)
            break;
        m.id = nextId++;
        m.timeSec = juce::jmax (0.0, m.timeSec);
        m.settings = ChopSettings::clamped (m.settings);
        markers.push_back (m);
    }

    if (! markers.empty() && markers.front().timeSec > 1.0e-9)
        markers.insert (markers.begin(), { nextId++, 0.0, {} });
}

SliceMapSnapshot SliceModel::buildSnapshot (const SampleData* sample) const
{
    SliceMapSnapshot snap;
    if (sample == nullptr || sample->numFrames() <= 0 || markers.empty())
        return snap;

    const auto rate = sample->sessionRate;
    const auto totalFrames = sample->numFrames();
    const auto lengthSec = sample->lengthSeconds();
    const auto minLen = minGapSeconds();

    auto msToSamples = [rate] (float ms)
    {
        return static_cast<int> (std::lround (static_cast<double> (ms) * 0.001 * rate));
    };

    for (size_t i = 0; i < markers.size() && snap.numChops < SliceMapSnapshot::maxChops; ++i)
    {
        const auto s = ChopSettings::clamped (markers[i].settings);

        const auto baseStart = markers[i].timeSec;
        const auto baseEnd = i + 1 < markers.size() ? markers[i + 1].timeSec : lengthSec;

        // v17 trim limits: start ∈ [previous chop marker, effEnd − minLen],
        // end ∈ [effStart + minLen, end of the NEXT chop].
        const auto prevBound = i > 0 ? markers[i - 1].timeSec : 0.0;
        const auto nextBound = i + 2 < markers.size() ? markers[i + 2].timeSec : lengthSec;

        auto effEndSec = baseEnd + s.endOffsetSec;
        effEndSec = juce::jlimit (juce::jmin (baseStart + minLen, nextBound), nextBound, effEndSec);

        auto startHi = effEndSec - minLen;
        if (startHi < prevBound)
            startHi = prevBound; // degenerate (minLen grown after slicing) — keep safe
        const auto effStartSec = juce::jlimit (prevBound, startHi, baseStart + s.startOffsetSec);

        const auto start = juce::jlimit<juce::int64> (0, totalFrames,
            static_cast<juce::int64> (std::llround (effStartSec * rate)));
        const auto end = juce::jmax (start, juce::jlimit<juce::int64> (0, totalFrames,
            static_cast<juce::int64> (std::llround (effEndSec * rate))));

        // A degenerate chop (< 2 frames) still occupies its slot so snapshot
        // indices stay aligned with marker indices (triggering/editing chop k
        // must always address marker k); the voice rejects it silently.
        auto& chop = snap.chops[static_cast<size_t> (snap.numChops)];
        chop.start = start;
        chop.end = end;

        const auto chopFrames = end - start;
        const auto half = static_cast<int> (chopFrames / 2);
        chop.fadeInSamples = juce::jlimit (0, half, msToSamples (s.fadeInMs));
        chop.fadeOutSamples = juce::jlimit (0, half, msToSamples (s.fadeOutMs));

        chop.mode = s.mode;
        chop.warp = s.warp;
        chop.reverse = s.reverse;
        chop.formantPreserve = s.formantPreserve;
        chop.pitchSemitones = static_cast<float> (s.pitchSemitones);
        chop.unison = s.unison;
        chop.chokeGroup = s.chokeGroup;
        chop.delaySend = s.delaySend / 100.0f;
        chop.verbSend = s.verbSend / 100.0f;

        chop.loopStart = start + static_cast<juce::int64> (std::llround (s.loopStart
                             * static_cast<double> (chopFrames)));
        chop.loopEnd = start + static_cast<juce::int64> (std::llround (s.loopEnd
                           * static_cast<double> (chopFrames)));
        // Non-inverting clamps (a tiny chop may not fit a loop at all — the
        // voice only engages looping when loopEnd > loopStart + 1).
        chop.loopStart = juce::jlimit (start, end, chop.loopStart);
        chop.loopEnd = juce::jlimit (chop.loopStart, end, chop.loopEnd);
        const auto halfLoop = static_cast<int> ((chop.loopEnd - chop.loopStart) / 2);
        chop.loopXfadeSamples = juce::jlimit (0, halfLoop, msToSamples (s.loopXfadeMs));

        chop.grainLenSamples = juce::jmax (8, msToSamples (s.grainMs));
        chop.grainDensity = s.grainDensity;
        chop.grainJitterSamples = juce::jmax (0, msToSamples (s.grainJitterMs));
        chop.grainSpraySamples = juce::jmax (0, msToSamples (s.grainSprayMs));
        chop.grainPitchJitterCents = s.grainPitchJitterCents;

        chop.attackSamples = juce::jmax (0, msToSamples (s.attackMs));
        chop.holdSamples = juce::jmax (0, msToSamples (s.holdMs));
        chop.decaySamples = juce::jmax (1, msToSamples (s.decayMs));
        chop.releaseSamples = juce::jmax (1, msToSamples (s.releaseMs));
        chop.sustainLevel = s.sustainPct / 100.0f;

        ++snap.numChops;
    }

    return snap;
}

} // namespace chopline::engine
