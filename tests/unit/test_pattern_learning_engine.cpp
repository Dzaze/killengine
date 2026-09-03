// Tests unitaires de PatternLearningEngine (PROPOSITIONS-1 #2) — comble le
// point "clusterAddresses/suggestions avancées restent à tester/durcir"
// resté ouvert sur PATTERN-LEARNING-1 (docs/PHASE_TRACKER.md). Les 28 tests
// existants (test_feature_extractor.cpp, test_game_profile_database.cpp) ne
// couvrent que les deux couches en dessous (extraction de features brutes,
// persistance JSON pure) — aucun test n'exerçait encore detectEngine,
// classifyPattern, suggestResolutionPaths/suggestValueTypes ou
// clusterAddresses eux-mêmes.
//
// Convention identique à test_game_profile_database.cpp : QTemporaryDir pour
// un chemin de base isolé, pas de dépendance à AppData réel (PatternLearningEngine
// prend un chemin explicite dans initialize(), contrairement à
// PatternLearningManager qui utilise QStandardPaths::AppDataLocation).

#include "pattern_learning/pattern_learning_engine.h"

#include <gtest/gtest.h>

#include <QTemporaryDir>

#include <algorithm>
#include <set>

using killcore::EngineType;
using killcore::GameProfile;
using killcore::KnownOffset;
using killcore::PatternLearningEngine;
using killcore::PatternType;

namespace {

QString dbPathIn(const QTemporaryDir& dir) {
    return dir.filePath("pattern_learning.json");
}

} // namespace

class PatternLearningEngineTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(tempDir.isValid());
        engine = std::make_unique<PatternLearningEngine>();
        ASSERT_TRUE(engine->initialize(dbPathIn(tempDir).toStdString()));
    }

    QTemporaryDir tempDir;
    std::unique_ptr<PatternLearningEngine> engine;
};

// ---------------------------------------------------------------------------
// Garde-fous avant/à l'échec d'initialisation
// ---------------------------------------------------------------------------

TEST(PatternLearningEngineUninitializedTest, OperationsReturnSafeDefaultsBeforeInitialize) {
    PatternLearningEngine fresh;
    ASSERT_FALSE(fresh.isInitialized());

    const auto engineResult = fresh.detectEngine({"UnityPlayer.dll"}, {});
    EXPECT_EQ(engineResult.type, EngineType::Unknown);
    EXPECT_DOUBLE_EQ(engineResult.confidence, 0.0);

    const auto classification = fresh.classifyPattern(0x1000, {1.0, 2.0, 3.0}, {0, 100, 200});
    EXPECT_EQ(classification.type, PatternType::Unknown);

    EXPECT_FALSE(fresh.loadProfile("AnyGame").has_value());
    EXPECT_FALSE(fresh.saveProfile(GameProfile{}));
    EXPECT_TRUE(fresh.listKnownGames().empty());
    EXPECT_FALSE(fresh.deleteProfile("AnyGame"));
    EXPECT_TRUE(fresh.suggestResolutionPaths("AnyGame", PatternType::ResourceCounter).empty());
    EXPECT_DOUBLE_EQ(fresh.getTypeSuccessRate("AnyGame", "Int32"), 0.0);
}

// ---------------------------------------------------------------------------
// detectEngine
// ---------------------------------------------------------------------------

TEST_F(PatternLearningEngineTest, DetectEngineMatchesUnityFromModuleName) {
    const auto result = engine->detectEngine({"UnityPlayer.dll", "kernel32.dll"}, {});
    EXPECT_EQ(result.type, EngineType::Unity);
    EXPECT_GT(result.confidence, 0.0);
}

TEST_F(PatternLearningEngineTest, DetectEngineReturnsUnknownForUnrecognizedModules) {
    const auto result = engine->detectEngine({"user32.dll", "ntdll.dll"}, {});
    EXPECT_EQ(result.type, EngineType::Unknown);
    EXPECT_DOUBLE_EQ(result.confidence, 0.0);
}

TEST_F(PatternLearningEngineTest, DetectEngineMemorySampleBoostsConfidenceAndExtractsVersion) {
    const auto moduleOnly = engine->detectEngine({"UnityPlayer.dll"}, {});

    const std::string sample = "junk...UnityEngine 2022.3.5f1 more junk";
    const std::vector<uint8_t> memorySample(sample.begin(), sample.end());
    const auto withMemory = engine->detectEngine({"UnityPlayer.dll"}, memorySample);

    EXPECT_GT(withMemory.confidence, moduleOnly.confidence);
    EXPECT_EQ(withMemory.version, "2022.3.5");
}

// ---------------------------------------------------------------------------
// suggestValueTypes — table pure, déterministe, comportement figé par test
// ---------------------------------------------------------------------------

TEST_F(PatternLearningEngineTest, SuggestValueTypesUnityResourceCounter) {
    const auto types = engine->suggestValueTypes(EngineType::Unity, PatternType::ResourceCounter);
    EXPECT_EQ(types, (std::vector<std::string>{"Int32", "Int32x100", "Float32"}));
}

TEST_F(PatternLearningEngineTest, SuggestValueTypesUnrealHealthPool) {
    const auto types = engine->suggestValueTypes(EngineType::UnrealEngine, PatternType::HealthPool);
    EXPECT_EQ(types, (std::vector<std::string>{"Float32", "Int32x10"}));
}

