#include <gtest/gtest.h>

#include "investigation_notebook.h"

using killai::HypothesisStatus;
using killai::InvestigationNotebook;

TEST(InvestigationNotebookTest, AddHypothesisStartsActiveAtBaselineScore) {
    InvestigationNotebook notebook;
    QString id = notebook.addHypothesis("checksum dans le fichier .sgi");
    const auto* hypothesis = notebook.findHypothesis(id);
    ASSERT_NE(hypothesis, nullptr);
    EXPECT_EQ(hypothesis->status, HypothesisStatus::Active);
    EXPECT_EQ(hypothesis->confidenceScore, InvestigationNotebook::kDefaultBaselineScore);
    EXPECT_TRUE(hypothesis->evidenceLog.isEmpty());
}

TEST(InvestigationNotebookTest, ConfirmingTestRaisesScoreByFixedDelta) {
    InvestigationNotebook notebook;
    QString id = notebook.addHypothesis("valeur reste stable apres unchanged scan");
    ASSERT_TRUE(notebook.recordTestResult(id, true, "unchanged scan: candidat toujours present"));

    const auto* hypothesis = notebook.findHypothesis(id);
    ASSERT_NE(hypothesis, nullptr);
    EXPECT_EQ(hypothesis->confidenceScore, InvestigationNotebook::kDefaultBaselineScore + InvestigationNotebook::kConfirmDelta);
    EXPECT_EQ(hypothesis->status, HypothesisStatus::Active);
    EXPECT_EQ(hypothesis->evidenceLog.size(), 1);
}

TEST(InvestigationNotebookTest, ContradictingTestLowersScoreByFixedDelta) {
    InvestigationNotebook notebook;
    QString id = notebook.addHypothesis("checksum dans le fichier .sgi");
    ASSERT_TRUE(notebook.recordTestResult(id, false, "ecriture bloquee silencieusement en sauvegarde locale, pas de checksum trouve"));

    const auto* hypothesis = notebook.findHypothesis(id);
    ASSERT_NE(hypothesis, nullptr);
    EXPECT_EQ(hypothesis->confidenceScore, InvestigationNotebook::kDefaultBaselineScore + InvestigationNotebook::kContradictDelta);
}

TEST(InvestigationNotebookTest, ContradictDeltaIsStrongerThanConfirmDelta) {
    // Motivation d'origine (cas Bulles Solitaire) : une hypothese fausse ne doit
    // pas s'ancrer -- la contradiction doit faire perdre plus vite que la
    // confirmation ne fait gagner, pour eviter de s'accrocher a une premiere piste.
    EXPECT_GT(-InvestigationNotebook::kContradictDelta, InvestigationNotebook::kConfirmDelta);
}

TEST(InvestigationNotebookTest, RepeatedContradictionsEliminateHypothesis) {
    InvestigationNotebook notebook;
    QString id = notebook.addHypothesis("checksum dans le fichier .sgi");

    notebook.recordTestResult(id, false, "test 1 contradictoire");
    ASSERT_EQ(notebook.findHypothesis(id)->status, HypothesisStatus::Active);

    notebook.recordTestResult(id, false, "test 2 contradictoire");
    const auto* hypothesis = notebook.findHypothesis(id);
    ASSERT_NE(hypothesis, nullptr);
    EXPECT_LE(hypothesis->confidenceScore, InvestigationNotebook::kEliminationThreshold);
    EXPECT_EQ(hypothesis->status, HypothesisStatus::Refuted);
    EXPECT_EQ(hypothesis->evidenceLog.size(), 2);
}

TEST(InvestigationNotebookTest, RepeatedConfirmationsConfirmHypothesis) {
    InvestigationNotebook notebook;
    QString id = notebook.addHypothesis("l'override du cache WebView explique le compteur");

    for (int i = 0; i < 3; ++i) {
        notebook.recordTestResult(id, true, QString("confirmation %1").arg(i));
    }

    const auto* hypothesis = notebook.findHypothesis(id);
    ASSERT_NE(hypothesis, nullptr);
    EXPECT_GE(hypothesis->confidenceScore, InvestigationNotebook::kConfirmationThreshold);
    EXPECT_EQ(hypothesis->status, HypothesisStatus::Confirmed);
}

