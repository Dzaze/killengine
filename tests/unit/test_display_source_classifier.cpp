#include <gtest/gtest.h>

#include "scanner/display_source_classifier.h"

namespace {

killcore::WriteObservation obs(uint64_t instructionPointer, uint32_t elapsedMs) {
    return {instructionPointer, elapsedMs};
}

} // namespace

TEST(DisplaySourceClassifier, NoObservationsMeansNoWritesObserved) {
    const auto result = killcore::classifyFieldStability({});
    EXPECT_EQ(result.verdict, killcore::FieldStabilityVerdict::NoWritesObserved);
    EXPECT_EQ(result.writeCount, 0u);
    EXPECT_FALSE(result.rationale.isEmpty());
}

TEST(DisplaySourceClassifier, OneOrTwoObservationsAreInsufficientData) {
    const auto one = killcore::classifyFieldStability({obs(0x1000, 0)});
    EXPECT_EQ(one.verdict, killcore::FieldStabilityVerdict::InsufficientData);
    EXPECT_EQ(one.writeCount, 1u);

    const auto two = killcore::classifyFieldStability({obs(0x1000, 0), obs(0x1000, 50)});
    EXPECT_EQ(two.verdict, killcore::FieldStabilityVerdict::InsufficientData);
    EXPECT_EQ(two.writeCount, 2u);
}

// Reproduit g_counterDisplayed : meme instruction, tick fixe ~50ms, un peu de
// gigue realiste (WaitForDebugEvent/ContinueDebugEvent ne sont pas parfaits).
TEST(DisplaySourceClassifier, RegularSameInstructionPatternIsLikelyDerivedDisplay) {
    const QList<killcore::WriteObservation> observations = {
        obs(0xDEAD1000, 0),
        obs(0xDEAD1000, 48),
        obs(0xDEAD1000, 101),
        obs(0xDEAD1000, 149),
        obs(0xDEAD1000, 203),
        obs(0xDEAD1000, 248),
    };

    const auto result = killcore::classifyFieldStability(observations);
    EXPECT_EQ(result.verdict, killcore::FieldStabilityVerdict::LikelyDerivedDisplay);
    EXPECT_EQ(result.writeCount, 6u);
    EXPECT_EQ(result.distinctInstructionCount, 1u);
    EXPECT_EQ(result.dominantInstructionPointer, 0xDEAD1000u);
    EXPECT_NEAR(result.dominantInstructionShare, 1.0, 1e-9);
    EXPECT_NEAR(result.meanIntervalMs, 49.6, 1.0);
    EXPECT_LT(result.intervalCoefficientOfVariation, 0.2);
    EXPECT_FALSE(result.rationale.isEmpty());
}

// Reproduit g_counterSource/g_health sous ecriture explicite occasionnelle :
// intervalles tres irreguliers, sources d'instruction differentes (ex. un
// clic UI a un instant, un test a un autre).
TEST(DisplaySourceClassifier, IrregularMultiInstructionPatternIsLikelyEventDriven) {
    const QList<killcore::WriteObservation> observations = {
        obs(0xAAAA0001, 0),
        obs(0xBBBB0002, 812),
        obs(0xAAAA0001, 815),
        obs(0xCCCC0003, 4001),
    };

    const auto result = killcore::classifyFieldStability(observations);
    EXPECT_EQ(result.verdict, killcore::FieldStabilityVerdict::LikelyEventDriven);
    EXPECT_EQ(result.writeCount, 4u);
    EXPECT_EQ(result.distinctInstructionCount, 3u);
    EXPECT_FALSE(result.rationale.isEmpty());
}

// Meme instruction dominante mais rythme tres irregulier (rafale puis trou) :
// ne doit PAS etre classe "derived display" seulement parce qu'une instruction
// domine -- la regularite du rythme compte aussi.
TEST(DisplaySourceClassifier, SameInstructionButIrregularRhythmIsNotDerivedDisplay) {
    const QList<killcore::WriteObservation> observations = {
        obs(0x2000, 0),
        obs(0x2000, 5),
        obs(0x2000, 9),
        obs(0x2000, 2000),
    };

    const auto result = killcore::classifyFieldStability(observations);
    EXPECT_EQ(result.verdict, killcore::FieldStabilityVerdict::LikelyEventDriven);
    EXPECT_EQ(result.distinctInstructionCount, 1u);
    EXPECT_GT(result.intervalCoefficientOfVariation, 0.6);
}

TEST(DisplaySourceClassifier, CustomOptionsChangeThresholds) {
    const QList<killcore::WriteObservation> observations = {
        obs(0x3000, 0),
        obs(0x3000, 50),
        obs(0x3000, 100),
    };

    killcore::FieldStabilityOptions strict;
    strict.minHitsForPattern = 10; // plus d'observations que ce qu'on fournit
    const auto insufficient = killcore::classifyFieldStability(observations, strict);
    EXPECT_EQ(insufficient.verdict, killcore::FieldStabilityVerdict::InsufficientData);

    killcore::FieldStabilityOptions lenient;
    lenient.minHitsForPattern = 3;
    const auto pattern = killcore::classifyFieldStability(observations, lenient);
    EXPECT_EQ(pattern.verdict, killcore::FieldStabilityVerdict::LikelyDerivedDisplay);
}