TEST_F(PatternLearningEngineTest, SuggestValueTypesUnknownEngineFallsBackToGenericTypes) {
    const auto types = engine->suggestValueTypes(EngineType::Unknown, PatternType::Coordinate);
    EXPECT_EQ(types, (std::vector<std::string>{"Int32", "Float32", "Int64"}));
}

TEST_F(PatternLearningEngineTest, SuggestValueTypesUnityUnhandledPatternReturnsEmpty) {
    // Le switch Unity ne couvre que ResourceCounter/HealthPool -- tout autre
    // pattern retombe sur une liste vide (pas le fallback générique, qui est
    // réservé au `default:` du switch sur l'ENGINE, pas sur le pattern).
    const auto types = engine->suggestValueTypes(EngineType::Unity, PatternType::Timer);
    EXPECT_TRUE(types.empty());
}

// ---------------------------------------------------------------------------
// suggestResolutionPaths
// ---------------------------------------------------------------------------

TEST_F(PatternLearningEngineTest, SuggestResolutionPathsReturnsEmptyForUnknownGame) {
    EXPECT_TRUE(engine->suggestResolutionPaths("NeverSaved", PatternType::HealthPool).empty());
}

TEST_F(PatternLearningEngineTest, SuggestResolutionPathsReturnsSavedProfileSuccessfulPaths) {
    GameProfile profile;
    profile.gameName = "TestGame";
    profile.successfulPaths = {"module+0x10->+0x20", "module+0x30"};
    ASSERT_TRUE(engine->saveProfile(profile));

    // Note comportementale : targetType n'est en réalité pas utilisé par
    // l'implémentation actuelle pour filtrer -- suggestResolutionPaths
    // retourne TOUS les successfulPaths du profil quel que soit le type
    // demandé. Vérifié ici tel quel (deux types différents, même résultat)
    // plutôt que supposé, pour que ce comportement reste visible si une
    // future implémentation change ça sans le vouloir.
    const auto forHealth = engine->suggestResolutionPaths("TestGame", PatternType::HealthPool);
    const auto forTimer = engine->suggestResolutionPaths("TestGame", PatternType::Timer);
    EXPECT_EQ(forHealth, profile.successfulPaths);
    EXPECT_EQ(forTimer, profile.successfulPaths);
}

// ---------------------------------------------------------------------------
// clusterAddresses — non-déterministe par construction (centroïdes tirés au
// hasard), donc les tests portent sur des invariants qui tiennent quel que
// soit le tirage, pas sur une assignation exacte.
// ---------------------------------------------------------------------------

TEST_F(PatternLearningEngineTest, ClusterAddressesEmptyInputReturnsEmpty) {
    EXPECT_TRUE(engine->clusterAddresses({}, {}).empty());
}

TEST_F(PatternLearningEngineTest, ClusterAddressesRejectsMismatchedSizesInsteadOfCrashing) {
    // Durcissement du 03/09/2026 (voir commentaire dans
    // pattern_learning_engine.cpp) : avant le fix, ceci accédait
    // features[i] hors limites (UB / plantage), jamais testé jusqu'ici.
    const std::vector<uint64_t> addresses = {0x1000, 0x2000, 0x3000};
    const std::vector<std::vector<double>> tooFewFeatures = {{1.0, 2.0}};
    EXPECT_TRUE(engine->clusterAddresses(addresses, tooFewFeatures).empty());
}

TEST_F(PatternLearningEngineTest, ClusterAddressesPartitionsEveryAddressExactlyOnceAcrossThreeClusters) {
    std::vector<uint64_t> addresses;
    std::vector<std::vector<double>> features;
    for (int i = 0; i < 9; ++i) {
        addresses.push_back(0x1000 + static_cast<uint64_t>(i) * 0x10);
        features.push_back({static_cast<double>(i), static_cast<double>(i % 3)});
    }

    const auto clusters = engine->clusterAddresses(addresses, features);
    ASSERT_EQ(clusters.size(), 3u); // k=3, fixé dans l'implémentation.

    std::multiset<uint64_t> allAssigned;
    for (const auto& cluster : clusters) {
        for (uint64_t addr : cluster.addresses) {
            allAssigned.insert(addr);
        }
    }
    EXPECT_EQ(allAssigned.size(), addresses.size());
    for (uint64_t addr : addresses) {
        EXPECT_EQ(allAssigned.count(addr), 1u) << "address 0x" << std::hex << addr;
    }
}

TEST_F(PatternLearningEngineTest, ClusterAddressesSingleAddressDeterministicallyGoesToFirstCluster) {
    // Cas dégénéré déterministe malgré le tirage aléatoire : avec une seule
    // adresse, les 3 centroïdes initiaux sont TOUS tirés depuis ce même
    // unique point (dis(0, size-1) == dis(0,0) == 0 systématiquement), donc
    // la distance au cluster 0 est 0 et gagne le premier test `dist < minDist`
    // (comparaison stricte -> les ex-aequo suivants ne remplacent jamais).
    const std::vector<uint64_t> addresses = {0xDEAD};
    const std::vector<std::vector<double>> features = {{1.0, 2.0, 3.0}};

    const auto clusters = engine->clusterAddresses(addresses, features);
    ASSERT_EQ(clusters.size(), 3u);
    EXPECT_EQ(clusters[0].addresses, (std::vector<uint64_t>{0xDEAD}));
    EXPECT_TRUE(clusters[1].addresses.empty());
    EXPECT_TRUE(clusters[2].addresses.empty());
}
