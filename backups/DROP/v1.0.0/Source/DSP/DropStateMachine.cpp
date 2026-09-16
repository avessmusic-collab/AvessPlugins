#include "DropStateMachine.h"
#include <algorithm>

namespace drop
{

bool DropStateMachine::requestTrigger(const Durations& durationsForThisBuild) noexcept
{
    if (state != DropState::Idle)
        return false; // re-trigger safety: ignored unless currently Idle

    durations = durationsForThisBuild;
    enterState(DropState::Arming);
    return true;
}

int64_t DropStateMachine::lengthOfState(DropState s) const noexcept
{
    switch (s)
    {
        case DropState::Arming:   return durations.armingToBuildingSamples;
        case DropState::Building: return durations.buildingSamples;
        case DropState::Peak:     return durations.peakSamples;
        case DropState::Impact:   return durations.impactSamples;
        case DropState::Recovery: return durations.recoverySamples;
        case DropState::Idle:     break;
    }
    return 0;
}

void DropStateMachine::enterState(DropState newState) noexcept
{
    state = newState;
    samplesIntoState = 0;
}

bool DropStateMachine::tick() noexcept
{
    if (state == DropState::Idle)
        return false;

    const int64_t stateLength = lengthOfState(state);
    if (samplesIntoState < stateLength)
        return false;

    const int64_t carry = samplesIntoState - stateLength; // usually 0; only nonzero if advance() was ever called past the boundary

    switch (state)
    {
        case DropState::Arming:   enterState(DropState::Building); break;
        case DropState::Building: enterState(DropState::Peak);     break;
        case DropState::Peak:     enterState(DropState::Impact);   break;
        case DropState::Impact:   enterState(DropState::Recovery); break;
        case DropState::Recovery: enterState(DropState::Idle);     break;
        case DropState::Idle:     break;
    }

    samplesIntoState = std::max((int64_t) 0, carry);
    return true;
}

void DropStateMachine::advance(int64_t numSamples) noexcept
{
    if (state == DropState::Idle || numSamples <= 0)
        return;

    samplesIntoState += numSamples;
}

int64_t DropStateMachine::samplesUntilNextTransition() const noexcept
{
    if (state == DropState::Idle)
        return std::numeric_limits<int64_t>::max();

    return std::max((int64_t) 0, lengthOfState(state) - samplesIntoState);
}

void DropStateMachine::reset() noexcept
{
    state = DropState::Idle;
    samplesIntoState = 0;
    durations = Durations {};
}

} // namespace drop
