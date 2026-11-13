#pragma once
/*
  CpuGuard - the opt-in CPU guard behind the Settings dialog (architecture "Voice budget governor": a "CPU guard"
  (off by default) lowers OS for NEW notes if the 1 s EMA of block-time / block-duration exceeds 85 %).

  This class is the pure, deterministic decision logic.  It owns no clock: the caller measures how long a block took
  (PluginProcessor: juce::Time::getHighResolutionTicks, allocation-free) and feeds the ratio
  load = processing seconds / block seconds once per block.  The same load series therefore always produces the same
  levels (tests drive it with a synthetic series; PluginProcessor has a test-only injection hook).

      smoothed   one-pole EMA of the load, time constant `smoothSeconds` (1 s), exact for any block length:
                 smoothed += (load - smoothed) * (1 - exp(-blockSeconds / smoothSeconds))
      level      0..3.  Escalates by ONE when smoothed > engageLoad (0.85) and at least `upHoldSeconds` (0.25 s)
                 have passed since the last change; de-escalates by ONE when smoothed < releaseLoad (0.60) and at
                 least `downHoldSeconds` (2 s) have passed since the last change.  The hysteresis band and the holds
                 keep the guard from chattering (and from reacting to a single preempted block).
      effect     level L caps the oversampling factor LATCHED AT NOTE-ON of new notes to osCap(L) = {8, 4, 2, 1}.
                 It never touches a sounding voice (WarprSynth::startNewVoice is the only consumer), and it never
                 steals or drops a note.  The guard deliberately overrides the warp / shaper minOS floors: under a
                 CPU overload an aliasing note beats a dropout.

  Escalation after a sustained overload (load constant 1.0 at 48 kHz / 256): smoothed crosses 0.85 after
  ln(1/0.15) = 1.897 s (block 356), then one level per 0.25 s (blocks 403 and 450).  Recovery needs 2 s per level.
*/

#include <algorithm>
#include <cmath>

namespace warpr
{
    class CpuGuard
    {
    public:
        static constexpr int maxLevel = 3;

        struct Config
        {
            double engageLoad = 0.85;
            double releaseLoad = 0.60;
            double smoothSeconds = 1.0;
            double upHoldSeconds = 0.25;
            double downHoldSeconds = 2.0;
        };

        CpuGuard() noexcept { reset(); }
        explicit CpuGuard (const Config& configToUse) noexcept : config (configToUse) { reset(); }

        void reset() noexcept
        {
            smoothedLoad = 0.0;
            currentLevel = 0;
            sinceChange = 1.0e9;                  // the first escalation is not held back
        }

        /** One block.  `load` = processing time / block duration (non-finite counts as 0, clamped to 0..8);
            `blockSeconds` = block duration.  Returns the new level. */
        int update (double load, double blockSeconds) noexcept
        {
            if (! (blockSeconds > 0.0))
                return currentLevel;

            if (! std::isfinite (load))
                load = 0.0;

            load = std::min (8.0, std::max (0.0, load));

            smoothedLoad += (load - smoothedLoad) * (1.0 - std::exp (-blockSeconds / config.smoothSeconds));
            sinceChange += blockSeconds;

            if (currentLevel < maxLevel && smoothedLoad > config.engageLoad && sinceChange >= config.upHoldSeconds)
            {
                ++currentLevel;
                sinceChange = 0.0;
            }
            else if (currentLevel > 0 && smoothedLoad < config.releaseLoad && sinceChange >= config.downHoldSeconds)
            {
                --currentLevel;
                sinceChange = 0.0;
            }

            return currentLevel;
        }

        int level() const noexcept            { return currentLevel; }
        double smoothed() const noexcept      { return smoothedLoad; }
        const Config& getConfig() const noexcept { return config; }

        /** Oversampling cap for new notes at `level` (0 = no cap). */
        static constexpr int osCap (int level) noexcept
        {
            return level <= 0 ? 8 : (level == 1 ? 4 : (level == 2 ? 2 : 1));
        }

    private:
        Config config;
        double smoothedLoad = 0.0;
        int currentLevel = 0;
        double sinceChange = 1.0e9;
    };
}
