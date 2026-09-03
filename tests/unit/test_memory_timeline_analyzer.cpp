#include "visualization/memory_timeline_analyzer.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace {

std::vector<uint8_t> le32(uint32_t value) {
    return {
        static_cast<uint8_t>(value & 0xff),
        static_cast<uint8_t>((value >> 8) & 0xff),
        static_cast<uint8_t>((value >> 16) & 0xff),
        static_cast<uint8_t>((value >> 24) & 0xff),
    };
}

killcore::TimelineSeries makeSeries(uint64_t address, const std::vector<uint32_t>& values, uint64_t stepMs = 100) {
    killcore::TimelineSeries series;
    series.baseAddress = address;
    series.valueSize = 4;

    uint64_t timestamp = 0;
    for (const auto value : values) {
        killcore::TimelineDataPoint point;
        point.address = address;
        point.timestampMs = timestamp;
        point.value = le32(value);
        point.isValid = true;
        series.points.push_back(point);
        timestamp += stepMs;
    }

    return series;
}

uint32_t decodeLe32(const std::vector<uint8_t>& bytes) {
    if (bytes.size() < 4) {
        return 0;
    }
    return static_cast<uint32_t>(bytes[0])
        | (static_cast<uint32_t>(bytes[1]) << 8)
        | (static_cast<uint32_t>(bytes[2]) << 16)
        | (static_cast<uint32_t>(bytes[3]) << 24);
}

} // namespace

TEST(MemoryTimelineAnalyzerTest, DetectsConstantSeries) {
    const auto series = makeSeries(0x1000, {42, 42, 42, 42, 42});
    const killcore::MemoryTimelineAnalyzer analyzer;

    const auto pattern = analyzer.detectDominantPattern(series);

    EXPECT_EQ(pattern.type, killcore::TimelinePattern::Type::Constant);
    EXPECT_DOUBLE_EQ(pattern.confidence, 1.0);
}

TEST(MemoryTimelineAnalyzerTest, DetectsLinearTrendAndPredictsNextValue) {
    const auto series = makeSeries(0x2000, {10, 20, 30, 40, 50}, 100);
    const killcore::MemoryTimelineAnalyzer analyzer;

    const auto pattern = analyzer.detectDominantPattern(series);
    const auto prediction = analyzer.predictNextValue(series, 100);

    EXPECT_EQ(pattern.type, killcore::TimelinePattern::Type::Linear);
    EXPECT_NEAR(pattern.slope, 0.1, 0.0001);
    EXPECT_EQ(decodeLe32(prediction), 60u);
}

TEST(MemoryTimelineAnalyzerTest, DetectsCyclicSeries) {
    const auto series = makeSeries(0x3000, {1, 2, 3, 1, 2, 3, 1, 2, 3}, 50);
    const killcore::MemoryTimelineAnalyzer analyzer;

    const auto pattern = analyzer.detectDominantPattern(series);

    EXPECT_EQ(pattern.type, killcore::TimelinePattern::Type::Cyclic);
    EXPECT_EQ(pattern.periodMs, 150u);
    EXPECT_GE(pattern.confidence, 0.9);
}

TEST(MemoryTimelineAnalyzerTest, CreatesBehaviorProfile) {
    const auto series = makeSeries(0x4000, {5, 5, 6, 6, 7, 7}, 100);
    const killcore::MemoryTimelineAnalyzer analyzer;

    const auto profile = analyzer.createBehaviorProfile(series);

    EXPECT_EQ(profile.address, 0x4000u);
    EXPECT_EQ(profile.distinctValueCount, 3u);
    EXPECT_EQ(decodeLe32(profile.minValue), 5u);
    EXPECT_EQ(decodeLe32(profile.maxValue), 7u);
    EXPECT_GT(profile.changesPerSecond, 0.0);
}

TEST(MemoryTimelineAnalyzerTest, FindsPositiveAndNegativeCorrelations) {
    const auto seriesA = makeSeries(0x5000, {1, 2, 3, 4, 5});
    const auto seriesB = makeSeries(0x6000, {10, 20, 30, 40, 50});
    const auto seriesC = makeSeries(0x7000, {50, 40, 30, 20, 10});
    const killcore::MemoryTimelineAnalyzer analyzer;

    const auto correlations = analyzer.findCorrelations({seriesA, seriesB, seriesC}, 0.95);

    ASSERT_GE(correlations.size(), 2u);
    EXPECT_NEAR(analyzer.calculateSimilarity(seriesA, seriesB), 1.0, 0.0001);
}

TEST(MemoryTimelineAnalyzerTest, FlagsRegularChecksumLikeUpdates) {
    const auto series = makeSeries(0x8000, {100, 101, 102, 103, 104, 105, 106}, 100);
    const killcore::MemoryTimelineAnalyzer analyzer;

    EXPECT_TRUE(analyzer.isLikelyIntegrityCheck(series));
    const auto patterns = analyzer.detectAntiCheatPatterns(series);
    EXPECT_FALSE(patterns.empty());
}

TEST(MemoryTimelineAnalyzerTest, GeneratesReadableReport) {
    const auto seriesA = makeSeries(0x9000, {9, 9, 9});
    const auto seriesB = makeSeries(0x9010, {1, 2, 3});
    const killcore::MemoryTimelineAnalyzer analyzer;

    const auto report = analyzer.generateAnalysisReport({seriesA, seriesB});

    EXPECT_NE(report.find("Memory Timeline Analysis"), std::string::npos);
    EXPECT_NE(report.find("0x9000"), std::string::npos);
    EXPECT_NE(report.find("pattern=constant"), std::string::npos);
}
