// CHOPLINE — SnapshotExchange (spec §3, §8)
// Lock-free single-producer snapshot publication from the message thread to
// the audio thread, with deferred reclamation on the message thread.
//
// Algorithm: each published snapshot is wrapped in a heap Node carrying a
// monotonically increasing generation. The audio thread acquires the current
// Node at block start and acks its generation at block end. A retired Node is
// freed on the message thread only once the audio thread has COMPLETED a
// block on a strictly newer generation (so it can never load the old pointer
// again) and an optional predicate ("a voice still reads this sample")
// returns false. If the audio thread never runs, retired nodes simply
// accumulate (bounded by the number of publishes) and die with the exchange.

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace chopline::engine
{

template <typename T>
class SnapshotExchange
{
public:
    struct Node
    {
        std::shared_ptr<const T> object;
        std::uint64_t generation = 0;
    };

    SnapshotExchange() = default;
    ~SnapshotExchange() = default; // the audio thread must be stopped by now

    // ---- message thread ---------------------------------------------------

    void publish (std::shared_ptr<const T> next)
    {
        auto node = std::make_unique<Node>();
        node->object = std::move (next);
        node->generation = nextGeneration++;
        published.store (node.get(), std::memory_order_release);
        nodes.push_back (std::move (node));
        collectGarbage();
    }

    /// Latest published snapshot (message-thread view). May be null.
    std::shared_ptr<const T> current() const
    {
        return nodes.empty() ? nullptr : nodes.back()->object;
    }

    /// Frees retired snapshots the audio thread can no longer reach.
    /// stillInUse lets the caller veto reclamation (e.g. a voice tail still
    /// reading an old sample buffer).
    void collectGarbage (const std::function<bool (const T*)>& stillInUse = {})
    {
        if (nodes.size() <= 1)
            return;

        const auto acked = ackedGeneration.load (std::memory_order_acquire);

        for (size_t i = 0; i + 1 < nodes.size();) // never the newest node
        {
            auto& n = nodes[i];
            const bool audioDone = acked > n->generation;
            const bool retained = stillInUse != nullptr
                               && n->object != nullptr
                               && stillInUse (n->object.get());
            if (audioDone && ! retained)
                nodes.erase (nodes.begin() + static_cast<long> (i));
            else
                ++i;
        }
    }

    /// Number of snapshots currently kept alive (newest + retired). Test aid.
    size_t numAlive() const noexcept { return nodes.size(); }

    // ---- audio thread -----------------------------------------------------

    /// Grab once at block start; use node->object.get() for the whole block.
    const Node* acquire() const noexcept
    {
        return published.load (std::memory_order_acquire);
    }

    /// Call at block end with the node acquired at block start.
    void ack (const Node* node) noexcept
    {
        if (node != nullptr)
            ackedGeneration.store (node->generation, std::memory_order_release);
    }

private:
    std::atomic<const Node*> published { nullptr };
    std::atomic<std::uint64_t> ackedGeneration { 0 };
    std::uint64_t nextGeneration = 1;
    std::vector<std::unique_ptr<Node>> nodes; // message thread only

    SnapshotExchange (const SnapshotExchange&) = delete;
    SnapshotExchange& operator= (const SnapshotExchange&) = delete;
};

} // namespace chopline::engine
