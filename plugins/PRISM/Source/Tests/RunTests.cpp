/*
    PRISM - console test runner. Grows one block of checks per Stage-2 phase.

    Build:  cmake --build build --target PRISM_Tests
    Run:    ./build/plugins/PRISM/PRISM_Tests            (exit code 0 == all PASS)
    TSAN:   configure with -DPRISM_TEST_SANITIZE=thread, run test_stress_* (they
            honour PRISM_STRESS_SECONDS, default 2).

    HARNESS RULE: processor.setPlayConfigDetails(...) / setRateAndBufferSizeDetails
    BEFORE prepareToPlay, or getSampleRate()==0 -> NaN -> silent renders.

    The global operator new below counts allocations ONLY on threads that opened a
    ScopedAllocCount (test-binary only; the shipped plugin never links this file).
*/
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "Analysis/AnalysisEngine.h"
#include "Analysis/AnalysisWorker.h"
#include "Analysis/SnapshotMailbox.h"
#include "Analysis/SourceRegistry.h"
#include "Analysis/StreamHistory.h"
#include "Analysis/WaveformExtractor.h"
#include "Capture/BeatEventQueue.h"
#include "Capture/SpscFrameRing.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <new>
#include <random>
#include <string>
#include <thread>
#include <vector>

//==============================================================================
// Allocation hook (counts only while the CURRENT thread has a scope open)
namespace
{
    thread_local int tlsCountAllocs = 0;
    std::atomic<long> gAllocCount { 0 };
    std::atomic<long long> gAllocBytes { 0 };

    inline void noteAlloc(std::size_t n) noexcept
    {
        if (tlsCountAllocs != 0)
        {
            gAllocCount.fetch_add(1, std::memory_order_relaxed);
            gAllocBytes.fetch_add((long long) n, std::memory_order_relaxed);
        }
    }

    void* rawAlloc(std::size_t n)
    {
        noteAlloc(n);
        if (void* p = std::malloc(n != 0 ? n : 1))
            return p;
        throw std::bad_alloc();
    }

    void* rawAllocAligned(std::size_t n, std::size_t align)
    {
        noteAlloc(n);
        void* p = nullptr;
        if (align < sizeof(void*)) align = sizeof(void*);
        if (posix_memalign(&p, align, n != 0 ? n : 1) != 0)
            throw std::bad_alloc();
        return p;
    }

    struct ScopedAllocCount
    {
        ScopedAllocCount()  { start = gAllocCount.load(); startBytes = gAllocBytes.load(); ++tlsCountAllocs; }
        ~ScopedAllocCount() { if (open) --tlsCountAllocs; }
        long finish()       { --tlsCountAllocs; open = false; return gAllocCount.load() - start; }
        long long bytes() const { return gAllocBytes.load() - startBytes; }   // bytes requested through operator new since construction
        long start = 0;
        long long startBytes = 0;
        bool open = true;
    };
}

