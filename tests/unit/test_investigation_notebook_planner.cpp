#include "investigation_notebook_planner.h"

#include <gtest/gtest.h>

#include <QVariantList>

using killai::extractInvestigationNotebookPlanJson;
using killai::makeFallbackInvestigationNotebookPlan;
using killai::normalizeInvestigationNotebookPlan;

TEST(InvestigationNotebookPlannerTest, FallbackRoutesDisplayedValueNotFoundToUiStringTrace) {
    const QVariantMap plan = makeFallbackInvestigationNotebookPlan(
        "La valeur affichée à l'écran est introuvable par scan exact.");

    EXPECT_TRUE(plan.value("success").toBool());
    EXPECT_FALSE(plan.value("modelUsed").toBool());
    EXPECT_GE(plan.value("hypotheses").toStringList().size(), 3);

    const QVariantMap nextTest = plan.value("nextTest").toMap();
    EXPECT_EQ(nextTest.value("tool").toString(), "scan_ui_strings");
    EXPECT_EQ(nextTest.value("risk").toString(), "safe");
}

TEST(InvestigationNotebookPlannerTest, FallbackRoutesFlickeringFreezeToFieldStability) {
    const QVariantMap plan = makeFallbackInvestigationNotebookPlan(
        "Le freeze clignote et la valeur revient tout de suite.");

    const QVariantMap nextTest = plan.value("nextTest").toMap();
    EXPECT_EQ(nextTest.value("tool").toString(), "analyze_field_stability");
    EXPECT_EQ(nextTest.value("risk").toString(), "debug");
}

TEST(InvestigationNotebookPlannerTest, ExtractsJsonPlanFromModelOutput) {
    QString error;
    const QVariantMap parsed = extractInvestigationNotebookPlanJson(
        "texte ignoré {\"hypotheses\":[{\"description\":\"Copie UI\"}],"
        "\"nextTest\":{\"title\":\"Tracer\",\"tool\":\"scan_ui_strings\"}}",
        &error);

    EXPECT_TRUE(error.isEmpty());
    EXPECT_FALSE(parsed.isEmpty());
    EXPECT_TRUE(parsed.contains("hypotheses"));
    EXPECT_TRUE(parsed.contains("nextTest"));
}

TEST(InvestigationNotebookPlannerTest, ExtractJsonPlanDoesNotHangOnUnterminatedJsonAtStart) {
    QString error;
    const QVariantMap parsed = extractInvestigationNotebookPlanJson(
        "{\"hypotheses\":[{\"description\":\"Copie UI\"}]", &error);

    EXPECT_TRUE(parsed.isEmpty());
    EXPECT_FALSE(error.isEmpty());
}

TEST(InvestigationNotebookPlannerTest, NormalizeDropsModelScoresAndKeepsTextOnlyHypotheses) {
    QVariantMap proposal;
    QVariantMap scoredHypothesis;
    scoredHypothesis["description"] = "La valeur affichée est dérivée.";
    scoredHypothesis["confidenceScore"] = 99;
    proposal["hypotheses"] = QVariantList{scoredHypothesis, "La source vit dans un cache."};

    QVariantMap nextTest;
    nextTest["title"] = "Comparer l'affichage et la source";
    nextTest["tool"] = "analyze_field_stability";
    nextTest["risk"] = "write";
    nextTest["preconditions"] = QStringList{"Adresse candidate sélectionnée"};
    nextTest["expectedIfTrue"] = "Le champ est réécrit par une instruction dominante.";
    nextTest["expectedIfFalse"] = "Il faut chercher une autre représentation.";
    nextTest["rationale"] = "Le symptôme parle d'un freeze qui ne tient pas.";
    proposal["nextTest"] = nextTest;

    const QVariantMap plan = normalizeInvestigationNotebookPlan(proposal, "freeze clignote", "llama-server");

    EXPECT_TRUE(plan.value("success").toBool());
    EXPECT_TRUE(plan.value("modelUsed").toBool());
    const QStringList hypotheses = plan.value("hypotheses").toStringList();
    ASSERT_EQ(hypotheses.size(), 2);
    EXPECT_EQ(hypotheses.first(), "La valeur affichée est dérivée.");

    const QVariantMap normalizedNext = plan.value("nextTest").toMap();
    EXPECT_EQ(normalizedNext.value("risk").toString(), "confirmation");
    EXPECT_FALSE(normalizedNext.contains("confidenceScore"));
}
