#pragma once
/*
  AmpEnvelope - the single amp ADSR (architecture C10 / section H).

    Attack  : LINEAR from the CURRENT level to 1 over `attackMs` (retrigger and early re-attack are
              click-free because they start from wherever the level is).
    Decay   : exponential toward the sustain level, time constant tau = D / ln(1000): the level has
              fallen by 60 dB after D ms (the editor draws straight segments; the audible curve is the
              analog-style exponential - architecture ambiguity A12).
    Sustain : level follows a 10 ms slew of the sustain target.
    Release : exponential from the CURRENT level, 60 dB after R ms.  A note-off during the attack
              releases from the current level (no snap).
    Idle    : entered when the level falls below -90 dB in release, or in decay/sustain with a
              sustain target at -90 dB or lower (a zero-sustain "pluck" frees its voice).

  Header-only, allocation-free.  Call setParams() once per chunk, then process().
*/

#include <algorithm>
#include <cmath>

namespace warpr
{
    class AmpEnvelope
    {
    public:
        enum class State { Idle, Attack, Decay, Sustain, Release };

        static constexpr float  floorLevel = 3.1622777e-5f;       // -90 dB
        static constexpr double ln1000     = 6.907755278982137;   // 60 dB as a natural-log ratio

        void prepare (double newSampleRate) noexcept
        {
            sampleRate = newSampleRate;
            susCoef = (float) (1.0 - std::exp (-1.0 / (0.010 * newSampleRate)));
            reset();
        }

        void reset() noexcept
        {
            state = State::Idle;
            level = 0.0;
            attackRemaining = 0;
            attackInc = 0.0f;
            fast = false;
            sus = susTarget = 0.0f;
        }

        /** Per chunk: -60 dB times and the sustain target. */
        void setParams (float decayMs, float sustainTarget, float releaseMs) noexcept
        {
            decayCoef = coefFor (decayMs);
            relCoef = coefFor (releaseMs);
            susTarget = std::max (0.0f, std::min (1.0f, sustainTarget));
        }

        /** Jump the sustain follower to its target (voice start, tests). */
        void snapSustain() noexcept { sus = susTarget; }

        /** Start / retrigger the attack from the current level. */
        void noteOn (float attackMs) noexcept
        {
            fast = false;
            attackRemaining = std::max (1, (int) std::lround (attackMs * 0.001 * sampleRate));

            if (level >= 1.0f)
            {
                level = 1.0f;
                state = State::Decay;
                return;
            }

            attackInc = (1.0 - level) / (double) attackRemaining;
            state = State::Attack;
        }

        void noteOff() noexcept
        {
            fast = false;
            if (state != State::Idle)
                state = State::Release;
        }

        /** Release over a fixed short time (One-Shot end, voice mode changes), ignoring the release parameter. */
        void fastRelease (float ms) noexcept
        {
            fastCoef = coefFor (ms);
            fast = true;
            if (state != State::Idle)
                state = State::Release;
        }

        State getState() const noexcept { return state; }
        float getLevel() const noexcept { return (float) level; }
        bool  isActive() const noexcept { return state != State::Idle; }

        void process (float* out, int n) noexcept
        {
            for (int i = 0; i < n; ++i)
            {
                sus += (susTarget - sus) * susCoef;

                switch (state)
                {
                    case State::Idle:
                        level = 0.0f;
                        break;

                    case State::Attack:
                        level += attackInc;
                        if (--attackRemaining <= 0 || level >= 1.0f)
                        {
                            level = 1.0f;
                            state = State::Decay;
                        }
                        break;

                    case State::Decay:
                        level = sus + (level - sus) * decayCoef;
                        if (std::abs (level - sus) < 1.0e-5f)
                        {
                            level = sus;
                            state = State::Sustain;
                        }
                        break;

                    case State::Sustain:
                        level = sus;
                        break;

                    case State::Release:
                        level *= fast ? fastCoef : relCoef;
                        break;
                }

                if (state == State::Release)
                {
                    if (level < floorLevel)
                    {
                        level = 0.0f;
                        state = State::Idle;
                    }
                }
                else if ((state == State::Decay || state == State::Sustain) && level <= floorLevel && sus <= floorLevel)
                {
                    level = 0.0f;
                    state = State::Idle;
                }

                out[i] = (float) level;
            }
        }

    private:
        float coefFor (float ms) const noexcept
        {
            const double samples = std::max (1.0, (double) ms * 0.001 * sampleRate);
            return (float) std::exp (-ln1000 / samples);
        }

        double sampleRate = 44100.0;
        State  state = State::Idle;
        double level = 0.0;       // double: a float accumulator drifts enough to end the attack a sample early
        int    attackRemaining = 0;
        double attackInc = 0.0;
        float  decayCoef = 0.999f, relCoef = 0.999f, fastCoef = 0.99f;
        bool   fast = false;
        float  sus = 0.0f, susTarget = 0.0f, susCoef = 0.002f;
    };
}
