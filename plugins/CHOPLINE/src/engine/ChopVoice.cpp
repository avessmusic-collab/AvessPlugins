// CHOPLINE — ChopVoice implementation. See header for the design.

#include "ChopVoice.h"

#include <cmath>

namespace chopline::engine
{

namespace
{
    // Equal-power fade gain for position k of total (k in [0, total)).
    inline float fadeGain (juce::int64 k, int total) noexcept
    {
        return std::sin (juce::MathConstants<float>::halfPi
                         * (static_cast<float> (k) + 1.0f) / static_cast<float> (total));
    }

    // Unison constants (documented in CLAUDE.md): symmetric cents spread and
    // stereo positions per voice count, 1/√N gain compensation.
    constexpr float unisonCents[4][4] = {
        { 0.0f, 0.0f, 0.0f, 0.0f },
        { -7.0f, 7.0f, 0.0f, 0.0f },
        { -10.0f, 0.0f, 10.0f, 0.0f },
        { -12.0f, -4.0f, 4.0f, 12.0f },
    };
    constexpr float unisonPan[4][4] = {
        { 0.0f, 0.0f, 0.0f, 0.0f },
        { -0.6f, 0.6f, 0.0f, 0.0f },
        { -0.7f, 0.0f, 0.7f, 0.0f },
        { -0.8f, -0.3f, 0.3f, 0.8f },
    };

