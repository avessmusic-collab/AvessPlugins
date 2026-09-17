#include "DropRandomizer.h"
#include <cmath>

namespace drop
{

void DropRandomizer::setSeed(uint32_t newSeed) noexcept
{
    seed = newSeed;
    rng.seed(seed);
}

float DropRandomizer::getIntensityJitter(int moduleIndex) const noexcept
{
    if (moduleIndex < 0 || moduleIndex >= kNumIntensityModules)
        return 0.0f;
    return intensityJitter[(size_t) moduleIndex];
}

void DropRandomizer::drawForNewBuild() noexcept
{
    // implementation-spec.md CHAOS: "e.g. 1/8 delay, 1/16 gate, +5
    // semitones, rather than arbitrary nonsense" - explicit small tables,
    // matching PitchModule's own quantized semitone set exactly.
    static constexpr int kSemitoneChoices[] = { 0, 1, 2, 3, 5, 7, 12, -1, -2, -3, -5, -7, -12 };
    static constexpr int kNumSemitoneChoices = (int) (sizeof(kSemitoneChoices) / sizeof(int));

    static constexpr MusicalDivision kDivisionChoices[] = {
        MusicalDivision::Eighth, MusicalDivision::Sixteenth, MusicalDivision::ThirtySecond
    };
    static constexpr int kNumDivisionChoices = (int) (sizeof(kDivisionChoices) / sizeof(MusicalDivision));

    std::uniform_real_distribution<float> unit01(0.0f, 1.0f);

    // Chaos scales how far the draw may stray from index 0 (each table's
    // "default, least-varied" entry) - never WHICH values are legal
    // (architecture.md/spec: "Chaos changes variation amount, not whether
    // the result remains musical").
    const int maxSemitoneIndex = juce::jlimit(0, kNumSemitoneChoices - 1,
        (int) std::lround(chaosAmount01 * (float) (kNumSemitoneChoices - 1)));
    std::uniform_int_distribution<int> semitoneDist(0, maxSemitoneIndex);
    pitchSemitoneChoice = kSemitoneChoices[(size_t) semitoneDist(rng)];

    const int maxDivIndex = juce::jlimit(0, kNumDivisionChoices - 1,
        (int) std::lround(chaosAmount01 * (float) (kNumDivisionChoices - 1)));
    std::uniform_int_distribution<int> divDist(0, maxDivIndex);
    delayDivisionChoice = kDivisionChoices[(size_t) divDist(rng)];
    gateDivisionChoice = kDivisionChoices[(size_t) divDist(rng)];

    // Stutter's 5 modes are already an inherently small, closed musical
    // set (architecture.md STUTTERMODULE) - Chaos does not need to further
    // restrict which of the 5 can be chosen.
    std::uniform_int_distribution<int> stutterModeDist(0, 4);
    stutterModeChoice = stutterModeDist(rng);

    for (auto& jitter : intensityJitter)
        jitter = (unit01(rng) * 2.0f - 1.0f) * chaosAmount01 * kMaxIntensityJitter;
}

} // namespace drop
