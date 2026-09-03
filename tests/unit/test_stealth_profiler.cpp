// Tests unitaires de StealthProfiler (chantier Cline "Stealth Profiler",
// repris 03/09/2026). Logique de scoring 100% pure (core/debug/stealth_profiler.*)
// -- aucun appel Win32, aucun process réel requis. Les entrées réelles
// (énumération de modules, CheckRemoteDebuggerPresent) sont rassemblées côté
// ApplicationController::analyzeStealthRisk(), vérifié séparément en live via
// le pipe d'automation (voir docs/PHASE_TRACKER.md).

#include "debug/stealth_profiler.h"

#include <gtest/gtest.h>

using killcore::StealthAnalysis;
using killcore::StealthProfiler;

namespace {

bool anyRecommendationContains(const StealthAnalysis& analysis, const QString& substring) {
    for (const auto& rec : analysis.recommendations) {
        if (rec.contains(substring, Qt::CaseInsensitive)) return true;
    }
    return false;
}

bool hasThreatWithSource(const StealthAnalysis& analysis, const QString& source) {
    for (const auto& threat : analysis.threats) {
        if (threat.source == source) return true;
    }
    return false;
}

const killcore::StealthThreat* findThreatWithSource(const StealthAnalysis& analysis, const QString& source) {
    for (const auto& threat : analysis.threats) {
        if (threat.source == source) return &threat;
    }
    return nullptr;
}

} // namespace

TEST(StealthProfilerTest, NoThreatsReturnsLowRiskWithCoverageGapRecommendation) {
    const auto analysis = StealthProfiler::analyze({}, /*debuggerVisible=*/false,
        /*antiDebug=*/false, /*processMask=*/false, /*dllMask=*/false);

    EXPECT_EQ(analysis.riskScore, 0);
    EXPECT_EQ(analysis.riskLevel, "low");
    EXPECT_TRUE(analysis.threats.empty());
    ASSERT_EQ(analysis.recommendations.size(), 1);
    EXPECT_TRUE(anyRecommendationContains(analysis, "Warden"));
}

TEST(StealthProfilerTest, DetectsSingleKnownAntiCheatModule) {
    const auto analysis = StealthProfiler::analyze({"BEService.exe"}, false, false, false, false);

    // 1 menace "module" (BattlEye) + 2 "coverage_gap" (processMask/dllMask
    // inactifs alors qu'une protection est présente) = 3 menaces au total.
    ASSERT_EQ(analysis.threats.size(), 3u);
    const auto* moduleThreat = findThreatWithSource(analysis, "module");
    ASSERT_NE(moduleThreat, nullptr);
    EXPECT_EQ(moduleThreat->name, "BattlEye");
    EXPECT_EQ(moduleThreat->detail, "BEService.exe");
    // 30 (module) + 15 (processMask gap) + 15 (dllMask gap) = 60.
    EXPECT_EQ(analysis.riskScore, 60);
    EXPECT_EQ(analysis.riskLevel, "high");
}

TEST(StealthProfilerTest, DedupesMultipleModulesFromSameProduct) {
    const auto analysis = StealthProfiler::analyze(
        {"BEClient_x64.dll", "BEService.exe"}, false, false, false, false);

    // Une seule menace "BattlEye", pas deux, même si 2 modules matchent.
    int battlEyeCount = 0;
    for (const auto& threat : analysis.threats) {
        if (threat.name == "BattlEye") battlEyeCount++;
    }
    EXPECT_EQ(battlEyeCount, 1);
}

TEST(StealthProfilerTest, DebuggerVisibleWithoutAntiDebugAddsThreatAndRecommendation) {
    const auto analysis = StealthProfiler::analyze({}, /*debuggerVisible=*/true,
        /*antiDebug=*/false, false, false);

    ASSERT_TRUE(hasThreatWithSource(analysis, "debugger_visible"));
    EXPECT_EQ(analysis.riskScore, 25);
    EXPECT_EQ(analysis.riskLevel, "medium");
    EXPECT_TRUE(anyRecommendationContains(analysis, "antiDebug"));
}

TEST(StealthProfilerTest, DebuggerVisibleWithAntiDebugActiveIsWorseNotBetter) {
    // Contradiction : le module est actif mais le flag reste visible --
    // signal plus grave qu'un simple "pas encore activé", pas moins.
    const auto withoutAntiDebug = StealthProfiler::analyze({}, true, false, false, false);
    const auto withAntiDebugButStillVisible = StealthProfiler::analyze({}, true, true, false, false);

    EXPECT_GT(withAntiDebugButStillVisible.riskScore, withoutAntiDebug.riskScore);
    EXPECT_EQ(withAntiDebugButStillVisible.riskScore, 40);
    EXPECT_TRUE(anyRecommendationContains(withAntiDebugButStillVisible, "hooks"));
}

TEST(StealthProfilerTest, AntiCheatDetectedWithAllMasksActiveSkipsCoverageGapRecommendations) {
    const auto analysis = StealthProfiler::analyze(
        {"EasyAntiCheat.exe"}, /*debuggerVisible=*/false,
        /*antiDebug=*/true, /*processMask=*/true, /*dllMask=*/true);

    EXPECT_EQ(analysis.riskScore, 30); // Que le module -- aucun gap de couverture.
    EXPECT_EQ(analysis.riskLevel, "medium");
    // Une seule menace (le module) et une seule recommandation générique --
    // pas de menace/recommandation "coverage_gap" puisque les deux masks
    // sont actifs. Note : ne pas chercher "processMask"/"dllMask" en
    // sous-chaîne dans le texte des recommandations, la recommandation
    // générique ("...activer le profil 'sc2' (antiDebug+processMask+dllMask)")
    // les mentionne déjà en décrivant le profil -- ces deux assertions par
    // taille sont le signal fiable, pas un grep de texte.
    EXPECT_EQ(analysis.threats.size(), 1u);
    EXPECT_EQ(analysis.recommendations.size(), 1);
    EXPECT_FALSE(hasThreatWithSource(analysis, "coverage_gap"));
}

TEST(StealthProfilerTest, RiskScoreClampedAtHundred) {
    const auto analysis = StealthProfiler::analyze(
        {"BEService.exe", "EasyAntiCheat.exe", "vgc.exe"}, /*debuggerVisible=*/true,
        /*antiDebug=*/true, /*processMask=*/false, /*dllMask=*/false);

    // 3*30 (modules) + 40 (contradiction debugger) + 15 + 15 (masks) = 160 -> clampé à 100.
    EXPECT_EQ(analysis.riskScore, 100);
    EXPECT_EQ(analysis.riskLevel, "high");
}

TEST(StealthProfilerTest, ModuleMatchIsCaseInsensitive) {
    const auto analysis = StealthProfiler::analyze({"beservice.exe"}, false, false, false, false);
    const auto* moduleThreat = findThreatWithSource(analysis, "module");
    ASSERT_NE(moduleThreat, nullptr);
    EXPECT_EQ(moduleThreat->name, "BattlEye");
}

TEST(StealthProfilerTest, KnownSignaturesTableIsNonEmptyAndIncludesCommonProducts) {
    const auto& signatures = StealthProfiler::knownSignatures();
    EXPECT_GT(signatures.size(), 5u);

    bool hasBattlEye = false;
    bool hasEasyAntiCheat = false;
    for (const auto& sig : signatures) {
        if (sig.displayName == "BattlEye") hasBattlEye = true;
        if (sig.displayName == "Easy Anti-Cheat") hasEasyAntiCheat = true;
    }
    EXPECT_TRUE(hasBattlEye);
    EXPECT_TRUE(hasEasyAntiCheat);
}
