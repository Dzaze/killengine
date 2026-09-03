#include <gtest/gtest.h>
#include "smart_watchdog/smart_watchdog.h"

using namespace killcore;

class SmartWatchdogTest : public ::testing::Test {
protected:
    void SetUp() override {
        watchdog = std::make_unique<SmartWatchdog>();
    }

    std::unique_ptr<SmartWatchdog> watchdog;
};

TEST_F(SmartWatchdogTest, AddAndRemoveEntry) {
    WatchdogEntry entry;
    entry.address = 0x1000;
    entry.writtenValue = 999;
    entry.originalValue = 100;
    entry.valueSize = 4;
    entry.active = true;

    watchdog->addEntry(entry);
    EXPECT_EQ(watchdog->entries().size(), 1);

    watchdog->removeEntry(0x1000);
    EXPECT_TRUE(watchdog->entries().empty());
}

TEST_F(SmartWatchdogTest, DetectResync) {
    WatchdogConfig config;
    config.resyncThreshold = 1;
    watchdog->setConfig(config);

    WatchdogEntry entry;
    entry.address = 0x1000;
    entry.writtenValue = 999;
    entry.originalValue = 100;
    entry.valueSize = 4;
    entry.startTimeMs = 0;
    entry.active = true;

    // Valeur revenue a l'originale = resync
    auto result = watchdog->checkEntry(entry, 1000, 100);
    EXPECT_EQ(result.state, WatchdogState::ResyncDetected);
    EXPECT_TRUE(result.resyncConfirmed);
}

TEST_F(SmartWatchdogTest, DetectStable) {
    WatchdogEntry entry;
    entry.address = 0x1000;
    entry.writtenValue = 999;
    entry.originalValue = 100;
    entry.valueSize = 4;
    entry.startTimeMs = 0;
    entry.active = true;

    // Valeur maintenue = stable avant expiration, puis expiree apres la fenetre.
    auto config = watchdog->config();
    auto stableResult = watchdog->checkEntry(entry, config.watchDurationMs - 1, 999);
    EXPECT_EQ(stableResult.state, WatchdogState::Stable);

    auto result = watchdog->checkEntry(entry, config.watchDurationMs + 100, 999);
    EXPECT_EQ(result.state, WatchdogState::Expired);
}

TEST_F(SmartWatchdogTest, ThresholdPreventsFalsePositive) {
    WatchdogConfig config;
    config.resyncThreshold = 3;
    watchdog->setConfig(config);

    WatchdogEntry entry;
    entry.address = 0x1000;
    entry.writtenValue = 999;
    entry.originalValue = 100;
    entry.valueSize = 4;
    entry.consecutiveResyncTicks = 1; // Juste sous le seuil
    entry.active = true;

    auto result = watchdog->checkEntry(entry, 1000, 100);
    EXPECT_FALSE(result.resyncConfirmed); // Pas encore confirme
    EXPECT_EQ(result.resyncTickCount, 2);
}

TEST_F(SmartWatchdogTest, CheckAllAccumulatesResyncTicksAcrossPolls) {
    WatchdogConfig config;
    config.resyncThreshold = 3;
    watchdog->setConfig(config);

    WatchdogEntry entry;
    entry.address = 0x1000;
    entry.writtenValue = 999;
    entry.originalValue = 100;
    entry.valueSize = 4;
    entry.active = true;
    watchdog->addEntry(entry);

    auto readOriginal = [](uint64_t, uint32_t) -> std::optional<uint64_t> {
        return 100;
    };

    auto first = watchdog->checkAll(100, readOriginal);
    ASSERT_EQ(first.size(), 1);
    EXPECT_FALSE(first[0].second.resyncConfirmed);
    EXPECT_EQ(first[0].second.resyncTickCount, 1);
    EXPECT_TRUE(watchdog->entries()[0].active);

    auto second = watchdog->checkAll(200, readOriginal);
    ASSERT_EQ(second.size(), 1);
    EXPECT_FALSE(second[0].second.resyncConfirmed);
    EXPECT_EQ(second[0].second.resyncTickCount, 2);
    EXPECT_TRUE(watchdog->entries()[0].active);

    auto third = watchdog->checkAll(300, readOriginal);
    ASSERT_EQ(third.size(), 1);
    EXPECT_TRUE(third[0].second.resyncConfirmed);
    EXPECT_EQ(third[0].second.state, WatchdogState::ResyncDetected);
    EXPECT_FALSE(watchdog->entries()[0].active);
}