    constexpr int microEndFade = 32; // samples: output-domain fade at a hard cut
} // namespace

// ---- lifecycle --------------------------------------------------------------------

void ChopVoice::prepare (double newSampleRate, int maxBlockSize, StretcherPool& stretchers,
                         int newVoiceIndex)
{
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 48000.0;
    maxBlock = juce::jmax (16, maxBlockSize);
    pool = &stretchers;
    voiceIndex = newVoiceIndex;

    fastFadeSamples = juce::jmax (1, static_cast<int> (std::llround (sampleRate * fastFadeSeconds)));
    fastFadeStep = 1.0f / static_cast<float> (fastFadeSamples);

    envScratch.assign (static_cast<size_t> (maxBlock), 0.0f);
    outScratchL.assign (static_cast<size_t> (maxBlock), 0.0f);
    outScratchR.assign (static_cast<size_t> (maxBlock), 0.0f);
    const auto inCap = static_cast<size_t> (stretchers.maxInputScratchSamples() + 16);
    inScratchL.assign (inCap, 0.0f);
    inScratchR.assign (inCap, 0.0f);
    tailL.assign (static_cast<size_t> (fastFadeSamples + 8), 0.0f);
    tailR.assign (static_cast<size_t> (fastFadeSamples + 8), 0.0f);
    mixL.assign (static_cast<size_t> (maxBlock), 0.0f);
    mixR.assign (static_cast<size_t> (maxBlock), 0.0f);

    reset();
}

void ChopVoice::reset() noexcept
{
    stopPlayback (pb);
    tailRemaining = 0;
    tailPos = 0;
    stamp = 0;
    uiChop.store (-1, std::memory_order_relaxed);
    uiPos.store (0.0f, std::memory_order_relaxed);
}

void ChopVoice::stopPlayback (Playback& p) noexcept
{
    releaseStreams (p);
    p.active = false;
    p.stage = EnvStage::idle;
    p.sample.store (nullptr, std::memory_order_relaxed);
}

void ChopVoice::releaseStreams (Playback& p) noexcept
{
    for (auto& s : p.streams)
    {
        if (s.sharedStretcher && s.st != nullptr && pool != nullptr)
            pool->returnShared (s.st);
        s.st = nullptr;
        s.sharedStretcher = false;
        s.dead = true;
    }
}

// ---- triggering --------------------------------------------------------------------

void ChopVoice::trigger (const SampleData* sample, const SliceMapSnapshot::Chop& chop,
                         int chopIndex, float gain, std::uint64_t newStamp, int tag,
                         juce::int64 gateSamples, int midiNote, double startNorm) noexcept
{
    sourceTag = tag;
    if (pb.active)
        prerenderTail(); // 2 ms fade of the running playback, rendered now

    setupPlayback (sample, chop, chopIndex, gain, gateSamples, midiNote, startNorm);
    stamp = newStamp;

    if (pb.active)
    {
        uiChop.store (chopIndex, std::memory_order_relaxed);
        const auto norm = pb.chopLen > 0.0
            ? (pb.chop.reverse ? 1.0 - pb.x0 / pb.chopLen : pb.x0 / pb.chopLen)
            : 0.0;
        uiPos.store (static_cast<float> (norm), std::memory_order_relaxed);
    }
    else if (tailRemaining <= 0) // rejected trigger: never leave a stuck playhead
    {
        uiChop.store (-1, std::memory_order_relaxed);
        uiPos.store (0.0f, std::memory_order_relaxed);
    }
}

void ChopVoice::setupPlayback (const SampleData* sample, const SliceMapSnapshot::Chop& chop,
                               int chopIndex, float gain, juce::int64 gateSamples,
                               int midiNote, double startNorm) noexcept
{
    auto& p = pb;
    p.chop = chop;
    p.chop.fadeInSamples = juce::jmax (1, chop.fadeInSamples);
    p.chop.fadeOutSamples = juce::jmax (1, chop.fadeOutSamples);
    p.chopIndex = chopIndex;
    p.midiNote = midiNote;
    p.outPos = 0;
    p.fastFading = false;
    p.fastGain = 1.0f;
    p.chopLen = static_cast<double> (chop.end - chop.start);

    if (sample == nullptr || p.chopLen < 2.0 || sample->audio.getNumSamples() <= 0)
    {
        p.active = false;
        p.sample.store (nullptr, std::memory_order_relaxed);
        return;
    }

    auto mode = chop.mode;
    if (mode == PlaybackMode::fitStep && gateSamples < 0)
        mode = PlaybackMode::oneShot; // span-less Fit (plain MIDI / audition) = One-shot

    // Loop region in the LOGICAL domain (x = 0 at playback start, increasing).
    p.looping = mode == PlaybackMode::loop && chop.loopEnd > chop.loopStart + 1;
    if (p.looping)
    {
        if (chop.reverse)
        {
            p.loopA = chop.end - chop.loopEnd;
            p.loopB = chop.end - chop.loopStart;
        }
        else
        {
            p.loopA = chop.loopStart - chop.start;
            p.loopB = chop.loopEnd - chop.start;
        }
        // Crossfade needs pre-loop content and at most half the loop.
        p.loopXfade = static_cast<int> (juce::jlimit<juce::int64> (
            0, juce::jmin (p.loopA, (p.loopB - p.loopA) / 2),
            static_cast<juce::int64> (chop.loopXfadeSamples)));
    }
    else
    {
        p.loopA = p.loopB = 0;
        p.loopXfade = 0;
    }

    p.x0 = juce::jlimit (0.0, p.chopLen - 1.0, startNorm * p.chopLen);
    const auto x0i = static_cast<juce::int64> (std::llround (p.x0));
    const auto remaining = juce::jmax (1.0, p.chopLen - p.x0);

    const bool useStretch = mode == PlaybackMode::fitStep || chop.warp == WarpMode::stretch;
    // (Granular / Freeze are Phase 5: they currently play as Varispeed.)

    p.numStreams = juce::jlimit (1, maxUnison, chop.unison);
    const auto uniIdx = p.numStreams - 1;
    p.gain = gain / std::sqrt (static_cast<float> (p.numStreams));

    double inRate = 1.0;
    if (mode == PlaybackMode::fitStep)
        inRate = juce::jlimit (1.0 / StretcherPool::maxInputRate, StretcherPool::maxInputRate,
                               remaining / static_cast<double> (juce::jmax<juce::int64> (1, gateSamples)));

    if (p.looping)
        p.expectedOut = -1;
    else if (useStretch)
    {
        const auto natural = static_cast<juce::int64> (std::llround (remaining / inRate));
        p.expectedOut = mode == PlaybackMode::fitStep ? juce::jmin (gateSamples, natural) : natural;
    }
    else
    {
        p.expectedOut = -1; // varispeed streams end individually at the chop end
    }

    // Envelope + gate.
    p.stage = EnvStage::attack;
    p.stagePos = 0;
    p.envLevel = 0.0;
    p.releaseStep = 0.0;
    p.gateApplies = mode == PlaybackMode::gate || mode == PlaybackMode::loop;
    const auto baseRate = std::pow (2.0, static_cast<double> (chop.pitchSemitones) / 12.0);
    if (! p.gateApplies)
    {
        p.gateRemaining = -3; // no gate (One-shot / Fit ignore note-off, spec §6)
    }
    else if (gateSamples == gateHeld)
    {
        p.gateRemaining = -1; // closed by noteOff()
    }
    else if (gateSamples == gateNatural)
    {
        const auto naturalOut = p.looping ? p.chopLen
                              : (useStretch ? remaining / inRate : remaining / baseRate);
        p.gateRemaining = juce::jmax<juce::int64> (1, static_cast<juce::int64> (std::llround (naturalOut)));
    }
    else
    {
        p.gateRemaining = juce::jmax<juce::int64> (1, gateSamples);
    }

    // Streams. The sample pointer must be published before readFrame is used
    // by the stretch pre-roll below.
    p.sample.store (sample, std::memory_order_relaxed);

    for (int k = 0; k < maxUnison; ++k)
    {
        auto& s = p.streams[k];
        s.st = nullptr;
        s.sharedStretcher = false;
        s.dead = k >= p.numStreams;
        if (s.dead)
            continue;

        s.useStretch = useStretch;
        const auto cents = unisonCents[uniIdx][k];
        const auto pan = unisonPan[uniIdx][k];
        s.panL = juce::jmin (1.0f, 1.0f - pan);
        s.panR = juce::jmin (1.0f, 1.0f + pan);

        if (useStretch)
        {
            s.st = k == 0 ? pool->dedicated (voiceIndex) : pool->takeShared();
            s.sharedStretcher = k != 0;
            if (s.st == nullptr)
            {
                s.dead = true; // shared pool exhausted: this unison copy is silent
                continue;
            }

            s.inRate = inRate;
            s.outDone = 0;
            s.expectedOut = p.looping ? -1
                : static_cast<juce::int64> (std::llround (remaining / inRate));

            s.st->setTransposeSemitones (chop.pitchSemitones + cents / 100.0f,
                                         static_cast<float> (8000.0 / sampleRate));
            s.st->setFormantBase (0.0f);
            if (chop.formantPreserve)
                s.st->setFormantSemitones (0.0f, true); // formants stay put under transpose
            else
                s.st->setFormantFactor (1.0f, false);

            // Pre-roll (spec §6): outputSeek so the chop starts on time.
            const auto seekLen = juce::jmin (static_cast<int> (inScratchL.size()),
                                             s.st->outputSeekLength (static_cast<float> (inRate)));
            for (int i = 0; i < seekLen; ++i)
                readFrame (p, x0i + i, inScratchL[static_cast<size_t> (i)],
                           inScratchR[static_cast<size_t> (i)]);
            const float* ins[2] = { inScratchL.data(), inScratchR.data() };
            s.st->outputSeek (ins, seekLen);
            s.inFed = seekLen;
        }
        else
        {
            s.pos = p.x0;
            s.rate = std::pow (2.0, (static_cast<double> (chop.pitchSemitones)
                                     + static_cast<double> (cents) / 100.0) / 12.0);
        }
    }

    p.active = true;
}

void ChopVoice::noteOff (int midiNote) noexcept
{
    if (pb.active && pb.midiNote == midiNote && pb.gateApplies && pb.gateRemaining == -1)
        pb.gateRemaining = 0; // enters the release stage at the next sample
}

void ChopVoice::fastRelease() noexcept
{
    if (pb.active)
        pb.fastFading = true; // keeps the current fastGain if already fading
}

// ---- envelope ----------------------------------------------------------------------

void ChopVoice::enterRelease (Playback& p) noexcept
{
    p.releaseStep = p.envLevel / static_cast<double> (juce::jmax (1, p.chop.releaseSamples));
    p.stage = EnvStage::release;
}

double ChopVoice::advanceEnv (Playback& p) noexcept
{
    for (;;)
    {
        switch (p.stage)
        {
            case EnvStage::attack:
                if (p.stagePos >= p.chop.attackSamples)
                {
                    p.stage = EnvStage::hold;
                    p.stagePos = 0;
                    continue;
                }
                p.envLevel = static_cast<double> (p.stagePos + 1)
                           / static_cast<double> (p.chop.attackSamples);
                ++p.stagePos;
                return p.envLevel;

            case EnvStage::hold:
                if (p.stagePos >= p.chop.holdSamples)
                {
                    p.stage = EnvStage::decay;
                    p.stagePos = 0;
                    continue;
                }
                p.envLevel = 1.0;
                ++p.stagePos;
                return 1.0;

            case EnvStage::decay:
                if (p.stagePos >= p.chop.decaySamples)
                {
                    p.stage = EnvStage::sustain;
                    continue;
                }
                p.envLevel = 1.0 - (1.0 - static_cast<double> (p.chop.sustainLevel))
                                       * static_cast<double> (p.stagePos + 1)
                                       / static_cast<double> (p.chop.decaySamples);
                ++p.stagePos;
                return p.envLevel;

            case EnvStage::sustain:
                p.envLevel = static_cast<double> (p.chop.sustainLevel);
                return p.envLevel;

            case EnvStage::release:
                p.envLevel -= p.releaseStep;
                if (p.envLevel <= 0.0)
                {
                    p.envLevel = 0.0;
                    p.stage = EnvStage::idle;
                }
                return p.envLevel;

            case EnvStage::idle:
                return 0.0;
        }
    }
}

// ---- source reading ------------------------------------------------------------------

void ChopVoice::readFrame (const Playback& p, juce::int64 x, float& l, float& r) const noexcept
{
    l = r = 0.0f;
    const auto* s = p.sample.load (std::memory_order_relaxed);
    if (s == nullptr || x < 0)
        return;

    const auto chopLenI = static_cast<juce::int64> (p.chopLen);

    if (p.looping && p.loopB > p.loopA && x >= p.loopB)
        x = p.loopA + (x - p.loopA) % (p.loopB - p.loopA);

    if (x >= chopLenI)
        return; // past the end (non-loop)

    const auto total = static_cast<juce::int64> (s->audio.getNumSamples());
    const auto* srcL = s->audio.getReadPointer (0);
    const auto* srcR = s->audio.getNumChannels() > 1 ? s->audio.getReadPointer (1) : srcL;

    auto fetch = [&] (juce::int64 xl, float& fl, float& fr) noexcept
    {
        const auto frame = p.chop.reverse ? (p.chop.end - 1 - xl) : (p.chop.start + xl);
        if (frame < 0 || frame >= total)
        {
            fl = fr = 0.0f;
            return;
        }
        fl = srcL[frame];
        fr = srcR[frame];
    };

    float fl = 0.0f, fr = 0.0f;
    if (p.looping && p.loopXfade > 0 && x >= p.loopB - p.loopXfade)
    {
        // LINEAR crossfade into the pre-loop content (correlated material at
        // the loop point: linear keeps the level flat; equal-power would bump).
        const auto w = static_cast<float> (x - (p.loopB - p.loopXfade) + 1)
                     / static_cast<float> (p.loopXfade);
        float al = 0.0f, ar = 0.0f, bl = 0.0f, br = 0.0f;
        fetch (x, al, ar);
        fetch (x - (p.loopB - p.loopA), bl, br);
        fl = al * (1.0f - w) + bl * w;
        fr = ar * (1.0f - w) + br * w;
    }
    else
    {
        fetch (x, fl, fr);
    }

    // Non-destructive declick fades in the logical domain (spec §4).
    auto g = 1.0f;
    if (x < p.chop.fadeInSamples)
        g *= fadeGain (x, p.chop.fadeInSamples);
    if (! p.looping)
    {
        const auto remain = chopLenI - x;
        if (remain <= p.chop.fadeOutSamples)
            g *= fadeGain (remain - 1, p.chop.fadeOutSamples);
    }

    l = fl * g;
    r = fr * g;
}

void ChopVoice::readInterp (const Playback& p, double x, float& l, float& r) const noexcept
{
    const auto xi = static_cast<juce::int64> (std::floor (x));
    const auto t = static_cast<float> (x - static_cast<double> (xi));

    if (t < 1.0e-6f) // exact integer position: bit-exact passthrough at rate 1
    {
        readFrame (p, xi, l, r);
        return;
    }

    float l0, r0, l1, r1, l2, r2, l3, r3;
    readFrame (p, xi - 1, l0, r0);
    readFrame (p, xi, l1, r1);
    readFrame (p, xi + 1, l2, r2);
    readFrame (p, xi + 2, l3, r3);

    const auto t2 = t * t;
    const auto t3 = t2 * t;
    auto catmull = [t, t2, t3] (float a, float b, float c, float d) noexcept
    {
        return 0.5f * (2.0f * b + (-a + c) * t
                       + (2.0f * a - 5.0f * b + 4.0f * c - d) * t2
                       + (-a + 3.0f * b - 3.0f * c + d) * t3);
    };
    l = catmull (l0, l1, l2, l3);
    r = catmull (r0, r1, r2, r3);
}

// ---- rendering ---------------------------------------------------------------------------

void ChopVoice::renderVarispeed (Playback& p, Stream& s, float* outL, float* outR, int n) noexcept
{
    const bool stereo = outR != outL; // mono bus: left only (no double-add)
    for (int i = 0; i < n; ++i)
    {
        if (! p.looping && s.pos >= p.chopLen)
        {
            s.dead = true;
            return;
        }
        float l, r;
        readInterp (p, s.pos, l, r);
        const auto g = envScratch[static_cast<size_t> (i)];
        outL[i] += l * g * s.panL;
        if (stereo)
            outR[i] += r * g * s.panR;
        s.pos += s.rate;
    }

    // Keep the loop position bounded (precision over long holds).
    if (p.looping && p.loopB > p.loopA && s.pos >= static_cast<double> (p.loopB))
        s.pos = static_cast<double> (p.loopA)
              + std::fmod (s.pos - static_cast<double> (p.loopA),
                           static_cast<double> (p.loopB - p.loopA));
}

void ChopVoice::renderStretch (Playback& p, Stream& s, float* outL, float* outR, int n) noexcept
{
    if (s.st == nullptr)
    {
        s.dead = true;
        return;
    }

    // Input needed so that fed-input stays inputLatency + rate·(output+outputLatency)
    // ahead (the Signalsmith steady-state relationship after outputSeek).
    const auto target = static_cast<juce::int64> (std::llround (
                            s.inRate * static_cast<double> (s.outDone + n + s.st->outputLatency())))
                        + s.st->inputLatency();
    const auto need = static_cast<int> (juce::jlimit<juce::int64> (
        0, static_cast<juce::int64> (inScratchL.size()), target - s.inFed));

    const auto x0i = static_cast<juce::int64> (std::llround (p.x0));
    for (int k = 0; k < need; ++k)
        readFrame (p, x0i + s.inFed + k, inScratchL[static_cast<size_t> (k)],
                   inScratchR[static_cast<size_t> (k)]);

    const float* ins[2] = { inScratchL.data(), inScratchR.data() };
    float* outs[2] = { outScratchL.data(), outScratchR.data() };
    s.st->process (ins, need, outs, n);
    s.inFed += need;

    const bool stereo = outR != outL; // mono bus: left only (no double-add)
    for (int i = 0; i < n; ++i)
    {
        const auto g = envScratch[static_cast<size_t> (i)];
        outL[i] += outScratchL[static_cast<size_t> (i)] * g * s.panL;
        if (stereo)
            outR[i] += outScratchR[static_cast<size_t> (i)] * g * s.panR;
    }

    s.outDone += n;
    if (s.expectedOut >= 0 && s.outDone >= s.expectedOut)
        s.dead = true;
}

void ChopVoice::renderChunk (Playback& p, float* outL, float* outR, int n) noexcept
{
    // 1. Envelope × gate × fast fade × end micro-fade into envScratch.
    int endAt = -1;
    for (int i = 0; i < n; ++i)
    {
        if (p.expectedOut >= 0 && p.outPos + i >= p.expectedOut)
        {
            endAt = i;
            break;
        }

        if (p.gateApplies && p.gateRemaining >= 0)
        {
            if (p.gateRemaining == 0)
            {
                if (p.stage != EnvStage::release && p.stage != EnvStage::idle)
                    enterRelease (p);
                p.gateRemaining = -3; // closed
            }
            else
            {
                --p.gateRemaining;
            }
        }

        const auto e = advanceEnv (p);
        if (p.stage == EnvStage::idle && e <= 0.0)
        {
            endAt = i;
            break;
        }

        auto f = 1.0f;
        bool lastSample = false;
        if (p.fastFading)
        {
            f = p.fastGain;
            p.fastGain -= fastFadeStep;
            if (p.fastGain <= 0.0f)
                lastSample = true;
        }

        auto micro = 1.0;
        if (p.expectedOut >= 0)
        {
            const auto remainOut = p.expectedOut - (p.outPos + i);
            if (remainOut < microEndFade)
                micro = static_cast<double> (remainOut) / static_cast<double> (microEndFade);
        }

        envScratch[static_cast<size_t> (i)] = p.gain * static_cast<float> (e * micro) * f;
        if (lastSample)
        {
            endAt = i + 1;
            break;
        }
    }

    const auto effN = endAt >= 0 ? endAt : n;

    // 2. Streams add on top, scaled by envScratch.
    bool anyAlive = false;
    if (effN > 0)
    {
        for (int k = 0; k < p.numStreams; ++k)
        {
            auto& s = p.streams[k];
            if (s.dead)
                continue;
            if (s.useStretch)
                renderStretch (p, s, outL, outR, effN);
            else
                renderVarispeed (p, s, outL, outR, effN);
        }
    }
    for (int k = 0; k < p.numStreams; ++k)
        anyAlive = anyAlive || ! p.streams[k].dead;

    p.outPos += effN;
    if (endAt >= 0 || ! anyAlive)
        stopPlayback (p);
}

void ChopVoice::renderPlayback (Playback& p, float* outL, float* outR, int numSamples) noexcept
{
    int rendered = 0;
    while (rendered < numSamples && p.active)
    {
        const auto chunk = juce::jmin (numSamples - rendered, maxBlock);
        renderChunk (p, outL + rendered, outR + rendered, chunk);
        rendered += chunk;
    }
}

void ChopVoice::prerenderTail() noexcept
{
    // Compact whatever is left of a previous tail to the front.
    if (tailRemaining > 0 && tailPos > 0)
    {
        for (int i = 0; i < tailRemaining; ++i)
        {
            tailL[static_cast<size_t> (i)] = tailL[static_cast<size_t> (tailPos + i)];
            tailR[static_cast<size_t> (i)] = tailR[static_cast<size_t> (tailPos + i)];
        }
    }
    const auto oldRemaining = juce::jmax (0, tailRemaining);
    const auto fadeN = juce::jmin (fastFadeSamples, static_cast<int> (tailL.size()));
    for (int i = oldRemaining; i < fadeN; ++i)
    {
        tailL[static_cast<size_t> (i)] = 0.0f;
        tailR[static_cast<size_t> (i)] = 0.0f;
    }

    // Render the running playback's 2 ms fade NOW (renderChunk applies the
    // fade through fastFading and stops the playback at zero), then free its
    // stretchers for the new note. The tail keeps the faded chop's send
    // levels so its last 2 ms still feed the master delay/verb buses; when an
    // older tail is still fading in the same buffer, the merged PCM keeps the
    // HIGHER of the two levels (bounded ≤ 2 ms approximation).
    if (oldRemaining > 0)
    {
        tailDelaySend = juce::jmax (tailDelaySend, pb.chop.delaySend);
        tailVerbSend = juce::jmax (tailVerbSend, pb.chop.verbSend);
    }
    else
    {
        tailDelaySend = pb.chop.delaySend;
        tailVerbSend = pb.chop.verbSend;
    }
    pb.fastFading = true; // keeps the current fastGain if already fading
    renderPlayback (pb, tailL.data(), tailR.data(), fadeN);
    stopPlayback (pb);

    tailPos = 0;
    tailRemaining = juce::jmax (oldRemaining, fadeN);
}

void ChopVoice::renderAdd (juce::AudioBuffer<float>& out, int startSample, int numSamples,
                           juce::AudioBuffer<float>* delayBus,
                           juce::AudioBuffer<float>* verbBus) noexcept
{
    if (numSamples <= 0 || out.getNumChannels() == 0)
        return;
    if (! pb.active && tailRemaining <= 0)
        return;

    auto* outL = out.getWritePointer (0) + startSample;
    auto* outR = out.getNumChannels() > 1 ? out.getWritePointer (1) + startSample : outL;
    auto* dL = delayBus != nullptr ? delayBus->getWritePointer (0) + startSample : nullptr;
    auto* dR = delayBus != nullptr && delayBus->getNumChannels() > 1
                   ? delayBus->getWritePointer (1) + startSample : dL;
    auto* vL = verbBus != nullptr ? verbBus->getWritePointer (0) + startSample : nullptr;
    auto* vR = verbBus != nullptr && verbBus->getNumChannels() > 1
                   ? verbBus->getWritePointer (1) + startSample : vL;

    if (tailRemaining > 0)
    {
        const auto n = juce::jmin (numSamples, tailRemaining);
        for (int i = 0; i < n; ++i)
        {
            const auto tl = tailL[static_cast<size_t> (tailPos + i)];
            const auto tr = tailR[static_cast<size_t> (tailPos + i)];
            outL[i] += tl;
            if (outR != outL)
                outR[i] += tr;
            if (dL != nullptr && tailDelaySend > 0.0f)
            {
                dL[i] += tl * tailDelaySend;
                dR[i] += tr * tailDelaySend;
            }
            if (vL != nullptr && tailVerbSend > 0.0f)
            {
                vL[i] += tl * tailVerbSend;
                vR[i] += tr * tailVerbSend;
            }
        }
        tailPos += n;
        tailRemaining -= n;
    }

    if (pb.active)
    {
        const auto sendD = dL != nullptr ? pb.chop.delaySend : 0.0f;
        const auto sendV = vL != nullptr ? pb.chop.verbSend : 0.0f;
        if (sendD <= 0.0f && sendV <= 0.0f)
        {
            // Fast path (no sends): bit-identical to the Phase 3 render.
            renderPlayback (pb, outL, outR, numSamples);
        }
        else
        {
            // Render into the per-voice scratch, then distribute: dry at unity
            // plus the chop's send levels into the master delay/verb buses.
            auto remaining = numSamples;
            auto offset = 0;
            while (remaining > 0 && pb.active)
            {
                const auto n = juce::jmin (remaining, maxBlock);
                std::fill (mixL.begin(), mixL.begin() + n, 0.0f);
                std::fill (mixR.begin(), mixR.begin() + n, 0.0f);
                renderPlayback (pb, mixL.data(), mixR.data(), n);
                for (int i = 0; i < n; ++i)
                {
                    const auto l = mixL[static_cast<size_t> (i)];
                    const auto r = mixR[static_cast<size_t> (i)];
                    if (outR != outL)
                    {
                        outL[offset + i] += l;
                        outR[offset + i] += r;
                    }
                    else
                    {
                        outL[offset + i] += l + r; // mono fold (matches Phase 3)
                    }
                    if (sendD > 0.0f)
                    {
                        dL[offset + i] += l * sendD;
                        dR[offset + i] += r * sendD;
                    }
                    if (sendV > 0.0f)
                    {
                        vL[offset + i] += l * sendV;
                        vR[offset + i] += r * sendV;
                    }
                }
                offset += n;
                remaining -= n;
            }
        }
    }

    updateUiFeedback();
}

void ChopVoice::updateUiFeedback() noexcept
{
    if (pb.active)
    {
        const auto& s0 = pb.streams[0];
        auto x = s0.useStretch
            ? pb.x0 + s0.inRate * static_cast<double> (s0.outDone)
            : s0.pos;
        if (pb.looping && pb.loopB > pb.loopA && x >= static_cast<double> (pb.loopB))
            x = static_cast<double> (pb.loopA)
              + std::fmod (x - static_cast<double> (pb.loopA),
                           static_cast<double> (pb.loopB - pb.loopA));
        auto norm = juce::jlimit (0.0, 1.0, x / juce::jmax (1.0, pb.chopLen));
        if (pb.chop.reverse)
            norm = 1.0 - norm;
        uiPos.store (static_cast<float> (norm), std::memory_order_relaxed);
    }
    else if (tailRemaining <= 0)
    {
        uiChop.store (-1, std::memory_order_relaxed);
        uiPos.store (0.0f, std::memory_order_relaxed);
    }
}

} // namespace chopline::engine
