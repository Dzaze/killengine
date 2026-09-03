// Tests unitaires du collecteur Win32 (PROPOSITIONS-1 #5 — Memory Heatmap).
// Contrairement aux autres tests de ce fichier "Timeline/Heatmap/Pattern
// Learning" (tous purs, aucun process réel), celui-ci exerce réellement
// VirtualQueryEx/ReadProcessMemory via MemoryHeatmapCollector::startCollection
// contre une vraie région mémoire — pas de mock. Utilise le process de test
// lui-même comme cible (VirtualAlloc + GetCurrentProcess()) plutôt que de
// lancer KillEngineTestTarget.exe : ReadProcessMemory/VirtualQueryEx
// fonctionnent normalement sur le pseudo-handle du process courant, donc
// c'est un vrai test Win32 sans dépendance externe ni process séparé à
// synchroniser. Comble le point "pas de tests unitaires dédiés au
// collecteur Win32" resté ouvert sur MEMORY-TIMELINE-VIS-1/ANALYSE-CLINE-1
// (docs/PHASE_TRACKER.md), côté Memory Heatmap.

#include "visualization/memory_heatmap_collector.h"

#include <gtest/gtest.h>

#include <windows.h>

#include <chrono>
#include <cstring>
#include <thread>

using namespace killcore;
using namespace std::chrono_literals;

namespace {

constexpr uint32_t kRegionSize = 4096;
constexpr uint32_t kPageCount = 3;

} // namespace

