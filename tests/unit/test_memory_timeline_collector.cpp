// Tests unitaires du collecteur Win32 (PROPOSITIONS-1 #3 — Memory Timeline).
// Même technique que test_memory_heatmap_collector.cpp : VirtualAlloc +
// GetCurrentProcess() pour que le process de test soit sa propre cible --
// ReadProcessMemory fonctionne normalement sur le pseudo-handle du process
// courant, donc c'est un vrai test Win32 (pas un mock) sans process séparé
// à lancer/synchroniser. Comble le point "pas de tests unitaires dédiés au
// collecteur Win32" resté ouvert sur MEMORY-TIMELINE-VIS-1 (docs/PHASE_TRACKER.md),
// côté Memory Timeline (le pendant Heatmap a été clos le 03/09/2026 dans
// test_memory_heatmap_collector.cpp).

#include "visualization/memory_timeline_collector.h"

#include <gtest/gtest.h>

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <thread>

using namespace killcore;
using namespace std::chrono_literals;

class MemoryTimelineCollectorTest : public ::testing::Test {
protected:
    void SetUp() override {
        buffer = static_cast<uint8_t*>(VirtualAlloc(
            nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        ASSERT_NE(buffer, nullptr);
        addrA = reinterpret_cast<uint64_t>(buffer);
        addrB = addrA + 4;
        writeInt32(addrA, 100);
        writeInt32(addrB, 200);
        collector = std::make_unique<MemoryTimelineCollector>();
    }

    void TearDown() override {
        if (collector && collector->isCollecting()) {
            collector->stopCollection();
        }
        collector.reset();
        if (buffer) {
            VirtualFree(buffer, 0, MEM_RELEASE);
        }
    }

    void writeInt32(uint64_t address, int32_t value) {
        std::memcpy(reinterpret_cast<void*>(address), &value, sizeof(value));
    }

    int32_t pointValue(const TimelineDataPoint& point) const {
        int32_t value = 0;
        if (point.value.size() >= sizeof(value)) {
            std::memcpy(&value, point.value.data(), sizeof(value));
        }
        return value;
    }

    TimelineCollectorConfig makeConfig(bool trackOnlyChanges = true) const {
        TimelineCollectorConfig config;
        config.samplingIntervalMs = 15;
        config.maxDurationMs = 5000;
        config.trackOnlyChanges = trackOnlyChanges;
        config.calculateStatistics = true;
        return config;
    }

    uint8_t* buffer = nullptr;
    uint64_t addrA = 0;
    uint64_t addrB = 0;
    std::unique_ptr<MemoryTimelineCollector> collector;
};

TEST_F(MemoryTimelineCollectorTest, TrackOnlyChangesStoresJustOnePointWhenValueNeverChanges) {
    collector->setConfig(makeConfig(/*trackOnlyChanges=*/true));
    collector->addAddress(addrA, 4);
    ASSERT_TRUE(collector->startCollection(GetCurrentProcess()));

    std::this_thread::sleep_for(100ms);
    collector->stopCollection();

    const auto series = collector->getSeriesForAddress(addrA);
    EXPECT_EQ(series.points.size(), 1u);
    ASSERT_FALSE(series.points.empty());
    EXPECT_TRUE(series.points.front().isValid);
    EXPECT_EQ(pointValue(series.points.front()), 100);
}

TEST_F(MemoryTimelineCollectorTest, TrackOnlyChangesCapturesARealMutation) {
    collector->setConfig(makeConfig(/*trackOnlyChanges=*/true));
    collector->addAddress(addrA, 4);
    ASSERT_TRUE(collector->startCollection(GetCurrentProcess()));

    std::this_thread::sleep_for(60ms);
    writeInt32(addrA, 999);
    std::this_thread::sleep_for(80ms);
    collector->stopCollection();

    const auto series = collector->getSeriesForAddress(addrA);
    ASSERT_EQ(series.points.size(), 2u);
    EXPECT_EQ(pointValue(series.points[0]), 100);
    EXPECT_EQ(pointValue(series.points[1]), 999);
    EXPECT_GT(series.points[1].timestampMs, series.points[0].timestampMs);
}

TEST_F(MemoryTimelineCollectorTest, TrackOnlyChangesFalseStoresEveryProbeRegardlessOfContent) {
    collector->setConfig(makeConfig(/*trackOnlyChanges=*/false));
    collector->addAddress(addrA, 4);
    ASSERT_TRUE(collector->startCollection(GetCurrentProcess()));

    std::this_thread::sleep_for(100ms); // ~6-7 ticks à 15ms, valeur jamais modifiée.
    collector->stopCollection();

    const auto series = collector->getSeriesForAddress(addrA);
    EXPECT_GT(series.points.size(), 1u);
    for (const auto& point : series.points) {
        EXPECT_EQ(pointValue(point), 100);
    }
}

TEST_F(MemoryTimelineCollectorTest, TwoWatchedAddressesTrackedIndependently) {
    collector->setConfig(makeConfig());
    collector->addAddress(addrA, 4);
    collector->addAddress(addrB, 4);
    ASSERT_TRUE(collector->startCollection(GetCurrentProcess()));

    std::this_thread::sleep_for(50ms);
    writeInt32(addrA, 111); // Seule addrA change.
    std::this_thread::sleep_for(80ms);
    collector->stopCollection();

    const auto seriesA = collector->getSeriesForAddress(addrA);
    const auto seriesB = collector->getSeriesForAddress(addrB);

    ASSERT_EQ(seriesA.points.size(), 2u);
    EXPECT_EQ(pointValue(seriesA.points.back()), 111);
    EXPECT_EQ(seriesB.points.size(), 1u); // Jamais modifiée -> 1 seul point.
    EXPECT_EQ(pointValue(seriesB.points.front()), 200);
}

TEST_F(MemoryTimelineCollectorTest, StatisticsComputedOnlyOnceAtLeastTwoPointsExist) {
    collector->setConfig(makeConfig());
    collector->addAddress(addrA, 4);
    ASSERT_TRUE(collector->startCollection(GetCurrentProcess()));

    // Une seule valeur jamais modifiée -> updateStatistics() sort tôt
    // (points.size() < 2) -- changeCount/volatilityScore restent à leur
    // valeur par défaut (0), pas une valeur "stable" calculée.
    std::this_thread::sleep_for(80ms);
    const auto untouched = collector->getSeriesForAddress(addrA);
    EXPECT_EQ(untouched.changeCount, 0u);
    EXPECT_DOUBLE_EQ(untouched.volatilityScore, 0.0);

    writeInt32(addrA, 555);
    std::this_thread::sleep_for(60ms);
    collector->stopCollection();

    const auto changed = collector->getSeriesForAddress(addrA);
    EXPECT_EQ(changed.changeCount, 2u);
    EXPECT_GT(changed.averageIntervalMs, 0.0);
}

TEST_F(MemoryTimelineCollectorTest, FindVolatileAddressesRequiresIrregularChangeIntervals) {
    collector->setConfig(makeConfig());
    collector->addAddress(addrA, 4);
    ASSERT_TRUE(collector->startCollection(GetCurrentProcess()));

    // 3 points avec des intervalles très inégaux (court puis long) ->
    // écart-type élevé relatif à la moyenne -> volatilityScore élevé. Marge
    // large (30ms vs 250ms) pour rester fiable malgré la granularité du
    // scheduler Windows (~15ms) et le tick de collecte (15ms lui aussi) --
    // un premier essai avec des intervalles proches de 15ms perdait parfois
    // un point (deux écritures fusionnées dans le même tick de collecte).
    std::this_thread::sleep_for(30ms);
    writeInt32(addrA, 1);
    std::this_thread::sleep_for(250ms);
    writeInt32(addrA, 2);
    std::this_thread::sleep_for(60ms);
    collector->stopCollection();

    const auto series = collector->getSeriesForAddress(addrA);
    ASSERT_GE(series.points.size(), 3u);
    EXPECT_GT(series.volatilityScore, 0.3);

    const auto volatileAddrs = collector->findVolatileAddresses(0.3);
    EXPECT_NE(std::find(volatileAddrs.begin(), volatileAddrs.end(), addrA), volatileAddrs.end());
}

TEST_F(MemoryTimelineCollectorTest, FindStableAddressesRequiresOneChangeSeparatedByMinDuration) {
    // Contrat réel de findStableAddresses (changeCount <= 2 ET la durée entre
    // le 1er et le DERNIER point enregistré >= minDurationMs) : une adresse
    // qui ne change jamais n'a qu'1 point pour toujours et duration=0 (voir
    // le test StatisticsComputedOnlyOnceAtLeastTwoPointsExist ci-dessus) --
    // "stable" ici veut dire "a changé une fois puis plus rien pendant un
    // moment", pas "n'a jamais changé".
    collector->setConfig(makeConfig());
    collector->addAddress(addrA, 4);
    ASSERT_TRUE(collector->startCollection(GetCurrentProcess()));

    std::this_thread::sleep_for(150ms); // point initial, rien ne change encore.
    writeInt32(addrA, 42);
    std::this_thread::sleep_for(60ms); // capture le changement, puis plus rien.
    collector->stopCollection();

    const auto series = collector->getSeriesForAddress(addrA);
    ASSERT_EQ(series.changeCount, 2u);
    EXPECT_GE(series.lastChangeMs - series.firstChangeMs, 100u);

    const auto stableAddrs = collector->findStableAddresses(100);
    EXPECT_NE(std::find(stableAddrs.begin(), stableAddrs.end(), addrA), stableAddrs.end());
}

TEST_F(MemoryTimelineCollectorTest, RestartAfterNaturalCompletionDoesNotCrash) {
    // Régression du bug corrigé le 02/09/2026 (std::terminate reproduit en
    // live, voir commentaire dans memory_timeline_collector.cpp) :
    // réassigner le thread de collecte sans le joindre d'abord plantait tout
    // le process quand la collecte précédente s'était arrêtée d'elle-même
    // (maxDurationMs atteint), pas via stopCollection().
    TimelineCollectorConfig shortConfig = makeConfig();
    shortConfig.maxDurationMs = 30; // Se termine naturellement, vite.
    collector->setConfig(shortConfig);
    collector->addAddress(addrA, 4);
    ASSERT_TRUE(collector->startCollection(GetCurrentProcess()));

    // Laisse la collecte se terminer d'elle-même (pas de stopCollection()
    // explicite ici -- c'est précisément le cas qui plantait).
    std::this_thread::sleep_for(150ms);
    ASSERT_FALSE(collector->isCollecting());

    // Ce second démarrage n'aurait jamais dû être atteint avant le fix
    // (std::terminate aurait tué le process au premier reassignment de
    // thread ci-dessus, dans le fix lui-même). S'il ne plante pas, c'est
    // gagné -- pas besoin d'assertion supplémentaire sur le résultat.
    EXPECT_TRUE(collector->startCollection(GetCurrentProcess()));
    collector->stopCollection();
}

TEST_F(MemoryTimelineCollectorTest, FinishedCallbackFiresExactlyOnceOnNaturalCompletion) {
    // AUDIT-PIPE-A2 : avant ce correctif, rien ne notifiait la fin naturelle
    // d'une collecte (durée max atteinte sans stopCollection() explicite) --
    // repro live confirmée (getStatus() restait "running" pour toujours).
    std::atomic<int> callCount{0};
    std::atomic<int> lastReason{-1};
    collector->setFinishedCallback([&](TimelineStopReason reason) {
        ++callCount;
        lastReason.store(static_cast<int>(reason));
    });

    TimelineCollectorConfig shortConfig = makeConfig();
    shortConfig.maxDurationMs = 30;
    collector->setConfig(shortConfig);
    collector->addAddress(addrA, 4);
    ASSERT_TRUE(collector->startCollection(GetCurrentProcess()));

    std::this_thread::sleep_for(150ms);
    ASSERT_FALSE(collector->isCollecting());

    EXPECT_EQ(callCount.load(), 1);
    EXPECT_EQ(static_cast<TimelineStopReason>(lastReason.load()), TimelineStopReason::DurationReached);
}

TEST_F(MemoryTimelineCollectorTest, FinishedCallbackFiresExactlyOnceOnExplicitStop) {
    std::atomic<int> callCount{0};
    std::atomic<int> lastReason{-1};
    collector->setFinishedCallback([&](TimelineStopReason reason) {
        ++callCount;
        lastReason.store(static_cast<int>(reason));
    });

    collector->addAddress(addrA, 4);
    ASSERT_TRUE(collector->startCollection(GetCurrentProcess()));
    std::this_thread::sleep_for(60ms);
    collector->stopCollection();

    EXPECT_EQ(callCount.load(), 1);
    EXPECT_EQ(static_cast<TimelineStopReason>(lastReason.load()), TimelineStopReason::UserStop);
}

TEST_F(MemoryTimelineCollectorTest, StartCollectionFailsWithoutWatchedAddresses) {
    EXPECT_FALSE(collector->startCollection(GetCurrentProcess()));
}

TEST_F(MemoryTimelineCollectorTest, StartCollectionFailsWithNullHandle) {
    collector->addAddress(addrA, 4);
    EXPECT_FALSE(collector->startCollection(nullptr));
}

TEST_F(MemoryTimelineCollectorTest, ClearAddressesRemovesWatchedSeriesEntirely) {
    collector->addAddress(addrA, 4);
    collector->addAddress(addrB, 4);
    EXPECT_EQ(collector->watchedAddresses().size(), 2u);

    collector->clearAddresses();

    EXPECT_TRUE(collector->watchedAddresses().empty());
    EXPECT_TRUE(collector->getSeries().empty());
}
