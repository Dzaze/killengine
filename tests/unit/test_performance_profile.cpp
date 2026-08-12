#include "scanner/performance_profile.h"

#include <gtest/gtest.h>

using namespace killcore;

namespace {

constexpr uint64_t GiB = 1024ull * 1024ull * 1024ull;

} // namespace

TEST(PerformanceProfile, AutoUsesEcoOnSmallMachines) {
    const auto profile = makePerformanceProfile(
        PerformanceMode::Auto,
        SystemPerformanceInfo{2, 2ull * GiB});

    EXPECT_EQ(profile.requestedMode, PerformanceMode::Auto);
    EXPECT_EQ(profile.resolvedMode, PerformanceMode::Eco);
    EXPECT_EQ(profile.workerThreads, 1u);
    EXPECT_GE(profile.reserveThreads, 1u);
}

TEST(PerformanceProfile, AutoUsesPerformanceOnLargeMachines) {
    const auto profile = makePerformanceProfile(
        PerformanceMode::Auto,
        SystemPerformanceInfo{16, 32ull * GiB});

    EXPECT_EQ(profile.resolvedMode, PerformanceMode::Performance);
    EXPECT_GE(profile.workerThreads, 8u);
    EXPECT_LT(profile.workerThreads, 16u);
    EXPECT_GE(profile.maxInFlightBytes, 32ull * 1024ull * 1024ull);
}

TEST(PerformanceProfile, MaxKeepsOneThreadAvailable) {
    const auto profile = makePerformanceProfile(
        PerformanceMode::Max,
        SystemPerformanceInfo{8, 16ull * GiB});

    EXPECT_EQ(profile.resolvedMode, PerformanceMode::Max);
    EXPECT_EQ(profile.reserveThreads, 1u);
    EXPECT_EQ(profile.workerThreads, 7u);
    EXPECT_EQ(profile.chunkSize, 4u * 1024u * 1024u);
}

TEST(PerformanceProfile, ParseModeIsCaseInsensitive) {
    EXPECT_EQ(parsePerformanceMode("AUTO"), PerformanceMode::Auto);
    EXPECT_EQ(parsePerformanceMode("eco"), PerformanceMode::Eco);
    EXPECT_EQ(parsePerformanceMode("Performance"), PerformanceMode::Performance);
    EXPECT_EQ(parsePerformanceMode("wat", PerformanceMode::Normal), PerformanceMode::Normal);
}