void* operator new(std::size_t n)                                     { return rawAlloc(n); }
void* operator new[](std::size_t n)                                   { return rawAlloc(n); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept     { try { return rawAlloc(n); } catch (...) { return nullptr; } }
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept   { try { return rawAlloc(n); } catch (...) { return nullptr; } }
void* operator new(std::size_t n, std::align_val_t a)                 { return rawAllocAligned(n, (std::size_t) a); }
void* operator new[](std::size_t n, std::align_val_t a)               { return rawAllocAligned(n, (std::size_t) a); }
void operator delete(void* p) noexcept                                { std::free(p); }
void operator delete[](void* p) noexcept                              { std::free(p); }
void operator delete(void* p, std::size_t) noexcept                   { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept                 { std::free(p); }
void operator delete(void* p, std::align_val_t) noexcept              { std::free(p); }
void operator delete[](void* p, std::align_val_t) noexcept            { std::free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept   { std::free(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }

//==============================================================================
namespace
{
    int failures = 0;
    int checks = 0;
    std::vector<std::string> testNames;

    void check(bool cond, const char* name)
    {
        ++checks;
        std::printf("  %s  %s\n", cond ? "PASS" : "FAIL", name);
        if (! cond) ++failures;
    }

    void checkf(bool cond, const char* what, double a, double b = 0.0)
    {
        ++checks;
        std::printf("  %s  %s  (%g, %g)\n", cond ? "PASS" : "FAIL", what, a, b);
        if (! cond) ++failures;
    }

    void section(const char* name)
    {
        std::printf("\n[%s]\n", name);
        testNames.emplace_back(name);
    }

    using Clock = std::chrono::steady_clock;
    double msSince(Clock::time_point t0)
    {
        return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    }

    void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

    //--------------------------------------------------------------------------
    // Processor configuration: main bus mono/stereo, sidechain 0 (off) / 1 / 2 channels.
    bool configure(PRISMAudioProcessor& p, int mainCh, int scCh, double sr, int block)
    {
        // NOTE: AudioProcessor::setPlayConfigDetails() ends with disableNonMainBuses(), i.e. it
        // DISABLES the sidechain. So: main layout (sidechain off) -> setPlayConfigDetails
        // (harness rule: before prepareToPlay; also sets rate/block) -> THEN enable the sidechain.
        auto layout = p.getBusesLayout();
        const auto mainSet = mainCh == 1 ? juce::AudioChannelSet::mono() : juce::AudioChannelSet::stereo();
        layout.inputBuses.getReference(0)  = mainSet;
        layout.outputBuses.getReference(0) = mainSet;
        layout.inputBuses.getReference(1)  = juce::AudioChannelSet::disabled();
        bool ok = p.setBusesLayout(layout);

        p.setPlayConfigDetails(mainCh, mainCh, sr, block);

        if (scCh != 0)
        {
            layout = p.getBusesLayout();
            layout.inputBuses.getReference(1) = scCh == 1 ? juce::AudioChannelSet::mono()
                                                          : juce::AudioChannelSet::stereo();
            ok &= p.setBusesLayout(layout);
        }
        p.setRateAndBufferSizeDetails(sr, block);   // belt and braces: rate is never 0 at prepareToPlay
        return ok && p.getTotalNumInputChannels() == mainCh + scCh && p.getTotalNumOutputChannels() == mainCh;
    }

    int bufferChannels(int mainCh, int scCh) { return mainCh + scCh; }

    //--------------------------------------------------------------------------
    struct FakePlayHead : juce::AudioPlayHead
    {
        double ppq = 0.0, bpm = 120.0;
        bool playing = true, hasPpq = true, hasBpm = true;

        juce::Optional<PositionInfo> getPosition() const override
        {
            PositionInfo i;
            if (hasBpm) i.setBpm(bpm);
            if (hasPpq) i.setPpqPosition(ppq);
            i.setIsPlaying(playing);
            i.setTimeSignature(TimeSignature { 4, 4 });
            return i;
        }
    };

    //--------------------------------------------------------------------------
    std::vector<float> concat(const prism::StreamHistory::Segments& s)
    {
        std::vector<float> v;
        v.reserve((size_t) s.total());
        v.insert(v.end(), s.a, s.a + s.na);
        if (s.nb > 0) v.insert(v.end(), s.b, s.b + s.nb);
        return v;
    }

    // Deterministic per-channel test signal as a function of the absolute frame index.
    float sig(int ch, std::uint64_t i)
    {
        std::uint32_t h = (std::uint32_t) i * 2654435761u + (std::uint32_t) ch * 40503u + 12345u;
        h ^= h >> 15; h *= 2246822519u; h ^= h >> 13;
        const float noise = ((float) (h & 0xffff) / 65535.0f - 0.5f) * 0.2f;
        return 0.5f * std::sin((float) (i % 100000) * (0.01f + 0.003f * (float) ch)) + noise;
    }

    prism::EngineConfig makeCfg(int ch0, int ch1, double sr = 48000.0)
    {
        prism::EngineConfig c;
        c.sampleRate = sr;
        c.busChannels[0] = ch0;
        c.busChannels[1] = ch1;
        return c;
    }
}

//==============================================================================
// RING
static void test_ring_wraparound()
{
    section("ring_wraparound");
    prism::SpscFrameRing ring;
    ring.prepare(2, 6);   // 64 frames
    check(ring.getCapacity() == 64, "capacity is 2^6");

    std::mt19937 rng(1);
    std::uint64_t nextWrite = 0, nextRead = 0;
    bool ok = true;
    std::vector<float> l(64), r(64), dl(64), dr(64);

    for (int iter = 0; iter < 5000; ++iter)
    {
        const int wn = 1 + (int) (rng() % 40);
        if ((int) ring.getCapacity() - ring.readAvailable() >= wn)
        {
            for (int i = 0; i < wn; ++i) { l[(size_t) i] = (float) (nextWrite + (std::uint64_t) i); r[(size_t) i] = -l[(size_t) i]; }
            const float* src[2] = { l.data(), r.data() };
            ok &= ring.write(src, wn);
            nextWrite += (std::uint64_t) wn;
        }
        const int rn = 1 + (int) (rng() % 50);
        float* dst[2] = { dl.data(), dr.data() };
        const int got = ring.read(dst, rn);
        for (int i = 0; i < got; ++i)
        {
            ok &= (dl[(size_t) i] == (float) nextRead) && (dr[(size_t) i] == -(float) nextRead);
            ++nextRead;
        }
    }
    check(ok, "all frames read back in order across many wraps (no corruption)");
    check(nextWrite > 64 * 20, "wrapped the 64-frame ring many times");
    check(ring.getOverrunCount() == 0, "no overruns when the reader keeps up");
    check(ring.getTotalWritten() == nextWrite, "total written counter");
}

static void test_ring_overrun_drops_whole_blocks()
{
    section("ring_overrun_drops_whole_blocks");
    prism::SpscFrameRing ring;
    ring.prepare(1, 6);   // 64 frames, mono

    auto block = [](float v, int n) { return std::vector<float>((size_t) n, v); };
    auto put = [&](float v, int n) { auto b = block(v, n); const float* p[1] = { b.data() }; return ring.write(p, n); };

    check(put(1.0f, 40), "40 frames fit");
    check(! put(2.0f, 30), "30 more do NOT fit (24 free) -> whole block dropped");
    check(ring.getOverrunCount() == 1 && ring.getDroppedFrames() == 30, "overrun counted (1 block, 30 frames)");
    check(put(3.0f, 24), "exact fit (24) accepted");
    check(! put(4.0f, 1), "full ring drops even a single frame");
    check(ring.getOverrunCount() == 2 && ring.getDroppedFrames() == 31, "counters accumulate");

    std::vector<float> out(64);
    float* dst[1] = { out.data() };
    check(ring.read(dst, 100) == 64, "reader sees exactly the accepted 64 frames");
    bool ok = true;
    for (int i = 0; i < 40; ++i) ok &= out[(size_t) i] == 1.0f;
    for (int i = 40; i < 64; ++i) ok &= out[(size_t) i] == 3.0f;
    check(ok, "no partial block and nothing overwritten (dropped data never appears)");

    check(put(5.0f, 64), "after draining, a full-capacity block fits");
    ring.read(dst, 100);
    check(! put(6.0f, 65), "block larger than capacity is dropped even when empty");
    check(ring.readAvailable() == 0, "oversized block leaves the ring untouched");
    check(put(7.0f, 10) && ring.readAvailable() == 10, "ring keeps working after drops");
}

static void test_ring_spsc_stress_no_torn_frames()
{
    section("ring_spsc_stress_no_torn_frames");
    prism::SpscFrameRing ring;
    ring.prepare(2, 10);   // 1024 frames: forces frequent overruns

    constexpr std::uint64_t kTotal = 3'000'000;   // < 2^24 so float(idx) is exact
    struct Range { std::uint64_t from, to; };
    std::vector<Range> producerDrops;
    std::uint64_t producedAccepted = 0, droppedBlocks = 0, droppedFrames = 0;
    std::atomic<bool> producerDone { false };

    std::thread producer([&] {
        std::mt19937 rng(7);
        std::vector<float> l(512), r(512);
        std::uint64_t idx = 0;
        while (idx < kTotal)
        {
            const int n = (int) std::min<std::uint64_t>(1 + rng() % 300, kTotal - idx);
            for (int i = 0; i < n; ++i) { l[(size_t) i] = (float) (idx + (std::uint64_t) i); r[(size_t) i] = -l[(size_t) i]; }
            const float* src[2] = { l.data(), r.data() };
            if (ring.write(src, n))
                producedAccepted += (std::uint64_t) n;
            else
            {
                ++droppedBlocks; droppedFrames += (std::uint64_t) n;
                if (! producerDrops.empty() && producerDrops.back().to == idx) producerDrops.back().to = idx + (std::uint64_t) n;
                else producerDrops.push_back({ idx, idx + (std::uint64_t) n });
            }
            idx += (std::uint64_t) n;
            if ((rng() & 63) == 0) std::this_thread::yield();
        }
        producerDone.store(true, std::memory_order_release);
    });

    std::vector<Range> consumerGaps;
    std::uint64_t received = 0, expected = 0;
    bool torn = false;
    {
        std::mt19937 rng(11);
        std::vector<float> l(256), r(256);
        float* dst[2] = { l.data(), r.data() };
        for (;;)
        {
            const bool doneBefore = producerDone.load(std::memory_order_acquire);
            const int got = ring.read(dst, 1 + (int) (rng() % 200));
            for (int i = 0; i < got; ++i)
            {
                const std::uint64_t idx = (std::uint64_t) l[(size_t) i];
                if (r[(size_t) i] != -l[(size_t) i]) torn = true;              // the two channels of a frame disagree
                if (idx < expected) torn = true;                                // went backwards
                if (idx > expected) consumerGaps.push_back({ expected, idx });
                expected = idx + 1;
                ++received;
            }
            if (got == 0 && doneBefore) break;
            if ((rng() & 31) == 0) std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
    }
    producer.join();
    if (expected < kTotal) consumerGaps.push_back({ expected, kTotal });   // trailing drops

    check(! torn, "no torn frames (channel pairing + monotonic index)");
    check(received == producedAccepted, "every accepted frame delivered exactly once");
    check(received + droppedFrames == kTotal, "accepted + dropped == produced");
    check(droppedBlocks > 0 && received > 0, "stress actually produced both drops and deliveries");
    check(ring.getOverrunCount() == droppedBlocks, "overrunCount == dropped blocks");
    check(ring.getDroppedFrames() == droppedFrames, "droppedFrames counter exact");

    bool gapsMatch = consumerGaps.size() == producerDrops.size();
    for (size_t i = 0; gapsMatch && i < producerDrops.size(); ++i)
        gapsMatch = consumerGaps[i].from == producerDrops[i].from && consumerGaps[i].to == producerDrops[i].to;
    check(gapsMatch, "gaps seen by the consumer == whole dropped blocks (embedded sequence pattern)");
    std::printf("      produced %llu frames: %llu delivered, %llu blocks dropped\n",
                (unsigned long long) kTotal, (unsigned long long) received, (unsigned long long) droppedBlocks);
}

static void test_beat_event_queue()
{
    section("beat_event_queue");
    prism::BeatEventQueue q;
    bool ok = true;
    for (int i = 0; i < prism::BeatEventQueue::kCapacity; ++i)
        ok &= q.push({ (std::uint64_t) i, (i % 4) == 0 });
    check(ok, "fills to capacity (256)");
    check(! q.push({ 999, false }) && q.getDroppedCount() == 1, "drop on full, counted");

    prism::BeatEvent e;
    ok = true;
    for (int i = 0; i < prism::BeatEventQueue::kCapacity; ++i)
        ok &= q.pop(e) && e.frameIndex == (std::uint64_t) i && e.isBar == ((i % 4) == 0);
    check(ok, "FIFO order and payload preserved");
    check(! q.pop(e), "empty after draining");
    check(q.push({ 5, true }) && q.pop(e) && e.frameIndex == 5 && e.isBar, "usable again after wrap");
}

//==============================================================================
// HISTORY / REGISTRY
static void test_stream_history()
{
    section("stream_history");
    prism::StreamHistory h;
    h.prepare(2);

    const std::uint64_t total = 65536 * 2 + 1234;
    std::mt19937 rng(3);
    std::vector<float> l, r;
    for (std::uint64_t i = 0; i < total;)
    {
        const int n = (int) std::min<std::uint64_t>(1 + rng() % 5000, total - i);
        l.resize((size_t) n); r.resize((size_t) n);
        for (int k = 0; k < n; ++k) { l[(size_t) k] = (float) ((i + (std::uint64_t) k) % 100000); r[(size_t) k] = -l[(size_t) k]; }
        const float* src[2] = { l.data(), r.data() };
        h.push(src, n);
        i += (std::uint64_t) n;
    }
    check(h.framesSeen() == total, "framesSeen counts every pushed frame");

    bool ok = true;
    for (int n : { 1, 100, 4096, 16384, 65536 })
    {
        const auto v = concat(h.latest(0, n));
        const auto w = concat(h.latest(1, n));
        ok &= (int) v.size() == n && (int) w.size() == n;
        for (int k = 0; k < n && ok; ++k)
        {
            const float expect = (float) ((total - (std::uint64_t) n + (std::uint64_t) k) % 100000);
            ok &= v[(size_t) k] == expect && w[(size_t) k] == -expect;
        }
    }
    check(ok, "latest(n) is the contiguous tail of the stream (both segments, both channels)");
    check(h.latest(0, 100000).total() == 65536, "latest(n) clamps to the 65536-frame capacity");
    check(h.latest(0, 0).total() == 0, "latest(0) is empty");

    // Sanitisation
    prism::StreamHistory h2;
    h2.prepare(1);
    float bad[4] = { 1.0f, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity() };
    const float* p[1] = { bad };
    h2.push(p, 4);
    const auto v = concat(h2.latest(0, 4));
    check(v.size() == 4 && v[0] == 1.0f && v[1] == 0.0f && v[2] == 0.0f && v[3] == 0.0f, "NaN/+-inf replaced by 0");
    check(h2.nonFiniteSanitized() == 3, "non-finite values counted");
    const auto vr = concat(h2.latest(1, 4));
    check(vr == v, "mono history duplicates L into R");
}

static void test_source_registry()
{
    section("source_registry");
    prism::SourceRegistry reg;

    reg.build(makeCfg(2, 0));
    check(reg.getActiveCount() == 1 && reg[0].active && ! reg[1].active, "stereo main, sidechain off -> 1 active source");

    reg.build(makeCfg(2, 2));
    check(reg.getActiveCount() == 2 && reg[0].channelOffset == 0 && reg[1].channelOffset == 2 && reg[1].numChannels == 2,
          "stereo + stereo sidechain -> offsets 0 and 2");

    reg.build(makeCfg(1, 2));
    check(reg[0].numChannels == 1 && reg[1].channelOffset == 1, "mono main shifts the sidechain offset to 1");

    reg.build(makeCfg(2, 6));
    check(reg.getActiveCount() == 1 && ! reg[1].active, "unsupported (6ch) sidechain stays inactive");

    prism::EngineConfig many = makeCfg(2, 2);
    for (int i = 2; i < prism::kMaxSources; ++i) many.busChannels[(size_t) i] = 2;
    reg.build(many);
    check(reg.getActiveCount() == prism::kSourceCount, "v1 activates at most kSourceCount (2) of kMaxSources (16) entries");
    check(prism::kMaxSources == 16 && prism::kSourceCount == 2, "kMaxSources=16, kSourceCount=2");

    prism::EngineConfig a = makeCfg(2, 2), b = makeCfg(2, 2);
    check(a == b, "identical configs compare equal");
    b.sampleRate = 44100.0;
    check(a != b, "sample rate change -> config differs");
    b = makeCfg(2, 0);
    check(a != b, "layout change -> config differs");
}

//==============================================================================
// ENGINE (synchronous, no thread)
static void test_engine_capture_and_block_size_independence()
{
    section("engine_capture_block_size_independence");

    const std::uint64_t totalFrames = 100000;   // > 65536 so StreamHistory wraps
    const int kTail = 20000;
    std::vector<std::vector<float>> reference;   // 4 channels: tail of each

    bool allSame = true, expectedOk = true, peaksOk = true;
    for (int bs : { 1, 7, 64, 480, 4096 })
    {
        prism::AnalysisEngine eng(makeCfg(2, 2));
        std::vector<std::vector<float>> bufs(4, std::vector<float>(4096));
        std::uint64_t pos = 0;
        while (pos < totalFrames)
        {
            const int n = (int) std::min<std::uint64_t>((std::uint64_t) bs, totalFrames - pos);
            for (int c = 0; c < 4; ++c)
                for (int i = 0; i < n; ++i)
                    bufs[(size_t) c][(size_t) i] = sig(c, pos + (std::uint64_t) i);
            const float* ptrs[4] = { bufs[0].data(), bufs[1].data(), bufs[2].data(), bufs[3].data() };
            eng.captureBlock(ptrs, 4, n);
            eng.pump();
            pos += (std::uint64_t) n;
        }

        std::vector<std::vector<float>> tails;
        for (int s = 0; s < 2; ++s)
            for (int c = 0; c < 2; ++c)
                tails.push_back(concat(eng.getHistory(s).latest(c, kTail)));

        if (reference.empty()) reference = tails;
        else for (size_t k = 0; k < tails.size(); ++k)
            allSame &= tails[k].size() == reference[k].size()
                    && std::memcmp(tails[k].data(), reference[k].data(), tails[k].size() * sizeof(float)) == 0;

        for (int s = 0; s < 2; ++s)
            for (int c = 0; c < 2; ++c)
                for (int k = 0; k < kTail; ++k)
                    expectedOk &= tails[(size_t) (s * 2 + c)][(size_t) k] == sig(s * 2 + c, totalFrames - (std::uint64_t) kTail + (std::uint64_t) k);

        for (int s = 0; s < 2; ++s)
        {
            float expectPeak = 0.0f;
            for (std::uint64_t i = 0; i < totalFrames; ++i)
                expectPeak = std::max({ expectPeak, std::abs(sig(s * 2, i)), std::abs(sig(s * 2 + 1, i)) });
            peaksOk &= eng.getSamplePeakMaxLinear(s) == expectPeak;
        }
        allSame &= eng.getHistory(0).framesSeen() == totalFrames && eng.getHistory(1).framesSeen() == totalFrames;
    }
    check(expectedOk, "history holds exactly the captured samples (both sources, both channels)");
    check(allSame, "history identical for block sizes {1,7,64,480,4096} (bit-for-bit)");
    check(peaksOk, "folded sample peak exact for every block size");
}

static void test_engine_mono_and_inactive_sources()
{
    section("engine_mono_and_inactive_sources");
    prism::AnalysisEngine eng(makeCfg(1, 0));
    std::vector<float> a(256), sc(256, 0.9f);
    for (int i = 0; i < 256; ++i) a[(size_t) i] = 0.001f * (float) i;
    const float* ptrs[2] = { a.data(), sc.data() };   // buffer carries a sidechain channel the engine must ignore
    eng.captureBlock(ptrs, 2, 256);
    eng.pump();
    check(eng.getHistory(0).framesSeen() == 256, "mono main captured");
    check(eng.getHistory(1).framesSeen() == 0, "disabled sidechain: no frames (no fabricated zeros)");
    check(eng.getHistory(0).numChannels() == 1, "mono source reported as 1 channel");
    check(eng.getSamplePeakMaxLinear(1) == 0.0f, "inactive source has no peak");

    // Buffer smaller than the negotiated layout: skipped safely, no out-of-bounds read.
    prism::AnalysisEngine eng2(makeCfg(2, 2));
    const float* two[2] = { a.data(), a.data() };
    eng2.captureBlock(two, 2, 256);
    eng2.pump();
    check(eng2.getHistory(0).framesSeen() == 256 && eng2.getHistory(1).framesSeen() == 0,
          "short host buffer: sidechain skipped instead of overrunning the channel array");

    auto snap = eng.buildSnapshot();
    check(snap->sources[1].hasSpectrum == false && snap->sources[1].scopeCount == 0 && std::isinf(snap->sources[1].samplePeakMaxDb),
          "inactive source snapshot stays at 'unavailable' defaults");
}

static void test_engine_peak_lossless_across_overrun()
{
    section("engine_peak_lossless_across_overrun");
    prism::AnalysisEngine eng(makeCfg(2, 0), 8);   // 256-frame ring
    std::vector<float> l(100, 0.1f), r(100, 0.1f);
    const float* ptrs[2] = { l.data(), r.data() };
    eng.captureBlock(ptrs, 2, 100);
    eng.captureBlock(ptrs, 2, 100);
    l[37] = -0.9f;   // peak lives in a block that will be dropped
    eng.captureBlock(ptrs, 2, 100);
    eng.pump();
    check(eng.getOverrunCount(0) == 1, "third block overran the ring");
    check(eng.getHistory(0).framesSeen() == 200, "dropped block never reaches the history");
    check(eng.getSamplePeakMaxLinear(0) == 0.9f, "sample peak still captured losslessly from the dropped block");
    check(eng.getGapEvents(0) == 1, "worker registered the discontinuity");

    // After draining, capture continues normally.
    l[37] = 0.1f;
    eng.captureBlock(ptrs, 2, 100);
    eng.pump();
    check(eng.getHistory(0).framesSeen() == 300 && eng.getGapEvents(0) == 1, "capture resumes after the gap");
}

static void test_engine_non_finite_input()
{
    section("engine_non_finite_input");
    prism::AnalysisEngine eng(makeCfg(2, 0));
    const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
    std::vector<float> l = { 0.25f, nan, inf, -0.5f, 1.0e-42f }, r = { 0.0f, -inf, 0.125f, nan, 0.0f };
    const float* ptrs[2] = { l.data(), r.data() };
    eng.captureBlock(ptrs, 2, 5);
    eng.pump();
    check(eng.getNonFiniteCount(0) == 4, "4 non-finite samples counted by the audio thread");
    check(eng.getSamplePeakMaxLinear(0) == 0.5f, "non-finite samples excluded from the peak");
    const auto hl = concat(eng.getHistory(0).latest(0, 5));
    const auto hr = concat(eng.getHistory(0).latest(1, 5));
    check(hl[1] == 0.0f && hl[2] == 0.0f && hr[1] == 0.0f && hr[3] == 0.0f, "worker copy replaces non-finite by 0");
    check(eng.getHistory(0).nonFiniteSanitized() == 4, "history counts the sanitised samples");
}

static void test_engine_beat_events()
{
    section("engine_beat_events");

    auto run = [](int blockSize, bool playing, bool withBpm, std::vector<prism::BeatEvent>& out, float* bpmOut) {
        prism::AnalysisEngine eng(makeCfg(2, 0));
        FakePlayHead ph;
        ph.playing = playing; ph.hasBpm = withBpm; ph.bpm = 120.0;
        std::vector<float> buf((size_t) blockSize, 0.1f);
        const float* ptrs[2] = { buf.data(), buf.data() };
        const std::uint64_t total = 480000;   // 10 s at 120 bpm = 20 beats
        for (std::uint64_t pos = 0; pos < total;)
        {
            const int n = (int) std::min<std::uint64_t>((std::uint64_t) blockSize, total - pos);
            ph.ppq = (double) pos * 2.0 / 48000.0;
            eng.captureBlock(ptrs, 2, n);
            eng.capturePlayhead(&ph, n);
            eng.pump();
            pos += (std::uint64_t) n;
        }
        for (std::uint64_t i = 0; i < eng.getBeatEventsSeen(); ++i)
            out.push_back(eng.getBeatLogEntry(i));
        *bpmOut = eng.getBpm();
    };

    for (int bs : { 480, 333, 4096, 1 << 12, 64 })
    {
        std::vector<prism::BeatEvent> ev;
        float bpm = 0;
        run(bs, true, true, ev, &bpm);
        bool ok = ev.size() == 20;
        for (size_t k = 0; ok && k < ev.size(); ++k)
        {
            const auto expectFrame = (std::int64_t) (24000 * k);
            ok &= std::abs((std::int64_t) ev[k].frameIndex - expectFrame) <= 1;
            ok &= ev[k].isBar == (k % 4 == 0);
        }
        char name[96];
        std::snprintf(name, sizeof(name), "120 bpm 4/4: 20 beats, bar every 4th, +-1 frame (block %d)", bs);
        check(ok, name);
        checkf(bpm == 120.0f, "bpm atomic published", bpm);
    }

    {
        std::vector<prism::BeatEvent> ev;
        float bpm = 0;
        run(480, false, true, ev, &bpm);
        check(ev.empty(), "transport stopped -> no beat events");
    }
    {
        std::vector<prism::BeatEvent> ev;
        float bpm = 0;
        run(480, true, false, ev, &bpm);
        check(ev.empty() && std::isnan(bpm), "no tempo from host -> no events, bpm NaN");
    }
    {
        prism::AnalysisEngine eng(makeCfg(2, 0));
        std::vector<float> buf(64, 0.0f);
        const float* ptrs[2] = { buf.data(), buf.data() };
        eng.captureBlock(ptrs, 2, 64);
        eng.capturePlayhead(nullptr, 64);
        check(std::isnan(eng.getBpm()), "no playhead -> bpm NaN");
    }
}

static void test_snapshot_sequence_sync()
{
    section("snapshot_sequence_sync");
    prism::AnalysisEngine eng(makeCfg(2, 2, 96000.0));
    std::uint64_t prev = 0;
    bool strict = true;
    for (int i = 0; i < 1000; ++i)
    {
        auto s = eng.buildSnapshot();
        strict &= s->sequence > prev && s->sequence == prev + 1;
        prev = s->sequence;
    }
    check(strict, "1000 snapshots: strictly increasing, starting at 1");
    auto s = eng.buildSnapshot();
    check(s->sampleRate == 96000.0, "snapshot carries the engine sample rate");
    check(std::isnan(s->bpm) && ! s->signalPresent && std::isnan(s->momentaryLufs) && std::isnan(s->integratedLufs),
          "nothing measured yet: bpm/loudness NaN, signalPresent false");

    eng.setSequenceBase(5000);
    check(eng.buildSnapshot()->sequence == 5001, "sequence base carries over (rebuild keeps sequence monotonic)");
}

//==============================================================================
// MAILBOX
static void test_mailbox_semantics()
{
    section("mailbox_semantics");
    prism::SnapshotMailbox mb;
    check(mb.fetch() == nullptr, "empty before the first publish");

    auto a = std::make_shared<prism::AnalysisSnapshot>(); a->sequence = 1;
    mb.publish(a);
    auto got = mb.fetch();
    check(got != nullptr && got->sequence == 1, "fetch returns the published snapshot");

    auto b = std::make_shared<prism::AnalysisSnapshot>(); b->sequence = 2;
    mb.publish(b);
    check(mb.fetch()->sequence == 2, "newest wins");
    check(got->sequence == 1, "a held snapshot is immutable / unaffected by later publishes");
    a.reset();
    check(got.use_count() == 1, "mailbox released its reference to the replaced snapshot");
}

static void test_mailbox_concurrent_nonblocking()
{
    section("mailbox_concurrent_nonblocking");
    prism::SnapshotMailbox mb;
    std::atomic<bool> stop { false };
    std::atomic<double> worstFetchMs { 0.0 };
    std::atomic<int> orderViolations { 0 };

    auto reader = [&] {
        std::uint64_t last = 0;
        while (! stop.load(std::memory_order_acquire))
        {
            const auto t0 = Clock::now();
            auto s = mb.fetch();
            const double ms = msSince(t0);
            double w = worstFetchMs.load();
            while (ms > w && ! worstFetchMs.compare_exchange_weak(w, ms)) {}
            if (s != nullptr)
            {
                if (s->sequence < last) ++orderViolations;
                last = s->sequence;
            }
        }
    };
    std::vector<std::thread> readers;
    for (int i = 0; i < 3; ++i) readers.emplace_back(reader);

    const int kPublishes = 3000;
    for (int i = 1; i <= kPublishes; ++i)
    {
        auto s = std::make_shared<prism::AnalysisSnapshot>();
        s->sequence = (std::uint64_t) i;
        mb.publish(s);
        if ((i & 15) == 0) std::this_thread::yield();
    }
    stop.store(true, std::memory_order_release);
    for (auto& t : readers) t.join();

    check(orderViolations.load() == 0, "readers never observe a sequence going backwards");
    checkf(worstFetchMs.load() < 250.0, "worst fetch latency under publish storm (ms) < 250", worstFetchMs.load());
    check(mb.fetch()->sequence == (std::uint64_t) kPublishes, "final snapshot is the last published");
}

static void test_destruction_while_snapshot_held()
{
    section("destruction_while_snapshot_held");
    std::shared_ptr<const prism::AnalysisSnapshot> held;
    {
        PRISMAudioProcessor proc;
        check(configure(proc, 2, 0, 48000.0, 480), "configure stereo / no sidechain");
        proc.prepareToPlay(48000.0, 480);
        proc.setEditorActive(true);

        std::vector<float> data(480, 0.1f);
        juce::AudioBuffer<float> buf(2, 480);
        juce::MidiBuffer midi;
        for (int ch = 0; ch < 2; ++ch) std::memcpy(buf.getWritePointer(ch), data.data(), 480 * sizeof(float));

        const auto t0 = Clock::now();
        while (msSince(t0) < 1500.0 && held == nullptr)
        {
            proc.processBlock(buf, midi);
            sleepMs(5);
            held = proc.getLatestAnalysisSnapshot();
        }
        check(held != nullptr, "worker published a snapshot");
        proc.setEditorActive(false);
    }   // processor, engine, worker, mailbox destroyed while `held` is alive
    check(held != nullptr && held->sequence >= 1 && held->sampleRate == 48000.0,
          "snapshot remains valid and readable after the processor is destroyed");
    held.reset();
}

//==============================================================================
// PROCESSOR: passthrough + allocation
static void test_processblock_zero_allocations()
{
    section("processblock_zero_allocations");

    {   // prove the hook is live
        ScopedAllocCount sc;
        int* volatile sink = new int[16];
        delete[] sink;
        check(sc.finish() >= 1, "allocation hook detects operator new (sanity)");
    }

    PRISMAudioProcessor proc;
    FakePlayHead ph;
    juce::MidiBuffer midi;
    long totalBlocks = 0, totalAllocs = 0;
    std::mt19937 rng(5);
    const int sizes[] = { 1, 2, 3, 7, 32, 64, 100, 480, 512, 1024, 2048, 4096 };

    for (int mainCh : { 1, 2 })
        for (int scCh : { 0, 1, 2 })
        {
            const bool cfgOk = configure(proc, mainCh, scCh, 48000.0, 4096);
            if (! cfgOk) { check(false, "configure layout"); continue; }
            proc.prepareToPlay(48000.0, 4096);

            for (int size : sizes)
            {
                juce::AudioBuffer<float> buf(bufferChannels(mainCh, scCh), size);
                for (int ch = 0; ch < buf.getNumChannels(); ++ch)
                    for (int i = 0; i < size; ++i)
                        buf.setSample(ch, i, (float) (rng() & 0xffff) / 32768.0f - 1.0f);

                const int blocks = 1400;
                for (int pass = 0; pass < 2; ++pass)
                {
                    proc.setPlayHead(pass == 0 ? nullptr : &ph);
                    ph.bpm = 300.0;

                    ScopedAllocCount sc;
                    for (int b = 0; b < blocks / 2; ++b)
                    {
                        ph.ppq = (double) b * 0.37;
                        proc.processBlock(buf, midi);
                    }
                    totalAllocs += sc.finish();
                    totalBlocks += blocks / 2;
                }
            }
            proc.setPlayHead(nullptr);
            proc.releaseResources();
        }

    checkf(totalBlocks >= 100000, "processed >= 100k blocks (mono/stereo, sidechain off/mono/stereo, sizes 1..4096)", (double) totalBlocks);
    checkf(totalAllocs == 0, "ZERO operator new calls inside processBlock", (double) totalAllocs);
}

static void test_passthrough_bit_exact()
{
    section("passthrough_bit_exact");
    PRISMAudioProcessor proc;
    juce::MidiBuffer midi;
    std::mt19937 rng(9);

    const float nan1 = std::numeric_limits<float>::quiet_NaN();
    float nan2; { std::uint32_t b = 0x7fc12345u; std::memcpy(&nan2, &b, 4); }
    float snan; { std::uint32_t b = 0x7f800001u; std::memcpy(&snan, &b, 4); }
    const float inf = std::numeric_limits<float>::infinity();
    const float denorm = std::numeric_limits<float>::denorm_min();
    const std::vector<float> specials = { 0.0f, -0.0f, 1.0f, -1.0f, 1.0001f, nan1, nan2, snan, inf, -inf, denorm, -denorm,
                                          1.0e-40f, -1.0e-42f, std::numeric_limits<float>::min() * 0.5f,
                                          std::numeric_limits<float>::max(), -std::numeric_limits<float>::max() };

    bool allExact = true, cfgOk = true;
    for (int mainCh : { 1, 2 })
        for (int scCh : { 0, 1, 2 })
            for (bool workerRunning : { true, false })
            {
                cfgOk &= configure(proc, mainCh, scCh, 44100.0, 512);
                proc.prepareToPlay(44100.0, 512);
                if (! workerRunning)
                    proc.releaseResources();

                for (int size : { 1, 5, 64, 513, 4096 })
                {
                    const int chs = bufferChannels(mainCh, scCh);
                    juce::AudioBuffer<float> buf(chs, size);
                    for (int kind = 0; kind < 6; ++kind)
                    {
                        for (int ch = 0; ch < chs; ++ch)
                            for (int i = 0; i < size; ++i)
                            {
                                float v;
                                switch (kind)
                                {
                                    case 0: v = (float) (rng() & 0xffffff) / 8388608.0f - 1.0f; break;                     // random
                                    case 1: v = specials[rng() % specials.size()]; break;                                  // NaN/inf/denormal/FS mix
                                    case 2: v = (i & 1) ? 1.0f : -1.0f; break;                                              // full-scale square
                                    case 3: v = denorm * (float) (1 + (rng() % 1000)); break;                               // all denormal
                                    case 4: { std::uint32_t b = (std::uint32_t) rng(); std::memcpy(&v, &b, 4); break; }     // arbitrary bit patterns
                                    default: v = nan1; break;
                                }
                                buf.setSample(ch, i, v);
                            }

                        std::vector<float> ref((size_t) chs * (size_t) size);
                        for (int ch = 0; ch < chs; ++ch)
                            std::memcpy(ref.data() + (size_t) ch * (size_t) size, buf.getReadPointer(ch), (size_t) size * sizeof(float));

                        const float* const* before = buf.getArrayOfReadPointers();
                        const float* p0 = before[0];
                        for (int rep = 0; rep < 3; ++rep)
                            proc.processBlock(buf, midi);

                        bool same = buf.getNumChannels() == chs && buf.getNumSamples() == size && buf.getReadPointer(0) == p0;
                        for (int ch = 0; ch < chs && same; ++ch)
                            same &= std::memcmp(ref.data() + (size_t) ch * (size_t) size, buf.getReadPointer(ch), (size_t) size * sizeof(float)) == 0;
                        allExact &= same;
                    }
                }
                proc.releaseResources();
            }

    check(cfgOk, "all bus layouts accepted by the processor");
    check(allExact, "output bit-identical to input: random, NaN (payloads), denormal, +-inf, full-scale, arbitrary bit patterns; sidechain untouched; worker on/off");
}

//==============================================================================
// PROCESSOR: end-to-end, lifecycle
static void test_end_to_end_publish_and_gating()
{
    section("end_to_end_publish_and_gating");
    PRISMAudioProcessor proc;
    check(configure(proc, 2, 2, 48000.0, 480), "configure stereo + stereo sidechain");
    proc.prepareToPlay(48000.0, 480);

    juce::AudioBuffer<float> buf(4, 480);
    juce::MidiBuffer midi;
    for (int ch = 0; ch < 4; ++ch)
        for (int i = 0; i < 480; ++i)
            buf.setSample(ch, i, 0.2f * std::sin(0.05f * (float) i));

    check(proc.getLatestAnalysisSnapshot() == nullptr, "no snapshot before any audio");

    // 1) Editor closed: audio flows, worker drains, but nothing is built or published.
    for (int i = 0; i < 40; ++i) { proc.processBlock(buf, midi); sleepMs(5); }
    check(proc.getAnalysisWorker()->getPublishCount() == 0 && proc.getLatestAnalysisSnapshot() == nullptr,
          "editor inactive: no snapshot is built or published");
    check(proc.getAnalysisWorker()->getCycleCount() > 0, "worker is polling while inactive");

    // 2) Editor opens: ~60 Hz publishing, strictly increasing sequence.
    proc.setEditorActive(true);
    std::vector<std::uint64_t> seen;
    std::uint64_t prevSeq = 0;
    bool nonDecreasing = true;
    const auto t0 = Clock::now();
    int blocks = 0;
    while (msSince(t0) < 700.0)
    {
        proc.processBlock(buf, midi); ++blocks;
        auto s = proc.getLatestAnalysisSnapshot();
        if (s != nullptr)
        {
            nonDecreasing &= s->sequence >= prevSeq;
            if (s->sequence != prevSeq) seen.push_back(s->sequence);
            prevSeq = s->sequence;
        }
        sleepMs(2);
    }
    bool strict = true;
    for (size_t i = 1; i < seen.size(); ++i) strict &= seen[i] > seen[i - 1];
    const double secs = msSince(t0) / 1000.0;
    check(nonDecreasing && strict, "fetched sequence numbers strictly increasing");
    checkf(seen.size() >= 10, "snapshots flow while the editor is active (distinct fetched)", (double) seen.size());
    checkf((double) seen.size() <= secs * 60.0 + 6.0, "publish rate capped at ~60 Hz", (double) seen.size(), secs * 60.0);
    check(proc.getLatestAnalysisSnapshot()->sampleRate == 48000.0, "snapshot sample rate matches the host");
    check(proc.getLastFetchedSequence() == prevSeq, "fetch records the last fetched sequence (ack seam)");

    // 3) Audio stops: no new frames -> no new sequence.
    sleepMs(100);
    const auto seqAfter = proc.getLatestAnalysisSnapshot()->sequence;
    sleepMs(250);
    check(proc.getLatestAnalysisSnapshot()->sequence == seqAfter, "no audio -> no new sequence (editor idles)");

    // 4) Heartbeat expiry: editor 'open' but nobody fetching for > 500 ms -> worker drops to inactive.
    const auto t1 = Clock::now();
    while (msSince(t1) < 700.0) { proc.processBlock(buf, midi); sleepMs(5); }
    const auto c1 = proc.getAnalysisWorker()->getPublishCount();
    const auto t2 = Clock::now();
    while (msSince(t2) < 300.0) { proc.processBlock(buf, midi); sleepMs(5); }
    check(proc.getAnalysisWorker()->getPublishCount() == c1, "stale fetch heartbeat (>500 ms) stops publishing");

    // 5) Fetching again re-arms it.
    const auto t3 = Clock::now();
    while (msSince(t3) < 300.0) { proc.processBlock(buf, midi); proc.getLatestAnalysisSnapshot(); sleepMs(5); }
    check(proc.getAnalysisWorker()->getPublishCount() > c1, "publishing resumes once the editor fetches again");

    proc.setEditorActive(false);
    proc.releaseResources();
}

static void test_lifecycle_rebuild_rules()
{
    section("lifecycle_rebuild_rules");
    PRISMAudioProcessor proc;
    check(configure(proc, 2, 0, 48000.0, 512), "configure");
    check(proc.getAnalysisEngine() == nullptr, "no engine before prepareToPlay");

    proc.prepareToPlay(48000.0, 512);
    auto* e1 = proc.getAnalysisEngine();
    check(e1 != nullptr && proc.getEngineRebuildCount() == 1, "first prepare builds the engine");
    check(proc.getAnalysisWorker()->isThreadRunning(), "worker running after prepare");

    proc.prepareToPlay(48000.0, 256);   // hosts re-call prepare with a new block size
    check(proc.getAnalysisEngine() == e1 && proc.getEngineRebuildCount() == 1, "same {rate, layout}: engine NOT rebuilt (block size irrelevant)");

    // Feed frames, release, verify history survives a same-config prepare.
    juce::AudioBuffer<float> buf(2, 480);
    juce::MidiBuffer midi;
    for (int i = 0; i < 5; ++i) proc.processBlock(buf, midi);
    proc.releaseResources();
    check(! proc.getAnalysisWorker()->isThreadRunning(), "releaseResources joins the worker");
    e1->pump();
    check(e1->getHistory(0).framesSeen() == 2400, "frames captured before release are preserved");
    proc.prepareToPlay(48000.0, 512);
    proc.releaseResources();
    check(proc.getAnalysisEngine() == e1 && e1->getHistory(0).framesSeen() == 2400 && proc.getEngineRebuildCount() == 1,
          "release + same-config prepare keeps analysis state");

    // Sequence continuity across a rebuild.
    e1->buildSnapshot(); e1->buildSnapshot(); e1->buildSnapshot();
    const auto lastSeq = e1->getLastSequence();
    proc.prepareToPlay(96000.0, 512);
    check(proc.getEngineRebuildCount() == 2 && proc.getAnalysisEngine()->getConfig().sampleRate == 96000.0, "sample-rate change rebuilds");
    check(proc.getAnalysisEngine()->getLastSequence() == lastSeq, "sequence base carried over -> editor never sees it go backwards");
    check(proc.getAnalysisEngine()->getHistory(0).framesSeen() == 0, "rebuilt engine starts clean");

    check(configure(proc, 2, 2, 96000.0, 512), "enable stereo sidechain");
    proc.prepareToPlay(96000.0, 512);
    check(proc.getEngineRebuildCount() == 3 && proc.getAnalysisEngine()->getRegistry().getActiveCount() == 2, "layout change rebuilds, two sources active");

    check(configure(proc, 1, 0, 96000.0, 512), "switch to mono, sidechain off");
    proc.prepareToPlay(96000.0, 512);
    check(proc.getEngineRebuildCount() == 4 && proc.getAnalysisEngine()->getRegistry()[0].numChannels == 1, "mono layout rebuilds with 1-channel source");

    proc.releaseResources();
    check(! proc.getAnalysisWorker()->isThreadRunning(), "stopped at the end");
}

static void test_worker_start_stop_1000x()
{
    section("worker_start_stop_1000x");

    // (a) Bare worker object: start/stop and destroy-while-running.
    {
        prism::AnalysisEngine eng(makeCfg(2, 2), 10);
        prism::SnapshotMailbox mb;
        std::atomic<int> active { 1 };
        std::atomic<std::int64_t> hb { prism::steadyNowMs() };
        std::vector<float> data(256, 0.3f);
        const float* ptrs[4] = { data.data(), data.data(), data.data(), data.data() };

        bool ok = true;
        const auto t0 = Clock::now();
        {
            prism::AnalysisWorker w(eng, mb, { &active, &hb });
            for (int i = 0; i < 1000; ++i)
            {
                w.start();
                ok &= w.isThreadRunning();
                eng.captureBlock(ptrs, 4, 256);          // keep it busy: stop lands mid-drain/publish
                hb.store(prism::steadyNowMs());
                if ((i & 7) == 0) std::this_thread::yield();
                w.stop();
                ok &= ! w.isThreadRunning();
            }
        }
        check(ok, "1000 start/stop cycles (bare worker) join cleanly");
        checkf(msSince(t0) < 20000.0, "1000 cycles finished promptly (ms)", msSince(t0));

        for (int i = 0; i < 200; ++i)
        {
            auto w = std::make_unique<prism::AnalysisWorker>(eng, mb, prism::WorkerControls { &active, &hb });
            w->start();
            eng.captureBlock(ptrs, 4, 256);
            w.reset();   // destructor must stop + join
        }
        check(true, "200x destroy-while-running");
    }

    // (b) Through the processor: prepare/release cycles with data in flight.
    {
        PRISMAudioProcessor proc;
        check(configure(proc, 2, 2, 48000.0, 4096), "configure");
        juce::AudioBuffer<float> buf(4, 4096);
        juce::MidiBuffer midi;
        for (int ch = 0; ch < 4; ++ch)
            for (int i = 0; i < 4096; ++i)
                buf.setSample(ch, i, 0.5f * std::sin(0.03f * (float) i));

        bool ok = true;
        std::mt19937 rng(21);
        proc.setEditorActive(true);
        for (int cycle = 0; cycle < 1000; ++cycle)
        {
            proc.prepareToPlay(48000.0, 4096);
            ok &= proc.getAnalysisWorker()->isThreadRunning();
            const int blocks = 1 + (int) (rng() % 4);
            for (int b = 0; b < blocks; ++b) proc.processBlock(buf, midi);
            if ((cycle % 5) == 0) sleepMs((int) (rng() % 3));            // sometimes let it publish
            if ((cycle % 3) == 0) proc.getLatestAnalysisSnapshot();
            proc.releaseResources();                                        // mid-processing: ring still holds data
            ok &= ! proc.getAnalysisWorker()->isThreadRunning();
        }
        proc.setEditorActive(false);
        check(ok, "1000 prepare/release cycles with processBlock traffic: worker always starts and joins");
        check(proc.getEngineRebuildCount() == 1, "engine built once across all 1000 same-config cycles");
    }
    check(true, "processor destroyed with a live worker/after cycles without hanging");
}

//==============================================================================
// STRESS (TSAN target)
static void test_stress_audio_worker_message_threads()
{
    section("stress_audio_worker_message_threads");
    double seconds = 2.0;
    if (const char* env = std::getenv("PRISM_STRESS_SECONDS")) seconds = std::max(0.2, std::atof(env));

    PRISMAudioProcessor proc;
    check(configure(proc, 2, 2, 48000.0, 4096), "configure");
    proc.prepareToPlay(48000.0, 4096);
    proc.setEditorActive(true);

    std::atomic<bool> stop { false };
    std::atomic<bool> passthroughOk { true };
    std::atomic<long> audioBlocks { 0 };
    FakePlayHead ph;

    std::thread audio([&] {
        juce::MidiBuffer midi;
        std::mt19937 rng(77);
        std::vector<std::vector<float>> ref(4, std::vector<float>(4096)), work(4, std::vector<float>(4096));
        for (int c = 0; c < 4; ++c)
            for (int i = 0; i < 4096; ++i)
                ref[(size_t) c][(size_t) i] = sig(c, (std::uint64_t) i);
        proc.setPlayHead(&ph);
        double ppq = 0.0;
        while (! stop.load(std::memory_order_acquire))
        {
            const int n = 1 + (int) (rng() % 4096);
            for (int c = 0; c < 4; ++c) std::memcpy(work[(size_t) c].data(), ref[(size_t) c].data(), (size_t) n * sizeof(float));
            float* ptrs[4] = { work[0].data(), work[1].data(), work[2].data(), work[3].data() };
            juce::AudioBuffer<float> view(ptrs, 4, n);
            ph.ppq = ppq; ppq += (double) n * 2.0 / 48000.0;
            proc.processBlock(view, midi);
            for (int c = 0; c < 4; ++c)
                if (std::memcmp(work[(size_t) c].data(), ref[(size_t) c].data(), (size_t) n * sizeof(float)) != 0)
                    passthroughOk.store(false);
            audioBlocks.fetch_add(1);
            if ((rng() & 127) == 0) std::this_thread::yield();
        }
        proc.setPlayHead(nullptr);
    });

    std::uint64_t prev = 0;
    bool monotonic = true;
    long fetches = 0, distinct = 0;
    int toggles = 0;
    const auto t0 = Clock::now();
    while (msSince(t0) < seconds * 1000.0)
    {
        auto s = proc.getLatestAnalysisSnapshot();
        ++fetches;
        if (s != nullptr)
        {
            monotonic &= s->sequence >= prev;
            if (s->sequence != prev) ++distinct;
            prev = s->sequence;
        }
        if ((fetches % 500) == 0)
        {
            proc.requestResetAll();
            proc.requestResetIntegrated();
            proc.setEditorActive(false);
            proc.setEditorActive(true);
            ++toggles;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    stop.store(true, std::memory_order_release);
    audio.join();
    proc.setEditorActive(false);
    proc.releaseResources();

    check(passthroughOk.load(), "audio thread: output bit-identical to input throughout");
    check(monotonic, "message thread: snapshot sequence never goes backwards");
    checkf(audioBlocks.load() > 100, "audio blocks processed", (double) audioBlocks.load());
    checkf(distinct > 10, "distinct snapshots fetched", (double) distinct);
    std::printf("      %ld blocks, %ld fetches, %ld distinct snapshots, %d editor toggles, overruns=%llu\n",
                audioBlocks.load(), fetches, distinct, toggles,
                (unsigned long long) proc.getAnalysisEngine()->getOverrunCount(0));
}

//==============================================================================
// PHASE 2.2 - peaks, stereo metrics, scope + gonio, silence semantics
namespace
{
    using Gen = std::function<float(int ch, std::uint64_t i)>;

    float hnoise(int ch, std::uint64_t i)   // deterministic uniform noise in [-0.5, 0.5)
    {
        std::uint32_t h = (std::uint32_t) i * 2654435761u + (std::uint32_t) (ch + 1) * 97531u;
        h ^= h >> 16; h *= 2246822519u; h ^= h >> 13; h *= 3266489917u; h ^= h >> 16;
        return (float) (h & 0xffffff) / 16777216.0f - 0.5f;
    }

    int totalChannels(const prism::AnalysisEngine& e)
    {
        int n = 0;
        for (int c : e.getConfig().busChannels) n += c;
        return n;
    }

    // Capture + pump `frames` frames [from, from+frames) generated by g(channel, absoluteIndex).
    void feed(prism::AnalysisEngine& eng, std::uint64_t from, std::uint64_t frames, int block, const Gen& g)
    {
        const int chs = totalChannels(eng);
        std::vector<std::vector<float>> bufs((size_t) chs, std::vector<float>((size_t) std::max(1, block)));
        std::vector<const float*> ptrs((size_t) chs);
        for (int c = 0; c < chs; ++c) ptrs[(size_t) c] = bufs[(size_t) c].data();

        for (std::uint64_t pos = 0; pos < frames;)
        {
            const int n = (int) std::min<std::uint64_t>((std::uint64_t) block, frames - pos);
            for (int c = 0; c < chs; ++c)
                for (int i = 0; i < n; ++i)
                    bufs[(size_t) c][(size_t) i] = g(c, from + pos + (std::uint64_t) i);
            eng.captureBlock(ptrs.data(), chs, n);
            eng.pump();
            pos += (std::uint64_t) n;
        }
    }

    float dbOf(float lin) { return (float) (20.0 * std::log10((double) lin)); }

    prism::SourceSnapshot measureStereo(const Gen& g, std::uint64_t frames = 48000, int block = 480)
    {
        prism::AnalysisEngine eng(makeCfg(2, 0), 14);
        feed(eng, 0, frames, block, g);
        return eng.buildSnapshot()->sources[0];
    }
}

static void test_stereo_metrics_definitions()
{
    section("stereo_metrics_definitions");
    const auto tone = [](std::uint64_t i) { return 0.5f * std::sin(0.0731f * (float) (i % 100000)); };

    auto identical = measureStereo([&](int, std::uint64_t i) { return tone(i); });
    check(identical.hasStereo, "stereo source: hasStereo");
    checkf(std::abs(identical.correlation - 1.0f) < 1e-5f, "identical L=R: correlation +1", identical.correlation);
    checkf(identical.widthPercent < 0.01f, "identical L=R: width 0 (mono)", identical.widthPercent);
    checkf(std::abs(identical.balanceDb) < 1e-4f, "identical L=R: balance 0 dB", identical.balanceDb);

    auto inverted = measureStereo([&](int c, std::uint64_t i) { return c == 0 ? tone(i) : -tone(i); });
    checkf(std::abs(inverted.correlation + 1.0f) < 1e-5f, "inverted R=-L: correlation -1", inverted.correlation);
    checkf(std::abs(inverted.widthPercent - 100.0f) < 0.1f, "anti-phase: width 100", inverted.widthPercent);
    checkf(std::abs(inverted.balanceDb) < 1e-4f, "anti-phase: balance 0 dB", inverted.balanceDb);

    auto noise = measureStereo([&](int c, std::uint64_t i) { return hnoise(c, i); });
    checkf(std::abs(noise.correlation) < 0.05f, "independent noise: correlation 0 +- 0.05", noise.correlation);
    checkf(std::abs(noise.widthPercent - 50.0f) < 2.0f, "independent noise: width ~50", noise.widthPercent);
    checkf(std::abs(noise.balanceDb) < 0.3f, "independent noise: balance ~0 dB", noise.balanceDb);

    auto hardL = measureStereo([&](int c, std::uint64_t i) { return c == 0 ? tone(i) : 0.0f; });
    checkf(hardL.correlation == 0.0f, "hard-left: correlation 0 (denominator guard)", hardL.correlation);
    checkf(std::abs(hardL.widthPercent - 50.0f) < 0.1f, "hard-left: width 50", hardL.widthPercent);
    checkf(hardL.balanceDb == -60.0f, "hard-left: balance clamps to -60 dB (left-heavy negative)", hardL.balanceDb);

    auto hardR = measureStereo([&](int c, std::uint64_t i) { return c == 1 ? tone(i) : 0.0f; });
    checkf(hardR.correlation == 0.0f && std::abs(hardR.widthPercent - 50.0f) < 0.1f && hardR.balanceDb == 60.0f,
           "hard-right: corr 0, width 50, balance +60 dB", hardR.balanceDb);

    auto ratio = measureStereo([&](int c, std::uint64_t i) { return c == 0 ? tone(i) : tone(i) * 1.41421356f; });
    checkf(std::abs(ratio.balanceDb - 3.0103f) < 0.01f, "R = sqrt2 x L: balance +3.01 dB (right-heavy positive)", ratio.balanceDb);
    checkf(std::abs(ratio.correlation - 1.0f) < 1e-5f, "R = sqrt2 x L: still fully correlated", ratio.correlation);

    auto silence = measureStereo([&](int, std::uint64_t) { return 0.0f; });
    check(silence.correlation == 0.0f && silence.widthPercent == 0.0f && silence.balanceDb == 0.0f,
          "silence: corr 0, width 0, balance 0 (no NaN)");

    // Rolling 400 ms window: old content is forgotten, silence afterwards returns to 0.
    {
        prism::AnalysisEngine eng(makeCfg(2, 0), 14);
        feed(eng, 0, 48000, 480, [&](int c, std::uint64_t i) { return c == 0 ? tone(i) : -tone(i); });
        checkf(eng.buildSnapshot()->sources[0].correlation < -0.999f, "window primed with anti-phase", eng.buildSnapshot()->sources[0].correlation);
        feed(eng, 48000, 24000, 480, [&](int, std::uint64_t i) { return tone(i); });   // 500 ms identical
        checkf(eng.buildSnapshot()->sources[0].correlation > 0.999f, "400 ms window forgets older content", eng.buildSnapshot()->sources[0].correlation);
        feed(eng, 72000, 24000, 480, [&](int, std::uint64_t) { return 0.0f; });         // 500 ms silence
        const auto s = eng.buildSnapshot()->sources[0];
        check(s.correlation == 0.0f && s.widthPercent == 0.0f && s.balanceDb == 0.0f, "silence after signal: metrics return to 0");
    }

    // Frame-keyed: identical results for any block size.
    bool same = true;
    prism::SourceSnapshot ref = measureStereo([&](int c, std::uint64_t i) { return hnoise(c, i) * (c ? 0.7f : 1.0f); }, 30000, 480);
    for (int bs : { 1, 7, 64, 4096 })
    {
        auto s = measureStereo([&](int c, std::uint64_t i) { return hnoise(c, i) * (c ? 0.7f : 1.0f); }, 30000, bs);
        same &= s.correlation == ref.correlation && s.widthPercent == ref.widthPercent && s.balanceDb == ref.balanceDb;
    }
    check(same, "metrics bit-identical for block sizes {1,7,64,480,4096}");

    // Non-finite input never poisons the meters.
    auto nasty = measureStereo([&](int c, std::uint64_t i) {
        if (i % 97 == 0) return std::numeric_limits<float>::quiet_NaN();
        if (i % 89 == 0) return c ? std::numeric_limits<float>::infinity() : -std::numeric_limits<float>::infinity();
        return tone(i);
    }, 20000);
    check(std::isfinite(nasty.correlation) && std::isfinite(nasty.widthPercent) && std::isfinite(nasty.balanceDb),
          "NaN/inf input: meters stay finite");
}

static void test_sample_peak_snapshot()
{
    section("sample_peak_snapshot");

    // Exact peaks at boundaries, blocks of 1 sample and larger, either channel, holds forever.
    bool exact = true, holds = true;
    for (int bs : { 1, 7, 480, 4096 })
        for (std::uint64_t pos : { std::uint64_t(0), std::uint64_t(6), std::uint64_t(7), std::uint64_t(479), std::uint64_t(480), std::uint64_t(1999) })
            for (int ch : { 0, 1 })
            {
                prism::AnalysisEngine eng(makeCfg(2, 0), 14);
                const float v = 0.731f;
                feed(eng, 0, 2000, bs, [&](int c, std::uint64_t i) { return (c == ch && i == pos) ? -v : 0.0f; });
                const auto s = eng.buildSnapshot()->sources[0];
                exact &= s.samplePeakMaxDb == dbOf(v);
                feed(eng, 2000, 5000, bs, [](int, std::uint64_t) { return 0.0f; });
                holds &= eng.buildSnapshot()->sources[0].samplePeakMaxDb == dbOf(v);
            }
    check(exact, "impulse peak exact (|-0.731| -> 20log10): pos 0/6/7/479/480/1999, ch L/R, blocks 1/7/480/4096");
    check(holds, "sample peak never decays");

    {
        prism::AnalysisEngine eng(makeCfg(2, 0), 14);
        check(std::isinf(eng.buildSnapshot()->sources[0].samplePeakMaxDb) && eng.buildSnapshot()->sources[0].samplePeakMaxDb < 0,
              "before any signal: -INF (dashes)");
        feed(eng, 0, 1000, 100, [](int, std::uint64_t) { return 0.0f; });
        check(eng.buildSnapshot()->sources[0].samplePeakMaxDb < -1.0e30f, "digital silence keeps -INF");
        feed(eng, 1000, 100, 100, [](int c, std::uint64_t i) { return i == 1050 ? (c ? 1.0f : 0.25f) : 0.0f; });
        check(eng.buildSnapshot()->sources[0].samplePeakMaxDb == 0.0f, "full-scale peak reads exactly 0 dBFS");
        feed(eng, 1100, 100, 100, [](int, std::uint64_t i) { return i == 1150 ? 0.5f : 0.0f; });
        check(eng.buildSnapshot()->sources[0].samplePeakMaxDb == 0.0f, "a later smaller peak does not lower the maximum");
    }

    {   // Lossless across ring overruns (the dropped block still contributes its peak).
        prism::AnalysisEngine eng(makeCfg(2, 0), 8);   // 256-frame ring
        std::vector<float> l(100, 0.1f), r(100, 0.1f);
        const float* p[2] = { l.data(), r.data() };
        eng.captureBlock(p, 2, 100);
        eng.captureBlock(p, 2, 100);
        r[63] = 0.8f;
        eng.captureBlock(p, 2, 100);   // dropped
        eng.pump();
        check(eng.getOverrunCount(0) == 1, "overrun happened");
        check(eng.buildSnapshot()->sources[0].samplePeakMaxDb == dbOf(0.8f), "published sample peak includes the dropped block");
    }
}

static void test_reset_all_epoch()
{
    section("reset_all_epoch");
    std::atomic<std::uint32_t> resetAll { 0 }, resetInt { 0 };
    prism::AnalysisEngine eng(makeCfg(2, 0), 14);
    eng.attachControls({ &resetAll, &resetInt, nullptr });

    feed(eng, 0, 480, 480, [](int, std::uint64_t i) { return i == 100 ? 0.9f : 0.0f; });
    check(eng.buildSnapshot()->sources[0].samplePeakMaxDb == dbOf(0.9f), "peak published");

    resetInt.fetch_add(1);
    eng.pump();
    check(eng.buildSnapshot()->sources[0].samplePeakMaxDb == dbOf(0.9f), "reset_integrated does NOT clear peaks");

    resetAll.fetch_add(1);
    eng.pump();
    check(eng.buildSnapshot()->sources[0].samplePeakMaxDb < -1.0e30f, "reset_all clears the peak to -INF");

    feed(eng, 480, 480, 480, [](int, std::uint64_t i) { return i == 600 ? 0.1f : 0.0f; });
    check(eng.buildSnapshot()->sources[0].samplePeakMaxDb == dbOf(0.1f), "measurement restarts from the new signal");

    // A peak captured before the reset but not yet pumped is part of 'before the reset'.
    std::vector<float> loud(64, 0.7f);
    const float* p[2] = { loud.data(), loud.data() };
    eng.captureBlock(p, 2, 64);
    resetAll.fetch_add(1);
    eng.pump();
    check(eng.buildSnapshot()->sources[0].samplePeakMaxDb < -1.0e30f, "pre-reset pending peak is discarded");

    // Epochs requested before the engine existed are not re-applied (baseline at attach).
    resetAll.fetch_add(5);
    prism::AnalysisEngine eng2(makeCfg(2, 0), 14);
    eng2.attachControls({ &resetAll, &resetInt, nullptr });
    feed(eng2, 0, 100, 100, [](int, std::uint64_t i) { return i == 5 ? 0.3f : 0.0f; });
    check(eng2.buildSnapshot()->sources[0].samplePeakMaxDb == dbOf(0.3f), "stale epochs not re-applied to a fresh engine");
}

static void test_scope_window()
{
    section("scope_window");
    check(prism::AnalysisEngine::scopeLengthFor(1.0e6) == prism::kScopeSamples, "scope length clamps to 8192");

    struct Case { double fs; int expect; };
    const Case cases[] = { { 44100.0, 1938 }, { 48000.0, 2016 }, { 96000.0, 2976 }, { 192000.0, 4896 } };
    std::atomic<float> mode { 0.0f };   // L/R: scope = L

    for (const auto& c : cases)
    {
        char name[128];
        prism::AnalysisEngine eng(makeCfg(2, 0, c.fs), 14);
        eng.attachControls({ nullptr, nullptr, &mode });
        const std::uint64_t total = 30000;
        feed(eng, 0, total, 480, [](int ch, std::uint64_t i) { return ch == 0 ? (float) i : -(float) i; });
        const auto s = eng.buildSnapshot()->sources[0];

        std::snprintf(name, sizeof(name), "fs %.0f: scopeCount == min(8192, ceil(0.02 fs)+800+256) == %d", c.fs, c.expect);
        check(s.scopeCount == c.expect && s.scopeCount == prism::AnalysisEngine::scopeLengthFor(c.fs), name);

        bool contiguous = true;
        for (int k = 0; k < s.scopeCount; ++k)
            contiguous &= s.scope[(size_t) k] == (float) (total - (std::uint64_t) s.scopeCount + (std::uint64_t) k);
        std::snprintf(name, sizeof(name), "fs %.0f: scope is the last-N contiguous frames (counter ramp, oldest first)", c.fs);
        check(contiguous, name);
    }

    {   // wraps the 65536-frame history, odd block size
        prism::AnalysisEngine eng(makeCfg(2, 0, 192000.0), 14);
        eng.attachControls({ nullptr, nullptr, &mode });
        const std::uint64_t total = 70001;
        feed(eng, 0, total, 333, [](int ch, std::uint64_t i) { return ch == 0 ? (float) (i % 1000000) : 0.0f; });
        const auto s = eng.buildSnapshot()->sources[0];
        bool ok = s.scopeCount == 4896;
        for (int k = 0; k < s.scopeCount && ok; ++k)
            ok &= s.scope[(size_t) k] == (float) ((total - 4896 + (std::uint64_t) k) % 1000000);
        check(ok, "contiguity holds across the history wrap (192 kHz, block 333)");
    }

    {   // fewer frames than the window: only real frames are published
        prism::AnalysisEngine eng(makeCfg(2, 0), 14);
        eng.attachControls({ nullptr, nullptr, &mode });
        feed(eng, 0, 100, 100, [](int ch, std::uint64_t i) { return ch == 0 ? (float) i + 1.0f : 0.0f; });
        const auto s = eng.buildSnapshot()->sources[0];
        check(s.scopeCount == 100 && s.scope[0] == 1.0f && s.scope[99] == 100.0f, "warm-up: scopeCount = frames available, no zero padding");
    }
}

static void test_scope_analysis_mode_channel()
{
    section("scope_analysis_mode_channel");
    std::atomic<float> mode { 0.0f };
    auto scopeLast = [&](int m, float l, float r, int mainCh) {
        mode.store((float) m);
        prism::AnalysisEngine eng(makeCfg(mainCh, 0), 14);
        eng.attachControls({ nullptr, nullptr, &mode });
        feed(eng, 0, 4000, 400, [&](int c, std::uint64_t) { return c == 0 ? l : r; });
        const auto s = eng.buildSnapshot()->sources[0];
        return s.scope[(size_t) (s.scopeCount - 1)];
    };
    const float inv = 0.70710678f;
    checkf(std::abs(scopeLast(0, 1.0f, 0.0f, 2) - 1.0f) < 1e-6f, "leftRight: scope = L", scopeLast(0, 1.0f, 0.0f, 2));
    checkf(std::abs(scopeLast(1, 1.0f, 0.0f, 2) - inv) < 1e-6f, "midSide: scope = (L+R)/sqrt2", scopeLast(1, 1.0f, 0.0f, 2));
    checkf(std::abs(scopeLast(2, 1.0f, 0.0f, 2) - 0.5f) < 1e-6f, "combined: scope = (L+R)/2", scopeLast(2, 1.0f, 0.0f, 2));
    checkf(std::abs(scopeLast(1, 0.6f, -0.6f, 2)) < 1e-6f && std::abs(scopeLast(2, 0.6f, -0.6f, 2)) < 1e-6f,
           "anti-phase: Mid and combined scopes cancel to 0", scopeLast(1, 0.6f, -0.6f, 2));
    checkf(std::abs(scopeLast(0, 0.6f, -0.6f, 2) - 0.6f) < 1e-6f, "anti-phase: L/R mode shows L", 0);
    check(scopeLast(0, 0.4f, 0.0f, 1) == 0.4f && scopeLast(1, 0.4f, 0.0f, 1) == 0.4f && scopeLast(2, 0.4f, 0.0f, 1) == 0.4f,
          "mono source ignores analysis_mode (single channel)");

    // No attached control -> default 'combined'.
    {
        prism::AnalysisEngine eng(makeCfg(2, 0), 14);
        feed(eng, 0, 1000, 100, [](int c, std::uint64_t) { return c == 0 ? 1.0f : 0.0f; });
        const auto s = eng.buildSnapshot()->sources[0];
        check(s.scope[(size_t) (s.scopeCount - 1)] == 0.5f, "default mode is combined");
    }

    // Processor wiring: the engine reads the real APVTS analysis_mode atomic.
    {
        PRISMAudioProcessor proc;
        check(configure(proc, 2, 0, 48000.0, 480), "configure");
        proc.prepareToPlay(48000.0, 480);
        juce::AudioBuffer<float> buf(2, 480);
        juce::MidiBuffer midi;
        for (int i = 0; i < 480; ++i) { buf.setSample(0, i, 1.0f); buf.setSample(1, i, 0.0f); }
        auto* prm = proc.apvts.getParameter(prism::ids::analysisMode);

        prm->setValueNotifyingHost(prm->convertTo0to1(1.0f));   // midSide
        for (int i = 0; i < 10; ++i) proc.processBlock(buf, midi);
        proc.releaseResources();                                // stop the worker: drive the core synchronously
        auto* eng = proc.getAnalysisEngine();
        eng->pump();
        auto s = eng->buildSnapshot()->sources[0];
        checkf(std::abs(s.scope[(size_t) (s.scopeCount - 1)] - inv) < 1e-6f, "processor: analysis_mode=midSide reaches the engine", s.scope[(size_t) (s.scopeCount - 1)]);

        prm->setValueNotifyingHost(prm->convertTo0to1(0.0f));   // leftRight
        s = eng->buildSnapshot()->sources[0];
        checkf(s.scope[(size_t) (s.scopeCount - 1)] == 1.0f, "processor: analysis_mode=leftRight reaches the engine", s.scope[(size_t) (s.scopeCount - 1)]);
    }
}

static void test_gonio_pairs()
{
    section("gonio_pairs");
    check(prism::AnalysisEngine::gonioStrideFor(48000.0) == 3 && prism::AnalysisEngine::gonioStrideFor(96000.0) == 6
          && prism::AnalysisEngine::gonioStrideFor(192000.0) == 12 && prism::AnalysisEngine::gonioStrideFor(44100.0) == 3,
          "stride = max(1, round(3 fs/48000)): 3 @48k, 6 @96k, 12 @192k, 3 @44.1k");

    // Counter ramp: pair p must be frame (total-1) - (899-p)*stride.
    for (double fs : { 44100.0, 48000.0, 96000.0, 192000.0 })
    {
        prism::AnalysisEngine eng(makeCfg(2, 0, fs), 14);
        const std::uint64_t total = 20000;
        feed(eng, 0, total, 480, [](int c, std::uint64_t i) { return c == 0 ? (float) i : -(float) i; });
        const auto s = eng.buildSnapshot()->sources[0];
        const int stride = prism::AnalysisEngine::gonioStrideFor(fs);
        bool ok = s.gonioPairCount == prism::kGonioPairs;
        for (int p = 0; p < s.gonioPairCount && ok; ++p)
        {
            const float f = (float) (total - 1 - (std::uint64_t) (899 - p) * (std::uint64_t) stride);
            ok &= s.gonio[(size_t) (2 * p)] == f && s.gonio[(size_t) (2 * p + 1)] == -f;
        }
        char name[96];
        std::snprintf(name, sizeof(name), "fs %.0f: 900 pairs, stride %d, newest frame last (interleaved L,R)", fs, stride);
        check(ok, name);
    }

    {   // warm-up
        prism::AnalysisEngine eng(makeCfg(2, 0), 14);
        feed(eng, 0, 100, 100, [](int c, std::uint64_t i) { return c == 0 ? (float) i : 0.0f; });
        const auto s = eng.buildSnapshot()->sources[0];
        check(s.gonioPairCount == 34 && s.gonio[2 * 33] == 99.0f && s.gonio[0] == 0.0f, "warm-up: floor((frames-1)/stride)+1 pairs, ending at the newest frame");
    }

    // Documented diagonals, checked numerically with the JS mapping mid=(L+R)/sqrt2, side=(R-L)/sqrt2.
    auto diag = [](const Gen& g) {
        prism::AnalysisEngine eng(makeCfg(2, 0), 14);
        feed(eng, 0, 10000, 480, g);
        const auto s = eng.buildSnapshot()->sources[0];
        struct R { double maxAbsMidPlusSide = 0, maxAbsMidMinusSide = 0, maxAbsSide = 0, maxAbsMid = 0; bool posMidNegSide = true, posMidPosSide = true; int n = 0; };
        R r;
        const double k = 1.0 / std::sqrt(2.0);
        for (int p = 0; p < s.gonioPairCount; ++p)
        {
            const double L = s.gonio[(size_t) (2 * p)], Rr = s.gonio[(size_t) (2 * p + 1)];
            const double mid = (L + Rr) * k, side = (Rr - L) * k;
            r.maxAbsMidPlusSide  = std::max(r.maxAbsMidPlusSide, std::abs(mid + side));
            r.maxAbsMidMinusSide = std::max(r.maxAbsMidMinusSide, std::abs(mid - side));
            r.maxAbsSide = std::max(r.maxAbsSide, std::abs(side));
            r.maxAbsMid  = std::max(r.maxAbsMid, std::abs(mid));
            if (mid > 1e-3) { r.posMidNegSide &= side < 0.0; r.posMidPosSide &= side > 0.0; ++r.n; }
        }
        return r;
    };
    const auto tone = [](std::uint64_t i) { return 0.8f * std::sin(0.31f * (float) (i % 10000)); };

    const auto hl = diag([&](int c, std::uint64_t i) { return c == 0 ? tone(i) : 0.0f; });
    check(hl.maxAbsMidPlusSide < 1e-6 && hl.n > 100 && hl.posMidNegSide, "hard-left: side = -mid (upper-left diagonal)");
    const auto hr = diag([&](int c, std::uint64_t i) { return c == 1 ? tone(i) : 0.0f; });
    check(hr.maxAbsMidMinusSide < 1e-6 && hr.n > 100 && hr.posMidPosSide, "hard-right: side = +mid (upper-right diagonal)");
    const auto mono = diag([&](int, std::uint64_t i) { return tone(i); });
    check(mono.maxAbsSide < 1e-6 && mono.maxAbsMid > 0.5, "mono: side = 0, vertical line");
    const auto anti = diag([&](int c, std::uint64_t i) { return c == 0 ? tone(i) : -tone(i); });
    check(anti.maxAbsMid < 1e-6 && anti.maxAbsSide > 0.5, "anti-phase: mid = 0, horizontal line");
}

static void test_mono_and_absent_sources_snapshot()
{
    section("mono_and_absent_sources_snapshot");

    {   // mono main, no sidechain
        prism::AnalysisEngine eng(makeCfg(1, 0), 14);
        feed(eng, 0, 5000, 480, [](int, std::uint64_t i) { return 0.5f * std::sin(0.1f * (float) i); });
        const auto snap = eng.buildSnapshot();
        const auto& s0 = snap->sources[0];
        check(! s0.hasStereo, "mono source: hasStereo=false");
        check(s0.correlation == 0.0f && s0.widthPercent == 0.0f && s0.balanceDb == 0.0f, "mono source: stereo metrics unpublished");
        check(s0.scopeCount > 0 && s0.gonioPairCount > 0 && s0.samplePeakMaxDb > -7.0f, "mono source: scope, gonio and peak still published");
        bool lr = true;
        for (int p = 0; p < s0.gonioPairCount; ++p) lr &= s0.gonio[(size_t) (2 * p)] == s0.gonio[(size_t) (2 * p + 1)];
        check(lr, "mono gonio has L == R (vertical line)");
        check(snap->signalPresent, "signalPresent from the mono source");

        const auto& s1 = snap->sources[1];
        check(! s1.hasStereo && ! s1.hasSpectrum && s1.scopeCount == 0 && s1.gonioPairCount == 0 && s1.waveformColumnCount == 0,
              "absent sidechain: no scope, gonio, spectrum or waveform (no fabricated zeros)");
        check(std::isinf(s1.samplePeakMaxDb) && s1.samplePeakMaxDb < 0 && std::isinf(s1.truePeakMaxDb),
              "absent sidechain: peaks -INF");
        check(s1.correlation == 0.0f && s1.widthPercent == 0.0f && s1.balanceDb == 0.0f, "absent sidechain: meters at defaults");
    }

    {   // stereo main + mono sidechain
        prism::AnalysisEngine eng(makeCfg(2, 1), 14);
        feed(eng, 0, 5000, 480, [](int c, std::uint64_t i) { return c == 2 ? 0.25f : 0.5f * std::sin(0.1f * (float) i); });
        const auto snap = eng.buildSnapshot();
        check(snap->sources[0].hasStereo && ! snap->sources[1].hasStereo, "stereo main + mono sidechain: hasStereo [true,false]");
        check(snap->sources[1].scopeCount > 0 && snap->sources[1].samplePeakMaxDb == dbOf(0.25f), "mono sidechain measured independently");
    }

    {   // stereo main + stereo sidechain are independent
        prism::AnalysisEngine eng(makeCfg(2, 2), 14);
        feed(eng, 0, 48000, 480, [](int c, std::uint64_t i) {
            if (c < 2) return 0.5f * std::sin(0.05f * (float) i);                 // main: identical
            return c == 2 ? 0.4f * std::sin(0.05f * (float) i) : 0.0f;            // sidechain: hard-left
        });
        const auto snap = eng.buildSnapshot();
        check(snap->sources[0].correlation > 0.999f && snap->sources[1].correlation == 0.0f
              && snap->sources[1].balanceDb == -60.0f && std::abs(snap->sources[0].balanceDb) < 1e-3f,
              "two stereo sources keep separate metrics");
    }
}

static void test_signal_present()
{
    section("signal_present");
    auto tone = [](float amp) { return [amp](int, std::uint64_t i) { return amp * std::sin(0.2f * (float) (i % 5000)); }; };
    auto silence = [](int, std::uint64_t) { return 0.0f; };

    prism::AnalysisEngine eng(makeCfg(2, 0), 14);
    check(! eng.buildSnapshot()->signalPresent, "false before any audio");
    feed(eng, 0, 4800, 480, tone(0.5f));
    check(eng.buildSnapshot()->signalPresent, "true with signal");
    feed(eng, 4800, 14400, 480, silence);   // 300 ms
    check(eng.buildSnapshot()->signalPresent, "held for 500 ms after the signal stops");
    feed(eng, 19200, 14400, 480, silence);  // 600 ms total
    check(! eng.buildSnapshot()->signalPresent, "false after >500 ms of silence");
    check(eng.buildSnapshot()->sources[0].samplePeakMaxDb > -7.0f, "peak hold survives silence");

    prism::AnalysisEngine quiet(makeCfg(2, 0), 14);
    feed(quiet, 0, 9600, 480, tone(1.0e-5f));   // -100 dBFS
    check(! quiet.buildSnapshot()->signalPresent, "-100 dBFS is below the -90 dBFS threshold");
    feed(quiet, 9600, 9600, 480, tone(1.0e-4f));   // -80 dBFS
    check(quiet.buildSnapshot()->signalPresent, "-80 dBFS counts as signal");

    prism::AnalysisEngine sc(makeCfg(2, 2), 14);
    feed(sc, 0, 4800, 480, [](int c, std::uint64_t i) { return c >= 2 ? 0.3f * std::sin(0.2f * (float) i) : 0.0f; });
    check(sc.buildSnapshot()->signalPresent, "signal on the sidechain alone counts");
}

static void test_processor_peak_reset_end_to_end()
{
    section("processor_peak_reset_end_to_end");
    PRISMAudioProcessor proc;
    check(configure(proc, 2, 0, 48000.0, 480), "configure");
    proc.prepareToPlay(48000.0, 480);
    proc.setEditorActive(true);

    juce::AudioBuffer<float> loud(2, 480), quiet(2, 480);
    juce::MidiBuffer midi;
    for (int i = 0; i < 480; ++i)
        for (int c = 0; c < 2; ++c) { loud.setSample(c, i, c == 0 ? 0.5f : 0.25f); quiet.setSample(c, i, 0.0f); }

    auto waitFor = [&](juce::AudioBuffer<float>& buf, auto&& pred) {
        const auto t0 = Clock::now();
        while (msSince(t0) < 3000.0)
        {
            proc.processBlock(buf, midi);
            auto s = proc.getLatestAnalysisSnapshot();
            if (s != nullptr && pred(*s)) return true;
            sleepMs(3);
        }
        return false;
    };

    const float expectDb = dbOf(0.5f);
    check(waitFor(loud, [&](const prism::AnalysisSnapshot& s) {
              return s.sources[0].samplePeakMaxDb == expectDb && s.signalPresent && s.sources[0].hasStereo && s.sources[0].scopeCount > 0
                     && s.sources[0].balanceDb < -5.9f && s.sources[0].balanceDb > -6.1f;
          }), "worker publishes peak, signalPresent, hasStereo, scope and balance end to end");

    proc.requestResetAll();
    check(waitFor(quiet, [&](const prism::AnalysisSnapshot& s) { return s.sources[0].samplePeakMaxDb < -1.0e30f; }),
          "requestResetAll() reaches the worker and clears the published peak");

    check(waitFor(loud, [&](const prism::AnalysisSnapshot& s) { return s.sources[0].samplePeakMaxDb == expectDb; }),
          "peak measurement restarts after the reset");
    check(proc.getLatestAnalysisSnapshot()->sources[1].scopeCount == 0, "absent sidechain stays empty end to end");

    proc.setEditorActive(false);
    proc.releaseResources();
}

//==============================================================================
// PHASE 2.3 - spectrum engine
namespace
{
    constexpr int kSizeList[5] = { 1024, 2048, 4096, 8192, 16384 };
    const char* winName(int w) { return w == 0 ? "hann" : "blackmanHarris"; }

    // Holds an engine wired to controllable fft_size / window / analysis_mode "parameters".
    struct SpecRig
    {
        std::atomic<float> size, win, mode;
        std::unique_ptr<prism::AnalysisEngine> eng;

        SpecRig(double fs, int sizeIdx, int winIdx, int modeIdx = 2, int mainCh = 2, int sc = 0)
            : size((float) sizeIdx), win((float) winIdx), mode((float) modeIdx)
        {
            eng = std::make_unique<prism::AnalysisEngine>(makeCfg(mainCh, sc, fs), 16);
            eng->attachControls({ nullptr, nullptr, &mode, &size, &win });
        }
        void feedFrames(std::uint64_t from, std::uint64_t frames, int block, const Gen& g) { feed(*eng, from, frames, block, g); }
        prism::SourceSnapshot snap(int src = 0) { return eng->buildSnapshot()->sources[(size_t) src]; }
    };

    // Sine at num/den cycles per sample, exact phase via integer arithmetic. chMask bit c -> channel c carries it.
    Gen sineRat(std::uint64_t num, std::uint64_t den, float amp, int chMask, float sign1 = 1.0f)
    {
        return [=](int c, std::uint64_t i) {
            if (! ((chMask >> c) & 1)) return 0.0f;
            const double ph = (double) ((num * i) % den) / (double) den;
            return (c == 1 ? sign1 : 1.0f) * amp * (float) std::sin(2.0 * 3.14159265358979323846 * ph);
        };
    }

    float gauss(int ch, std::uint64_t i, float sigma)
    {
        auto u = [&](std::uint32_t seed) {
            std::uint32_t h = (std::uint32_t) i * 2654435761u + (std::uint32_t) (ch + 1) * 97531u + seed * 40503u;
            h ^= h >> 16; h *= 2246822519u; h ^= h >> 13; h *= 3266489917u; h ^= h >> 16;
            return ((double) (h & 0xffffff) + 0.5) / 16777216.0;
        };
        return sigma * (float) (std::sqrt(-2.0 * std::log(u(1))) * std::cos(2.0 * 3.14159265358979323846 * u(2)));
    }

    int pickBin(double fs, int n, double hz) { return std::max(4, (int) std::lround(hz * n / fs)); }

    struct CurvePeak { float db = -1000.0f; int idx = -1; };
    CurvePeak curvePeak(const prism::SourceSnapshot& s)
    {
        CurvePeak p;
        for (int i = 0; i < prism::kSpectrumPoints; ++i)
            if (s.spectrumDb[(size_t) i] > p.db) { p.db = s.spectrumDb[(size_t) i]; p.idx = i; }
        return p;
    }
    double octaves(double a, double b) { return std::abs(std::log2(a / b)); }
}

static void test_spectrum_basics_and_map()
{
    section("spectrum_basics_and_map");
    check(std::abs(prism::SpectrumEngine::pointFrequency(0) - 20.0) < 1e-9 && std::abs(prism::SpectrumEngine::pointFrequency(639) - 20000.0) < 1e-6,
          "log points span 20 Hz .. 20 kHz (640 points)");

    prism::SpectrumEngine se;
    se.prepare(48000.0);
    bool ok = true;
    for (int s = 0; s < 5; ++s)
    {
        const int n = kSizeList[s];
        ok &= std::abs(se.getWindowSum(s, 0) - n * 0.5) < 1e-3;                       // Hann: S = N/2
        ok &= std::abs(se.getWindowSum(s, 1) / n - 0.35875) < 1e-6;                   // Blackman-Harris: S = 0.35875 N
        ok &= std::abs(se.getWindowValue(s, 0, n / 2) - 1.0f) < 1e-6f && se.getWindowValue(s, 0, 0) == 0.0f;
        ok &= std::abs(se.getWindowValue(s, 1, n / 2) - 1.0f) < 1e-5f;
        ok &= std::abs(se.getWindowSumSquares(s, 0) / n - 0.375) < 1e-6;              // sum(hann^2) = 3N/8
    }
    check(ok, "window tables: Hann S=N/2, BH S=0.35875N (numeric), peak = 1 at N/2, sum(hann^2)=3N/8");
}

static void test_spectrum_hop_timing()
{
    section("spectrum_hop_timing");
    bool ok = true;
    for (int sizeIdx : { 0, 2, 4 })
        for (int bs : { 1, 7, 480, 4096 })
        {
            SpecRig rig(48000.0, sizeIdx, 0);
            const int n = kSizeList[sizeIdx], hop = n / 4;
            const std::uint64_t frames = (std::uint64_t) n + 5000 + 37;
            rig.feedFrames(0, frames, bs, [](int, std::uint64_t i) { return 0.1f * (float) (i % 7); });
            const std::uint64_t expect = (frames - (std::uint64_t) n) / (std::uint64_t) hop + 1;
            ok &= rig.eng->getSpectrum().getTotalHops(0) == expect;
        }
    check(ok, "hop count = floor((frames-N)/(N/4))+1 for every block size (hops keyed to absolute frame index)");
}

static void test_spectrum_bin_centred_accuracy()
{
    section("spectrum_bin_centred_accuracy");
    bool ok = true, freqOk = true, noInvention = true;
    int combos = 0;
    double worst = 0.0;

    auto run = [&](double fs, int sizeIdx, int win) {
        const int n = kSizeList[sizeIdx];
        const int k = pickBin(fs, n, 2000.0);
        const double f = (double) k * fs / n;
        for (double db : { -0.1, -20.0, -60.0 })
        {
            SpecRig rig(fs, sizeIdx, win);
            rig.feedFrames(0, (std::uint64_t) (2 * n), 4096, sineRat((std::uint64_t) k, (std::uint64_t) n, (float) std::pow(10.0, db / 20.0), 3));
            const auto s = rig.snap();
            const auto pk = curvePeak(s);
            const double err = pk.db - db;
            worst = std::max(worst, std::abs(err));
            const bool lvl = s.hasSpectrum && std::abs(err) <= 0.1;
            const bool fq = octaves(prism::SpectrumEngine::pointFrequency(pk.idx), f) <= 0.0157;
            bool inv = true;
            for (int i = 0; i < prism::kSpectrumPoints; ++i) inv &= s.spectrumDb[(size_t) i] <= (float) db + 0.05f;
            ok &= lvl; freqOk &= fq; noInvention &= inv; ++combos;
            if (! (lvl && fq && inv))
                std::printf("      FAIL detail: fs %.0f N %d %s %.1f dB -> peak %.3f dB at point %d (%.1f Hz, tone %.1f Hz)\n",
                            fs, n, winName(win), db, pk.db, pk.idx, prism::SpectrumEngine::pointFrequency(pk.idx), f);
        }
    };

    for (int sizeIdx = 0; sizeIdx < 5; ++sizeIdx)          // all 5 sizes x 2 windows @ 48 kHz
        for (int win = 0; win < 2; ++win)
            run(48000.0, sizeIdx, win);
    for (double fs : { 44100.0, 96000.0, 192000.0 })       // all rates @ 4096 (48 kHz covered above)
        for (int win = 0; win < 2; ++win)
            run(fs, 2, win);

    checkf(ok, "bin-centred sine at -0.1/-20/-60 dBFS reads true level +-0.1 dB (5 sizes x 2 windows @48k; 4 rates x 2 windows @4096)", (double) combos, worst);
    check(freqOk, "peak point within one log-point (0.0156 oct) of the tone");
    check(noInvention, "no point anywhere exceeds the true level (log map never invents energy)");
}

static void test_spectrum_scalloping()
{
    section("spectrum_scalloping");
    bool hannOk = true, bhOk = true;
    double hannWorst = 0.0, bhWorst = 0.0;
    auto run = [&](double fs, int sizeIdx, int win) {
        const int n = kSizeList[sizeIdx];
        const int k = pickBin(fs, n, 2000.0);
        SpecRig rig(fs, sizeIdx, win);
        rig.feedFrames(0, (std::uint64_t) (2 * n), 4096, sineRat((std::uint64_t) (2 * k + 1), (std::uint64_t) (2 * n), 0.1f, 3));   // (k+0.5) bins
        const double err = curvePeak(rig.snap()).db - (-20.0);
        if (win == 0) { hannWorst = std::min(hannWorst, err); hannOk &= err >= -1.5 && err <= 0.05; }
        else          { bhWorst = std::min(bhWorst, err);     bhOk &= err >= -1.0 && err <= 0.05; }
    };
    for (int sizeIdx = 0; sizeIdx < 5; ++sizeIdx) for (int win = 0; win < 2; ++win) run(48000.0, sizeIdx, win);
    for (double fs : { 44100.0, 96000.0, 192000.0 }) for (int win = 0; win < 2; ++win) run(fs, 2, win);
    checkf(hannOk, "half-bin sine, Hann: scalloping error within -1.5 dB (worst dB)", hannWorst);
    checkf(bhOk, "half-bin sine, Blackman-Harris: scalloping error within -1.0 dB (worst dB)", bhWorst);
}

static void test_spectrum_coherent_gain_equality()
{
    section("spectrum_coherent_gain_equality");
    bool ok = true;
    double worst = 0.0;
    for (int sizeIdx : { 0, 2, 4 })
        for (double fs : { 48000.0, 96000.0 })
        {
            const int n = kSizeList[sizeIdx];
            const int k = pickBin(fs, n, 3000.0);
            float lvl[2] = {};
            for (int win = 0; win < 2; ++win)
            {
                SpecRig rig(fs, sizeIdx, win);
                rig.feedFrames(0, (std::uint64_t) (2 * n), 4096, sineRat((std::uint64_t) k, (std::uint64_t) n, 0.3f, 3));
                lvl[win] = curvePeak(rig.snap()).db;
            }
            worst = std::max(worst, (double) std::abs(lvl[0] - lvl[1]));
            ok &= std::abs(lvl[0] - lvl[1]) <= 0.01f;
        }
    checkf(ok, "Hann and Blackman-Harris read the same level for the same sine (dB diff)", worst);
}

static void test_spectrum_white_noise_density()
{
    section("spectrum_white_noise_density");
    const float sigma = 0.1f;
    struct Case { int sizeIdx, win; };
    double levelHann4096 = 0.0, levelBh4096 = 0.0;
    for (const Case c : { Case { 0, 0 }, Case { 2, 0 }, Case { 2, 1 }, Case { 4, 0 } })
    {
        const int n = kSizeList[c.sizeIdx], hop = n / 4;
        SpecRig rig(48000.0, c.sizeIdx, c.win);
        const std::uint64_t frames = (std::uint64_t) n + 200 * (std::uint64_t) hop;   // 201 overlapped frames
        rig.feedFrames(0, frames, 4096, [&](int ch, std::uint64_t i) { return gauss(ch, i, sigma); });

        std::vector<double> p;
        const bool got = rig.eng->getSpectrum().getMeanBinPower(0, p);
        const int hops = rig.eng->getSpectrum().getAccumulatedHops(0);

        const double S = rig.eng->getSpectrum().getWindowSum(c.sizeIdx, c.win), S2 = rig.eng->getSpectrum().getWindowSumSquares(c.sizeIdx, c.win);
        const double expected = 4.0 * (double) sigma * sigma * S2 / (S * S);   // per-bin power: sigma^2 * sum(w^2) * (2/S)^2
        double sum = 0.0;
        int cnt = 0;
        for (int k = 20; k < n / 2 - 20; ++k) { sum += p[(size_t) k]; ++cnt; }
        const double meas = 10.0 * std::log10(sum / cnt), exp_db = 10.0 * std::log10(expected);

        char name[160];
        std::snprintf(name, sizeof(name), "N=%d %s: mean bin level %.2f dB vs analytical %.2f dB over %d frames (+-0.5)", n, winName(c.win), meas, exp_db, hops);
        check(got && hops >= 200 && std::abs(meas - exp_db) <= 0.5, name);
        if (c.sizeIdx == 2) (c.win == 0 ? levelHann4096 : levelBh4096) = meas;
    }
    // Windows differ only by their ENBW: analytic ENBW ratio BH/Hann = (sum w^2 / S^2) ratio.
    prism::SpectrumEngine se; se.prepare(48000.0);
    const double enbwHann = se.getWindowSumSquares(2, 0) / (se.getWindowSum(2, 0) * se.getWindowSum(2, 0));
    const double enbwBh   = se.getWindowSumSquares(2, 1) / (se.getWindowSum(2, 1) * se.getWindowSum(2, 1));
    const double analyticDiff = 10.0 * std::log10(enbwBh / enbwHann);
    checkf(std::abs((levelBh4096 - levelHann4096) - analyticDiff) <= 0.3, "BH vs Hann noise level differs by the ENBW ratio (dB, analytic)", levelBh4096 - levelHann4096, analyticDiff);
}

static void test_spectrum_impulse()
{
    section("spectrum_impulse");
    bool flat = true, level = true, hasData = true;
    double maxSpread = 0.0, maxErr = 0.0;
    for (int sizeIdx : { 0, 2, 4 })
        for (int win = 0; win < 2; ++win)
            for (int pos : { 2, 4 })   // impulse at N/2 (window peak) and N/4
            {
                const int n = kSizeList[sizeIdx];
                const int n0 = n / pos;
                SpecRig rig(48000.0, sizeIdx, win);
                const float a = 0.5f;
                rig.feedFrames(0, (std::uint64_t) n, 480, [&](int, std::uint64_t i) { return i == (std::uint64_t) n0 ? a : 0.0f; });
                const auto s = rig.snap();
                hasData &= s.hasSpectrum && rig.eng->getSpectrum().getTotalHops(0) == 1;

                const double S = rig.eng->getSpectrum().getWindowSum(sizeIdx, win);
                const double wn0 = rig.eng->getSpectrum().getWindowValue(sizeIdx, win, n0);
                const double expectDb = 20.0 * std::log10(2.0 * a * wn0 / S);   // |X_k| = a * w[n0] for every k
                float lo = 1e9f, hi = -1e9f;
                const double df = 48000.0 / n;
                for (int i = 0; i < prism::kSpectrumPoints; ++i)
                {
                    // Points below one bin spacing interpolate against the DC bin, which carries 1/S (not 2/S)
                    // by definition (a real signal's DC counts once) -> excluded from the flatness criterion.
                    if (prism::SpectrumEngine::pointFrequency(i) < df)
                        continue;
                    lo = std::min(lo, s.spectrumDb[(size_t) i]);
                    hi = std::max(hi, s.spectrumDb[(size_t) i]);
                }
                maxSpread = std::max(maxSpread, (double) (hi - lo));
                maxErr = std::max(maxErr, std::abs((hi + lo) * 0.5 - expectDb));
                flat &= (hi - lo) < 0.02f;
                level &= std::abs((hi + lo) * 0.5 - expectDb) < 0.02;
            }
    check(hasData, "single hop completed exactly when N frames are in");
    checkf(flat, "impulse spectrum is flat across all 640 points (no spurious tones), max spread dB", maxSpread);
    checkf(level, "impulse level = 20log10(2 a w[n0] / S) (windowed-impulse prediction), max error dB", maxErr);
}

static void test_spectrum_multitone_leakage()
{
    section("spectrum_multitone_leakage");
    const double fs = 48000.0;
    const int sizeIdx = 4, n = 16384;
    const double df = fs / n;
    const double toneHz[4] = { 100.0, 1000.0, 5000.0, 10000.0 };
    const double toneDb[4] = { -12.0, -18.0, -24.0, -30.0 };
    int kk[4];
    for (int t = 0; t < 4; ++t) kk[t] = (int) std::lround(toneHz[t] / df);

    for (int win = 0; win < 2; ++win)
    {
        SpecRig rig(fs, sizeIdx, win);
        rig.feedFrames(0, (std::uint64_t) (2 * n), 4096, [&](int, std::uint64_t i) {
            double v = 0.0;
            for (int t = 0; t < 4; ++t)
                v += std::pow(10.0, toneDb[t] / 20.0) * std::sin(2.0 * 3.14159265358979323846 * (double) (((std::uint64_t) kk[t] * i) % (std::uint64_t) n) / n);
            return (float) v;
        });
        const auto s = rig.snap();

        bool levelsOk = true;
        double worst = 0.0;
        for (int t = 0; t < 4; ++t)
        {
            const double f = kk[t] * df;
            float best = -1000.0f;
            for (int i = 0; i < prism::kSpectrumPoints; ++i)
                if (octaves(prism::SpectrumEngine::pointFrequency(i), f) < 0.02)
                    best = std::max(best, s.spectrumDb[(size_t) i]);
            worst = std::max(worst, std::abs(best - toneDb[t]));
            levelsOk &= std::abs(best - toneDb[t]) <= 0.1;
        }
        char name[128];
        std::snprintf(name, sizeof(name), "%s: each tone level within +-0.1 dB (-12/-18/-24/-30), worst %.3f", winName(win), worst);
        check(levelsOk, name);

        const double specDb = win == 0 ? 31.0 : 92.0;   // first-sidelobe spec of the window
        float farMax = -1000.0f;
        for (int i = 0; i < prism::kSpectrumPoints; ++i)
        {
            const double fi = prism::SpectrumEngine::pointFrequency(i);
            bool far = true;
            for (int t = 0; t < 4; ++t) far &= std::abs(fi - kk[t] * df) > 10.0 * df + 0.012 * fi;
            if (far) farMax = std::max(farMax, s.spectrumDb[(size_t) i]);
        }
        std::snprintf(name, sizeof(name), "%s: everything away from the tones <= (-12 dB - %.0f dB sidelobe spec); max %.1f dB", winName(win), specDb, farMax);
        check(farMax <= -12.0 - specDb, name);
    }
}

static void test_spectrum_analysis_modes()
{
    section("spectrum_analysis_modes");
    const double fs = 48000.0;
    const int sizeIdx = 2, n = 4096;
    const int k = pickBin(fs, n, 2000.0);
    const float amp = 0.1f;   // -20 dBFS
    auto peakFor = [&](int mode, int chMask, float sign1, int mainCh = 2) {
        SpecRig rig(fs, sizeIdx, 0, mode, mainCh, 0);
        rig.feedFrames(0, 2 * n, 4096, sineRat((std::uint64_t) k, (std::uint64_t) n, amp, chMask, sign1));
        return rig.snap();
    };

    checkf(std::abs(curvePeak(peakFor(0, 1, 1.0f)).db - (-20.0)) <= 0.1, "L-only sine: leftRight shows -20 dB", curvePeak(peakFor(0, 1, 1.0f)).db);
    checkf(std::abs(curvePeak(peakFor(1, 1, 1.0f)).db - (-23.01)) <= 0.1, "L-only sine: midSide shows -3.01 dB lower (M=(L+R)/sqrt2)", curvePeak(peakFor(1, 1, 1.0f)).db);
    checkf(std::abs(curvePeak(peakFor(2, 1, 1.0f)).db - (-23.01)) <= 0.1, "L-only sine: combined (power mean) shows -3.01 dB lower", curvePeak(peakFor(2, 1, 1.0f)).db);
    checkf(std::abs(curvePeak(peakFor(2, 3, 1.0f)).db - (-20.0)) <= 0.1, "identical L=R: combined reads the true -20 dB", curvePeak(peakFor(2, 3, 1.0f)).db);

    checkf(std::abs(curvePeak(peakFor(2, 3, -1.0f)).db - (-20.0)) <= 0.1, "anti-phase: combined unaffected (no cancellation)", curvePeak(peakFor(2, 3, -1.0f)).db);
    checkf(std::abs(curvePeak(peakFor(0, 3, -1.0f)).db - (-20.0)) <= 0.1, "anti-phase: leftRight shows the Left channel", curvePeak(peakFor(0, 3, -1.0f)).db);
    {
        const auto s = peakFor(1, 3, -1.0f);
        bool allFloor = s.hasSpectrum;
        for (int i = 0; i < prism::kSpectrumPoints; ++i) allFloor &= s.spectrumDb[(size_t) i] == -120.0f;
        check(allFloor, "anti-phase: midSide Mid cancels to exactly -120 dB everywhere");
    }
    check(std::abs(curvePeak(peakFor(0, 1, 1.0f, 1)).db - (-20.0)) <= 0.1 && std::abs(curvePeak(peakFor(1, 1, 1.0f, 1)).db - (-20.0)) <= 0.1
          && std::abs(curvePeak(peakFor(2, 1, 1.0f, 1)).db - (-20.0)) <= 0.1, "mono source: single channel in every mode (no -3 dB)");
}

static void test_spectrum_silence_and_warmup()
{
    section("spectrum_silence_and_warmup");
    bool floorOk = true, warmOk = true, noNan = true;
    for (int sizeIdx = 0; sizeIdx < 5; ++sizeIdx)
    {
        const int n = kSizeList[sizeIdx];
        SpecRig rig(48000.0, sizeIdx, sizeIdx % 2);
        rig.feedFrames(0, (std::uint64_t) (n - 1), 480, [](int, std::uint64_t) { return 0.0f; });
        warmOk &= ! rig.snap().hasSpectrum;
        rig.feedFrames((std::uint64_t) (n - 1), 1, 480, [](int, std::uint64_t) { return 0.0f; });
        const auto s = rig.snap();
        warmOk &= s.hasSpectrum;
        for (int i = 0; i < prism::kSpectrumPoints; ++i)
        {
            floorOk &= s.spectrumDb[(size_t) i] == -120.0f;
            noNan &= std::isfinite(s.spectrumDb[(size_t) i]);
        }
        // Same warm-up for a real signal: unavailable until N frames, never zeros-as-data.
        SpecRig sig2(48000.0, sizeIdx, 0);
        sig2.feedFrames(0, (std::uint64_t) (n - 1), 480, sineRat(5, 64, 0.5f, 3));
        warmOk &= ! sig2.snap().hasSpectrum;
    }
    check(warmOk, "hasSpectrum false until exactly N frames are in (all 5 sizes)");
    check(floorOk && noNan, "digital silence: all 640 points exactly -120.0 dBFS (no -inf/NaN)");

    {   // signal then silence: floor reached once the window is silent
        SpecRig rig(48000.0, 0, 0);
        rig.feedFrames(0, 4096, 480, sineRat(5, 64, 0.5f, 3));
        rig.snap();
        rig.feedFrames(4096, 4096, 480, [](int, std::uint64_t) { return 0.0f; });
        rig.snap();   // consumes the hops that still overlapped the signal
        rig.feedFrames(8192, 4096, 480, [](int, std::uint64_t) { return 0.0f; });
        const auto s = rig.snap();
        bool allFloor = s.hasSpectrum;
        for (int i = 0; i < prism::kSpectrumPoints; ++i) allFloor &= s.spectrumDb[(size_t) i] == -120.0f;
        check(allFloor, "after the signal stops, the spectrum falls to exactly -120 dBFS");
    }
    {   // mono & absent sidechain
        prism::AnalysisEngine eng(makeCfg(1, 0), 14);
        feed(eng, 0, 8192, 480, sineRat(5, 64, 0.5f, 1));
        const auto snap = eng.buildSnapshot();
        check(snap->sources[0].hasSpectrum && ! snap->sources[1].hasSpectrum, "mono source has a spectrum; absent sidechain has none");
        bool zero = true;
        for (float v : snap->sources[1].spectrumDb) zero &= v == 0.0f;
        check(zero, "absent sidechain curve left untouched (hasSpectrum=false is the signal)");
    }
}

static void test_spectrum_block_size_bit_identity()
{
    section("spectrum_block_size_bit_identity");
    struct Cfg { double fs; int size, win, mode; };
    const Cfg cfgs[] = { { 48000.0, 0, 0, 2 }, { 48000.0, 2, 1, 1 }, { 48000.0, 3, 0, 0 }, { 44100.0, 2, 0, 2 }, { 96000.0, 1, 1, 2 } };
    bool same = true, hopsSame = true;
    for (const auto& c : cfgs)
    {
        prism::SourceSnapshot ref;
        std::uint64_t refHops = 0;
        bool first = true;
        for (int bs : { 1, 7, 64, 480, 4096 })
        {
            SpecRig rig(c.fs, c.size, c.win, c.mode);
            rig.feedFrames(0, 30000, bs, [](int ch, std::uint64_t i) {
                return 0.3f * std::sin(0.05f * (float) (i % 20000) + (float) ch) + 0.1f * hnoise(ch, i);
            });
            const auto s = rig.snap();
            const auto hops = rig.eng->getSpectrum().getTotalHops(0);
            if (first) { ref = s; refHops = hops; first = false; continue; }
            same &= s.hasSpectrum == ref.hasSpectrum && std::memcmp(s.spectrumDb.data(), ref.spectrumDb.data(), sizeof(float) * prism::kSpectrumPoints) == 0;
            hopsSame &= hops == refHops;
        }
        same &= ref.hasSpectrum;
    }
    check(hopsSame, "same number of hops for block sizes {1,7,64,480,4096}");
    check(same, "published spectra bit-identical for block sizes {1,7,64,480,4096} (5 size/window/mode/rate configs)");
}

static void test_spectrum_parameter_switching()
{
    section("spectrum_parameter_switching");
    const double fs = 48000.0;
    const int k1024 = pickBin(fs, 1024, 2000.0);   // bin-centred for every FFT size (k_N = k1024 * N/1024)
    auto tone = sineRat((std::uint64_t) k1024, 1024, 0.1f, 3);   // -20 dBFS

    SpecRig rig(fs, 0, 0);
    rig.feedFrames(0, 3000, 480, tone);
    auto s = rig.snap();
    check(s.hasSpectrum && std::abs(curvePeak(s).db + 20.0) <= 0.1, "N=1024 reads -20 dB");

    rig.size.store(2.0f);   // -> 4096
    rig.feedFrames(3000, 1000, 480, tone);   // 4000 frames: not enough for N=4096
    s = rig.snap();
    check(! s.hasSpectrum, "after growing N, hasSpectrum is false until N' frames are in (no stale curve)");
    rig.feedFrames(4000, 200, 100, tone);
    s = rig.snap();
    check(s.hasSpectrum && std::abs(curvePeak(s).db + 20.0) <= 0.1, "N=4096 valid at the next hop; level correct");
    check(rig.eng->getSpectrum().getActiveParams(0).sizeIndex == 2, "active size switched");

    rig.win.store(1.0f);   // -> Blackman-Harris
    rig.feedFrames(4200, 1000, 100, tone);   // next hop boundary (multiple of N/4) lies ahead; 1000 frames cover it
    s = rig.snap();
    check(s.hasSpectrum && std::abs(curvePeak(s).db + 20.0) <= 0.1 && rig.eng->getSpectrum().getActiveParams(0).windowIndex == 1,
          "window switch applied at the next hop; level unchanged (coherent gain)");

    // Rapid random switching: never crashes, curves always finite and in range.
    std::mt19937 rng(99);
    bool fine = true;
    std::uint64_t pos = 5200;
    for (int it = 0; it < 300; ++it)
    {
        rig.size.store((float) (rng() % 5));
        rig.win.store((float) (rng() % 2));
        rig.mode.store((float) (rng() % 3));
        rig.feedFrames(pos, 480, 480, tone);
        pos += 480;
        const auto sn = rig.snap();
        if (sn.hasSpectrum)
            for (int i = 0; i < prism::kSpectrumPoints; ++i) fine &= std::isfinite(sn.spectrumDb[(size_t) i]) && sn.spectrumDb[(size_t) i] >= -120.0f && sn.spectrumDb[(size_t) i] <= 0.5f;
    }
    check(fine, "300 random fft_size/window/analysis_mode switches: curves always finite and in [-120, 0.5]");

    rig.size.store(4.0f); rig.win.store(0.0f); rig.mode.store(2.0f);
    rig.feedFrames(pos, 40000, 4096, tone);
    s = rig.snap();
    check(s.hasSpectrum && std::abs(curvePeak(s).db + 20.0) <= 0.1, "after the storm, spectrum is correct for the final settings");
}

static void test_spectrum_enable_gating_and_processor_wiring()
{
    section("spectrum_enable_gating_and_processor_wiring");
    {
        SpecRig rig(48000.0, 2, 0);
        const int k = pickBin(48000.0, 4096, 2000.0);
        auto tone = sineRat((std::uint64_t) k, 4096, 0.1f, 3);
        rig.eng->setSpectrumEnabled(false);
        rig.feedFrames(0, 10000, 480, tone);
        check(! rig.snap().hasSpectrum && rig.eng->getSpectrum().getTotalHops(0) == 0, "editor inactive: no FFT hops, no spectrum");
        rig.eng->setSpectrumEnabled(true);
        rig.feedFrames(10000, 5000, 480, tone);
        auto s = rig.snap();
        check(s.hasSpectrum && rig.eng->getSpectrum().getTotalHops(0) > 0 && std::abs(curvePeak(s).db + 20.0) <= 0.1, "re-enabled: spectrum resumes with correct level");

        rig.eng->setSpectrumEnabled(false);
        rig.feedFrames(15000, 100000, 4096, tone);   // longer than the 65536-frame history
        rig.eng->setSpectrumEnabled(true);
        rig.feedFrames(115000, 600, 600, tone);
        s = rig.snap();
        check(s.hasSpectrum && std::abs(curvePeak(s).db + 20.0) <= 0.1, "re-enable after a long gap skips to recent history (no crash, correct level)");
    }

    {   // The processor wires fft_size / window / analysis_mode APVTS atomics to the engine.
        PRISMAudioProcessor proc;
        check(configure(proc, 2, 0, 48000.0, 1100), "configure");
        proc.prepareToPlay(48000.0, 1100);
        juce::AudioBuffer<float> buf(2, 1100);
        juce::MidiBuffer midi;
        for (int i = 0; i < 1100; ++i) for (int c = 0; c < 2; ++c) buf.setSample(c, i, 0.1f * std::sin(0.2f * (float) i));
        proc.processBlock(buf, midi);
        proc.releaseResources();
        auto* eng = proc.getAnalysisEngine();
        eng->setSpectrumEnabled(true);
        eng->pump();
        check(! eng->buildSnapshot()->sources[0].hasSpectrum, "default fft_size 4096: 1100 frames is not enough");

        auto* fftPrm = proc.apvts.getParameter(prism::ids::fftSize);
        fftPrm->setValueNotifyingHost(fftPrm->convertTo0to1(0.0f));   // 1024
        eng->pump();
        check(eng->buildSnapshot()->sources[0].hasSpectrum && eng->getSpectrum().getActiveParams(0).sizeIndex == 0, "fft_size=1024 reaches the engine (spectrum valid with 1100 frames)");

        auto* winPrm = proc.apvts.getParameter(prism::ids::window);
        winPrm->setValueNotifyingHost(winPrm->convertTo0to1(1.0f));
        auto* modePrm = proc.apvts.getParameter(prism::ids::analysisMode);
        modePrm->setValueNotifyingHost(modePrm->convertTo0to1(0.0f));
        eng->pump();
        const auto ap = eng->getSpectrum().getActiveParams(0);
        check(ap.windowIndex == 1 && ap.mode == 0, "window=blackmanHarris and analysis_mode=leftRight reach the engine");
    }
}

static void test_spectrum_cpu_budget()
{
    section("spectrum_cpu_budget");
    struct Case { int sizeIdx; const char* label; };
    for (const Case c : { Case { 4, "N=16384" }, Case { 0, "N=1024" } })
    {
        SpecRig rig(192000.0, c.sizeIdx, 0, 2, 2, 2);   // 2 stereo sources, combined (2 FFTs / hop / source)
        std::vector<std::vector<float>> bufs(4, std::vector<float>(4096));
        for (int ch = 0; ch < 4; ++ch) for (int i = 0; i < 4096; ++i) bufs[(size_t) ch][(size_t) i] = 0.2f * hnoise(ch, (std::uint64_t) i);
        const float* ptrs[4] = { bufs[0].data(), bufs[1].data(), bufs[2].data(), bufs[3].data() };

        const double audioSeconds = 10.0;
        const int blocks = (int) (audioSeconds * 192000.0 / 4096.0);
        const auto t0 = Clock::now();
        for (int b = 0; b < blocks; ++b) { rig.eng->captureBlock(ptrs, 4, 4096); rig.eng->pump(); }
        const double wall = msSince(t0) / 1000.0;
        const double pct = 100.0 * wall / (blocks * 4096.0 / 192000.0);

        std::printf("      %s @192 kHz, 2 stereo sources, combined: %.2f %% of one core (capture + pump + FFT), %llu hops/source\n",
                    c.label, pct, (unsigned long long) rig.eng->getSpectrum().getTotalHops(0));
        if (pct > 5.0)
            std::printf("      WARN: above the 5%% budget (debug build or loaded machine?)\n");
        char name[96];
        std::snprintf(name, sizeof(name), "%s @192k keeps up with real time (%.2f %% core; budget 5 %% is WARN-only)", c.label, pct);
        check(pct < 100.0, name);
    }
}

//==============================================================================
// PHASE 2.4 - loudness (BS.1770-5 / EBU R128 / TECH 3341 / TECH 3342)
namespace
{
    constexpr double kTwoPi = 6.283185307179586476925286766559;

    bool nearf(double a, double b, double tol) { return std::isfinite(a) && std::isfinite(b) && std::abs(a - b) <= tol; }

    // Engine driven by a synthetic 997 Hz source, spectrum off (loudness is always-on work).
    struct LoudRig
    {
        double fs;
        int mainCh, scCh;
        std::atomic<std::uint32_t> resetAll { 0 }, resetInt { 0 };
        std::unique_ptr<prism::AnalysisEngine> eng;
        std::uint64_t pos = 0;
        std::vector<std::vector<float>> bufs;

        LoudRig(double sr, int mainChannels = 2, int sidechain = 0, int ringLog2 = 14)
            : fs(sr), mainCh(mainChannels), scCh(sidechain)
        {
            eng = std::make_unique<prism::AnalysisEngine>(makeCfg(mainCh, scCh, fs), ringLog2);
            eng->setSpectrumEnabled(false);
            eng->attachControls({ &resetAll, &resetInt, nullptr });
            bufs.assign((size_t) (mainCh + scCh), std::vector<float>(2048));
        }

        // Peak amplitude of a 997 Hz sine that reads `lufs` when nActive channels (weight 1 each) carry it.
        //   LUFS = -0.691 + 10 log10( nActive * A^2 * g^2 / 2 )   =>   A = sqrt( 2 * 10^((LUFS+0.691)/10) / (nActive * g^2) )
        double ampForLufs(double lufs, int nActive) const
        {
            if (lufs <= -200.0) return 0.0;
            const double g = std::pow(10.0, prism::KWeighting::gainDb(fs, 997.0) / 20.0);
            return std::sqrt(2.0 * std::pow(10.0, (lufs + 0.691) / 10.0) / ((double) nActive * g * g));
        }

        // amps[c] = peak amplitude for host-buffer channel c (0 = silent).
        void toneAmps(double seconds, double hz, const std::vector<double>& amps, int block = 2048)
        {
            const std::uint64_t frames = (std::uint64_t) std::llround(seconds * fs);
            const int chs = mainCh + scCh;
            for (auto& b : bufs) if ((int) b.size() < block) b.resize((size_t) block);
            std::vector<const float*> ptrs((size_t) chs);
            for (int c = 0; c < chs; ++c) ptrs[(size_t) c] = bufs[(size_t) c].data();

            const double w = kTwoPi * hz / fs;
            for (std::uint64_t done = 0; done < frames;)
            {
                const int n = (int) std::min<std::uint64_t>((std::uint64_t) block, frames - done);
                for (int i = 0; i < n; ++i)
                {
                    const double s = std::sin(w * (double) (pos + (std::uint64_t) i));
                    for (int c = 0; c < chs; ++c)
                        bufs[(size_t) c][(size_t) i] = (c < (int) amps.size() && amps[(size_t) c] != 0.0) ? (float) (amps[(size_t) c] * s) : 0.0f;
                }
                eng->captureBlock(ptrs.data(), chs, n);
                eng->pump();
                pos += (std::uint64_t) n;
                done += (std::uint64_t) n;
            }
        }

        void lufs(double seconds, double l, int nActive = 2, int block = 2048)
        {
            const double a = ampForLufs(l, nActive);
            std::vector<double> amps((size_t) (mainCh + scCh), 0.0);
            for (int c = 0; c < std::min(nActive, mainCh); ++c) amps[(size_t) c] = a;
            toneAmps(seconds, 997.0, amps, block);
        }
        void silence(double seconds) { toneAmps(seconds, 997.0, {}, 2048); }

        std::shared_ptr<const prism::AnalysisSnapshot> snap() { return eng->buildSnapshot(); }
    };

    //--------------------------------------------------------------------------
    // Independent reference model for piecewise-constant programmes (no filters, no histograms):
    // block / window energies are the time-weighted average of the segment powers.
    struct RefSeg { double seconds; double lufs; };   // lufs <= -200 -> digital silence

    double refPower(double lufs) { return lufs <= -200.0 ? 0.0 : std::pow(10.0, (lufs + 0.691) / 10.0); }

    double refAvgPower(const std::vector<RefSeg>& segs, double t0, double t1)
    {
        double e = 0.0, t = 0.0;
        for (const auto& s : segs)
        {
            const double s0 = t, s1 = t + s.seconds;
            e += std::max(0.0, std::min(t1, s1) - std::max(t0, s0)) * refPower(s.lufs);
            t = s1;
        }
        return e / (t1 - t0);
    }

    double refTotal(const std::vector<RefSeg>& segs) { double t = 0; for (auto& s : segs) t += s.seconds; return t; }

    std::vector<double> refWindows(const std::vector<RefSeg>& segs, double len)
    {
        std::vector<double> z;
        const double total = refTotal(segs);
        for (int k = 0; 0.1 * k + len <= total + 1e-9; ++k)
            z.push_back(refAvgPower(segs, 0.1 * k, 0.1 * k + len));
        return z;
    }

    double lufsOf(double z) { return -0.691 + 10.0 * std::log10(z); }

    double refIntegratedFrom(const std::vector<double>& blocks)
    {
        std::vector<double> kept;
        for (double z : blocks) if (z > 0.0 && lufsOf(z) > -70.0) kept.push_back(z);
        if (kept.empty()) return NAN;
        double m = 0; for (double z : kept) m += z; m /= (double) kept.size();
        const double gate = lufsOf(m) - 10.0;
        double s = 0; int n = 0;
        for (double z : kept) if (lufsOf(z) > gate) { s += z; ++n; }
        return n ? lufsOf(s / n) : NAN;
    }

    double refLraFrom(const std::vector<double>& shortTerm)
    {
        std::vector<double> kept;
        for (double z : shortTerm) if (z > 0.0 && lufsOf(z) > -70.0) kept.push_back(z);
        if (kept.empty()) return NAN;
        double m = 0; for (double z : kept) m += z; m /= (double) kept.size();
        const double gate = lufsOf(m) - 20.0;
        std::vector<double> l;
        for (double z : kept) if (lufsOf(z) > gate) l.push_back(lufsOf(z));
        if (l.empty()) return NAN;
        std::sort(l.begin(), l.end());
        const auto rank = [&](double p) { return (size_t) std::floor(p * (double) (l.size() - 1) + 0.5); };
        return l[rank(0.95)] - l[rank(0.10)];
    }

    void playSegs(LoudRig& rig, const std::vector<RefSeg>& segs, int nActive = 2)
    {
        for (const auto& s : segs) rig.lufs(s.seconds, s.lufs, nActive);
    }
}

static void test_loudness_kweighting()
{
    section("loudness_kweighting");

    // BS.1770 48 kHz coefficient table (libebur128 convention).
    const auto s1 = prism::KWeighting::stage1(48000.0), s2 = prism::KWeighting::stage2(48000.0);
    const bool shelfOk = std::abs(s1.b0 - 1.53512485958697) < 1e-9 && std::abs(s1.b1 - (-2.69169618940638)) < 1e-9
                      && std::abs(s1.b2 - 1.19839281085285) < 1e-9 && std::abs(s1.a1 - (-1.69065929318241)) < 1e-9
                      && std::abs(s1.a2 - 0.73248077421585) < 1e-9;
    const bool rlbOk = s2.b0 == 1.0 && s2.b1 == -2.0 && s2.b2 == 1.0 && std::abs(s2.a1 - (-1.99004745483398)) < 1e-9
                    && std::abs(s2.a2 - 0.99007225036621) < 1e-9;
    checkf(shelfOk, "48 kHz stage 1 (shelf) coefficients match the BS.1770 table to 1e-9", s1.b0 - 1.53512485958697, s1.a2 - 0.73248077421585);
    checkf(rlbOk, "48 kHz stage 2 (RLB) coefficients match the BS.1770 table to 1e-9", s2.a1 - (-1.99004745483398), s2.a2 - 0.99007225036621);

    const double g48 = prism::KWeighting::gainDb(48000.0, 997.0);
    checkf(std::abs(g48 - 0.691) < 0.02, "K-weighting gain at 997 Hz is ~ +0.691 dB (the -0.691 LUFS offset cancels it)", g48);
    bool ok = true;
    double worst = 0.0;
    for (double fs : { 44100.0, 88200.0, 96000.0, 176400.0, 192000.0 })
    {
        const double d = prism::KWeighting::gainDb(fs, 997.0) - g48;
        worst = std::max(worst, std::abs(d));
        // Bound is the measured bilinear-warp spread (worst 0.027 dB at 44.1 kHz),
        // not a standards limit: BS.1770 fixes the 48 kHz table and libebur128-style
        // analytic redesign at other rates inherently warps a few hundredths of a dB.
        // The normative +-0.1 LU EBU tolerance is asserted by the case-1 tests per rate.
        ok &= std::abs(d) <= 0.05;
    }
    checkf(ok, "997 Hz response at 44.1/88.2/96/176.4/192 kHz within 0.05 dB of the 48 kHz response (worst dB)", worst);

    // RLB high-pass: -3 dB region near 60 Hz region is steeply attenuated at 20 Hz, flat above 500 Hz.
    check(prism::KWeighting::gainDb(48000.0, 20.0) < -2.0 && std::abs(prism::KWeighting::gainDb(48000.0, 500.0) - prism::KWeighting::gainDb(48000.0, 400.0)) < 0.5,
          "RLB high-pass attenuates 20 Hz, shelf region is flat-ish at 400-500 Hz");

    bool chunks = true;
    const std::pair<double, int> chunkTable[] = { { 44100.0, 441 }, { 48000.0, 480 }, { 88200.0, 882 }, { 96000.0, 960 }, { 176400.0, 1764 }, { 192000.0, 1920 } };
    for (const auto& entry : chunkTable)
    {
        prism::LoudnessMeter m;
        m.prepare(entry.first, 2);
        chunks &= m.getChunkLength() == entry.second;
    }
    check(chunks, "10 ms chunk = round(0.01 fs): 441/480/882/960/1764/1920");
}

static void test_loudness_tech3341_case1_and_mono()
{
    section("loudness_tech3341_case1_and_mono");

    // TECH 3341 case 1: 997 Hz sine, -23.0 dBFS peak in BOTH channels, reads -23.0 LUFS (+-0.1) for M, S and I.
    // (K gain at 997 Hz = +0.691 dB cancels the -0.691 LUFS offset: LUFS = 10log10(A^2) = -23.0.)
    for (double fs : { 44100.0, 48000.0, 96000.0, 192000.0 })
    {
        LoudRig rig(fs);
        rig.toneAmps(20.0, 997.0, { std::pow(10.0, -23.0 / 20.0), std::pow(10.0, -23.0 / 20.0) });
        const auto s = rig.snap();
        char name[160];
        std::snprintf(name, sizeof(name), "fs %.0f: case 1 reads M %.3f / S %.3f / I %.3f LUFS (expect -23.0 +-0.1), LRA %.3f", fs, s->momentaryLufs, s->shortTermLufs, s->integratedLufs, s->loudnessRange);
        check(nearf(s->momentaryLufs, -23.0, 0.1) && nearf(s->shortTermLufs, -23.0, 0.1) && nearf(s->integratedLufs, -23.0, 0.1)
              && s->loudnessRange < 0.05f, name);
    }

    // Channel weights L = R = 1.0: the same sine on ONE channel reads 3.01 dB lower; a mono bus equals that.
    for (double fs : { 48000.0, 96000.0 })
    {
        LoudRig both(fs), left(fs), mono(fs, 1, 0);
        const double a = std::pow(10.0, -23.0 / 20.0);
        both.toneAmps(12.0, 997.0, { a, a });
        left.toneAmps(12.0, 997.0, { a, 0.0 });
        mono.toneAmps(12.0, 997.0, { a });
        const auto sb = both.snap(), sl = left.snap(), sm = mono.snap();
        char name[160];
        std::snprintf(name, sizeof(name), "fs %.0f: one channel reads 3.01 dB lower (M %.3f, S %.3f, I %.3f dB)", fs,
                      sb->momentaryLufs - sl->momentaryLufs, sb->shortTermLufs - sl->shortTermLufs, sb->integratedLufs - sl->integratedLufs);
        check(nearf(sb->momentaryLufs - sl->momentaryLufs, 3.0103, 0.02) && nearf(sb->shortTermLufs - sl->shortTermLufs, 3.0103, 0.02)
              && nearf(sb->integratedLufs - sl->integratedLufs, 3.0103, 0.02), name);
        check(sm->momentaryLufs == sl->momentaryLufs && sm->integratedLufs == sl->integratedLufs && sm->shortTermLufs == sl->shortTermLufs,
              "mono bus (single channel, weight 1) equals the same signal on the left channel only");
    }
}

static void test_loudness_availability()
{
    section("loudness_availability");
    LoudRig rig(48000.0);
    rig.lufs(0.39, -23.0);   // 39 chunks
    auto s = rig.snap();
    check(std::isnan(s->momentaryLufs) && std::isnan(s->shortTermLufs) && std::isnan(s->integratedLufs) && std::isnan(s->loudnessRange),
          "before 400 ms: M, S, I, LRA are all NaN (never 0)");
    rig.lufs(0.02, -23.0);   // 0.41 s
    s = rig.snap();
    check(nearf(s->momentaryLufs, -23.0, 0.1) && nearf(s->integratedLufs, -23.0, 0.1) && std::isnan(s->shortTermLufs) && std::isnan(s->loudnessRange),
          "at 0.41 s: momentary and integrated (first gated block) available; short-term and LRA still NaN");
    rig.lufs(2.58, -23.0);   // 2.99 s
    s = rig.snap();
    check(std::isnan(s->shortTermLufs) && std::isnan(s->loudnessRange), "at 2.99 s: short-term and LRA still NaN");
    rig.lufs(0.02, -23.0);   // 3.01 s
    s = rig.snap();
    check(nearf(s->shortTermLufs, -23.0, 0.1) && std::isfinite(s->loudnessRange), "at 3.01 s: short-term and LRA available");

    LoudRig quiet(48000.0);
    quiet.silence(5.0);
    s = quiet.snap();
    check(std::isnan(s->momentaryLufs) && std::isnan(s->shortTermLufs) && std::isnan(s->integratedLufs) && std::isnan(s->loudnessRange),
          "digital silence: everything NaN (dashes), not 0 and not -inf");

    LoudRig below(48000.0);
    below.lufs(10.0, -80.0);   // below the -70 LUFS absolute gate
    s = below.snap();
    check(nearf(s->momentaryLufs, -80.0, 0.1) && std::isnan(s->integratedLufs) && std::isnan(s->loudnessRange),
          "signal at -80 LUFS: M reads it, I and LRA stay NaN (absolute gate rejects every block)");
}

static void test_loudness_gating_absolute()
{
    section("loudness_gating_absolute");

    // HAND CALCULATION. Stereo 997 Hz, T1 seconds at -80 LUFS then T2 seconds at -23 LUFS.
    //  * -80 LUFS blocks are below the -70 LUFS absolute gate -> rejected, they never enter the statistics.
    //  * Blocks are 400 ms long, one every 100 ms. A block fully inside the -23 part has energy E23.
    //    The -23 part of length T2 holds n = (T2-0.4)/0.1 + 1 such blocks (T2 = 30 s -> 297, 20 s -> 197).
    //  * Three blocks straddle the step (they overlap the loud part by 100/200/300 ms): energy f*E23 + (1-f)*E80
    //    with f = 1/4, 1/2, 3/4 (E80 is 5e-6 of E23 and ignored) -> loudness -23-6.0, -23-3.0, -23-1.2 LUFS.
    //    Relative gate = mean energy - 10 LU ~ -33 LUFS, so all three are KEPT.
    //  * Integrated = -23 + 10 log10( (n + 1.5) / (n + 3) )       [1.5 = 1/4+1/2+3/4]
    //      30 s: 10 log10(298.5/300) = -0.0218 dB -> -23.0218 LUFS
    //      20 s: 10 log10(198.5/200) = -0.0327 dB -> -23.0327 LUFS
    struct Case { double fs, t1, t2, expect; };
    for (const Case c : { Case { 48000.0, 30.0, 30.0, -23.0218 }, Case { 44100.0, 20.0, 20.0, -23.0327 }, Case { 96000.0, 20.0, 20.0, -23.0327 } })
    {
        LoudRig rig(c.fs);
        const std::vector<RefSeg> segs = { { c.t1, -80.0 }, { c.t2, -23.0 } };
        playSegs(rig, segs);
        const auto s = rig.snap();
        const double ref = refIntegratedFrom(refWindows(segs, 0.4));
        char name[200];
        std::snprintf(name, sizeof(name), "fs %.0f, %.0f s @-80 + %.0f s @-23: I = %.4f (hand calc %.4f, reference model %.4f)", c.fs, c.t1, c.t2, s->integratedLufs, c.expect, ref);
        check(nearf(s->integratedLufs, c.expect, 0.05) && nearf(s->integratedLufs, ref, 0.03), name);
    }
}

static void test_loudness_gating_relative()
{
    section("loudness_gating_relative");

    // HAND CALCULATION A (loud / quiet / loud, quiet = loud - 20 LU).
    //  Segments: L s at -20, Q s at -40, L s at -20 (L = 20 s). Blocks fully inside one 20 s segment: n = 197.
    //  Mean energy of the abs-gated blocks ~ (E20*(2*20) + E40*20)/60 = 0.6733 E20 -> -1.72 dB
    //  -> relative gate = -20 - 1.72 - 10 = -31.7 LUFS: the -40 LUFS blocks are REJECTED (without the relative
    //  gate the result would be about -21.7).
    //  Straddling blocks: 3 per step, 2 steps: f = 1/4,1/2,3/4 each time -> loudness -26,-23,-21.2 > gate, KEPT.
    //  Integrated = -20 + 10 log10( (2*197 + 2*1.5) / (2*197 + 6) ) = -20 + 10 log10(397/400) = -20.0327 LUFS.
    {
        LoudRig rig(48000.0);
        const std::vector<RefSeg> segs = { { 20.0, -20.0 }, { 20.0, -40.0 }, { 20.0, -20.0 } };
        playSegs(rig, segs);
        const auto s = rig.snap();
        const double ref = refIntegratedFrom(refWindows(segs, 0.4));
        char name[200];
        std::snprintf(name, sizeof(name), "20/-20, 20/-40, 20/-20 @48k: I = %.4f (hand calc -20.0327, reference %.4f); ungated would be ~ -21.7", s->integratedLufs, ref);
        check(nearf(s->integratedLufs, -20.0327, 0.05) && nearf(s->integratedLufs, ref, 0.03) && s->integratedLufs > -20.3f, name);
    }
    // Same shape at 10 s segments, 96 kHz: n = 97 per segment -> I = -20 + 10 log10((194+3)/(194+6)) = -20.0654
    {
        LoudRig rig(96000.0);
        const std::vector<RefSeg> segs = { { 10.0, -20.0 }, { 10.0, -40.0 }, { 10.0, -20.0 } };
        playSegs(rig, segs);
        const auto s = rig.snap();
        char name[160];
        std::snprintf(name, sizeof(name), "10/-20, 10/-40, 10/-20 @96k: I = %.4f (hand calc -20.0654)", s->integratedLufs);
        check(nearf(s->integratedLufs, -20.0654, 0.05), name);
    }
    // TECH 3341 case-3 shape: 10 s @-36, 60 s @-23, 10 s @-36 -> -23.0 (+-0.1).
    //  Mean energy = (60 E23 + 20 E36)/80 = 0.7513 E23 -> -1.24 dB; gate = -23 -1.24 -10 = -34.24 > -36: the -36 blocks are rejected.
    //  Loud blocks n = 597; 6 straddlers (each step: f=1/4,1/2,3/4 plus (1-f)*E36 leak 0.0501) add 2*(1.5+0.075) = 3.15.
    //  I = -23 + 10 log10((597+3.15)/603) = -23.0206 LUFS.
    {
        LoudRig rig(48000.0);
        const std::vector<RefSeg> segs = { { 10.0, -36.0 }, { 60.0, -23.0 }, { 10.0, -36.0 } };
        playSegs(rig, segs);
        const auto s = rig.snap();
        char name[160];
        std::snprintf(name, sizeof(name), "TECH 3341 case-3 shape (10 s @-36, 60 s @-23, 10 s @-36): I = %.4f (hand calc -23.0206, spec -23.0 +-0.1)", s->integratedLufs);
        check(nearf(s->integratedLufs, -23.0206, 0.05) && nearf(s->integratedLufs, -23.0, 0.1), name);
    }
}

static void test_loudness_range()
{
    section("loudness_range");
    struct Case { const char* label; std::vector<RefSeg> segs; double expect; };
    // TECH 3342-style programmes (997 Hz stands in for pink noise; expected LRA is the level span):
    //  A) 20 s @-20, 20 s @-30 : P10 sits in the -30 plateau, P95 in the -20 plateau -> 10 LU
    //  B) 20 s @-20, 20 s @-15 : 5 LU
    //  C) -50 / -35 / -20 / -35 / -50, 20 s each: the relative gate (-20 LU below the mean, ~ -46.7 LUFS) removes
    //     the -50 plateaus; the remaining distribution spans -35..-20 -> 15 LU
    const Case cases[] = {
        { "A: -20/-30", { { 20, -20 }, { 20, -30 } }, 10.0 },
        { "B: -20/-15", { { 20, -20 }, { 20, -15 } }, 5.0 },
        { "C: -50/-35/-20/-35/-50", { { 20, -50 }, { 20, -35 }, { 20, -20 }, { 20, -35 }, { 20, -50 } }, 15.0 },
    };
    for (const auto& c : cases)
    {
        LoudRig rig(48000.0);
        playSegs(rig, c.segs);
        const auto s = rig.snap();
        const double ref = refLraFrom(refWindows(c.segs, 3.0));
        char name[200];
        std::snprintf(name, sizeof(name), "LRA %s: %.3f LU (expect %.1f +-0.2 [TECH 3342 allows +-1], reference model %.3f)", c.label, s->loudnessRange, c.expect, ref);
        check(nearf(s->loudnessRange, c.expect, 0.2) && nearf(s->loudnessRange, ref, 0.06), name);
    }

    LoudRig steady(44100.0);
    steady.lufs(15.0, -23.0);
    check(steady.snap()->loudnessRange < 0.05f, "steady tone: LRA ~ 0 (single-bin distribution)");

    LoudRig lra44(44100.0);
    const std::vector<RefSeg> segs = { { 15, -25 }, { 15, -35 } };
    playSegs(lra44, segs);
    check(nearf(lra44.snap()->loudnessRange, 10.0, 0.2), "LRA 10 LU also at 44.1 kHz");
}

static void test_loudness_block_size_independence()
{
    section("loudness_block_size_independence");
    bool same = true;
    float ref[4] = {};
    bool first = true;
    for (int bs : { 1, 7, 64, 480, 4096 })
    {
        LoudRig rig(48000.0, 2, 0, 16);
        const std::vector<RefSeg> segs = { { 2.0, -30.0 }, { 2.0, -20.0 }, { 2.0, -33.0 } };
        for (const auto& s : segs) rig.lufs(s.seconds, s.lufs, 2, bs);
        const auto s = rig.snap();
        const float v[4] = { s->momentaryLufs, s->shortTermLufs, s->integratedLufs, s->loudnessRange };
        if (first) { std::memcpy(ref, v, sizeof(v)); first = false; continue; }
        same &= std::memcmp(ref, v, sizeof(v)) == 0;
    }
    check(same && std::isfinite(ref[2]), "M / S / I / LRA bit-identical for block sizes {1,7,64,480,4096}");
}

static void test_loudness_reset_semantics()
{
    section("loudness_reset_semantics");
    LoudRig rig(48000.0);
    rig.lufs(8.0, -23.0);
    auto s = rig.snap();
    check(std::isfinite(s->integratedLufs) && std::isfinite(s->loudnessRange) && std::isfinite(s->sources[0].samplePeakMaxDb), "primed: I, LRA, sample peak available");
    const float m0 = s->momentaryLufs, st0 = s->shortTermLufs, sp0 = s->sources[0].samplePeakMaxDb;
    const float tp0 = s->sources[0].truePeakMaxDb;
    check(std::isfinite(tp0) && tp0 >= sp0, "primed: true peak available and >= sample peak");

    rig.resetInt.fetch_add(1);
    rig.lufs(0.1, -23.0);
    s = rig.snap();
    check(std::isnan(s->integratedLufs) && std::isnan(s->loudnessRange), "reset_integrated: I and LRA are NaN until new data exists (never 0)");
    check(nearf(s->momentaryLufs, m0, 0.01) && nearf(s->shortTermLufs, st0, 0.01), "reset_integrated: momentary and short-term windows unaffected");
    check(nearf(s->sources[0].samplePeakMaxDb, sp0, 0.001), "reset_integrated: sample peak NOT cleared");
    check(nearf(s->sources[0].truePeakMaxDb, tp0, 0.001), "reset_integrated: true peak NOT cleared");

    rig.lufs(0.5, -23.0);
    s = rig.snap();
    check(nearf(s->integratedLufs, -23.0, 0.1) && std::isnan(s->loudnessRange), "0.6 s after reset: I is back (first gated block), LRA waits for a full 3 s window");
    rig.lufs(2.6, -23.0);
    check(std::isfinite(rig.snap()->loudnessRange), "3.2 s after reset: LRA available again");

    rig.resetAll.fetch_add(1);
    rig.silence(0.05);
    s = rig.snap();
    check(std::isnan(s->integratedLufs) && std::isnan(s->loudnessRange), "reset_all clears I and LRA");
    check(std::isinf(s->sources[0].samplePeakMaxDb) && s->sources[0].samplePeakMaxDb < 0, "reset_all also clears the sample-peak maximum");
    check(std::isinf(s->sources[0].truePeakMaxDb) && s->sources[0].truePeakMaxDb < 0, "reset_all also clears the true-peak maximum (-INF -> dashes)");
    check(std::isfinite(s->momentaryLufs) && std::isfinite(s->shortTermLufs), "reset_all leaves the moving M / S windows alone");
}

static void test_loudness_gap_handling()
{
    section("loudness_gap_handling");
    LoudRig rig(48000.0, 2, 0, 12);   // 4096-frame ring
    const int block = 2048;
    std::vector<float> l(block), r(block);
    const float* ptrs[2] = { l.data(), r.data() };
    const double a = std::pow(10.0, -23.0 / 20.0), w = kTwoPi * 997.0 / 48000.0;
    std::uint64_t pos = 0;
    auto fillBlock = [&] {
        for (int i = 0; i < block; ++i) l[(size_t) i] = r[(size_t) i] = (float) (a * std::sin(w * (double) (pos + (std::uint64_t) i)));
        pos += (std::uint64_t) block;
    };

    int expectedGaps = 0;
    for (int sec = 0; sec < 12; ++sec)
    {
        for (int b = 0; b < 23; ++b) { fillBlock(); rig.eng->captureBlock(ptrs, 2, block); rig.eng->pump(); }   // ~1 s, drained every block
        // Overrun: three blocks without draining -> the third (2048 frames) is dropped.
        for (int b = 0; b < 3; ++b) { fillBlock(); rig.eng->captureBlock(ptrs, 2, block); }
        ++expectedGaps;
        rig.eng->pump();
    }
    // A gap sitting exactly at the end of a drained chunk is applied when the next chunk arrives: flush.
    fillBlock(); rig.eng->captureBlock(ptrs, 2, block); rig.eng->pump();
    const auto s = rig.snap();
    check(rig.eng->getOverrunCount(0) == (std::uint64_t) expectedGaps, "forced overruns happened");
    check(rig.eng->getLoudness(0).getGapCount() == (std::uint64_t) expectedGaps, "every discontinuity reached the loudness meter (filter state reset, in-flight block dropped)");
    check(nearf(s->integratedLufs, -23.0, 0.2) && nearf(s->momentaryLufs, -23.0, 0.3) && nearf(s->shortTermLufs, -23.0, 0.3),
          "levels stay correct across the gaps (no glitch, no NaN)");
}

static void test_loudness_sources_and_publish()
{
    section("loudness_sources_and_publish");
    LoudRig rig(48000.0, 2, 2);
    const double aMain = rig.ampForLufs(-23.0, 2), aSc = rig.ampForLufs(-33.0, 2);
    rig.toneAmps(10.0, 997.0, { aMain, aMain, aSc, aSc });
    const auto s = rig.snap();
    check(nearf(s->integratedLufs, -23.0, 0.05) && nearf(s->momentaryLufs, -23.0, 0.05), "snapshot loudness fields are source 0 (main input)");
    check(nearf(rig.eng->getLoudness(1).integratedLufs(), -33.0, 0.05) && nearf(rig.eng->getLoudness(1).momentaryLufs(), -33.0, 0.05),
          "sidechain loudness is measured independently (computed, unpublished)");

    LoudRig monoMain(48000.0, 1, 0);
    monoMain.lufs(6.0, -23.0, 1);
    check(nearf(monoMain.snap()->integratedLufs, -23.0, 0.05), "mono main bus measured as a single weight-1 channel");
}

static void test_loudness_multi_hour_memory()
{
    section("loudness_multi_hour_memory");
    // Chunk-domain simulation of a 10 hour session: 3.6 million 10 ms chunks through the real gating /
    // histogram / LRA code (the K-weighting filter is bypassed via pushChunkEnergy; it is memory-free).
    prism::LoudnessMeter m;
    m.prepare(48000.0, 2);
    const std::size_t bytesBefore = m.getHistogramBytes();

    constexpr int kChunksPerHour = 360000;
    constexpr int kHours = 10;
    const std::size_t total = (std::size_t) kChunksPerHour * kHours;
    std::vector<double> chunks;
    chunks.reserve(total);
    {
        std::mt19937 rng(2026);
        std::uniform_real_distribution<double> lvl(-45.0, -8.0);
        std::uniform_int_distribution<int> die(0, 5);
        for (std::size_t i = 0; i < total; i += 1000)   // 10 s segments
        {
            const double z = die(rng) == 0 ? 0.0 : refPower(lvl(rng));
            for (int k = 0; k < 1000 && i + (size_t) k < total; ++k) chunks.push_back(z);
        }
    }

    const auto t0 = Clock::now();
    long allocs;
    {
        ScopedAllocCount sc;
        for (double z : chunks) m.pushChunkEnergy(z);
        allocs = sc.finish();
    }
    const double simMs = msSince(t0);
    checkf(allocs == 0, "10 h of chunks: zero allocations in the metering path", (double) allocs);
    check(m.getHistogramBytes() == bytesBefore, "histogram memory is constant over the session");
    std::printf("      10 h simulated in %.0f ms; %llu gated blocks, %llu LRA samples; histogram memory %.0f KB per source (constant)\n",
                simMs, (unsigned long long) m.getGatedBlockCount(), (unsigned long long) m.getLraSampleCount(), (double) bytesBefore / 1024.0);

    // Exact (vector based) reference for the same chunk stream.
    std::vector<double> prefix(total + 1, 0.0);
    for (std::size_t i = 0; i < total; ++i) prefix[i + 1] = prefix[i] + chunks[i];
    std::vector<double> blocks, shorts;
    for (std::size_t s = 0; s + 40 <= total; s += 10) blocks.push_back((prefix[s + 40] - prefix[s]) / 40.0);
    for (std::size_t s = 0; s + 300 <= total; s += 10) shorts.push_back((prefix[s + 300] - prefix[s]) / 300.0);
    const double refI = refIntegratedFrom(blocks), refL = refLraFrom(shorts);
    char name[160];
    std::snprintf(name, sizeof(name), "10 h integrated: histogram %.4f vs exact %.4f LUFS (+-0.03)", m.integratedLufs(), refI);
    check(nearf(m.integratedLufs(), refI, 0.03), name);
    std::snprintf(name, sizeof(name), "10 h LRA: histogram %.3f vs exact %.3f LU (+-0.05)", m.loudnessRange(), refL);
    check(nearf(m.loudnessRange(), refL, 0.05), name);

    // Publish-time recompute cost (O(8000) histogram walks), measured with fresh dirty state each time.
    std::mt19937 rng(5);
    double worstUs = 0.0, sumUs = 0.0;
    const int reps = 1000;
    for (int i = 0; i < reps; ++i)
    {
        for (int k = 0; k < 10; ++k) m.pushChunkEnergy(refPower(-30.0 + (double) (rng() % 100) * 0.05));
        const auto t1 = Clock::now();
        volatile float a = m.integratedLufs(), b = m.loudnessRange();
        (void) a; (void) b;
        const double us = msSince(t1) * 1000.0;
        worstUs = std::max(worstUs, us);
        sumUs += us;
    }
    std::printf("      publish-time recompute (I + LRA walks): mean %.1f us, worst %.1f us  [budget 1000 us]\n", sumUs / reps, worstUs);
    if (worstUs > 1000.0)
        std::printf("      WARN: worst recompute above 1 ms (debug build or loaded machine?)\n");
    checkf(worstUs < 5000.0, "recompute stays far below a publish period (worst us; budget 1000 us is WARN-only)", worstUs);
}

static void test_loudness_golden_fixtures()
{
    section("loudness_golden_fixtures");

    juce::File file;
    {
        const juce::String rel = "loudness_golden.json";
        juce::StringArray candidates;
       #ifdef PRISM_FIXTURES_DIR
        candidates.add(juce::String(PRISM_FIXTURES_DIR) + "/" + rel);
       #endif
        candidates.add("plugins/PRISM/Source/Tests/fixtures/" + rel);
        candidates.add("../plugins/PRISM/Source/Tests/fixtures/" + rel);
        candidates.add("../../plugins/PRISM/Source/Tests/fixtures/" + rel);
        candidates.add("../../../plugins/PRISM/Source/Tests/fixtures/" + rel);
        for (auto& c : candidates)
        {
            juce::File f = juce::File::isAbsolutePath(c) ? juce::File(c) : juce::File::getCurrentWorkingDirectory().getChildFile(c);
            if (f.existsAsFile()) { file = f; break; }
        }
    }
    if (! file.existsAsFile())
    {
        std::printf("  SKIP  fixtures file loudness_golden.json not found\n");
        check(true, "golden comparison skipped (fixtures file not found)");
        return;
    }

    const juce::var root = juce::JSON::parse(file);
    const juce::var values = root["values"];
    auto* valuesObj = values.getDynamicObject();
    if (! root.isObject() || valuesObj == nullptr || valuesObj->getProperties().size() == 0)
    {
        std::printf("  SKIP  %s has no \"values\" yet. Generate true goldens with the python script in its \"generator\" field "
                    "(libebur128 via ffmpeg + pyloudnorm); this comparison then runs automatically.\n", file.getFullPathName().toRawUTF8());
        check(true, "golden comparison skipped (no values in fixtures file yet)");
        return;
    }

    bool allOk = true;
    int compared = 0;
    const juce::var signals = root["signals"];
    for (int si = 0; si < signals.size(); ++si)
    {
        const juce::var sig = signals[si];
        const juce::String nm = sig["name"].toString();
        if (! values.hasProperty(nm)) continue;
        const juce::var gold = values[juce::Identifier(nm)];

        const double fs = (double) sig["fs"];
        const int chans = (int) sig["channels"];
        LoudRig rig(fs, chans, 0, 14);
        const juce::var segs = sig["segments"];
        for (int k = 0; k < segs.size(); ++k)
        {
            const juce::var seg = segs[k];
            const juce::String chStr = seg["channels"].toString();
            std::vector<double> amps((size_t) chans, 0.0);
            if (! seg["level_dbfs_peak"].isVoid())
            {
                const double a = std::pow(10.0, (double) seg["level_dbfs_peak"] / 20.0);
                if (chStr.contains("L") && chans >= 1) amps[0] = a;
                if (chStr.contains("R") && chans >= 2) amps[1] = a;
            }
            rig.toneAmps((double) seg["seconds"], (double) seg["freq_hz"], amps);
        }
        const auto s = rig.snap();
        // Tolerances per reference: ffmpeg's ebur128 prints I/LRA quantized to 0.1 LU,
        // so its own rounding contributes up to 0.05 -> I vs ffmpeg within 0.1.
        // pyloudnorm reports full precision -> compare I within 0.05 when present.
        // LRA: implementations legitimately differ by a few tenths (EBU TECH 3342's
        // normative tolerance is +-1 LU); we hold 0.5 LU, measured worst 0.25.
        const double gi = (double) gold["integrated"], gl = (double) gold["lra"];
        const bool iOk = nearf(s->integratedLufs, gi, 0.1);
        bool iPyOk = true;
        if (gold.hasProperty("integrated_pyloudnorm"))
            iPyOk = nearf(s->integratedLufs, (double) gold["integrated_pyloudnorm"], 0.05);
        const bool lOk = nearf(s->loudnessRange, gl, 0.5);
        std::printf("      %-26s I %.3f (golden %.3f)  LRA %.3f (golden %.3f)\n", nm.toRawUTF8(), s->integratedLufs, gi, s->loudnessRange, gl);
        allOk &= iOk && iPyOk && lOk;
        ++compared;
    }
    char name[128];
    std::snprintf(name, sizeof(name), "golden fixtures: %d signals, I +-0.1 LU vs ffmpeg (0.1-quantized) and +-0.05 vs pyloudnorm, LRA +-0.5 LU", compared);
    check(compared > 0 && allOk, name);
}

//==============================================================================
// PHASE 2.5 - true peak
namespace
{
    // fs/4 sine: x[n] = A sin(phi + n pi/2) -> samples A*(sin phi, cos phi, -sin phi, -cos phi), exact for any fs.
    Gen fsQuarterSine(double amp, double phaseDeg, int mask = 3)
    {
        const double phi = phaseDeg * kTwoPi / 360.0;
        return [=](int c, std::uint64_t i) {
            if (! ((mask >> c) & 1)) return 0.0f;
            return (float) (amp * std::sin(phi + (double) (i % 4) * kTwoPi / 4.0));
        };
    }

    // Steady-state true-peak measurement. The tone fades in over 600 samples (raised cosine): a tone that starts
    // abruptly at a crest is a STEP, and a step genuinely overshoots when band-limited (Gibbs) -- a correct
    // true-peak meter reads that, but it is not what TECH 3341 style steady-tone cases measure. (The first
    // version of these tests started abruptly and over-read by up to +0.5 dB at phases whose first sample
    // sits on a crest; the interpolator itself was within 2 % in steady state.)
    prism::SourceSnapshot measureTp(double fs, const Gen& g, std::uint64_t frames = 3000, int mainCh = 2)
    {
        prism::AnalysisEngine eng(makeCfg(mainCh, 0, fs), 14);
        eng.setSpectrumEnabled(false);
        constexpr std::uint64_t kFade = 600;
        feed(eng, 0, frames, 480, [&](int c, std::uint64_t i) {
            const double w = i >= kFade ? 1.0 : 0.5 - 0.5 * std::cos(3.14159265358979323846 * (double) i / (double) kFade);
            return (float) (w * (double) g(c, i));
        });
        return eng.buildSnapshot()->sources[0];
    }

    double dbOfLin(double lin) { return 20.0 * std::log10(lin); }
}

static void test_true_peak_filter_response()
{
    section("true_peak_filter_response");

    const auto& c = prism::TruePeakMeter::coefficients();
    bool unity = true;
    for (int p = 0; p < 4; ++p)
    {
        double s = 0.0;
        for (int t = 0; t < 12; ++t) s += (double) c[(size_t) p][(size_t) t];
        unity &= std::abs(s - 1.0) < 1e-6;
    }
    check(unity, "4 phases x 12 taps (derived Kaiser windowed-sinc): every polyphase branch has unity DC gain");

    // Passband: interpolated outputs reconstruct an ideal sine on the 4x grid (output phase p of input n sits at n-6+p/4).
    double worstErr = 0.0;
    for (double f : { 0.02, 0.05, 0.10, 0.15, 0.20, 0.25 })
    {
        prism::TruePeakMeter m;
        m.prepare(1);
        for (int n = 0; n < 600; ++n)
        {
            float y[4];
            m.filterSample(0, (float) std::sin(kTwoPi * f * n + 0.3), y);
            if (n < 40) continue;
            for (int p = 0; p < 4; ++p)
                worstErr = std::max(worstErr, std::abs((double) y[p] - std::sin(kTwoPi * f * (n - 6 + p / 4.0) + 0.3)));
        }
    }
    checkf(worstErr < 0.02, "sine reconstruction error on the 4x grid < 2 % (-34 dB) for f <= fs/4 (worst)", worstErr);

    double passDev = 0.0;
    for (double f = 0.0; f <= 0.25 + 1e-9; f += 0.005) passDev = std::max(passDev, std::abs(prism::TruePeakMeter::responseDb(f)));
    checkf(passDev < 0.1, "composite passband 0..fs/4 flat within 0.1 dB (worst dB)", passDev);

    // PER-BRANCH gain. The composite above can hide branch imbalance (branch errors partly cancel in the sum),
    // and the true-peak reading is a MAX over branches, so every single fractional-delay branch must be flat:
    // |H_p(f)| within +0.2 / -0.4 dB (TECH 3341 window) for all four phases, swept finely over 0..0.25 fs
    // (0.25..0.30 fs is swept too and printed for information).
    double branchHi = -1000.0, branchLo = 1000.0, branchHi25 = -1000.0, branchHi30 = -1000.0, branchLo30 = 1000.0;
    for (int p = 0; p < 4; ++p)
        for (double f = 0.0; f <= 0.30 + 1e-9; f += 0.0025)
        {
            const double w = kTwoPi * f;
            double re = 0.0, im = 0.0;
            for (int j = 0; j < 12; ++j)
            {
                re += (double) c[(size_t) p][(size_t) j] * std::cos(w * (11 - j));
                im -= (double) c[(size_t) p][(size_t) j] * std::sin(w * (11 - j));
            }
            const double db = 20.0 * std::log10(std::sqrt(re * re + im * im));
            branchHi30 = std::max(branchHi30, db);
            branchLo30 = std::min(branchLo30, db);
            if (f <= 0.25 + 1e-9) { branchHi = std::max(branchHi, db); branchLo = std::min(branchLo, db); }
        }
    branchHi25 = branchHi;
    char bname[200];
    std::snprintf(bname, sizeof(bname), "every polyphase branch within +0.2/-0.4 dB over 0..fs/4 (range %.3f .. %.3f dB; to 0.30 fs: %.3f .. %.3f)", branchLo, branchHi, branchLo30, branchHi30);
    check(branchHi25 <= 0.2 && branchLo >= -0.4, bname);

    // Image rejection: images of a tone at f appear at fs +- f. For tones up to fs/4 the nearest image is at 0.75 fs.
    double worstStop = -1000.0;
    for (double f = 0.75; f <= 2.0 + 1e-9; f += 0.005) worstStop = std::max(worstStop, prism::TruePeakMeter::responseDb(f));
    checkf(worstStop <= -38.0, "image rejection >= 38 dB over 0.75 fs .. 2 fs (images of tones up to fs/4) (worst dB)", worstStop);
    std::printf("      composite passband flatness 0..fs/4: %.4f dB; stopband (0.75..2 fs) worst %.1f dB\n", passDev, worstStop);
}

static void test_true_peak_fs4_sine()
{
    section("true_peak_fs4_sine");
    // 45 degrees, amplitude 1.0: samples are +-0.7071 -> SP = -3.01 dBFS, the continuous peak (1.0, 0 dBTP) lies
    // exactly half-way between samples (output phase 2 of the 4x grid). Same digital sequence at every rate.
    for (double fs : { 44100.0, 48000.0, 96000.0, 192000.0 })
    {
        const auto s = measureTp(fs, fsQuarterSine(1.0, 45.0));
        char name[160];
        std::snprintf(name, sizeof(name), "fs %.0f: SP %.3f dBFS (expect -3.01), TP %.3f dBTP (expect 0.00, +0.2/-0.4)", fs, s.samplePeakMaxDb, s.truePeakMaxDb);
        check(nearf(s.samplePeakMaxDb, -3.0103, 0.01) && s.truePeakMaxDb >= -0.4f && s.truePeakMaxDb <= 0.2f && s.truePeakMaxDb >= s.samplePeakMaxDb, name);
    }
}

static void test_true_peak_phase_and_level_sweep()
{
    section("true_peak_phase_and_level_sweep");
    // Worst-case phase scan: the 4x grid only samples every 0.25 sample, so the reading can miss the true crest by
    // at most cos(pi/2 * 0.125) = -0.17 dB. Allowed window (TECH 3341): +0.2 / -0.4 dB.
    for (double fs : { 48000.0, 96000.0 })
    {
        double lo = 1e9, hi = -1e9;
        bool ok = true;
        for (double deg = 0.0; deg < 360.0; deg += 5.0)
        {
            const auto s = measureTp(fs, fsQuarterSine(0.5, deg), 1800);
            const double err = s.truePeakMaxDb - dbOfLin(0.5);
            lo = std::min(lo, err); hi = std::max(hi, err);
            ok &= err >= -0.4 && err <= 0.2 && s.truePeakMaxDb >= s.samplePeakMaxDb;
        }
        char name[160];
        std::snprintf(name, sizeof(name), "fs %.0f: A=0.5 over 72 phases: TP error in [%.3f, %.3f] dB (limits -0.4 / +0.2)", fs, lo, hi);
        check(ok, name);
    }

    // Inter-sample over: +3 dBTP crest whose samples never exceed -0.01 dBFS.
    for (double fs : { 44100.0, 48000.0, 96000.0, 192000.0 })
    {
        const double amp = std::pow(10.0, 3.0 / 20.0);
        const auto s = measureTp(fs, fsQuarterSine(amp, 45.0));
        char name[160];
        std::snprintf(name, sizeof(name), "fs %.0f: inter-sample over: SP %.3f dBFS but TP %.3f dBTP (expect +3.0, +0.2/-0.4)", fs, s.samplePeakMaxDb, s.truePeakMaxDb);
        check(s.samplePeakMaxDb < 0.0f && nearf(s.truePeakMaxDb, 3.0, 0.4) && s.truePeakMaxDb - s.samplePeakMaxDb > 2.5f, name);
    }

    bool levels = true;
    for (double fs : { 44100.0, 48000.0, 96000.0, 192000.0 })
        for (double lvl : { 3.0, 0.0, -1.0, -3.0, -6.0, -12.0, -30.0 })
        {
            const auto s = measureTp(fs, fsQuarterSine(std::pow(10.0, lvl / 20.0), 45.0));
            const double err = s.truePeakMaxDb - lvl;
            levels &= err >= -0.4 && err <= 0.2;
        }
    check(levels, "level sweep +3 .. -30 dBTP at 45 deg, all four rates: within +0.2/-0.4 dB");

    {   // Information: an ABRUPT start on a crest is a step; band-limited reconstruction of a step overshoots (Gibbs).
        prism::AnalysisEngine eng(makeCfg(2, 0, 48000.0), 14);
        eng.setSpectrumEnabled(false);
        feed(eng, 0, 1800, 480, fsQuarterSine(0.5, 90.0));
        const double err = eng.buildSnapshot()->sources[0].truePeakMaxDb - dbOfLin(0.5);
        std::printf("      abrupt start on a crest (phase 90 deg, no fade, history primed with the first sample): TP error %+.3f dB\n", err);
        check(err > -0.4 && err < 1.0, "abrupt-start reading stays bounded (< +1 dB); steady-state tests use a fade-in");
    }

    // Mid-band tones (fs/8, 0.2 fs) at several phases also stay inside the window.
    bool mid = true;
    for (double f : { 0.125, 0.2 })
        for (double deg : { 0.0, 37.0, 90.0, 133.0 })
        {
            const double ph = deg * kTwoPi / 360.0;
            const auto s = measureTp(48000.0, [&](int, std::uint64_t i) { return (float) (0.7 * std::sin(kTwoPi * f * (double) i + ph)); }, 4000);
            const double err = s.truePeakMaxDb - dbOfLin(0.7);
            mid &= err >= -0.4 && err <= 0.2;
        }
    check(mid, "fs/8 and 0.2 fs tones at 4 phases each: TP within +0.2/-0.4 dB of the analytic crest");
}

static void test_true_peak_invariant_random()
{
    section("true_peak_invariant_random");
    // 1e6 random blocks in total (250k per rate, two stereo sources): published TP >= SP at every check.
    bool invariant = true, finite = true;
    std::uint64_t blocksTotal = 0;
    for (double fs : { 44100.0, 48000.0, 96000.0, 192000.0 })
    {
        prism::AnalysisEngine eng(makeCfg(2, 2, fs), 14);
        eng.setSpectrumEnabled(false);
        std::mt19937 rng((unsigned) fs);
        std::vector<std::vector<float>> bufs(4, std::vector<float>(64));
        const float* ptrs[4] = { bufs[0].data(), bufs[1].data(), bufs[2].data(), bufs[3].data() };
        for (int b = 0; b < 250000; ++b)
        {
            const int n = 1 + (int) (rng() % 64);
            const int kind = (int) (rng() % 6);
            const float scale = (float) (rng() % 1000) / 800.0f;   // up to 1.25: overs included
            for (int c = 0; c < 4; ++c)
                for (int i = 0; i < n; ++i)
                {
                    float v;
                    switch (kind)
                    {
                        case 0: v = scale * ((float) (rng() & 0xffff) / 32768.0f - 1.0f); break;   // noise
                        case 1: v = (i & 1) ? scale : -scale; break;                                // Nyquist square
                        case 2: v = (i % 4 < 2) ? scale * 0.7071f : -scale * 0.7071f; break;        // fs/4 at 45 deg
                        case 3: v = i == n / 2 ? scale : 0.0f; break;                               // impulse
                        case 4: v = 0.0f; break;                                                    // silence
                        default: v = scale * std::sin(0.3f * (float) i + (float) c); break;
                    }
                    bufs[(size_t) c][(size_t) i] = v;
                }
            eng.captureBlock(ptrs, 4, n);
            eng.pump();
            ++blocksTotal;
            if ((b % 5000) == 4999)
            {
                const auto snap = eng.buildSnapshot();
                for (int s = 0; s < 2; ++s)
                {
                    const auto& src = snap->sources[(size_t) s];
                    invariant &= src.truePeakMaxDb >= src.samplePeakMaxDb;
                    finite &= std::isfinite(src.truePeakMaxDb) || (std::isinf(src.truePeakMaxDb) && src.truePeakMaxDb < 0);
                }
            }
        }
        const auto fin = eng.buildSnapshot();
        for (int s = 0; s < 2; ++s)
            invariant &= fin->sources[(size_t) s].truePeakMaxDb >= fin->sources[(size_t) s].samplePeakMaxDb;
        std::printf("      fs %.0f: final SP %.3f / TP %.3f dB (source 0), raw 4x-FIR max %.4f lin\n", fs, fin->sources[0].samplePeakMaxDb,
                    fin->sources[0].truePeakMaxDb, eng.getTruePeakMeter(0).maxLinear());
    }
    checkf(invariant, "TP >= SP in every published snapshot (1e6 random blocks, 4 rates, 2 sources)", (double) blocksTotal);
    check(finite, "TP is finite or exactly -INF, never NaN or +inf");
}

static void test_true_peak_hold_reset_and_sources()
{
    section("true_peak_hold_reset_and_sources");
    {
        prism::AnalysisEngine eng(makeCfg(2, 2, 48000.0), 14);
        eng.setSpectrumEnabled(false);
        auto tp = [&](int src) { return eng.buildSnapshot()->sources[(size_t) src].truePeakMaxDb; };

        check(std::isinf(tp(0)) && tp(0) < 0 && std::isinf(tp(1)) && tp(1) < 0, "before any signal: -INF on both sources (dashes)");
        feed(eng, 0, 4800, 480, [](int, std::uint64_t) { return 0.0f; });
        check(std::isinf(tp(0)) && tp(0) < 0, "digital silence keeps -INF");

        feed(eng, 4800, 480, 480, [](int c, std::uint64_t i) { return (c < 2 && i == 5000) ? 0.5f : 0.0f; });
        const float t1 = tp(0);
        check(std::isfinite(t1) && t1 >= dbOfLin(0.5) - 0.01f, "an impulse is registered");
        check(std::isinf(tp(1)) && tp(1) < 0, "the silent sidechain stays -INF (sources independent)");

        feed(eng, 5280, 96000, 4096, [](int, std::uint64_t) { return 0.0f; });
        check(tp(0) == t1, "true peak never decays (2 s of silence later: unchanged)");
        feed(eng, 101280, 480, 480, [](int c, std::uint64_t i) { return (c < 2 && i == 101400) ? 0.1f : 0.0f; });
        check(tp(0) == t1, "a later, smaller peak does not lower the maximum");
        feed(eng, 101760, 480, 480, [](int c, std::uint64_t i) { return (c < 2 && i == 101900) ? 0.9f : 0.0f; });
        check(tp(0) > t1 && tp(0) >= dbOfLin(0.9) - 0.01f, "a larger peak raises it");
    }
    {   // mono source: single channel, no stereo partner
        // Steady state (fade-in): an abrupt start is a step and legitimately overshoots; with the history primed by
        // the first sample, the 45 deg start looks like a long +0.707 plateau followed by the tone, whose first
        // negative crest rides on that step's Gibbs overshoot (about +0.5 dB) -- not a meter error.
        const auto s = measureTp(48000.0, fsQuarterSine(1.0, 45.0, 1), 3000, 1);
        check(s.truePeakMaxDb >= -0.4f && s.truePeakMaxDb <= 0.2f && s.truePeakMaxDb >= s.samplePeakMaxDb, "mono source: fs/4 45 deg reads ~0 dBTP");
    }
    {   // crest only on the right channel is found
        const auto s = measureTp(48000.0, fsQuarterSine(1.0, 45.0, 2));
        check(s.truePeakMaxDb >= -0.4f && s.truePeakMaxDb <= 0.2f, "right-channel-only crest is measured");
    }
}

static void test_true_peak_cpu_budget()
{
    section("true_peak_cpu_budget");
    prism::TruePeakMeter meters[2];
    meters[0].prepare(2); meters[1].prepare(2);
    std::vector<float> l(4096), r(4096);
    for (int i = 0; i < 4096; ++i) { l[(size_t) i] = 0.5f * hnoise(0, (std::uint64_t) i); r[(size_t) i] = 0.5f * hnoise(1, (std::uint64_t) i); }

    const int blocks = (int) (10.0 * 192000.0 / 4096.0);
    const auto t0 = Clock::now();
    for (int b = 0; b < blocks; ++b)
        for (auto& m : meters) m.process(l.data(), r.data(), 4096);
    const double wall = msSince(t0) / 1000.0;
    const double pct = 100.0 * wall / (blocks * 4096.0 / 192000.0);
    std::printf("      true peak @192 kHz, 2 stereo sources: %.3f %% of one core (4 channels x 48 MAC per sample)\n", pct);
    if (pct > 1.0)
        std::printf("      WARN: above the 1%% budget (debug build or loaded machine?)\n");
    char name[96];
    std::snprintf(name, sizeof(name), "true peak keeps up with real time at 192 kHz (%.3f %% core; 1 %% budget is WARN-only)", pct);
    check(pct < 100.0 && meters[0].maxLinear() > 0.0f, name);
}

//==============================================================================
// PHASE 2.6 - waveform columns + beat flags + consumer-acknowledged delivery
namespace
{
    using Col = prism::WaveformExtractor::Column;

    std::vector<Col> allColumns(const prism::WaveformExtractor& w)
    {
        std::vector<Col> v((size_t) prism::WaveformExtractor::kLogSize);
        const int n = w.collectNewer(-1, prism::WaveformExtractor::kLogSize, v.data());
        v.resize((size_t) n);
        return v;
    }

    // Inverse of the -54..0 dBFS column map (v -> linear peak); 0 means "silent".
    double unitToLin(float v) { return v <= 0.0f ? 0.0 : std::pow(10.0, ((double) v * 54.0 - 54.0) / 20.0); }

    // Run an extractor on a stereo sine (L = R) and return the per-band linear peak maxima over the last 30 columns.
    struct BandPeaks { double bass, mid, high; };
    BandPeaks bandPeaksForSine(double fs, double hz, double amp)
    {
        prism::WaveformExtractor w;
        w.prepare(fs, 2);
        const int block = 1024;
        std::vector<float> buf((size_t) block);
        const std::uint64_t frames = (std::uint64_t) fs;   // 1 s
        for (std::uint64_t pos = 0; pos < frames; pos += (std::uint64_t) block)
        {
            const int n = (int) std::min<std::uint64_t>((std::uint64_t) block, frames - pos);
            for (int i = 0; i < n; ++i) buf[(size_t) i] = (float) (amp * std::sin(kTwoPi * hz * (double) (pos + (std::uint64_t) i) / fs));
            w.process(buf.data(), buf.data(), n);
        }
        const auto cols = allColumns(w);
        BandPeaks p { 0, 0, 0 };
        for (size_t i = cols.size() - 30; i < cols.size(); ++i)
        {
            p.bass = std::max(p.bass, unitToLin(cols[i].bass));
            p.mid  = std::max(p.mid,  unitToLin(cols[i].mid));
            p.high = std::max(p.high, unitToLin(cols[i].high));
        }
        return p;
    }
}

static void test_waveform_column_rate()
{
    section("waveform_column_rate");
    bool rateOk = true, driftOk = true, firstOk = true;
    double worst = 0.0;
    for (double fs : { 44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0 })
    {
        prism::WaveformExtractor w;
        w.prepare(fs, 2);
        std::vector<float> zeros(4096, 0.0f);
        const std::uint64_t frames = (std::uint64_t) (60.0 * fs);
        for (std::uint64_t pos = 0; pos < frames; pos += 4096)
            w.process(zeros.data(), zeros.data(), (int) std::min<std::uint64_t>(4096, frames - pos));
        const double rate = (double) w.columnsDone() / 60.0;
        worst = std::max(worst, std::abs(rate - 84.33));
        rateOk &= std::abs(rate - 84.33) <= 0.01 && w.columnsDone() == 5060;   // 60 s * 253/3

        // Integer arithmetic: no drift over 10 hours (3.036e6 columns exactly), at every rate.
        prism::WaveformExtractor longRun;
        longRun.prepare(fs, 2);
        longRun.skipTo((std::uint64_t) (36000.0 * fs));
        driftOk &= longRun.columnsDone() == 3036000;

        // Column k closes exactly at frame ceil((k+1)*3*fs/253).
        prism::WaveformExtractor edge;
        edge.prepare(fs, 2);
        const std::uint64_t fsI = (std::uint64_t) fs;
        const std::uint64_t closeAt = (3 * fsI + 252) / 253;   // end of column 0 in frames
        edge.process(zeros.data(), zeros.data(), (int) (closeAt - 1));
        const bool before = edge.columnsDone() == 0;
        edge.process(zeros.data(), zeros.data(), 1);
        firstOk &= before && edge.columnsDone() == 1;
    }
    checkf(rateOk, "column rate over 60 s = 84.33 +- 0.01 /s at 44.1/48/88.2/96/176.4/192 kHz (worst deviation from 84.33)", worst);
    check(driftOk, "10 simulated hours: exactly 3,036,000 columns at every rate (no drift)");
    check(firstOk, "column boundaries land on ceil((k+1)*3 fs/253) frames");

    {   // through the engine, block-size independent
        for (int bs : { 1, 480, 4096 })
        {
            prism::AnalysisEngine eng(makeCfg(2, 0, 48000.0), 14);
            feed(eng, 0, 48000, bs, [](int, std::uint64_t) { return 0.0f; });
            check(eng.getWaveform(0).columnsDone() == 84, "engine: 1 s at 48 kHz = 84 columns (floor(253/3)) for any block size");
        }
    }
}

static void test_waveform_band_separation()
{
    section("waveform_band_separation");
    const double fs = 48000.0, a = 0.5;
    auto ratioBelow = [](double small, double big) { return big > 0.0 && small / big < 0.05; };

    const auto b60 = bandPeaksForSine(fs, 60.0, a);
    char name[200];
    std::snprintf(name, sizeof(name), "60 Hz: bass %.4f, mids %.5f, highs %.6f (linear peaks) -> mids/highs < 5 %% of bass", b60.bass, b60.mid, b60.high);
    check(ratioBelow(b60.mid, b60.bass) && ratioBelow(b60.high, b60.bass) && std::abs(dbOfLin(b60.bass) - dbOfLin(a)) < 0.5, name);

    const auto b1k = bandPeaksForSine(fs, 1000.0, a);
    std::snprintf(name, sizeof(name), "1 kHz: bass %.5f, mids %.4f, highs %.4f -> bass/highs < 5 %% of mids", b1k.bass, b1k.mid, b1k.high);
    check(ratioBelow(b1k.bass, b1k.mid) && ratioBelow(b1k.high, b1k.mid) && std::abs(dbOfLin(b1k.mid) - dbOfLin(a)) < 0.5, name);

    // 8123 Hz, NOT 8000: column peaks are SAMPLE peaks. 8000 Hz at 48 kHz is exactly 6 samples per cycle with a
    // fixed phase, so the samples never reach the crest (0.866 of it, -1.25 dB) in every column. A frequency that is
    // not commensurate with fs sweeps the sampling phase, and the max over 30 columns finds the crest (< 0.05 dB miss).
    const auto b8k = bandPeaksForSine(fs, 8123.0, a);
    std::snprintf(name, sizeof(name), "8.1 kHz: bass %.6f, mids %.5f, highs %.4f -> bass/mids < 5 %% of highs, highs within 0.5 dB of the tone", b8k.bass, b8k.mid, b8k.high);
    check(ratioBelow(b8k.bass, b8k.high) && ratioBelow(b8k.mid, b8k.high) && std::abs(dbOfLin(b8k.high) - dbOfLin(a)) < 0.5, name);

    // Same behaviour at another rate (crossover design is per-rate).
    const auto b1k96 = bandPeaksForSine(96000.0, 1000.0, a);
    check(ratioBelow(b1k96.bass, b1k96.mid) && ratioBelow(b1k96.high, b1k96.mid), "1 kHz at 96 kHz: mids dominate");

    {   // pink noise: all three bands alive
        prism::WaveformExtractor w;
        w.prepare(fs, 2);
        std::vector<float> l(1024), r(1024);
        double bL[7] = {}, bR[7] = {};
        for (std::uint64_t pos = 0; pos < 96000; pos += 1024)
        {
            for (int i = 0; i < 1024; ++i)
            {
                auto pink = [&](int ch, double* b) {
                    const double white = 2.0 * (double) hnoise(ch, pos + (std::uint64_t) i);
                    b[0] = 0.99886 * b[0] + white * 0.0555179; b[1] = 0.99332 * b[1] + white * 0.0750759;
                    b[2] = 0.96900 * b[2] + white * 0.1538520; b[3] = 0.86650 * b[3] + white * 0.3104856;
                    b[4] = 0.55000 * b[4] + white * 0.5329522; b[5] = -0.7616 * b[5] - white * 0.0168980;
                    const double out = b[0] + b[1] + b[2] + b[3] + b[4] + b[5] + b[6] + white * 0.5362;
                    b[6] = white * 0.115926;
                    return (float) (0.25 * out);
                };
                l[(size_t) i] = pink(0, bL);
                r[(size_t) i] = pink(1, bR);
            }
            w.process(l.data(), r.data(), 1024);
        }
        const auto cols = allColumns(w);
        double mean[3] = {};
        for (size_t i = 20; i < cols.size(); ++i) { mean[0] += cols[i].bass; mean[1] += cols[i].mid; mean[2] += cols[i].high; }
        for (double& m : mean) m /= (double) (cols.size() - 20);
        std::snprintf(name, sizeof(name), "pink noise: mean column values bass %.3f / mids %.3f / highs %.3f are all non-zero (> 0.05)", mean[0], mean[1], mean[2]);
        check(mean[0] > 0.05 && mean[1] > 0.05 && mean[2] > 0.05, name);
    }

    {   // silence -> exact zeros; anti-phase stereo sums to nothing
        prism::WaveformExtractor w;
        w.prepare(fs, 2);
        std::vector<float> z(1024, 0.0f);
        for (int i = 0; i < 94; ++i) w.process(z.data(), z.data(), 1024);
        bool zero = true;
        for (const auto& c : allColumns(w)) zero &= c.bass == 0.0f && c.mid == 0.0f && c.high == 0.0f && c.flag == 0.0f;
        check(zero && w.columnsDone() > 100, "digital silence: every column is exactly 0 (bass, mids, highs, flag)");

        prism::WaveformExtractor ap;
        ap.prepare(fs, 2);
        std::vector<float> s(1024), ns(1024);
        for (int i = 0; i < 1024; ++i) { s[(size_t) i] = 0.5f * std::sin(0.1f * (float) i); ns[(size_t) i] = -s[(size_t) i]; }
        for (int i = 0; i < 20; ++i) ap.process(s.data(), ns.data(), 1024);
        bool zero2 = true;
        for (const auto& c : allColumns(ap)) zero2 &= c.bass == 0.0f && c.mid == 0.0f && c.high == 0.0f;
        check(zero2, "anti-phase L/R: (L+R)/2 = 0 -> zero columns");
    }
}

static void test_waveform_beat_flags()
{
    section("waveform_beat_flags");
    // 120 bpm, 4/4: a beat every 0.5 s (24000 frames @48k), a bar every 2 s; column k holds frame f for k = floor(f*253/(3*48000)).
    for (int bs : { 480, 333, 4096 })
    {
        prism::AnalysisEngine eng(makeCfg(2, 0, 48000.0), 14);
        FakePlayHead ph;
        std::vector<float> buf((size_t) bs, 0.1f);
        const float* ptrs[2] = { buf.data(), buf.data() };
        const std::uint64_t total = 480000;   // 10 s -> 20 beats
        for (std::uint64_t pos = 0; pos < total;)
        {
            const int n = (int) std::min<std::uint64_t>((std::uint64_t) bs, total - pos);
            ph.ppq = (double) pos * 2.0 / 48000.0;
            eng.captureBlock(ptrs, 2, n);
            eng.capturePlayhead(&ph, n);
            eng.pump();
            pos += (std::uint64_t) n;
        }
        // one more silent pump block so the last in-flight column closes
        const auto cols = allColumns(eng.getWaveform(0));

        std::vector<std::pair<std::uint64_t, float>> flagged;
        for (const auto& c : cols) if (c.flag != 0.0f) flagged.push_back({ c.index, c.flag });

        bool shapeOk = flagged.size() == 20;
        bool posOk = true, kindOk = true;
        for (size_t k = 0; k < flagged.size() && k < 20; ++k)
        {
            const std::int64_t expectCol = (std::int64_t) (24000ull * k * 253ull / (3ull * 48000ull));
            posOk &= std::abs((std::int64_t) flagged[k].first - expectCol) <= 1;
            kindOk &= flagged[k].second == (k % 4 == 0 ? 2.0f : 1.0f);
        }
        bool spacing = true;
        for (size_t k = 1; k < flagged.size(); ++k)
        {
            const auto d = (std::int64_t) flagged[k].first - (std::int64_t) flagged[k - 1].first;
            spacing &= d >= 41 && d <= 44;   // 0.5 s = 42.17 columns (+-1 column)
        }
        std::int64_t lastBar = -1;
        bool barSpacing = true;
        for (size_t k = 0; k < flagged.size(); ++k)
            if (flagged[k].second == 2.0f)
            {
                if (lastBar >= 0) { const auto d = (std::int64_t) flagged[k].first - lastBar; barSpacing &= d >= 167 && d <= 170; }   // 2 s = 168.67
                lastBar = (std::int64_t) flagged[k].first;
            }
        char name[160];
        std::snprintf(name, sizeof(name), "120 bpm 4/4, block %d: 20 flagged columns, beat spacing ~42.17, bar every ~168.67 (+-1 column), bars = flag 2", bs);
        check(shapeOk && posOk && kindOk && spacing && barSpacing, name);
    }

    {   // no host playhead (or stopped transport): flags 0 and bpm NaN
        prism::AnalysisEngine eng(makeCfg(2, 0, 48000.0), 14);
        feed(eng, 0, 96000, 480, [](int, std::uint64_t i) { return 0.2f * std::sin(0.05f * (float) i); });
        bool noFlags = true;
        for (const auto& c : allColumns(eng.getWaveform(0))) noFlags &= c.flag == 0.0f;
        check(noFlags && eng.getWaveform(0).columnsDone() > 100 && std::isnan(eng.buildSnapshot()->bpm), "no playhead: columns flow with flag 0, bpm NaN");

        prism::AnalysisEngine eng2(makeCfg(2, 0, 48000.0), 14);
        FakePlayHead ph;
        ph.playing = false;
        std::vector<float> buf(480, 0.1f);
        const float* ptrs[2] = { buf.data(), buf.data() };
        for (int b = 0; b < 400; ++b) { ph.ppq = b * 0.02; eng2.captureBlock(ptrs, 2, 480); eng2.capturePlayhead(&ph, 480); eng2.pump(); }
        bool none = true;
        for (const auto& c : allColumns(eng2.getWaveform(0))) none &= c.flag == 0.0f;
        check(none && eng2.buildSnapshot()->bpm == 120.0f, "transport stopped: no flags, bpm still published");
    }
}

static void test_waveform_ack_delivery()
{
    section("waveform_ack_delivery");
    std::atomic<std::uint64_t> fetched { 0 };
    prism::AnalysisEngine eng(makeCfg(2, 0, 48000.0), 14);
    eng.setSpectrumEnabled(true);
    eng.attachControls({ nullptr, nullptr, nullptr, nullptr, nullptr, &fetched });

    // 60 Hz tone with a slowly moving level so every column has a distinct value (identity check by content).
    auto gen = [](std::uint64_t i) {
        const double t = (double) i / 48000.0;
        return (float) ((0.1 + 0.4 * std::abs(std::sin(kTwoPi * 0.7 * t))) * std::sin(kTwoPi * 60.0 * t));
    };
    std::uint64_t pos = 0;
    auto pushFrames = [&](int n) {
        std::vector<float> b((size_t) n);
        for (int i = 0; i < n; ++i) b[(size_t) i] = gen(pos + (std::uint64_t) i);
        const float* ptrs[2] = { b.data(), b.data() };
        eng.captureBlock(ptrs, 2, n);
        eng.pump();
        pos += (std::uint64_t) n;
    };

    // Editor-like consumer: snapshots every ~16.7 ms (800 frames), it fetches every 3rd one.
    std::vector<std::array<float, 4>> received;
    std::int64_t lastFetchedNewest = -1;
    bool consistent = true;
    int snapshots = 0;
    for (int it = 0; it < 360; ++it)   // 6 s
    {
        pushFrames(800);
        const auto snap = eng.buildSnapshot();
        const std::int64_t newest = eng.getWaveform(0).newestIndex();
        ++snapshots;
        if (snapshots % 3 == 0)
        {
            fetched.store(snap->sequence);
            const auto& s0 = snap->sources[0];
            for (int i = 0; i < s0.waveformColumnCount; ++i) received.push_back(s0.waveformColumns[(size_t) i]);
            lastFetchedNewest = newest;
            consistent &= (std::int64_t) received.size() == newest + 1;   // no missing, no duplicated column
        }
    }
    check(consistent && lastFetchedNewest > 400, "fetching every 3rd snapshot: received column count == newest column + 1 at every fetch (nothing missing or repeated)");

    const auto log = allColumns(eng.getWaveform(0));
    bool content = received.size() <= log.size();
    for (size_t i = 0; content && i < received.size(); ++i)
        content &= received[i][0] == log[i].bass && received[i][1] == log[i].mid && received[i][2] == log[i].high && received[i][3] == log[i].flag;
    check(content, "received columns equal the engine's column log element-for-element, in order");
    check(eng.getWaveformDroppedColumns(0) == 0, "no columns dropped while the consumer keeps up");

    // Stall: no fetches for 400 ms (~33 columns) while snapshots keep being built; then one fetch.
    const std::int64_t ackedBefore = lastFetchedNewest;
    std::shared_ptr<const prism::AnalysisSnapshot> last;
    for (int it = 0; it < 24; ++it) { pushFrames(800); last = eng.buildSnapshot(); }   // 24 unfetched snapshots
    const std::int64_t newest = eng.getWaveform(0).newestIndex();
    fetched.store(last->sequence);
    const std::int64_t pending = newest - ackedBefore;
    check(pending > 16, "stall produced more than 16 pending columns");
    check(last->sources[0].waveformColumnCount == 16, "stalled snapshot carries the newest 16 columns");
    const auto log2 = allColumns(eng.getWaveform(0));
    bool newest16 = true;
    for (int i = 0; i < 16; ++i)
    {
        const auto& c = log2[log2.size() - 16 + (size_t) i];
        const auto& r = last->sources[0].waveformColumns[(size_t) i];
        newest16 &= r[0] == c.bass && r[1] == c.mid && r[2] == c.high && r[3] == c.flag;
    }
    check(newest16, "...and they are exactly the newest 16 of the log (oldest dropped)");
    // The next snapshot after the fetch sees the ack; the drop is counted exactly once, not once per snapshot.
    pushFrames(800);
    const auto after = eng.buildSnapshot();
    char name[160];
    std::snprintf(name, sizeof(name), "dropped-column count = pending - 16 = %lld (reported %llu), counted once across the 24 stalled snapshots", (long long) (pending - 16),
                  (unsigned long long) eng.getWaveformDroppedColumns(0));
    check(eng.getWaveformDroppedColumns(0) == (std::uint64_t) (pending - 16), name);
    check(eng.getWaveformAckedColumn(0) == newest, "after the fetch the ack point jumps to the delivered column");
    check(after->sources[0].waveformColumnCount <= 2, "after catching up, snapshots carry only the new columns again");

    // Without a feedback channel every snapshot is assumed delivered (latest-only).
    prism::AnalysisEngine plain(makeCfg(2, 0, 48000.0), 14);
    feed(plain, 0, 48000, 480, [](int, std::uint64_t) { return 0.1f; });
    const auto s1 = plain.buildSnapshot();
    feed(plain, 48000, 4800, 480, [](int, std::uint64_t) { return 0.1f; });
    const auto s2 = plain.buildSnapshot();
    check(s1->sources[0].waveformColumnCount == 16 && s2->sources[0].waveformColumnCount <= 9, "no ack channel: the next snapshot carries only columns newer than the previous one");
}

static void test_waveform_editor_gating_and_wiring()
{
    section("waveform_editor_gating_and_wiring");
    {
        prism::AnalysisEngine eng(makeCfg(2, 0, 48000.0), 14);
        eng.setSpectrumEnabled(false);
        feed(eng, 0, 144000, 480, [](int, std::uint64_t i) { return 0.3f * std::sin(0.2f * (float) i); });
        check(eng.getWaveform(0).columnsLogged() == 0 && eng.buildSnapshot()->sources[0].waveformColumnCount == 0, "editor inactive: no columns are built");
        eng.setSpectrumEnabled(true);
        feed(eng, 144000, 48000, 480, [](int, std::uint64_t i) { return 0.3f * std::sin(0.2f * (float) i); });
        const auto& w = eng.getWaveform(0);
        const auto first = allColumns(w).front().index;
        check(w.columnsLogged() > 80 && w.newestIndex() == (std::int64_t) w.columnsDone() - 1, "re-enabled: columns resume on the absolute column grid, nothing before the gap is invented");
        check(first == w.columnOfFrame(144000) + 1 || first == w.columnOfFrame(144000), "the partial column at the resync point is skipped (first logged index is on the grid)");
        check(eng.buildSnapshot()->sources[0].waveformColumnCount == 16, "snapshot after re-enable carries columns");
    }
    {   // mono source and absent sidechain
        prism::AnalysisEngine eng(makeCfg(1, 0, 48000.0), 14);
        feed(eng, 0, 48000, 480, [](int, std::uint64_t i) { return 0.3f * std::sin(0.02f * (float) i); });
        const auto snap = eng.buildSnapshot();
        check(snap->sources[0].waveformColumnCount == 16 && snap->sources[1].waveformColumnCount == 0, "mono main produces columns; absent sidechain carries none");
    }
    {   // processor end to end: a fast consumer loses nothing
        PRISMAudioProcessor proc;
        check(configure(proc, 2, 0, 48000.0, 480), "configure");
        proc.prepareToPlay(48000.0, 480);
        proc.setEditorActive(true);
        juce::AudioBuffer<float> buf(2, 480);
        juce::MidiBuffer midi;
        for (int i = 0; i < 480; ++i) for (int c = 0; c < 2; ++c) buf.setSample(c, i, 0.3f * std::sin(0.3f * (float) i));
        std::uint64_t lastSeq = 0;
        std::uint64_t received = 0;
        const auto t0 = Clock::now();
        while (msSince(t0) < 1500.0)
        {
            proc.processBlock(buf, midi);
            const auto s = proc.getLatestAnalysisSnapshot();
            if (s != nullptr && s->sequence != lastSeq)
            {
                lastSeq = s->sequence;
                received += (std::uint64_t) s->sources[0].waveformColumnCount;
            }
            sleepMs(5);
        }
        proc.setEditorActive(false);
        proc.releaseResources();
        auto* eng = proc.getAnalysisEngine();
        eng->pump();
        const auto total = (std::uint64_t) (eng->getWaveform(0).newestIndex() + 1);
        char name[160];
        std::snprintf(name, sizeof(name), "processor: fast consumer received %llu of %llu columns, dropped %llu", (unsigned long long) received, (unsigned long long) total,
                      (unsigned long long) eng->getWaveformDroppedColumns(0));
        check(eng->getWaveformDroppedColumns(0) == 0 && received <= total && received + 16 + 8 >= total && received > 60, name);
    }
}

//==============================================================================
// PHASE 2.7 - integration, concurrency, profiling
namespace
{
    bool bitsEq(float a, float b) { return std::memcmp(&a, &b, sizeof(float)) == 0; }

    // First difference between two snapshots (bitwise on every float, sequence excluded), "" if identical.
    std::string diffSnapshots(const prism::AnalysisSnapshot& a, const prism::AnalysisSnapshot& b)
    {
        auto f = [](const char* what, float x, float y) { return bitsEq(x, y) ? std::string() : std::string(what); };
        std::string d;
        if (a.sampleRate != b.sampleRate) return "sampleRate";
        if (! bitsEq(a.bpm, b.bpm)) return "bpm";
        if (a.signalPresent != b.signalPresent) return "signalPresent";
        for (auto s : { f("momentary", a.momentaryLufs, b.momentaryLufs), f("shortTerm", a.shortTermLufs, b.shortTermLufs),
                        f("integrated", a.integratedLufs, b.integratedLufs), f("lra", a.loudnessRange, b.loudnessRange) })
            if (! s.empty()) return s;
        for (size_t i = 0; i < a.sources.size(); ++i)
        {
            const auto& x = a.sources[i];
            const auto& y = b.sources[i];
            const std::string tag = "source" + std::to_string(i) + ".";
            if (x.hasSpectrum != y.hasSpectrum) return tag + "hasSpectrum";
            if (std::memcmp(x.spectrumDb.data(), y.spectrumDb.data(), sizeof(float) * x.spectrumDb.size()) != 0) return tag + "spectrum";
            if (x.waveformColumnCount != y.waveformColumnCount) return tag + "waveformColumnCount";
            if (std::memcmp(x.waveformColumns.data(), y.waveformColumns.data(), sizeof(float) * 4 * (size_t) x.waveformColumnCount) != 0) return tag + "waveformColumns";
            if (x.gonioPairCount != y.gonioPairCount) return tag + "gonioPairCount";
            if (std::memcmp(x.gonio.data(), y.gonio.data(), sizeof(float) * 2 * (size_t) x.gonioPairCount) != 0) return tag + "gonio";
            if (x.scopeCount != y.scopeCount) return tag + "scopeCount";
            if (std::memcmp(x.scope.data(), y.scope.data(), sizeof(float) * (size_t) x.scopeCount) != 0) return tag + "scope";
            if (! bitsEq(x.truePeakMaxDb, y.truePeakMaxDb)) return tag + "truePeak";
            if (! bitsEq(x.samplePeakMaxDb, y.samplePeakMaxDb)) return tag + "samplePeak";
            if (x.hasStereo != y.hasStereo) return tag + "hasStereo";
            if (! bitsEq(x.correlation, y.correlation)) return tag + "correlation";
            if (! bitsEq(x.widthPercent, y.widthPercent)) return tag + "width";
            if (! bitsEq(x.balanceDb, y.balanceDb)) return tag + "balance";
        }
        return {};
    }

    // Rich deterministic multichannel programme: per-channel tones + noise, a slow level swell, one near-full-scale spike.
    std::vector<std::vector<float>> makeProgramme(int chs, std::size_t frames, double fs)
    {
        std::vector<std::vector<float>> d((size_t) chs, std::vector<float>(frames));
        for (int c = 0; c < chs; ++c)
            for (std::size_t i = 0; i < frames; ++i)
            {
                const double t = (double) i / fs;
                const double env = 0.35 + 0.25 * std::sin(kTwoPi * 0.9 * t);
                double v = env * std::sin(kTwoPi * (330.0 + 410.0 * c) * t) + 0.08 * std::sin(kTwoPi * (5100.0 + 700.0 * c) * t) + 0.04 * (double) hnoise(c, i);
                if (i == 12345) v = 0.95;
                d[(size_t) c][i] = (float) v;
            }
        return d;
    }

    // Property checks every published snapshot must satisfy.
    bool snapshotSane(const prism::AnalysisSnapshot& s, double expectedFs)
    {
        bool ok = s.sampleRate == expectedFs;
        auto loudOk = [](float v) { return std::isnan(v) || (v > -130.0f && v < 30.0f); };
        ok &= loudOk(s.momentaryLufs) && loudOk(s.shortTermLufs) && loudOk(s.integratedLufs) && (std::isnan(s.loudnessRange) || s.loudnessRange >= 0.0f);
        for (const auto& x : s.sources)
        {
            ok &= x.scopeCount >= 0 && x.scopeCount <= prism::kScopeSamples && x.gonioPairCount >= 0 && x.gonioPairCount <= prism::kGonioPairs;
            ok &= x.waveformColumnCount >= 0 && x.waveformColumnCount <= prism::kMaxWaveformColumns;
            for (int i = 0; i < x.waveformColumnCount; ++i)
            {
                const auto& c = x.waveformColumns[(size_t) i];
                ok &= c[0] >= 0.0f && c[0] <= 1.0f && c[1] >= 0.0f && c[1] <= 1.0f && c[2] >= 0.0f && c[2] <= 1.0f && (c[3] == 0.0f || c[3] == 1.0f || c[3] == 2.0f);
            }
            if (x.hasStereo)
                ok &= x.correlation >= -1.0f && x.correlation <= 1.0f && x.widthPercent >= 0.0f && x.widthPercent <= 100.0f && x.balanceDb >= -60.0f && x.balanceDb <= 60.0f;
            ok &= x.truePeakMaxDb >= x.samplePeakMaxDb;   // -INF >= -INF holds; NaN would fail
            if (x.hasSpectrum)
                for (float v : x.spectrumDb) ok &= std::isfinite(v) && v >= -120.0f && v <= 12.0f;
            for (int i = 0; i < x.scopeCount; ++i) ok &= std::isfinite(x.scope[(size_t) i]);
        }
        return ok;
    }
}

static void test_integration_processor_vs_synchronous_reference()
{
    section("integration_processor_vs_synchronous_reference");
    // The processor path (real processBlock, bus layout, channel offsets, ring, then ONE synchronous pump) must give
    // the same snapshot, bit for bit, as the reference path (engine fed block by block, pumped every block).
    // The worker is stopped (releaseResources) before audio so scheduling cannot change WHICH hops land in the
    // publish; the stream is shorter than the 65536-frame history so no hop is skipped by either path.
    struct Layout { int mainCh, scCh; };
    const Layout layouts[] = { { 1, 0 }, { 2, 0 }, { 2, 1 }, { 2, 2 }, { 1, 2 } };
    const std::size_t frames = 30000;

    int combos = 0;
    bool allSame = true;
    std::string firstBad;
    auto runCase = [&](double fs, Layout lay, int block) {
        const int chs = lay.mainCh + lay.scCh;
        const auto data = makeProgramme(chs, frames, fs);

        PRISMAudioProcessor proc;
        if (! configure(proc, lay.mainCh, lay.scCh, fs, block)) { allSame = false; firstBad = "configure"; return; }
        proc.prepareToPlay(fs, block);
        proc.releaseResources();
        juce::MidiBuffer midi;
        for (std::size_t pos = 0; pos < frames;)
        {
            const int n = (int) std::min<std::size_t>((std::size_t) block, frames - pos);
            float* ptrs[4];
            for (int c = 0; c < chs; ++c) ptrs[c] = const_cast<float*>(data[(size_t) c].data() + pos);
            juce::AudioBuffer<float> view(ptrs, chs, n);
            proc.processBlock(view, midi);
            pos += (std::size_t) n;
        }
        auto* eng = proc.getAnalysisEngine();
        eng->setSpectrumEnabled(true);
        eng->pump();
        const auto a = eng->buildSnapshot();

        prism::AnalysisEngine ref(makeCfg(lay.mainCh, lay.scCh, fs), 16);
        for (std::size_t pos = 0; pos < frames;)
        {
            const int n = (int) std::min<std::size_t>((std::size_t) block, frames - pos);
            const float* ptrs[4];
            for (int c = 0; c < chs; ++c) ptrs[c] = data[(size_t) c].data() + pos;
            ref.captureBlock(ptrs, chs, n);
            ref.pump();
            pos += (std::size_t) n;
        }
        const auto b = ref.buildSnapshot();

        const auto diff = diffSnapshots(*a, *b);
        ++combos;
        if (! diff.empty() || ! a->sources[0].hasSpectrum || ! snapshotSane(*a, fs))
        {
            allSame = false;
            if (firstBad.empty())
            {
                char buf[160];
                std::snprintf(buf, sizeof(buf), "fs %.0f main %d sc %d block %d: %s", fs, lay.mainCh, lay.scCh, block, diff.empty() ? "sanity/hasSpectrum" : diff.c_str());
                firstBad = buf;
            }
        }
    };

    for (double fs : { 44100.0, 48000.0, 96000.0, 192000.0 })
        for (const auto lay : layouts)
            for (int block : { 7, 480, 8192 })
                runCase(fs, lay, block);
    runCase(48000.0, { 2, 2 }, 1);
    runCase(96000.0, { 1, 0 }, 1);
    runCase(44100.0, { 2, 1 }, 64);
    runCase(192000.0, { 2, 2 }, 4096);

    char name[200];
    std::snprintf(name, sizeof(name), "%d configs (rates 44.1-192k, mono/stereo, sidechain off/mono/stereo, blocks 1..8192): processor snapshot bit-identical to the synchronous reference%s%s",
                  combos, firstBad.empty() ? "" : " -- first mismatch: ", firstBad.c_str());
    check(allSame, name);
}

static void test_integration_parameter_storm()
{
    section("integration_parameter_storm");
    // 6000 flips of fft_size / window / analysis_mode, one per 10 ms of audio = 60 s of simulated time, against the
    // live worker (editor active, snapshots fetched every block).
    PRISMAudioProcessor proc;
    check(configure(proc, 2, 0, 48000.0, 480), "configure");
    proc.prepareToPlay(48000.0, 480);
    proc.setEditorActive(true);

    auto* pSize = proc.apvts.getParameter(prism::ids::fftSize);
    auto* pWin = proc.apvts.getParameter(prism::ids::window);
    auto* pMode = proc.apvts.getParameter(prism::ids::analysisMode);

    const int k1024 = pickBin(48000.0, 1024, 2000.0);   // bin-centred for every FFT size
    auto tone = sineRat((std::uint64_t) k1024, 1024, 0.1f, 3);
    juce::AudioBuffer<float> buf(2, 480);
    juce::MidiBuffer midi;
    std::mt19937 rng(2027);

    long allocs = 0;
    bool valid = true;
    int withSpectrum = 0;
    std::uint64_t pos = 0, lastSeq = 0;
    bool monotonic = true;
    for (int flip = 0; flip < 6000; ++flip)
    {
        pSize->setValueNotifyingHost(pSize->convertTo0to1((float) (rng() % 5)));
        pWin->setValueNotifyingHost(pWin->convertTo0to1((float) (rng() % 2)));
        pMode->setValueNotifyingHost(pMode->convertTo0to1((float) (rng() % 3)));

        for (int i = 0; i < 480; ++i) { const float v = tone(0, pos + (std::uint64_t) i); buf.setSample(0, i, v); buf.setSample(1, i, v); }
        pos += 480;
        {
            ScopedAllocCount sc;
            proc.processBlock(buf, midi);
            allocs += sc.finish();
        }
        if ((flip & 3) == 3) sleepMs(1);
        const auto s = proc.getLatestAnalysisSnapshot();
        if (s != nullptr)
        {
            monotonic &= s->sequence >= lastSeq;
            lastSeq = s->sequence;
            valid &= snapshotSane(*s, 48000.0);
            withSpectrum += s->sources[0].hasSpectrum ? 1 : 0;
        }
    }
    checkf(allocs == 0, "processBlock allocated nothing during 6000 parameter flips", (double) allocs);
    check(valid && monotonic, "every snapshot during the storm is sane (finite spectra in [-120,12] dB, meters in range, sequence monotonic)");
    checkf(withSpectrum > 0, "valid spectra were published during the storm (fetches that carried hasSpectrum)", (double) withSpectrum);

    // After the storm: fixed settings, the spectrum must settle on the correct level.
    pSize->setValueNotifyingHost(pSize->convertTo0to1(2.0f));
    pWin->setValueNotifyingHost(pWin->convertTo0to1(0.0f));
    pMode->setValueNotifyingHost(pMode->convertTo0to1(2.0f));
    bool settled = false;
    const auto t0 = Clock::now();
    while (msSince(t0) < 4000.0 && ! settled)
    {
        for (int i = 0; i < 480; ++i) { const float v = tone(0, pos + (std::uint64_t) i); buf.setSample(0, i, v); buf.setSample(1, i, v); }
        pos += 480;
        proc.processBlock(buf, midi);
        sleepMs(4);
        const auto s = proc.getLatestAnalysisSnapshot();
        if (s != nullptr && s->sources[0].hasSpectrum && pos > 480 * 6200ull)
            settled = std::abs(curvePeak(s->sources[0]).db + 20.0) <= 0.15;   // a full-length 4096 window of steady tone
    }
    check(settled, "after the storm the 4096/Hann/combined spectrum reads the -20 dBFS tone correctly");
    proc.setEditorActive(false);
    proc.releaseResources();
}

static void test_integration_rate_and_layout_change()
{
    section("integration_rate_and_layout_change");
    PRISMAudioProcessor proc;
    juce::MidiBuffer midi;
    std::uint64_t lastSeq = 0;
    bool seqMonotonic = true, neverStale = true;
    double currentFs = 48000.0;

    auto play = [&](int mainCh, int scCh, double fs, double seconds, float amp) {
        juce::AudioBuffer<float> buf(mainCh + scCh, 480);
        const double w = kTwoPi * 997.0 / fs;
        std::uint64_t pos = 0;
        const auto t0 = Clock::now();
        std::shared_ptr<const prism::AnalysisSnapshot> last;
        while (msSince(t0) < seconds * 1000.0)
        {
            for (int i = 0; i < 480; ++i)
                for (int c = 0; c < mainCh + scCh; ++c) buf.setSample(c, i, amp * (float) std::sin(w * (double) (pos + (std::uint64_t) i)));
            pos += 480;
            proc.processBlock(buf, midi);
            const auto s = proc.getLatestAnalysisSnapshot();
            if (s != nullptr)
            {
                seqMonotonic &= s->sequence >= lastSeq;
                lastSeq = s->sequence;
                neverStale &= s->sampleRate == currentFs;
                last = s;
            }
            sleepMs(4);
        }
        return last;
    };

    check(configure(proc, 2, 0, 48000.0, 480), "configure 48k stereo");
    proc.prepareToPlay(48000.0, 480);
    proc.setEditorActive(true);
    auto s1 = play(2, 0, 48000.0, 1.8, 0.3f);
    check(s1 != nullptr && std::isfinite(s1->integratedLufs) && s1->sources[0].hasStereo && ! s1->sources[1].hasStereo, "session running: integrated loudness available, source 1 absent");
    const auto seq1 = lastSeq;
    auto* engBefore = proc.getAnalysisEngine();

    // Same configuration again (host re-prepare): nothing is reset.
    proc.prepareToPlay(48000.0, 256);
    check(proc.getAnalysisEngine() == engBefore && proc.getLatestAnalysisSnapshot() != nullptr, "re-prepare with the same rate/layout keeps the engine and the published snapshot");
    auto s1b = play(2, 0, 48000.0, 0.3, 0.3f);
    check(s1b != nullptr && std::isfinite(s1b->integratedLufs), "integrated loudness survives a same-config prepare");

    // Sample-rate change.
    proc.releaseResources();
    check(configure(proc, 2, 0, 96000.0, 480), "configure 96k stereo");
    currentFs = 96000.0;
    proc.prepareToPlay(96000.0, 480);
    check(proc.getAnalysisEngine() != engBefore && proc.getLatestAnalysisSnapshot() == nullptr, "rate change: engine rebuilt and the stale snapshot is gone immediately");
    auto s2 = play(2, 0, 96000.0, 0.25, 0.3f);   // < 400 ms of new audio
    check(s2 != nullptr && s2->sampleRate == 96000.0 && std::isnan(s2->integratedLufs) && s2->sequence > seq1,
          "first snapshot after the rate change: new rate, integrated reset (NaN), sequence continues upward");
    auto s2b = play(2, 0, 96000.0, 1.2, 0.3f);
    check(s2b != nullptr && std::isfinite(s2b->integratedLufs) && nearf(s2b->integratedLufs, 20.0 * std::log10(0.3) + 0.0, 2.0), "integrated loudness measures again at the new rate");

    // Layout change: enable the sidechain.
    proc.releaseResources();
    check(configure(proc, 2, 2, 96000.0, 480), "configure 96k stereo + sidechain");
    proc.prepareToPlay(96000.0, 480);
    check(proc.getLatestAnalysisSnapshot() == nullptr, "layout change: no stale snapshot");
    auto s3 = play(2, 2, 96000.0, 0.8, 0.2f);
    check(s3 != nullptr && s3->sources[1].hasStereo && s3->sources[1].scopeCount > 0, "layout change: sidechain source is analysed");

    // And back to mono.
    proc.releaseResources();
    check(configure(proc, 1, 0, 44100.0, 480), "configure 44.1k mono");
    currentFs = 44100.0;
    proc.prepareToPlay(44100.0, 480);
    auto s4 = play(1, 0, 44100.0, 0.8, 0.2f);
    check(s4 != nullptr && ! s4->sources[0].hasStereo && s4->sampleRate == 44100.0 && ! s4->sources[1].hasStereo && s4->sources[1].scopeCount == 0, "mono at 44.1 kHz: hasStereo false, no phantom source");

    check(seqMonotonic, "snapshot sequence never went backwards across three rebuilds");
    check(neverStale, "every snapshot ever fetched carries the sample rate that was active when it was fetched (nothing stale)");
    proc.setEditorActive(false);
    proc.releaseResources();
}

static void test_integration_editor_mode_transitions()
{
    section("integration_editor_mode_transitions");
    PRISMAudioProcessor proc;
    check(configure(proc, 2, 0, 48000.0, 480), "configure");
    proc.prepareToPlay(48000.0, 480);

    juce::AudioBuffer<float> buf(2, 480);
    juce::MidiBuffer midi;
    std::uint64_t pos = 0;
    auto feedFor = [&](double ms, bool fetch) {
        const auto t0 = Clock::now();
        while (msSince(t0) < ms)
        {
            for (int i = 0; i < 480; ++i)
            {
                const float v = 0.3f * std::sin(0.13f * (float) ((pos + (std::uint64_t) i) % 100000));
                buf.setSample(0, i, v); buf.setSample(1, i, v);
            }
            pos += 480;
            proc.processBlock(buf, midi);
            if (fetch) proc.getLatestAnalysisSnapshot();
            sleepMs(4);
        }
    };

    // 1) Editor closed: loudness / peaks keep running, nothing else does, nothing is published.
    feedFor(1200.0, true);   // fetching does not count: no editor is registered
    check(proc.getAnalysisWorker()->getPublishCount() == 0 && proc.getLatestAnalysisSnapshot() == nullptr, "editor closed: no snapshot built or published");
    proc.releaseResources();
    auto* eng = proc.getAnalysisEngine();
    eng->pump();
    check(eng->getSpectrum().getTotalHops(0) == 0 && eng->getWaveform(0).columnsLogged() == 0, "editor closed: no FFT hops and no waveform columns");
    check(std::isfinite(eng->getLoudness(0).momentaryLufs()) && eng->getHistory(0).framesSeen() == pos, "editor closed: loudness and capture keep running (every frame accounted for)");
    const float mBefore = eng->getLoudness(0).momentaryLufs();

    // 2) Editor opens: everything switches on.
    proc.prepareToPlay(48000.0, 480);
    proc.setEditorActive(true);
    feedFor(1500.0, true);
    const auto s = proc.getLatestAnalysisSnapshot();
    check(s != nullptr && s->sources[0].hasSpectrum && s->sources[0].waveformColumnCount > 0 && std::isfinite(s->integratedLufs), "editor open: spectrum, waveform columns and loudness are in the snapshot");
    check(std::abs(s->momentaryLufs - mBefore) < 0.5f, "loudness measured while the editor was closed is continuous with the open session (same signal, same level)");

    // 3) Editor closes (or stops fetching): publishing stops, audio keeps flowing.
    proc.setEditorActive(false);
    feedFor(300.0, false);
    const auto published = proc.getAnalysisWorker()->getPublishCount();
    feedFor(500.0, false);
    check(proc.getAnalysisWorker()->getPublishCount() == published, "editor closed again: publishing stops");

    // 4) Editor 'open' but no longer fetching: the heartbeat expires and the worker drops to inactive mode.
    proc.setEditorActive(true);
    feedFor(300.0, true);
    feedFor(900.0, false);   // > 500 ms without a fetch
    const auto published2 = proc.getAnalysisWorker()->getPublishCount();
    feedFor(400.0, false);
    check(proc.getAnalysisWorker()->getPublishCount() == published2, "stale fetch heartbeat: publishing stops without an explicit close");
    feedFor(300.0, true);
    check(proc.getAnalysisWorker()->getPublishCount() > published2, "fetching again resumes publishing");

    proc.setEditorActive(false);
    proc.releaseResources();
}

static void test_integration_reset_plumbing()
{
    section("integration_reset_plumbing");
    PRISMAudioProcessor proc;
    check(configure(proc, 2, 0, 48000.0, 480), "configure");
    proc.prepareToPlay(48000.0, 480);
    proc.setEditorActive(true);

    juce::AudioBuffer<float> loud(2, 480), quiet(2, 480);
    juce::MidiBuffer midi;
    for (int i = 0; i < 480; ++i) for (int c = 0; c < 2; ++c) { loud.setSample(c, i, 0.5f * std::sin(0.13f * (float) i)); quiet.setSample(c, i, 0.0f); }

    auto waitFor = [&](juce::AudioBuffer<float>& b, auto&& pred) {
        const auto t0 = Clock::now();
        while (msSince(t0) < 4000.0)
        {
            proc.processBlock(b, midi);
            const auto s = proc.getLatestAnalysisSnapshot();
            if (s != nullptr && pred(*s)) return true;
            sleepMs(3);
        }
        return false;
    };

    check(waitFor(loud, [](const prism::AnalysisSnapshot& s) { return std::isfinite(s.shortTermLufs) && std::isfinite(s.loudnessRange) && std::isfinite(s.sources[0].truePeakMaxDb); }),
          "primed: M/S/I/LRA, sample and true peak available");
    float tp0 = proc.getLatestAnalysisSnapshot()->sources[0].truePeakMaxDb;

    proc.requestResetIntegrated();
    check(waitFor(loud, [](const prism::AnalysisSnapshot& s) { return std::isnan(s.integratedLufs) && std::isnan(s.loudnessRange); }),
          "requestResetIntegrated(): published I and LRA go NaN");
    check(proc.getLatestAnalysisSnapshot()->sources[0].truePeakMaxDb >= tp0 - 0.001f && std::isfinite(proc.getLatestAnalysisSnapshot()->shortTermLufs),
          "...while true peak and the moving windows are untouched");
    check(waitFor(loud, [](const prism::AnalysisSnapshot& s) { return std::isfinite(s.integratedLufs); }), "...and integrated loudness restarts from new audio");

    // Quiet the input and let the pipeline drain BEFORE asking for the reset. A reset takes effect when the worker
    // reaches it; loud frames captured before the request but still in the ring when it fires are, by definition,
    // post-reset audio for the worker (true peak and the first 400 ms gating block are computed from the ring), so
    // asserting 'all cleared' while loud audio is still in flight would race against ~a block of pipeline latency.
    for (int i = 0; i < 30; ++i) { proc.processBlock(quiet, midi); proc.getLatestAnalysisSnapshot(); sleepMs(5); }
    check(std::isfinite(proc.getLatestAnalysisSnapshot()->integratedLufs) && std::isfinite(proc.getLatestAnalysisSnapshot()->sources[0].truePeakMaxDb),
          "silence does not clear anything by itself (I and peaks hold)");

    proc.requestResetAll();
    check(waitFor(quiet, [](const prism::AnalysisSnapshot& s) {
              return std::isnan(s.integratedLufs) && std::isnan(s.loudnessRange) && std::isinf(s.sources[0].samplePeakMaxDb) && s.sources[0].samplePeakMaxDb < 0
                     && std::isinf(s.sources[0].truePeakMaxDb) && s.sources[0].truePeakMaxDb < 0;
          }), "requestResetAll(): I, LRA, sample peak and true peak are all cleared in the published snapshot");

    // Measurements restart immediately from new audio: -INF / NaN only until data arrives.
    check(waitFor(loud, [](const prism::AnalysisSnapshot& s) { return std::isfinite(s.sources[0].samplePeakMaxDb) && std::isfinite(s.sources[0].truePeakMaxDb); }),
          "after the reset, sample and true peak restart from the next audio");
    proc.requestResetAll();
    check(waitFor(loud, [](const prism::AnalysisSnapshot& s) { return std::isnan(s.integratedLufs); }),
          "reset while a tone keeps playing: integrated shows NaN until its first post-reset gated block");

    proc.setEditorActive(false);
    proc.releaseResources();
}

static void test_integration_stress_all_engines()
{
    section("integration_stress_all_engines");
    double seconds = 2.0;
    if (const char* env = std::getenv("PRISM_STRESS_SECONDS")) seconds = std::max(0.2, std::atof(env));

    // Stereo + stereo sidechain, host playhead with beats, every analyzer live. Audio thread: random block sizes
    // 1..8192. Message thread: fetches + validates every snapshot, flips parameters, requests resets, opens/closes
    // the editor. (prepareToPlay / releaseResources never overlap processBlock: the host contract.)
    PRISMAudioProcessor proc;
    check(configure(proc, 2, 2, 48000.0, 8192), "configure");
    proc.prepareToPlay(48000.0, 8192);
    proc.setEditorActive(true);

    const auto programme = makeProgramme(4, 65536, 48000.0);
    std::atomic<bool> stop { false };
    std::atomic<long> audioBlocks { 0 };
    std::atomic<long> audioAllocs { 0 };
    std::atomic<bool> passthrough { true };
    FakePlayHead ph;

    std::thread audio([&] {
        juce::MidiBuffer midi;
        std::mt19937 rng(4242);
        std::vector<std::vector<float>> work(4, std::vector<float>(8192)), expect(4, std::vector<float>(8192));
        proc.setPlayHead(&ph);
        double ppq = 0.0;
        std::size_t pos = 0;
        while (! stop.load(std::memory_order_acquire))
        {
            const int n = 1 + (int) (rng() % 8192);
            for (int c = 0; c < 4; ++c)
            {
                for (int i = 0; i < n; ++i) work[(size_t) c][(size_t) i] = programme[(size_t) c][(pos + (std::size_t) i) % programme[0].size()];
                std::memcpy(expect[(size_t) c].data(), work[(size_t) c].data(), (size_t) n * sizeof(float));
            }
            pos += (std::size_t) n;
            float* ptrs[4] = { work[0].data(), work[1].data(), work[2].data(), work[3].data() };
            juce::AudioBuffer<float> view(ptrs, 4, n);
            ph.ppq = ppq;
            ppq += (double) n * 2.0 / 48000.0;
            long a;
            {
                ScopedAllocCount sc;
                proc.processBlock(view, midi);
                a = sc.finish();
            }
            audioAllocs.fetch_add(a);
            for (int c = 0; c < 4; ++c)
                if (std::memcmp(work[(size_t) c].data(), expect[(size_t) c].data(), (size_t) n * sizeof(float)) != 0)
                    passthrough.store(false);
            audioBlocks.fetch_add(1);
            if ((rng() & 7) == 0) std::this_thread::yield();
        }
        proc.setPlayHead(nullptr);
    });

    auto* pSize = proc.apvts.getParameter(prism::ids::fftSize);
    auto* pWin = proc.apvts.getParameter(prism::ids::window);
    auto* pMode = proc.apvts.getParameter(prism::ids::analysisMode);
    std::mt19937 rng(77);
    std::uint64_t prev = 0;
    bool monotonic = true, sane = true;
    long fetches = 0, distinct = 0;
    const auto t0 = Clock::now();
    while (msSince(t0) < seconds * 1000.0)
    {
        const auto s = proc.getLatestAnalysisSnapshot();
        ++fetches;
        if (s != nullptr)
        {
            monotonic &= s->sequence >= prev;
            if (s->sequence != prev) ++distinct;
            prev = s->sequence;
            sane &= snapshotSane(*s, 48000.0);
        }
        switch (fetches % 40)
        {
            case 3:  pSize->setValueNotifyingHost(pSize->convertTo0to1((float) (rng() % 5))); break;
            case 9:  pWin->setValueNotifyingHost(pWin->convertTo0to1((float) (rng() % 2))); break;
            case 15: pMode->setValueNotifyingHost(pMode->convertTo0to1((float) (rng() % 3))); break;
            case 21: proc.requestResetIntegrated(); break;
            case 27: proc.requestResetAll(); break;
            case 33: proc.setEditorActive(false); proc.setEditorActive(true); break;
            default: break;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(300));
    }
    stop.store(true, std::memory_order_release);
    audio.join();
    proc.setEditorActive(false);
    proc.releaseResources();

    check(passthrough.load(), "audio thread: output bit-identical to input for every block (host buffer incl. sidechain channels untouched)");
    check(monotonic, "snapshot sequence never goes backwards under concurrency");
    check(sane, "every fetched snapshot satisfies the range / invariant checks (TP >= SP, meters in range, finite spectra, <= 16 columns)");
    checkf(audioAllocs.load() == 0, "audio thread: zero allocations across the whole run (all engines, parameter flips, resets)", (double) audioAllocs.load());
    checkf(audioBlocks.load() > 20 && distinct > 5, "blocks processed / distinct snapshots", (double) audioBlocks.load(), (double) distinct);
    std::printf("      %ld blocks (1..8192 frames), %ld fetches, %ld distinct snapshots, overruns source0=%llu\n", audioBlocks.load(), fetches, distinct,
                (unsigned long long) proc.getAnalysisEngine()->getOverrunCount(0));
}

static void test_integration_profile_and_memory()
{
    section("integration_profile_and_memory");
    // Worker CPU = time spent in pump() (+ buildSnapshot() once per 1/60 s while the editor is open) per second of audio,
    // measured single-threaded so scheduling noise does not enter. Capture (the audio thread's job) is excluded.
    std::printf("      PROFILE|fs|sources|editor|cpu_percent\n");
    bool keepsUp = true;
    const double seconds = 5.0;
    struct Row { double fs; int sources; bool open; double cpu; };
    std::vector<Row> rows;

    for (double fs : { 44100.0, 48000.0, 96000.0, 192000.0 })
        for (int sources : { 1, 2 })
            for (bool open : { false, true })
            {
                prism::AnalysisEngine eng(makeCfg(2, sources == 2 ? 2 : 0, fs), 16);
                eng.setSpectrumEnabled(open);
                const int chs = sources == 2 ? 4 : 2;
                std::vector<std::vector<float>> bufs((size_t) chs, std::vector<float>(4096));
                for (int c = 0; c < chs; ++c)
                    for (int i = 0; i < 4096; ++i)
                        bufs[(size_t) c][(size_t) i] = 0.25f * std::sin(0.037f * (float) i * (float) (c + 1)) + 0.1f * hnoise(c, (std::uint64_t) i);
                const float* ptrs[4] = { bufs[0].data(), bufs[1].data(), bufs.back().data(), bufs.back().data() };
                if (chs == 4) { ptrs[2] = bufs[2].data(); ptrs[3] = bufs[3].data(); }

                const int blocks = (int) (seconds * fs / 4096.0);
                const double buildEvery = fs / 60.0;
                double sinceBuild = 0.0, busyMs = 0.0;
                for (int b = 0; b < blocks; ++b)
                {
                    eng.captureBlock(ptrs, chs, 4096);
                    const auto t0 = Clock::now();
                    eng.pump();
                    sinceBuild += 4096.0;
                    if (open)
                        while (sinceBuild >= buildEvery) { (void) eng.buildSnapshot(); sinceBuild -= buildEvery; }
                    busyMs += msSince(t0);
                }
                const double audioMs = 1000.0 * blocks * 4096.0 / fs;
                const double cpu = 100.0 * busyMs / audioMs;
                rows.push_back({ fs, sources, open, cpu });
                keepsUp &= cpu < 100.0;
                std::printf("      PROFILE|%.0f|%d|%s|%.3f\n", fs, sources, open ? "open" : "closed", cpu);
            }

    // Budgets (WARN-only): open < 3 % @48k, < 8 % @192k; closed < 0.5 % @48k (scaled with the sample rate elsewhere).
    for (const auto& r : rows)
    {
        const double budget = r.open ? (r.fs <= 48000.0 ? 3.0 : (r.fs >= 192000.0 ? 8.0 : 5.0)) : 0.5 * std::max(1.0, r.fs / 48000.0);
        if (r.cpu > budget)
            std::printf("      WARN: %.0f Hz, %d source(s), editor %s: %.3f %% exceeds the %.1f %% budget (debug build or loaded machine?)\n", r.fs, r.sources, r.open ? "open" : "closed", r.cpu, budget);
    }
    check(keepsUp, "worker keeps up with real time in every cell of the {44.1,48,96,192k} x {1,2 sources} x {open,closed} matrix (budgets are WARN-only)");

    {   // heaviest FFT configuration: N=1024 (hop 256), combined, two stereo sources, 192 kHz, editor open
        std::atomic<float> size { 0.0f }, win { 0.0f }, mode { 2.0f };
        prism::AnalysisEngine eng(makeCfg(2, 2, 192000.0), 16);
        eng.attachControls({ nullptr, nullptr, &mode, &size, &win });
        std::vector<std::vector<float>> bufs(4, std::vector<float>(4096));
        for (int c = 0; c < 4; ++c) for (int i = 0; i < 4096; ++i) bufs[(size_t) c][(size_t) i] = 0.2f * hnoise(c, (std::uint64_t) i);
        const float* ptrs[4] = { bufs[0].data(), bufs[1].data(), bufs[2].data(), bufs[3].data() };
        const int blocks = (int) (seconds * 192000.0 / 4096.0);
        double busyMs = 0.0, sinceBuild = 0.0;
        for (int b = 0; b < blocks; ++b)
        {
            eng.captureBlock(ptrs, 4, 4096);
            const auto t0 = Clock::now();
            eng.pump();
            sinceBuild += 4096.0;
            while (sinceBuild >= 192000.0 / 60.0) { (void) eng.buildSnapshot(); sinceBuild -= 192000.0 / 60.0; }
            busyMs += msSince(t0);
        }
        const double cpu = 100.0 * busyMs / (1000.0 * blocks * 4096.0 / 192000.0);
        std::printf("      PROFILE|192000|2|open_fft1024|%.3f\n", cpu);
        check(cpu < 100.0, "worst case (192 kHz, 2 sources, N=1024 hop 256, editor open) still keeps up with real time");
    }

    // Memory: bytes requested through operator new while building a 2-source engine and running it long enough to touch
    // every lazily allocated structure, plus a handful of live snapshots.
    {
        long long bytes;
        {
            ScopedAllocCount sc;
            auto eng = std::make_unique<prism::AnalysisEngine>(makeCfg(2, 2, 192000.0));   // default 2^18-frame rings
            std::vector<std::vector<float>> bufs(4, std::vector<float>(4096, 0.1f));
            const float* ptrs[4] = { bufs[0].data(), bufs[1].data(), bufs[2].data(), bufs[3].data() };
            for (int b = 0; b < 60; ++b) { eng->captureBlock(ptrs, 4, 4096); eng->pump(); }
            std::vector<std::shared_ptr<const prism::AnalysisSnapshot>> live;
            for (int i = 0; i < 4; ++i) live.push_back(eng->buildSnapshot());   // mailbox + editor + worker + spare
            bytes = sc.bytes();
            sc.finish();
        }
        const double mb = (double) bytes / (1024.0 * 1024.0);
        std::printf("      PROFILE|memory_mb|2_sources_192k_engine+4_snapshots|%.2f\n", mb);
        checkf(mb < 10.0, "engine footprint (2 sources, default rings, spectrum/loudness/waveform state, 4 live snapshots) < 10 MB (MB)", mb);
    }
}

//==============================================================================
int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    std::printf("PRISM tests  (Stage 2 / Phase 2.7)\n");

    test_ring_wraparound();
    test_ring_overrun_drops_whole_blocks();
    test_ring_spsc_stress_no_torn_frames();
    test_beat_event_queue();
    test_stream_history();
    test_source_registry();
    test_engine_capture_and_block_size_independence();
    test_engine_mono_and_inactive_sources();
    test_engine_peak_lossless_across_overrun();
    test_engine_non_finite_input();
    test_engine_beat_events();
    test_snapshot_sequence_sync();
    test_mailbox_semantics();
    test_mailbox_concurrent_nonblocking();
    test_destruction_while_snapshot_held();
    test_processblock_zero_allocations();
    test_passthrough_bit_exact();
    test_end_to_end_publish_and_gating();
    test_lifecycle_rebuild_rules();
    test_worker_start_stop_1000x();
    test_stress_audio_worker_message_threads();

    // Phase 2.2
    test_stereo_metrics_definitions();
    test_sample_peak_snapshot();
    test_reset_all_epoch();
    test_scope_window();
    test_scope_analysis_mode_channel();
    test_gonio_pairs();
    test_mono_and_absent_sources_snapshot();
    test_signal_present();
    test_processor_peak_reset_end_to_end();

    // Phase 2.3
    test_spectrum_basics_and_map();
    test_spectrum_hop_timing();
    test_spectrum_bin_centred_accuracy();
    test_spectrum_scalloping();
    test_spectrum_coherent_gain_equality();
    test_spectrum_white_noise_density();
    test_spectrum_impulse();
    test_spectrum_multitone_leakage();
    test_spectrum_analysis_modes();
    test_spectrum_silence_and_warmup();
    test_spectrum_block_size_bit_identity();
    test_spectrum_parameter_switching();
    test_spectrum_enable_gating_and_processor_wiring();
    test_spectrum_cpu_budget();

    // Phase 2.4
    test_loudness_kweighting();
    test_loudness_tech3341_case1_and_mono();
    test_loudness_availability();
    test_loudness_gating_absolute();
    test_loudness_gating_relative();
    test_loudness_range();
    test_loudness_block_size_independence();
    test_loudness_reset_semantics();
    test_loudness_gap_handling();
    test_loudness_sources_and_publish();
    test_loudness_multi_hour_memory();
    test_loudness_golden_fixtures();

    // Phase 2.5
    test_true_peak_filter_response();
    test_true_peak_fs4_sine();
    test_true_peak_phase_and_level_sweep();
    test_true_peak_invariant_random();
    test_true_peak_hold_reset_and_sources();
    test_true_peak_cpu_budget();

    // Phase 2.6
    test_waveform_column_rate();
    test_waveform_band_separation();
    test_waveform_beat_flags();
    test_waveform_ack_delivery();
    test_waveform_editor_gating_and_wiring();

    // Phase 2.7
    test_integration_processor_vs_synchronous_reference();
    test_integration_parameter_storm();
    test_integration_rate_and_layout_change();
    test_integration_editor_mode_transitions();
    test_integration_reset_plumbing();
    test_integration_stress_all_engines();
    test_integration_profile_and_memory();

    std::printf("\n%d checks, %d failure(s), %zu test cases\n", checks, failures, testNames.size());
    return failures == 0 ? 0 : 1;
}
