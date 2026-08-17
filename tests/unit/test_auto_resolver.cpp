#include "auto_resolver.h"

#include <gtest/gtest.h>

TEST(AutoResolverTest, PlansSafeProgressionForKnownValue) {
    killai::AutoResolver resolver;
    killai::AutoResolveGoal goal;
    goal.description = "mineraux 905 vers 10000";
    goal.targetValue = 905;
    goal.valueType = "Int32";

    const auto plan = resolver.planForGoal(goal);

    ASSERT_GE(plan.size(), 8);
    EXPECT_EQ(plan.front().type, killai::AutoResolveStepType::ScanExact);
    EXPECT_EQ(plan.front().params.value("value").toLongLong(), 905);
    EXPECT_EQ(plan.front().params.value("type").toString(), "Int32");
    EXPECT_EQ(plan.at(1).type, killai::AutoResolveStepType::UnknownCapture);
    EXPECT_EQ(plan.at(2).type, killai::AutoResolveStepType::UnknownCompare);
}

namespace {

const killai::AutoResolveTelemetryInsight* findInsight(
    const killai::AutoResolveTelemetryReport& report,
    const QString& id) {
    for (const auto& insight : report.insights) {
        if (insight.id == id) return &insight;
    }
    return nullptr;
}

} // namespace

TEST(AutoResolverTest, ComputesTelemetryFallbackSignals) {
    const QList<QVariantMap> events{
        QVariantMap{{"event", "exact_scan"}, {"matchesFound", 0}},
        QVariantMap{{"event", "unknown_next_async"}, {"stored", 250000}},
        QVariantMap{{"event", "ui_string_sources_analyze"}, {"matchesFound", 7}},
        QVariantMap{{"event", "ui_string_investigation_finish"}, {"globalValueHits", 3}},
        QVariantMap{{"event", "aob_signature"}, {"matchesFound", 4}},
    };

    const auto report = killai::computeAutoResolveTelemetryReport(events);

    EXPECT_EQ(report.exactZeroCount, 1);
    EXPECT_EQ(report.traceUiSourceCount, 7);
    EXPECT_EQ(report.traceUiGlobalHits, 3);
    EXPECT_EQ(report.aobMultiMatchCount, 1);
    EXPECT_EQ(report.aobWeakQualityCount, 0);
    EXPECT_EQ(report.trainerBlockedCount, 0);
    EXPECT_TRUE(report.displayValueSignals);

    EXPECT_NE(findInsight(report, "exact_zero_fallback"), nullptr);
    EXPECT_NE(findInsight(report, "unknown_too_large"), nullptr);
    // 7 sources reste sous le seuil de debordement : insight "pret", pas "trop".
    EXPECT_NE(findInsight(report, "trace_ui_sources_ready"), nullptr);
    EXPECT_EQ(findInsight(report, "trace_ui_sources_overflow"), nullptr);
    EXPECT_NE(findInsight(report, "aob_multimatch_guard"), nullptr);
}

TEST(AutoResolverTest, ComputesWeakAobAndTrainerBlocks) {
    const QList<QVariantMap> events{
        QVariantMap{
            {"event", "aob_signature"},
            {"matchesFound", 1},
            {"signatureRisk", "weak"},
            {"signatureQuality", QVariantMap{{"score", 24}, {"fixedBytes", 2}}},
        },
        QVariantMap{
            {"event", "trainer_patch_apply"},
            {"success", false},
            {"error", "Patch bloqué: signature AOB trop faible"},
        },
    };

    const auto report = killai::computeAutoResolveTelemetryReport(events);

    EXPECT_EQ(report.aobMultiMatchCount, 0);
    EXPECT_EQ(report.aobWeakQualityCount, 1);
    EXPECT_EQ(report.trainerBlockedCount, 1);
    EXPECT_NE(findInsight(report, "aob_quality_guard"), nullptr);
}

// Reproduit le cas signale par un utilisateur : Analyser sources retourne
// 500 pistes en un seul passage sur une cible reelle (StarCraft II), largement
// au-dessus du seuil de 40. Verifie que l'insight distingue bien "trop pour
// un checkpoint fiable" de "quelques sources exploitables", et que le
// nextAction pointe vers une deuxieme variation + reclique plutot que vers
// un freeze confirme immediat.
TEST(AutoResolverTest, FlagsTraceUiSourceOverflowInsteadOfReadyCheckpoint) {
    const QList<QVariantMap> events{
        QVariantMap{{"event", "ui_string_sources_analyze"}, {"matchesFound", 500}},
    };

    const auto report = killai::computeAutoResolveTelemetryReport(events);

    EXPECT_EQ(report.traceUiSourceCount, 500);

    const auto* overflow = findInsight(report, "trace_ui_sources_overflow");
    ASSERT_NE(overflow, nullptr);
    EXPECT_TRUE(overflow->safe);
    EXPECT_TRUE(overflow->nextAction.contains("Scan suivant (sources)"));
    EXPECT_EQ(findInsight(report, "trace_ui_sources_ready"), nullptr);
}

// Juste sous le seuil : doit rester sur l'insight "exploitable", pas "trop".
TEST(AutoResolverTest, KeepsReadyCheckpointJustBelowOverflowThreshold) {
    const QList<QVariantMap> events{
        QVariantMap{{"event", "ui_string_sources_analyze"}, {"matchesFound", killai::kTraceUiSourceOverflowThreshold}},
    };

    const auto report = killai::computeAutoResolveTelemetryReport(events);

    EXPECT_NE(findInsight(report, "trace_ui_sources_ready"), nullptr);
    EXPECT_EQ(findInsight(report, "trace_ui_sources_overflow"), nullptr);
}

// Un freeze polling qui derive (applyFreezeTick -> freeze_poll_instability)
// doit produire un insight actionnable pointant vers Freeze BP, pas juste
// rester invisible dans le rapport IA / Investigation.
TEST(AutoResolverTest, FlagsFreezeInstabilityWithBreakpointNextAction) {
    const QList<QVariantMap> events{
        QVariantMap{{"event", "freeze_poll_instability"}, {"address", "7FF6A1B2"}, {"consecutiveDriftTicks", 5}},
        QVariantMap{{"event", "freeze_poll_instability"}, {"address", "7FF6A1B2"}, {"consecutiveDriftTicks", 5}},
    };

    const auto report = killai::computeAutoResolveTelemetryReport(events);

    EXPECT_EQ(report.freezeInstabilityCount, 2);

    const auto* instability = findInsight(report, "freeze_instability_detected");
    ASSERT_NE(instability, nullptr);
    EXPECT_FALSE(instability->safe);
    EXPECT_TRUE(instability->nextAction.contains("Freeze BP"));
}
