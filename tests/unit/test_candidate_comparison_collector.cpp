// Tests unitaires du collecteur de comparaison (UX-PRODUIT-16, 16B). Même
// technique que test_memory_timeline_collector.cpp : VirtualAlloc +
// GetCurrentProcess() pour que le process de test soit sa propre cible --
// ReadProcessMemory fonctionne normalement sur le pseudo-handle du process
// courant, donc ce sont de vrais tests Win32 (pas des mocks).

#include "visualization/candidate_comparison_collector.h"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>
#include <windows.h>

#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>

using namespace killcore;
using namespace std::chrono_literals;

namespace {

void writeInt32(uint64_t address, int32_t value) {
    std::memcpy(reinterpret_cast<void*>(address), &value, sizeof(value));
}

ComparisonSeriesConfig makeSeries(const QString& id, uint64_t address, ValueType type = ValueType::Int32, double factor = 1.0) {
    ComparisonSeriesConfig cfg;
    cfg.id = id;
    cfg.address = address;
    cfg.type = type;
    cfg.factor = factor;
    cfg.label = id;
    return cfg;
}

} // namespace

class CandidateComparisonCollectorTest : public ::testing::Test {
protected:
    void SetUp() override {
        buffer = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        ASSERT_NE(buffer, nullptr);
        addrA = reinterpret_cast<uint64_t>(buffer);
        addrB = addrA + 4;
        addrC = addrA + 8;
        writeInt32(addrA, 0);
        writeInt32(addrB, 0);
        writeInt32(addrC, 777);
        collector = std::make_unique<CandidateComparisonCollector>();
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

    uint8_t* buffer = nullptr;
    uint64_t addrA = 0;
    uint64_t addrB = 0;
    uint64_t addrC = 0;
    std::unique_ptr<CandidateComparisonCollector> collector;
};

TEST_F(CandidateComparisonCollectorTest, ConfigureRejectsFewerThanTwoSeries) {
    std::vector<ComparisonSeriesConfig> series{makeSeries("a", addrA)};
    EXPECT_FALSE(collector->configure(series, 100, 30000));
}

TEST_F(CandidateComparisonCollectorTest, ConfigureRejectsMoreThanSixSeries) {
    std::vector<ComparisonSeriesConfig> series;
    for (int i = 0; i < 7; ++i) {
        series.push_back(makeSeries(QString("s%1").arg(i), addrA));
    }
    EXPECT_FALSE(collector->configure(series, 100, 30000));
}

TEST_F(CandidateComparisonCollectorTest, ConfigureAcceptsSixSeries) {
    std::vector<ComparisonSeriesConfig> series;
    for (int i = 0; i < 6; ++i) {
        series.push_back(makeSeries(QString("s%1").arg(i), addrA));
    }
    EXPECT_TRUE(collector->configure(series, 100, 30000));
}

TEST_F(CandidateComparisonCollectorTest, ConfigureRejectsDuplicateIds) {
    std::vector<ComparisonSeriesConfig> series{makeSeries("dup", addrA), makeSeries("dup", addrB)};
    EXPECT_FALSE(collector->configure(series, 100, 30000));
}

TEST_F(CandidateComparisonCollectorTest, ConfigureRejectsEmptyId) {
    std::vector<ComparisonSeriesConfig> series{makeSeries("", addrA), makeSeries("b", addrB)};
    EXPECT_FALSE(collector->configure(series, 100, 30000));
}

TEST_F(CandidateComparisonCollectorTest, ConfigureFailsWhileCollecting) {
    std::vector<ComparisonSeriesConfig> series{makeSeries("a", addrA), makeSeries("b", addrB)};
    ASSERT_TRUE(collector->configure(series, 50, 30000));
    ASSERT_TRUE(collector->startCollection(GetCurrentProcess()));

    EXPECT_FALSE(collector->configure(series, 50, 30000));

    collector->stopCollection();
}

TEST_F(CandidateComparisonCollectorTest, IntervalBelowMinimumIsClampedTo50ms) {
    std::vector<ComparisonSeriesConfig> series{makeSeries("a", addrA), makeSeries("b", addrB)};
    ASSERT_TRUE(collector->configure(series, /*intervalMs=*/1, /*maxDurationMs=*/30000));
    ASSERT_TRUE(collector->startCollection(GetCurrentProcess()));

    std::this_thread::sleep_for(300ms);
    collector->stopCollection();

    // A 1ms sans clamp produirait des centaines de tours en 300ms ; a 50ms
    // clampe, on attend un ordre de grandeur de 3-9 tours.
    const auto timings = collector->tourTimings();
    EXPECT_GE(timings.size(), 2u);
    EXPECT_LT(timings.size(), 30u);
}

TEST_F(CandidateComparisonCollectorTest, BatchIdsAreAlignedAcrossSeries) {
    std::vector<ComparisonSeriesConfig> series{makeSeries("a", addrA), makeSeries("b", addrB)};
    ASSERT_TRUE(collector->configure(series, 50, 30000));
    ASSERT_TRUE(collector->startCollection(GetCurrentProcess()));

    std::this_thread::sleep_for(250ms);
    collector->stopCollection();

    const auto pointsA = collector->pointsForSeries("a", 0, 10000);
    const auto pointsB = collector->pointsForSeries("b", 0, 10000);
    ASSERT_EQ(pointsA.size(), pointsB.size());
    ASSERT_GT(pointsA.size(), 0u);
    for (size_t i = 0; i < pointsA.size(); ++i) {
        EXPECT_EQ(pointsA[i].batchId, pointsB[i].batchId);
    }
    // batchId strictement croissant.
    for (size_t i = 1; i < pointsA.size(); ++i) {
        EXPECT_GT(pointsA[i].batchId, pointsA[i - 1].batchId);
    }
}

TEST_F(CandidateComparisonCollectorTest, InvalidAddressIsMarkedInvalidNeverCrashesOrFabricatesZero) {
    // Adresse manifestement non mappée -- ne doit jamais planter, seulement
    // marquer isValid=false avec des octets vides.
    std::vector<ComparisonSeriesConfig> series{makeSeries("a", addrA), makeSeries("bad", 0x1)};
    ASSERT_TRUE(collector->configure(series, 50, 30000));
    ASSERT_TRUE(collector->startCollection(GetCurrentProcess()));

    std::this_thread::sleep_for(150ms);
    collector->stopCollection();

    const auto badPoints = collector->pointsForSeries("bad", 0, 10000);
    ASSERT_GT(badPoints.size(), 0u);
    for (const auto& p : badPoints) {
        EXPECT_FALSE(p.isValid);
        EXPECT_TRUE(p.rawBytes.empty());
    }
}

TEST_F(CandidateComparisonCollectorTest, TargetLostStopsCollectionAfterConsecutiveInvalidTours) {
    std::vector<ComparisonSeriesConfig> series{makeSeries("bad1", 0x1), makeSeries("bad2", 0x2)};
    ASSERT_TRUE(collector->configure(series, 50, 30000));
    ASSERT_TRUE(collector->startCollection(GetCurrentProcess()));

    // 5 tours invalides consécutifs à 50ms -> devrait s'arrêter tout seul
    // bien avant la durée max (30s). Laisse une marge large.
    for (int i = 0; i < 40 && collector->isCollecting(); ++i) {
        std::this_thread::sleep_for(50ms);
    }
    EXPECT_FALSE(collector->isCollecting());
    EXPECT_EQ(collector->lastStopReason(), ComparisonStopReason::TargetLost);
    collector->stopCollection();
}

TEST_F(CandidateComparisonCollectorTest, PointsForSeriesPaginationRespectsOffsetAndLimit) {
    std::vector<ComparisonSeriesConfig> series{makeSeries("a", addrA), makeSeries("b", addrB)};
    ASSERT_TRUE(collector->configure(series, 50, 30000));
    ASSERT_TRUE(collector->startCollection(GetCurrentProcess()));
    std::this_thread::sleep_for(300ms);
    collector->stopCollection();

    const size_t total = collector->pointCountForSeries("a");
    ASSERT_GE(total, 3u);
    const auto firstTwo = collector->pointsForSeries("a", 0, 2);
    EXPECT_EQ(firstTwo.size(), 2u);
    const auto rest = collector->pointsForSeries("a", 2, 10000);
    EXPECT_EQ(rest.size(), total - 2);
    const auto beyondEnd = collector->pointsForSeries("a", total + 5, 10);
    EXPECT_TRUE(beyondEnd.empty());
}

TEST_F(CandidateComparisonCollectorTest, CorrelationRequiresAtLeastTenValidPairs) {
    std::vector<ComparisonSeriesConfig> series{makeSeries("a", addrA), makeSeries("b", addrB)};
    ASSERT_TRUE(collector->configure(series, 50, 30000));
    ASSERT_TRUE(collector->startCollection(GetCurrentProcess()));
    std::this_thread::sleep_for(120ms); // ~2-3 tours seulement, jamais 10.
    collector->stopCollection();

    const auto correlations = collector->correlations();
    ASSERT_EQ(correlations.size(), 1u);
    EXPECT_FALSE(correlations.front().computable);
    EXPECT_FALSE(correlations.front().reason.isEmpty());
}

TEST_F(CandidateComparisonCollectorTest, CorrelatedSeriesAreComputableAndConstantSeriesIsNot) {
    std::vector<ComparisonSeriesConfig> series{
        makeSeries("a", addrA), makeSeries("b", addrB), makeSeries("constant", addrC)};
    ASSERT_TRUE(collector->configure(series, 50, 30000));

    std::atomic<bool> stopWriter{false};
    std::thread writer([this, &stopWriter]() {
        int32_t v = 0;
        while (!stopWriter.load()) {
            writeInt32(addrA, v);
            writeInt32(addrB, v); // parfaitement corrélé avec A
            v += 10;
            std::this_thread::sleep_for(15ms);
        }
    });

    ASSERT_TRUE(collector->startCollection(GetCurrentProcess()));
    std::this_thread::sleep_for(800ms);
    collector->stopCollection();
    stopWriter.store(true);
    writer.join();

    const auto correlations = collector->correlations();
    ASSERT_EQ(correlations.size(), 3u); // C(3,2)

    bool foundAB = false;
    for (const auto& c : correlations) {
        const bool isAB = (c.seriesIdA == "a" && c.seriesIdB == "b") || (c.seriesIdA == "b" && c.seriesIdB == "a");
        const bool involvesConstant = c.seriesIdA == "constant" || c.seriesIdB == "constant";
        if (isAB) {
            foundAB = true;
            ASSERT_TRUE(c.computable) << "a/b devrait etre calculable (assez de paires, correles)";
            EXPECT_GT(c.coefficient, 0.9);
        }
        if (involvesConstant) {
            EXPECT_FALSE(c.computable) << "une serie constante a une variance nulle -- jamais calculable";
            EXPECT_FALSE(c.reason.isEmpty());
        }
    }
    EXPECT_TRUE(foundAB);
}

TEST_F(CandidateComparisonCollectorTest, StopReasonIsDurationReachedWhenMaxDurationElapses) {
    std::vector<ComparisonSeriesConfig> series{makeSeries("a", addrA), makeSeries("b", addrB)};
    ASSERT_TRUE(collector->configure(series, /*intervalMs=*/50, /*maxDurationMs=*/150));
    ASSERT_TRUE(collector->startCollection(GetCurrentProcess()));

    for (int i = 0; i < 40 && collector->isCollecting(); ++i) {
        std::this_thread::sleep_for(20ms);
    }
    EXPECT_FALSE(collector->isCollecting());
    EXPECT_EQ(collector->lastStopReason(), ComparisonStopReason::DurationReached);
}

TEST_F(CandidateComparisonCollectorTest, UserStopReasonWhenStoppedManually) {
    std::vector<ComparisonSeriesConfig> series{makeSeries("a", addrA), makeSeries("b", addrB)};
    ASSERT_TRUE(collector->configure(series, 50, 30000));
    ASSERT_TRUE(collector->startCollection(GetCurrentProcess()));
    std::this_thread::sleep_for(100ms);
    collector->stopCollection();
    EXPECT_EQ(collector->lastStopReason(), ComparisonStopReason::UserStop);
}

TEST_F(CandidateComparisonCollectorTest, RestartAfterNaturalCompletionDoesNotCrash) {
    // Meme piege documente que MemoryTimelineCollector : un thread fini mais
    // jamais joint reste joinable -- verifie que start->completion naturelle
    // ->reconfigure->restart fonctionne sans std::terminate().
    std::vector<ComparisonSeriesConfig> series{makeSeries("a", addrA), makeSeries("b", addrB)};
    ASSERT_TRUE(collector->configure(series, /*intervalMs=*/50, /*maxDurationMs=*/100));
    ASSERT_TRUE(collector->startCollection(GetCurrentProcess()));

    for (int i = 0; i < 40 && collector->isCollecting(); ++i) {
        std::this_thread::sleep_for(20ms);
    }
    ASSERT_FALSE(collector->isCollecting());

    ASSERT_TRUE(collector->configure(series, 50, 30000));
    ASSERT_TRUE(collector->startCollection(GetCurrentProcess()));
    std::this_thread::sleep_for(100ms);
    collector->stopCollection();
    SUCCEED();
}

// -- UX-PRODUIT-16 (16C) -- repères horodatés -------------------------------

TEST_F(CandidateComparisonCollectorTest, AddMarkerRejectedWhenNotCollecting) {
    ComparisonMarker out;
    EXPECT_FALSE(collector->addMarker("avant capture", &out));
    EXPECT_TRUE(collector->markers().empty());
}

TEST_F(CandidateComparisonCollectorTest, AddMarkerAcceptedWhileCollectingWithIncrementingIds) {
    std::vector<ComparisonSeriesConfig> series{makeSeries("a", addrA), makeSeries("b", addrB)};
    ASSERT_TRUE(collector->configure(series, 50, 30000));
    ASSERT_TRUE(collector->startCollection(GetCurrentProcess()));

    ComparisonMarker first;
    ComparisonMarker second;
    ASSERT_TRUE(collector->addMarker("action observee", &first));
    std::this_thread::sleep_for(20ms);
    ASSERT_TRUE(collector->addMarker("deuxieme repere", &second));
    collector->stopCollection();

    EXPECT_EQ(first.id, 1u);
    EXPECT_EQ(second.id, 2u);
    EXPECT_GE(second.timestampMs, first.timestampMs);
    EXPECT_EQ(first.text, "action observee");

    const auto markers = collector->markers();
    ASSERT_EQ(markers.size(), 2u);
    EXPECT_EQ(markers[0].text, "action observee");
    EXPECT_EQ(markers[1].text, "deuxieme repere");
}

TEST_F(CandidateComparisonCollectorTest, AddMarkerRefusedAfterCaptureEnds) {
    std::vector<ComparisonSeriesConfig> series{makeSeries("a", addrA), makeSeries("b", addrB)};
    ASSERT_TRUE(collector->configure(series, /*intervalMs=*/50, /*maxDurationMs=*/100));
    ASSERT_TRUE(collector->startCollection(GetCurrentProcess()));
    for (int i = 0; i < 40 && collector->isCollecting(); ++i) {
        std::this_thread::sleep_for(20ms);
    }
    ASSERT_FALSE(collector->isCollecting());

    ComparisonMarker out;
    EXPECT_FALSE(collector->addMarker("trop tard", &out));
}

TEST_F(CandidateComparisonCollectorTest, AddMarkerRejectsEmptyOrWhitespaceText) {
    std::vector<ComparisonSeriesConfig> series{makeSeries("a", addrA), makeSeries("b", addrB)};
    ASSERT_TRUE(collector->configure(series, 50, 30000));
    ASSERT_TRUE(collector->startCollection(GetCurrentProcess()));

    ComparisonMarker out;
    EXPECT_FALSE(collector->addMarker("", &out));
    EXPECT_FALSE(collector->addMarker("   ", &out));
    collector->stopCollection();
    EXPECT_TRUE(collector->markers().empty());
}

TEST_F(CandidateComparisonCollectorTest, AddMarkerRejectsTextOver500Characters) {
    std::vector<ComparisonSeriesConfig> series{makeSeries("a", addrA), makeSeries("b", addrB)};
    ASSERT_TRUE(collector->configure(series, 50, 30000));
    ASSERT_TRUE(collector->startCollection(GetCurrentProcess()));

    const QString tooLong(501, QChar('x'));
    const QString exactly500(500, QChar('x'));
    ComparisonMarker out;
    EXPECT_FALSE(collector->addMarker(tooLong, &out));
    EXPECT_TRUE(collector->addMarker(exactly500, &out));
    collector->stopCollection();
}

TEST_F(CandidateComparisonCollectorTest, AddMarkerCapsAtOneHundredMarkers) {
    std::vector<ComparisonSeriesConfig> series{makeSeries("a", addrA), makeSeries("b", addrB)};
    ASSERT_TRUE(collector->configure(series, 50, 30000));
    ASSERT_TRUE(collector->startCollection(GetCurrentProcess()));

    ComparisonMarker out;
    for (int i = 0; i < 100; ++i) {
        ASSERT_TRUE(collector->addMarker(QString("repere %1").arg(i), &out)) << "repere " << i;
    }
    EXPECT_FALSE(collector->addMarker("le cent-unieme, refuse", &out));
    collector->stopCollection();
    EXPECT_EQ(collector->markers().size(), 100u);
}

TEST_F(CandidateComparisonCollectorTest, MarkersClearedOnReconfigure) {
    std::vector<ComparisonSeriesConfig> series{makeSeries("a", addrA), makeSeries("b", addrB)};
    ASSERT_TRUE(collector->configure(series, 50, 30000));
    ASSERT_TRUE(collector->startCollection(GetCurrentProcess()));
    ComparisonMarker out;
    ASSERT_TRUE(collector->addMarker("repere de la premiere capture", &out));
    collector->stopCollection();
    ASSERT_EQ(collector->markers().size(), 1u);

    ASSERT_TRUE(collector->configure(series, 50, 30000));
    EXPECT_TRUE(collector->markers().empty());
}

// -- UX-PRODUIT-16 (16C) -- export JSON -------------------------------------

class CandidateComparisonExportTest : public CandidateComparisonCollectorTest {
protected:
    void TearDown() override {
        CandidateComparisonCollectorTest::TearDown();
        if (!exportPath.empty() && std::filesystem::exists(exportPath)) {
            std::filesystem::remove(exportPath);
        }
    }