class MemoryHeatmapCollectorTest : public ::testing::Test {
protected:
    void SetUp() override {
        // 3 pages contiguës, mêmes droits -> VirtualQueryEx les voit comme
        // UNE région (mbi.RegionSize = 3*4096), que le collecteur subdivise
        // lui-même en pages de regionSize (voir refreshTrackedPages).
        buffer = static_cast<uint8_t*>(VirtualAlloc(
            nullptr, kRegionSize * kPageCount, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        ASSERT_NE(buffer, nullptr);
        std::memset(buffer, 0xAA, static_cast<size_t>(kRegionSize) * kPageCount);
        base = reinterpret_cast<uint64_t>(buffer);
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

    HeatmapConfig makeConfig(bool trackReads = true, bool trackWrites = true) const {
        HeatmapConfig config;
        config.samplingIntervalMs = 15;
        config.regionSize = kRegionSize;
        config.minAddress = base;
        config.maxAddress = base + kRegionSize * kPageCount;
        config.maxPagesPerTick = kPageCount; // toutes les pages en un tick, pas de round-robin à attendre.
        config.trackReads = trackReads;
        config.trackWrites = trackWrites;
        return config;
    }

    const HeatmapRegion* findRegion(const std::vector<HeatmapRegion>& regions, uint64_t address) const {
        for (const auto& region : regions) {
            if (region.baseAddress == address) return &region;
        }
        return nullptr;
    }

    uint8_t* buffer = nullptr;
    uint64_t base = 0;
    std::unique_ptr<MemoryHeatmapCollector> collector;
};

TEST_F(MemoryHeatmapCollectorTest, DiscoversAllThreePagesFromOneVirtualAllocRegion) {
    collector = std::make_unique<MemoryHeatmapCollector>(makeConfig());
    ASSERT_TRUE(collector->startCollection(GetCurrentProcess()));

    std::this_thread::sleep_for(100ms);
    collector->stopCollection();

    const auto stats = collector->getStats();
    EXPECT_EQ(stats.totalRegions, kPageCount);
}

TEST_F(MemoryHeatmapCollectorTest, DetectsWriteViaContentHashChangeOnSpecificPageOnly) {
    collector = std::make_unique<MemoryHeatmapCollector>(makeConfig());
    ASSERT_TRUE(collector->startCollection(GetCurrentProcess()));

    // Laisse au moins un tick lire l'état initial (hash de référence) avant
    // toute mutation -- un changement au tout premier sample n'a rien à quoi
    // se comparer (hadPreviousHash == false, voir samplePage()).
    std::this_thread::sleep_for(60ms);

    // Ne modifie QUE la première page -- vérifie que le hash-diff est bien
    // par page, pas un hash global qui confondrait les 3 pages.
    buffer[0] = 0xFF;

    std::this_thread::sleep_for(80ms);
    collector->stopCollection();

    const auto regions = collector->getTopRegions(10);
    const auto* changedPage = findRegion(regions, base);
    const auto* untouchedPageA = findRegion(regions, base + kRegionSize);
    const auto* untouchedPageB = findRegion(regions, base + 2 * kRegionSize);

    ASSERT_NE(changedPage, nullptr);
    ASSERT_NE(untouchedPageA, nullptr);
    ASSERT_NE(untouchedPageB, nullptr);

    EXPECT_GE(changedPage->writeCount, 1u);
    EXPECT_EQ(untouchedPageA->writeCount, 0u);
    EXPECT_EQ(untouchedPageB->writeCount, 0u);

    // intensity = writeCount normalisé contre le max observé -> la page
    // modifiée doit être à 1.0, les autres à 0.0 (voir updateStats()).
    EXPECT_FLOAT_EQ(changedPage->intensity, 1.0f);
    EXPECT_FLOAT_EQ(untouchedPageA->intensity, 0.0f);
}

TEST_F(MemoryHeatmapCollectorTest, ReadCountIncrementsEverySampleNotJustOnce) {
    // Régression du bug corrigé le 02/09/2026 (voir commentaire dans
    // memory_heatmap_collector.cpp) : readCount restait bloqué à 1,
    // incrémenté seulement à la création de la région.
    collector = std::make_unique<MemoryHeatmapCollector>(makeConfig());
    ASSERT_TRUE(collector->startCollection(GetCurrentProcess()));

    std::this_thread::sleep_for(50ms);
    const auto midStats = collector->getStats();

    std::this_thread::sleep_for(80ms);
    collector->stopCollection();
    const auto finalStats = collector->getStats();

    EXPECT_GT(finalStats.totalReads, midStats.totalReads);
}

TEST_F(MemoryHeatmapCollectorTest, TrackWritesFalseSuppressesWriteCountButNotReadCount) {
    // Régression du même bug 02/09/2026 : trackReads/trackWrites étaient
    // ignorés avant la correction (toujours comptés quoi qu'il arrive).
    collector = std::make_unique<MemoryHeatmapCollector>(makeConfig(/*trackReads=*/true, /*trackWrites=*/false));
    ASSERT_TRUE(collector->startCollection(GetCurrentProcess()));

    std::this_thread::sleep_for(60ms);
    buffer[0] = 0x11;
    std::this_thread::sleep_for(80ms);
    collector->stopCollection();

    const auto regions = collector->getTopRegions(10);
    const auto* changedPage = findRegion(regions, base);
    ASSERT_NE(changedPage, nullptr);

    EXPECT_EQ(changedPage->writeCount, 0u);
    EXPECT_GE(changedPage->readCount, 1u);
}

TEST_F(MemoryHeatmapCollectorTest, TrackReadsFalseSuppressesReadCountButWritesStillCounted) {
    collector = std::make_unique<MemoryHeatmapCollector>(makeConfig(/*trackReads=*/false, /*trackWrites=*/true));
    ASSERT_TRUE(collector->startCollection(GetCurrentProcess()));

    std::this_thread::sleep_for(60ms);
    buffer[0] = 0x22;
    std::this_thread::sleep_for(80ms);
    collector->stopCollection();

    const auto regions = collector->getTopRegions(10);
    const auto* changedPage = findRegion(regions, base);
    ASSERT_NE(changedPage, nullptr);

    EXPECT_EQ(changedPage->readCount, 0u);
    EXPECT_GE(changedPage->writeCount, 1u);
    // trackReads==false -> accessCount suit writeCount (voir samplePage()).
    EXPECT_GE(changedPage->accessCount, 1u);
}

TEST_F(MemoryHeatmapCollectorTest, GetRegionsInRangeFiltersByAddressBounds) {
    collector = std::make_unique<MemoryHeatmapCollector>(makeConfig());
    ASSERT_TRUE(collector->startCollection(GetCurrentProcess()));
    std::this_thread::sleep_for(60ms);
    collector->stopCollection();

    const auto onlyFirstPage = collector->getRegionsInRange(base, base + kRegionSize);
    EXPECT_EQ(onlyFirstPage.size(), 1u);

    const auto allPages = collector->getRegionsInRange(base, base + kRegionSize * kPageCount);
    EXPECT_EQ(allPages.size(), kPageCount);
}

TEST_F(MemoryHeatmapCollectorTest, ResetClearsCollectedRegionsAndStats) {
    collector = std::make_unique<MemoryHeatmapCollector>(makeConfig());
    ASSERT_TRUE(collector->startCollection(GetCurrentProcess()));
    std::this_thread::sleep_for(60ms);
    collector->stopCollection();
    ASSERT_GT(collector->getStats().totalRegions, 0u);

    collector->reset();

    EXPECT_EQ(collector->getStats().totalRegions, 0u);
    EXPECT_TRUE(collector->getTopRegions(10).empty());
}

TEST_F(MemoryHeatmapCollectorTest, ExportToJsonContainsStatsAndRegionAddress) {
    collector = std::make_unique<MemoryHeatmapCollector>(makeConfig());
    ASSERT_TRUE(collector->startCollection(GetCurrentProcess()));
    std::this_thread::sleep_for(60ms);
    collector->stopCollection();

    const std::string json = collector->exportToJson();

    EXPECT_NE(json.find("\"totalRegions\""), std::string::npos);
    EXPECT_NE(json.find("\"regions\""), std::string::npos);

    char hexAddr[32];
    std::snprintf(hexAddr, sizeof(hexAddr), "%llx", static_cast<unsigned long long>(base));
    EXPECT_NE(json.find(hexAddr), std::string::npos);
}

TEST_F(MemoryHeatmapCollectorTest, StartCollectionFailsWhenAlreadyCollecting) {
    collector = std::make_unique<MemoryHeatmapCollector>(makeConfig());
    ASSERT_TRUE(collector->startCollection(GetCurrentProcess()));

    EXPECT_FALSE(collector->startCollection(GetCurrentProcess()));

    collector->stopCollection();
}