TEST(InvestigationNotebookTest, ScoreClampedToRangeNeverGoesNegativeOrAboveMax) {
    InvestigationNotebook notebook;
    QString lowId = notebook.addHypothesis("piste tres fragile", /*baselineScore=*/5);
    notebook.recordTestResult(lowId, false, "contredit une fois, deja proche de zero");
    EXPECT_GE(notebook.findHypothesis(lowId)->confidenceScore, InvestigationNotebook::kMinScore);

    InvestigationNotebook notebook2;
    QString highId = notebook2.addHypothesis("piste tres solide", /*baselineScore=*/95);
    notebook2.recordTestResult(highId, true, "confirme une fois de plus, deja pres du max");
    EXPECT_LE(notebook2.findHypothesis(highId)->confidenceScore, InvestigationNotebook::kMaxScore);
}

TEST(InvestigationNotebookTest, TerminalHypothesisIsLockedAgainstFurtherUpdates) {
    InvestigationNotebook notebook;
    QString id = notebook.addHypothesis("piste vite refutee", /*baselineScore=*/5);
    ASSERT_TRUE(notebook.recordTestResult(id, false, "contredit, tombe sous le seuil"));
    ASSERT_EQ(notebook.findHypothesis(id)->status, HypothesisStatus::Refuted);

    int scoreAfterRefutation = notebook.findHypothesis(id)->confidenceScore;
    bool accepted = notebook.recordTestResult(id, true, "tentative de reactivation apres coup");
    EXPECT_FALSE(accepted);
    EXPECT_EQ(notebook.findHypothesis(id)->confidenceScore, scoreAfterRefutation);
    EXPECT_EQ(notebook.findHypothesis(id)->evidenceLog.size(), 1);
}

TEST(InvestigationNotebookTest, RecordTestResultOnUnknownIdReturnsFalse) {
    InvestigationNotebook notebook;
    EXPECT_FALSE(notebook.recordTestResult("H999", true, "id inexistant"));
}

TEST(InvestigationNotebookTest, ActiveHypothesesExcludesTerminalOnes) {
    InvestigationNotebook notebook;
    QString stillActive = notebook.addHypothesis("hypothese encore ouverte");
    QString willBeRefuted = notebook.addHypothesis("hypothese qui va etre ecartee", /*baselineScore=*/5);
    QString willBeConfirmed = notebook.addHypothesis("hypothese qui va etre confirmee", /*baselineScore=*/95);

    notebook.recordTestResult(willBeRefuted, false, "contredit");
    notebook.recordTestResult(willBeConfirmed, true, "confirme");

    QList<killai::Hypothesis> active = notebook.activeHypotheses();
    ASSERT_EQ(active.size(), 1);
    EXPECT_EQ(active.first().id, stillActive);
}

TEST(InvestigationNotebookTest, SynthesisGroupsByStatusAndSortsByConfidenceDescending) {
    InvestigationNotebook notebook;
    QString low = notebook.addHypothesis("piste faible", /*baselineScore=*/30);
    QString high = notebook.addHypothesis("piste forte", /*baselineScore=*/70);
    QString refuted = notebook.addHypothesis("piste ecartee", /*baselineScore=*/5);
    notebook.recordTestResult(refuted, false, "contredit");

    QVariantMap synthesis = notebook.synthesis();
    QVariantList active = synthesis["active"].toList();
    QVariantList refutedList = synthesis["refuted"].toList();
    QVariantList confirmedList = synthesis["confirmed"].toList();

    ASSERT_EQ(active.size(), 2);
    EXPECT_EQ(active[0].toMap()["id"].toString(), high);
    EXPECT_EQ(active[1].toMap()["id"].toString(), low);
    ASSERT_EQ(refutedList.size(), 1);
    EXPECT_EQ(refutedList[0].toMap()["id"].toString(), refuted);
    EXPECT_TRUE(confirmedList.isEmpty());
}

TEST(InvestigationNotebookTest, ResetClearsAllHypothesesAndIdCounter) {
    InvestigationNotebook notebook;
    notebook.addHypothesis("premiere hypothese");
    notebook.reset();

    EXPECT_TRUE(notebook.hypotheses().isEmpty());
    QString newId = notebook.addHypothesis("nouvelle hypothese apres reset");
    EXPECT_EQ(newId, "H1");
}
