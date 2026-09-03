// PROPOSITIONS-1 #2 (Pattern Learning) — tests de FeatureExtractor, logique
// purement mathématique (aucune dépendance process/OS), jamais testée avant
// ce chantier (le fichier n'était même pas enregistré au build).

#include "pattern_learning/feature_extractor.h"

#include <gtest/gtest.h>

using killcore::FeatureExtractor;

TEST(FeatureExtractorTest, EmptyValuesReturnsDefaultFeatures) {
    FeatureExtractor extractor;
    const auto features = extractor.extract({});
    EXPECT_DOUBLE_EQ(features.mean, 0.0);
    EXPECT_EQ(features.uniqueValues, 0);
}

TEST(FeatureExtractorTest, BasicStatistics) {
    FeatureExtractor extractor;
    const auto features = extractor.extract({1.0, 2.0, 3.0, 4.0, 5.0});
    EXPECT_DOUBLE_EQ(features.mean, 3.0);
    EXPECT_DOUBLE_EQ(features.min, 1.0);
    EXPECT_DOUBLE_EQ(features.max, 5.0);
    EXPECT_DOUBLE_EQ(features.range, 4.0);
    EXPECT_EQ(features.uniqueValues, 5);
}

TEST(FeatureExtractorTest, DetectsMonotonicIncreasing) {
    FeatureExtractor extractor;
    const auto features = extractor.extract({10.0, 20.0, 30.0, 40.0});
    EXPECT_TRUE(features.isMonotonic);
}

TEST(FeatureExtractorTest, DetectsMonotonicDecreasing) {
    FeatureExtractor extractor;
    const auto features = extractor.extract({100.0, 80.0, 50.0, 10.0});
    EXPECT_TRUE(features.isMonotonic);
}

TEST(FeatureExtractorTest, NonMonotonicValuesNotFlagged) {
    FeatureExtractor extractor;
    const auto features = extractor.extract({10.0, 30.0, 5.0, 40.0});
    EXPECT_FALSE(features.isMonotonic);
}

TEST(FeatureExtractorTest, DetectsBinaryValues) {
    FeatureExtractor extractor;
    const auto features = extractor.extract({0.0, 1.0, 0.0, 1.0, 1.0, 0.0});
    EXPECT_TRUE(features.isBinary);
    EXPECT_DOUBLE_EQ(features.binaryRatio, 3.0 / 6.0); // 3 zeros sur 6
}

TEST(FeatureExtractorTest, NonBinaryValuesNotFlagged) {
    FeatureExtractor extractor;
    const auto features = extractor.extract({0.0, 1.0, 2.0, 3.0});
    EXPECT_FALSE(features.isBinary);
}

TEST(FeatureExtractorTest, RangeClassification) {
    FeatureExtractor extractor;
    EXPECT_TRUE(extractor.extract({10.0, 50.0}).inSmallRange);
    EXPECT_TRUE(extractor.extract({200.0, 500.0}).inMediumRange);
    EXPECT_TRUE(extractor.extract({5000.0, 9000.0}).inLargeRange);
}

TEST(FeatureExtractorTest, ConstantValuesHaveZeroEntropyAndVariance) {
    FeatureExtractor extractor;
    const auto features = extractor.extract({7.0, 7.0, 7.0, 7.0});
    EXPECT_DOUBLE_EQ(features.entropy, 0.0); // Une seule valeur -> p=1 -> -1*log2(1) = 0
    EXPECT_DOUBLE_EQ(features.variance, 0.0);
    EXPECT_EQ(features.uniqueValues, 1);
}

TEST(FeatureExtractorTest, HighEntropyForUniformlyDistinctValues) {
    FeatureExtractor extractor;
    const auto features = extractor.extract({1.0, 2.0, 3.0, 4.0});
    // 4 valeurs uniques equiprobables -> entropie maximale = log2(4) = 2.0
    EXPECT_NEAR(features.entropy, 2.0, 1e-9);
}

TEST(FeatureExtractorTest, TemporalChangeRateAndTrend) {
    FeatureExtractor extractor;
    const std::vector<double> values = {0.0, 10.0, 20.0, 30.0};
    const std::vector<int64_t> timestamps = {0, 1000, 2000, 3000};
    const auto features = extractor.extract(values, timestamps);
    // 30 unites de changement total sur 3000ms
    EXPECT_NEAR(features.changeRate, 30.0 / 3000.0, 1e-9);
    // Pente parfaitement lineaire : 10 par pas d'indice
    EXPECT_NEAR(features.trend, 10.0, 1e-9);
}

TEST(FeatureExtractorTest, CalculateTrendHandlesFlatSeries) {
    const auto slope = FeatureExtractor::calculateTrend({5.0, 5.0, 5.0, 5.0});
    EXPECT_NEAR(slope, 0.0, 1e-9);
}

TEST(FeatureExtractorTest, CalculateTrendTooShortReturnsZero) {
    EXPECT_DOUBLE_EQ(FeatureExtractor::calculateTrend({}), 0.0);
    EXPECT_DOUBLE_EQ(FeatureExtractor::calculateTrend({1.0}), 0.0);
}

TEST(FeatureExtractorTest, DetectMonotonicityTooShortReturnsFalse) {
    EXPECT_FALSE(FeatureExtractor::detectMonotonicity({}));
    EXPECT_FALSE(FeatureExtractor::detectMonotonicity({1.0}));
}

TEST(FeatureExtractorTest, PeriodicityDetectedForRepeatingPattern) {
    // Motif carre periodique : 0,10,0,10,... l'autocorrelation doit varier fortement selon le decalage.
    std::vector<double> values;
    for (int i = 0; i < 20; ++i) {
        values.push_back((i % 2 == 0) ? 0.0 : 10.0);
    }
    const auto periodicity = FeatureExtractor::calculatePeriodicity(values);
    EXPECT_GT(periodicity, 0.0);
}

TEST(FeatureExtractorTest, ToFeatureVectorProducesBoundedValues) {
    FeatureExtractor extractor;
    const auto features = extractor.extract({1.0, 2.0, 3.0, 100000.0});
    const auto vec = extractor.toFeatureVector(features);
    ASSERT_FALSE(vec.empty());
    for (double v : vec) {
        EXPECT_GE(v, -1.0); // trend peut etre negatif avant clamp -- verifie juste que rien n'explose
        EXPECT_LE(v, 1.0001);
    }
}