    std::string exportPath;
};

TEST_F(CandidateComparisonExportTest, ExportWritesParsableFileWithExpectedShape) {
    std::vector<ComparisonSeriesConfig> series{makeSeries("a", addrA), makeSeries("b", addrB)};
    ASSERT_TRUE(collector->configure(series, 50, 30000));
    ASSERT_TRUE(collector->startCollection(GetCurrentProcess()));
    ComparisonMarker marker;
    ASSERT_TRUE(collector->addMarker("point d'interet", &marker));
    std::this_thread::sleep_for(150ms);
    collector->stopCollection();

    exportPath = (std::filesystem::temp_directory_path()
        / "killengine_candidate_comparison_export_test.json").string();
    ASSERT_TRUE(collector->exportToJson(exportPath));

    std::ifstream file(exportPath);
    ASSERT_TRUE(file.is_open());
    nlohmann::json j;
    file >> j;

    EXPECT_EQ(j.at("kind").get<std::string>(), "candidate_comparison_export");
    EXPECT_EQ(j.at("schemaVersion").get<int>(), 1);
    EXPECT_EQ(j.at("capture").at("collecting").get<bool>(), false);
    EXPECT_EQ(j.at("capture").at("stopReason").get<std::string>(), "user_stop");

    ASSERT_EQ(j.at("series").size(), 2u);
    const auto& seriesA = j.at("series")[0];
    EXPECT_EQ(seriesA.at("id").get<std::string>(), "a");
    EXPECT_GT(seriesA.at("points").size(), 0u);
    const auto& firstPoint = seriesA.at("points")[0];
    EXPECT_TRUE(firstPoint.contains("timestampMs"));
    EXPECT_TRUE(firstPoint.contains("isValid"));

    ASSERT_EQ(j.at("markers").size(), 1u);
    EXPECT_EQ(j.at("markers")[0].at("text").get<std::string>(), "point d'interet");
    EXPECT_EQ(j.at("markers")[0].at("id").get<uint32_t>(), marker.id);

    ASSERT_EQ(j.at("correlations").size(), 1u);
    EXPECT_TRUE(j.at("correlations")[0].contains("computable"));

    EXPECT_EQ(j.at("method").at("correlation").get<std::string>(), "pearson_paired_by_batch_id");
    EXPECT_EQ(j.at("method").at("minPairsForComputable").get<int>(), 10);
}

TEST_F(CandidateComparisonExportTest, ExportWorksOnAStoppedCaptureNoActiveCaptureRequired) {
    std::vector<ComparisonSeriesConfig> series{makeSeries("a", addrA), makeSeries("b", addrB)};
    ASSERT_TRUE(collector->configure(series, 50, 30000));
    ASSERT_TRUE(collector->startCollection(GetCurrentProcess()));
    std::this_thread::sleep_for(100ms);
    collector->stopCollection();
    ASSERT_FALSE(collector->isCollecting());

    exportPath = (std::filesystem::temp_directory_path()
        / "killengine_candidate_comparison_export_stopped_test.json").string();
    EXPECT_TRUE(collector->exportToJson(exportPath));
    EXPECT_TRUE(std::filesystem::exists(exportPath));
}

TEST_F(CandidateComparisonExportTest, ExportContainsDecodedValuesNotJustRawBytes) {
    std::vector<ComparisonSeriesConfig> series{makeSeries("a", addrA), makeSeries("b", addrB)};
    ASSERT_TRUE(collector->configure(series, 50, 30000));
    ASSERT_TRUE(collector->startCollection(GetCurrentProcess()));
    std::this_thread::sleep_for(100ms);
    collector->stopCollection();

    exportPath = (std::filesystem::temp_directory_path()
        / "killengine_candidate_comparison_export_decoded_test.json").string();
    ASSERT_TRUE(collector->exportToJson(exportPath));

    std::ifstream file(exportPath);
    nlohmann::json j;
    file >> j;

    const auto& points = j.at("series")[0].at("points");
    bool foundDecodedValue = false;
    for (const auto& point : points) {
        if (point.at("isValid").get<bool>() && point.contains("exactValueText")) {
            foundDecodedValue = true;
            EXPECT_TRUE(point.contains("scaledValueText"));
            EXPECT_TRUE(point.contains("numericValue"));
        }
    }
    EXPECT_TRUE(foundDecodedValue) << "au moins un point valide devrait porter une valeur decodee exacte";
}
