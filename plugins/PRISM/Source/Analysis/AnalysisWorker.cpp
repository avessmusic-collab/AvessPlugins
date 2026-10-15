#include "AnalysisWorker.h"

#include <chrono>

namespace prism
{
    AnalysisWorker::AnalysisWorker(AnalysisEngine& e, SnapshotMailbox& m, WorkerControls c)
        : juce::Thread("PRISM Analysis"), engine(e), mailbox(m), ctl(c)
    {
    }

    AnalysisWorker::~AnalysisWorker()
    {
        stop();
    }

    void AnalysisWorker::start()
    {
        if (! isThreadRunning())
            startThread(Priority::low);   // normal - 1
    }

    void AnalysisWorker::stop()
    {
        if (isThreadRunning())
            stopThread(2000);   // signalThreadShouldExit() + notify + join
    }

    bool AnalysisWorker::isEditorActive(const WorkerControls& c, std::int64_t nowMs) noexcept
    {
        if (c.editorActiveCount == nullptr || c.editorActiveCount->load(std::memory_order_relaxed) <= 0)
            return false;
        if (c.lastFetchTimeMs == nullptr)
            return true;
        return nowMs - c.lastFetchTimeMs->load(std::memory_order_relaxed) <= kFetchHeartbeatMs;
    }

    void AnalysisWorker::run()
    {
        juce::ScopedNoDenormals noDenormals;   // FTZ/DAZ for the worker's whole run loop

        using clock = std::chrono::steady_clock;
        const auto toMs = [](clock::time_point t) {
            return std::chrono::duration<double, std::milli>(t.time_since_epoch()).count();
        };

        double nextPublishMs = toMs(clock::now());

        while (! threadShouldExit())
        {
            engine.pump();
            cycleCount.fetch_add(1, std::memory_order_relaxed);

            const bool active = isEditorActive(ctl, steadyNowMs());
            if (active)
            {
                const double now = toMs(clock::now());
                if (now >= nextPublishMs)
                {
                    // Drift-corrected cadence; resync if we fell more than 4 periods behind.
                    nextPublishMs += kPublishPeriodMs;
                    if (now - nextPublishMs > 4.0 * kPublishPeriodMs)
                        nextPublishMs = now + kPublishPeriodMs;

                    // Only publish when new frames arrived (transport stopped -> no new sequence).
                    if (engine.takeNewFramesFlag())
                    {
                        mailbox.publish(engine.buildSnapshot());
                        publishCount.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }
            else
            {
                engine.takeNewFramesFlag();               // nothing is built while inactive
                nextPublishMs = toMs(clock::now());       // publish promptly when the editor (re)opens
            }

            wait(active ? kActivePollMs : kIdlePollMs);
        }
    }
} // namespace prism
