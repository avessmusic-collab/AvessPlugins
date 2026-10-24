// CHOPLINE tests — SnapshotExchange swap safety (spec §3, §8): a retired
// snapshot must stay alive until the audio thread has completed a block on a
// newer one, and while a voice-style predicate still claims it.

#include <catch2/catch_test_macros.hpp>

#include "engine/SnapshotExchange.h"

namespace
{
    struct Tracked
    {
        explicit Tracked (int* counter) : destroyed (counter) {}
        ~Tracked() { ++(*destroyed); }
        int* destroyed;
    };
} // namespace

TEST_CASE ("Retired snapshots are freed only after the audio thread moves on",
           "[snapshot]")
{
    int destroyedA = 0, destroyedB = 0;
    chopline::engine::SnapshotExchange<Tracked> exchange;

    exchange.publish (std::make_shared<Tracked> (&destroyedA)); // gen 1
    const auto* nodeA = exchange.acquire();
    REQUIRE (nodeA != nullptr);

    // "Audio" is mid-block on A when B is published.
    exchange.publish (std::make_shared<Tracked> (&destroyedB)); // gen 2
    exchange.collectGarbage();
    REQUIRE (destroyedA == 0); // no ack on a newer generation yet

    exchange.ack (nodeA); // block that used A completes
    exchange.collectGarbage();
    REQUIRE (destroyedA == 0); // acked == gen(A), still not strictly newer

    const auto* nodeB = exchange.acquire();
    REQUIRE (nodeB != nodeA);
    exchange.ack (nodeB); // block completed on B
    exchange.collectGarbage();
    REQUIRE (destroyedA == 1); // A can never be loaded again — freed
    REQUIRE (destroyedB == 0); // current snapshot stays

    REQUIRE (exchange.numAlive() == 1);
}

TEST_CASE ("stillInUse predicate vetoes reclamation (voice retention)", "[snapshot]")
{
    int destroyedA = 0, destroyedB = 0;
    chopline::engine::SnapshotExchange<Tracked> exchange;

    exchange.publish (std::make_shared<Tracked> (&destroyedA));
    const auto* retained = exchange.current().get(); // what a voice would hold
    const auto* nodeA = exchange.acquire();
    exchange.ack (nodeA);

    exchange.publish (std::make_shared<Tracked> (&destroyedB));
    const auto* nodeB = exchange.acquire();
    exchange.ack (nodeB);

    // A voice still reads A: must survive.
    exchange.collectGarbage ([retained] (const Tracked* t) { return t == retained; });
    REQUIRE (destroyedA == 0);

    // Voice finished: A is reclaimed.
    exchange.collectGarbage ([] (const Tracked*) { return false; });
    REQUIRE (destroyedA == 1);
    REQUIRE (destroyedB == 0);
}

TEST_CASE ("Exchange with no audio activity never frees, dies cleanly", "[snapshot]")
{
    int destroyed = 0;
    {
        chopline::engine::SnapshotExchange<Tracked> exchange;
        exchange.publish (std::make_shared<Tracked> (&destroyed));
        exchange.publish (std::make_shared<Tracked> (&destroyed));
        exchange.publish (std::make_shared<Tracked> (&destroyed));
        exchange.collectGarbage();
        REQUIRE (destroyed == 0); // audio never ran — nothing reclaimed
        REQUIRE (exchange.numAlive() == 3);
    }
    REQUIRE (destroyed == 3); // destructor frees everything
}
