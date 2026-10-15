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
#include "Capture/BeatEventQueue.h"
#include "Capture/SpscFrameRing.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

    inline void noteAlloc() noexcept
    {
        if (tlsCountAllocs != 0)
            gAllocCount.fetch_add(1, std::memory_order_relaxed);
    }

    void* rawAlloc(std::size_t n)
    {
        noteAlloc();
        if (void* p = std::malloc(n != 0 ? n : 1))
            return p;
        throw std::bad_alloc();
    }

    void* rawAllocAligned(std::size_t n, std::size_t align)
    {
        noteAlloc();
        void* p = nullptr;
        if (align < sizeof(void*)) align = sizeof(void*);
        if (posix_memalign(&p, align, n != 0 ? n : 1) != 0)
            throw std::bad_alloc();
        return p;
    }

    struct ScopedAllocCount
    {
        ScopedAllocCount()  { start = gAllocCount.load(); ++tlsCountAllocs; }
        ~ScopedAllocCount() { if (open) --tlsCountAllocs; }
        long finish()       { --tlsCountAllocs; open = false; return gAllocCount.load() - start; }
        long start = 0;
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
int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    std::printf("PRISM tests  (Stage 2 / Phase 2.1)\n");

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

    std::printf("\n%d checks, %d failure(s), %zu test cases\n", checks, failures, testNames.size());
    return failures == 0 ? 0 : 1;
}
