#pragma once

#include <juce_core/juce_core.h>

#include <atomic>
#include <cstdint>

#include "AnalysisEngine.h"
#include "SnapshotMailbox.h"

namespace prism
{
    // Pointers into the processor's atomics. Lifetime: the processor outlives the worker.
    struct WorkerControls
    {
        const std::atomic<int>*          editorActiveCount = nullptr;   // > 0 while an editor exists
        const std::atomic<std::int64_t>* lastFetchTimeMs   = nullptr;   // steadyNowMs() of the last fetch / editor-open
    };

    // Thin scheduler around AnalysisEngine::pump()/buildSnapshot() (architecture C3).
    class AnalysisWorker : public juce::Thread
    {
    public:
        static constexpr int    kActivePollMs   = 4;
        static constexpr int    kIdlePollMs     = 20;
        static constexpr double kPublishPeriodMs = 1000.0 / 60.0;
        static constexpr std::int64_t kFetchHeartbeatMs = 500;

        AnalysisWorker(AnalysisEngine& engineToDrive, SnapshotMailbox& mailboxToFill, WorkerControls controls);
        ~AnalysisWorker() override;

        void start();   // idempotent
        void stop();    // signal + join (<= 2 s); idempotent

        // Editor counts as active when it exists and fetched (or opened) within the heartbeat window.
        static bool isEditorActive(const WorkerControls& c, std::int64_t nowMs) noexcept;

        std::uint64_t getPublishCount() const noexcept { return publishCount.load(std::memory_order_relaxed); }
        std::uint64_t getCycleCount() const noexcept   { return cycleCount.load(std::memory_order_relaxed); }

        void run() override;

    private:
        AnalysisEngine& engine;
        SnapshotMailbox& mailbox;
        WorkerControls ctl;
        std::atomic<std::uint64_t> publishCount { 0 };
        std::atomic<std::uint64_t> cycleCount { 0 };
    };
} // namespace prism
