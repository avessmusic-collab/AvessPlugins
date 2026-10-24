// CHOPLINE tests — transient detection (spec §12): synthetic click train with
// an F-measure-style assertion, plus determinism.

#include <catch2/catch_test_macros.hpp>

#include "TestHelpers.h"
#include "engine/TransientDetector.h"

using namespace chopline;
using namespace chopline::test;

namespace
{
    struct FMeasure
    {
        int truePositives = 0, falsePositives = 0, falseNegatives = 0;
        double f() const
        {
            const auto precision = truePositives + falsePositives > 0
                ? static_cast<double> (truePositives) / (truePositives + falsePositives) : 0.0;
            const auto recall = truePositives + falseNegatives > 0
                ? static_cast<double> (truePositives) / (truePositives + falseNegatives) : 0.0;
            return precision + recall > 0.0 ? 2.0 * precision * recall / (precision + recall) : 0.0;
        }
    };

    FMeasure score (const std::vector<double>& expected, const std::vector<double>& detected,
                    double toleranceSec)
    {
        FMeasure m;
        std::vector<bool> used (detected.size(), false);
        for (auto e : expected)
        {
            bool matched = false;
            for (size_t i = 0; i < detected.size(); ++i)
            {
                if (! used[i] && std::abs (detected[i] - e) <= toleranceSec)
                {
                    used[i] = true;
                    matched = true;
                    break;
                }
            }
            matched ? ++m.truePositives : ++m.falseNegatives;
        }
        for (auto u : used)
            if (! u)
                ++m.falsePositives;
        return m;
    }
} // namespace

TEST_CASE ("Transient detection finds a synthetic click train", "[transient]")
{
    constexpr double rate = 48000.0;
    const std::vector<double> expected { 0.25, 0.75, 1.25, 1.75, 2.25, 2.75, 3.25, 3.75 };
    const auto audio = makeClickTrain (expected, 4.5, rate);

    const auto analysis = engine::TransientDetector::analyze (audio, rate);
    REQUIRE (! analysis.envelope.empty());

    const auto onsets = engine::TransientDetector::pickOnsets (analysis, 0.55f, 0.08);
    const auto m = score (expected, onsets, 0.03); // ±30 ms

    INFO ("detected " << onsets.size() << " onsets, F = " << m.f());
    REQUIRE (m.f() >= 0.9);
}

TEST_CASE ("Transient detection is deterministic", "[transient]")
{
    constexpr double rate = 44100.0;
    const auto audio = makeClickTrain ({ 0.2, 0.5, 0.9, 1.4 }, 2.0, rate);

    const auto a1 = engine::TransientDetector::analyze (audio, rate);
    const auto a2 = engine::TransientDetector::analyze (audio, rate);
    REQUIRE (a1.envelope == a2.envelope);

    const auto o1 = engine::TransientDetector::pickOnsets (a1, 0.7f, 0.05);
    const auto o2 = engine::TransientDetector::pickOnsets (a2, 0.7f, 0.05);
    REQUIRE (o1 == o2);
    REQUIRE (! o1.empty());
}

TEST_CASE ("Sensitivity sweeps slice density monotonically", "[transient]")
{
    constexpr double rate = 48000.0;
    const auto audio = makeClickTrain ({ 0.25, 0.75, 1.25, 1.75, 2.25 }, 3.0, rate);
    const auto analysis = engine::TransientDetector::analyze (audio, rate);

    const auto low = engine::TransientDetector::pickOnsets (analysis, 0.05f, 0.05).size();
    const auto high = engine::TransientDetector::pickOnsets (analysis, 0.95f, 0.05).size();
    REQUIRE (high >= low);
    REQUIRE (high >= 5); // every click found at high sensitivity
}

TEST_CASE ("BPM estimate from regular onsets", "[transient]")
{
    // 0.5 s inter-onset interval = 120 BPM.
    const std::vector<double> onsets { 0.0, 0.5, 1.0, 1.5, 2.0, 2.5, 3.0 };
    const auto bpm = engine::TransientDetector::estimateBpm (onsets, 99.0);
    REQUIRE (bpm > 115.0);
    REQUIRE (bpm < 125.0);

    // Too few onsets → fallback.
    REQUIRE (juce::exactlyEqual (engine::TransientDetector::estimateBpm ({ 0.0, 1.0 }, 99.0), 99.0));
}