TEST_F(SmartWatchdogTest, CheckAllExpiresStableEntry) {
    WatchdogConfig config;
    config.watchDurationMs = 500;
    watchdog->setConfig(config);

    WatchdogEntry entry;
    entry.address = 0x1000;
    entry.writtenValue = 999;
    entry.originalValue = 100;
    entry.valueSize = 4;
    entry.startTimeMs = 0;
    entry.active = true;
    watchdog->addEntry(entry);

    auto readWritten = [](uint64_t, uint32_t) -> std::optional<uint64_t> {
        return 999;
    };

    auto results = watchdog->checkAll(600, readWritten);
    ASSERT_EQ(results.size(), 1);
    EXPECT_EQ(results[0].second.state, WatchdogState::Expired);
    EXPECT_FALSE(watchdog->entries()[0].active);
}

TEST_F(SmartWatchdogTest, GenerateSuggestionForResync) {
    WatchdogCheckResult result;
    result.state = WatchdogState::ResyncDetected;
    result.resyncConfirmed = true;

    QString suggestion = watchdog->generateSuggestion(result, 0x1000);
    EXPECT_FALSE(suggestion.isEmpty());
    EXPECT_TRUE(suggestion.contains("Resynchronisation"));
}

TEST_F(SmartWatchdogTest, GenerateSuggestionWithTwin) {
    WatchdogCheckResult result;
    result.state = WatchdogState::ResyncDetected;
    result.resyncConfirmed = true;
    result.twinAddress = 0x1008;

    QString suggestion = watchdog->generateSuggestion(result, 0x1000);
    EXPECT_TRUE(suggestion.contains("jumeau"));
    EXPECT_TRUE(suggestion.contains("0x1008"));
}

TEST_F(SmartWatchdogTest, MaskValueBySize) {
    WatchdogEntry entry;
    entry.address = 0x1000;
    entry.writtenValue = 0x12345678;
    entry.originalValue = 0xABCDEF78;
    entry.valueSize = 1;
    entry.active = true;

    WatchdogConfig config;
    config.resyncThreshold = 1;
    watchdog->setConfig(config);

    auto result = watchdog->checkEntry(entry, 1000, 0x00000078);
    EXPECT_TRUE(result.resyncConfirmed);
    EXPECT_EQ(result.state, WatchdogState::ResyncDetected);
}

TEST_F(SmartWatchdogTest, DetectTwinPatternRequiresMatchingValue) {
    WatchdogConfig config;
    config.detectTwinPattern = true;
    config.twinSearchRange = 16;
    watchdog->setConfig(config);

    auto twin = watchdog->detectTwinPattern(
        0x1000,
        999,
        4,
        [](uint64_t address) -> std::optional<uint64_t> {
            if (address == 0x1008) return 123;
            if (address == 0x1010) return 999;
            return std::nullopt;
        });

    ASSERT_TRUE(twin.has_value());
    EXPECT_EQ(twin.value(), 0x1010);
}

TEST_F(SmartWatchdogTest, ClearAllEntries) {
    for (int i = 0; i < 5; ++i) {
        WatchdogEntry entry;
        entry.address = 0x1000 + i * 0x10;
        entry.active = true;
        watchdog->addEntry(entry);
    }

    EXPECT_EQ(watchdog->entries().size(), 5);
    watchdog->clear();
    EXPECT_TRUE(watchdog->entries().empty());
}
