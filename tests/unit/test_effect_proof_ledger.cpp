#include <gtest/gtest.h>

#include "effect_proof_ledger.h"

using killai::EffectProofLedger;
using killai::EffectProofLevel;
using killai::EffectProofTargetStatus;

TEST(EffectProofLedgerTest, EmptyLedgerReportsNoTrackedGoal) {
    EffectProofLedger ledger;
    QVariantMap synthesis = ledger.synthesis();
    EXPECT_TRUE(synthesis["known"].toList().isEmpty());
    EXPECT_TRUE(synthesis["uncertain"].toList().isEmpty());
    EXPECT_FALSE(synthesis["overallNextAction"].toString().isEmpty());
}

TEST(EffectProofLedgerTest, WriteConfirmedAloneIsUncertainNotKnown) {
    // Motivation directe : cas Solitaire XP (docs/PHASE_TRACKER.md,
    // INVESTIGATION-SOLITAIRE-XP-2) -- une relecture correcte après écriture
    // ne doit jamais être présentée comme un objectif atteint.
    EffectProofLedger ledger;
    ledger.addRecord("Compteur XP", "0x9AC3A", EffectProofLevel::WriteConfirmed, "relecture", "", "s1", "octets relus corrects");

    QVariantMap synthesis = ledger.synthesis();
    EXPECT_TRUE(synthesis["known"].toList().isEmpty());
    ASSERT_EQ(synthesis["uncertain"].toList().size(), 1);
    QVariantMap status = synthesis["uncertain"].toList().first().toMap();
    EXPECT_EQ(status["bestLevel"].toString(), "write_confirmed");
    EXPECT_FALSE(status["nextAction"].toString().isEmpty());
}

TEST(EffectProofLedgerTest, EffectConfirmedMovesTargetToKnownButStillSuggestsDurabilityTest) {
    EffectProofLedger ledger;
    ledger.addRecord("Compteur XP", "0x9AC3A", EffectProofLevel::WriteConfirmed, "relecture", "", "s1", "");
    ledger.addRecord("Compteur XP", "0x9AC3A", EffectProofLevel::EffectConfirmed, "observation utilisateur", "", "s1", "affichage monte bien");

    QVariantMap synthesis = ledger.synthesis();
    ASSERT_EQ(synthesis["known"].toList().size(), 1);
    EXPECT_TRUE(synthesis["uncertain"].toList().isEmpty());
    QVariantMap status = synthesis["known"].toList().first().toMap();
    EXPECT_EQ(status["bestLevel"].toString(), "effect_confirmed");
    EXPECT_TRUE(status["nextAction"].toString().contains("persistance", Qt::CaseInsensitive)
                || status["nextAction"].toString().contains("persistence", Qt::CaseInsensitive));
}

TEST(EffectProofLedgerTest, DurableSolutionNeedsNoFurtherAction) {
    EffectProofLedger ledger;
    ledger.addRecord("Compteur XP", "0x9AC3A", EffectProofLevel::EffectConfirmed, "observation", "", "s1", "");
    ledger.addRecord("Compteur XP", "0x9AC3A", EffectProofLevel::DurableSolution, "test redemarrage", "apres redemarrage du jeu", "s2", "tient apres restart");

    EffectProofTargetStatus status = ledger.statusForTarget(EffectProofLedger::targetKeyFor("Compteur XP", "0x9AC3A"));
    EXPECT_EQ(status.bestLevel, EffectProofLevel::DurableSolution);
    ASSERT_EQ(status.history.size(), 2);
    EXPECT_EQ(status.history.first().level, EffectProofLevel::DurableSolution); // le plus recent en tete
}

TEST(EffectProofLedgerTest, LaterInconclusiveDoesNotDowngradeAnAlreadyConfirmedEffect) {
    EffectProofLedger ledger;
    ledger.addRecord("Or", "0x1000", EffectProofLevel::EffectConfirmed, "observation", "", "s1", "");
    ledger.addRecord("Or", "0x1000", EffectProofLevel::Inconclusive, "relance", "", "s2", "second essai pas clair");

    EffectProofTargetStatus status = ledger.statusForTarget(EffectProofLedger::targetKeyFor("Or", "0x1000"));
    EXPECT_EQ(status.bestLevel, EffectProofLevel::EffectConfirmed);
    EXPECT_EQ(status.history.size(), 2);
}

TEST(EffectProofLedgerTest, TargetsAreGroupedByAddressWhenPresentRegardlessOfLabel) {
    EffectProofLedger ledger;
    ledger.addRecord("Compteur XP (affiché)", "0x9AC3A", EffectProofLevel::WriteConfirmed, "relecture", "", "s1", "");
    ledger.addRecord("XP en cours de partie", "0x9AC3A", EffectProofLevel::EffectConfirmed, "observation", "", "s1", "");

    QVariantMap synthesis = ledger.synthesis();
    ASSERT_EQ(synthesis["known"].toList().size() + synthesis["uncertain"].toList().size(), 1);
}

TEST(EffectProofLedgerTest, TargetsWithoutAddressAreGroupedByLabel) {
    EffectProofLedger ledger;
    ledger.addRecord("Vies du joueur", "", EffectProofLevel::WriteConfirmed, "relecture", "", "s1", "");
    ledger.addRecord("Vies du joueur", "", EffectProofLevel::EffectConfirmed, "observation", "", "s1", "");

    EffectProofTargetStatus status = ledger.statusForTarget(EffectProofLedger::targetKeyFor("Vies du joueur", ""));
    EXPECT_EQ(status.bestLevel, EffectProofLevel::EffectConfirmed);
    EXPECT_EQ(status.history.size(), 2);
}

TEST(EffectProofLedgerTest, OverallNextActionPicksTheWeakestUncertainTarget) {
    EffectProofLedger ledger;
    ledger.addRecord("Cible A", "0xA", EffectProofLevel::EffectConfirmed, "observation", "", "s1", "");
    ledger.addRecord("Cible B", "0xB", EffectProofLevel::Unverified, "", "", "s1", "");
    ledger.addRecord("Cible C", "0xC", EffectProofLevel::WriteConfirmed, "relecture", "", "s1", "");

    QVariantMap synthesis = ledger.synthesis();
    // Cible B (Unverified, rang 0) doit dominer Cible C (WriteConfirmed, rang 2).
    EXPECT_EQ(synthesis["overallNextAction"].toString(),
              ledger.statusForTarget(EffectProofLedger::targetKeyFor("Cible B", "0xB")).nextAction);
}

TEST(EffectProofLedgerTest, MultipleTargetsAllConfirmedProducesGenericAllDoneMessage) {
    EffectProofLedger ledger;
    ledger.addRecord("Cible A", "0xA", EffectProofLevel::DurableSolution, "test redemarrage", "", "s1", "");
    ledger.addRecord("Cible B", "0xB", EffectProofLevel::EffectConfirmed, "observation", "", "s1", "");

    QVariantMap synthesis = ledger.synthesis();
    ASSERT_EQ(synthesis["known"].toList().size(), 2);
    EXPECT_TRUE(synthesis["uncertain"].toList().isEmpty());
    EXPECT_FALSE(synthesis["overallNextAction"].toString().isEmpty());
}

TEST(EffectProofLedgerTest, ClearResetsRecordsAndIdCounter) {
    EffectProofLedger ledger;
    ledger.addRecord("Cible A", "0xA", EffectProofLevel::WriteConfirmed, "relecture", "", "s1", "");
    ledger.clear();

    EXPECT_TRUE(ledger.records().isEmpty());
    QString newId = ledger.addRecord("Cible B", "0xB", EffectProofLevel::Unverified, "", "", "s1", "");
    EXPECT_EQ(newId, "P1");
}

TEST(EffectProofLevelStringConversion, RoundTripsAllKnownLevels) {
    const QList<EffectProofLevel> levels = {
        EffectProofLevel::Unverified,
        EffectProofLevel::Inconclusive,
        EffectProofLevel::WriteConfirmed,
        EffectProofLevel::EffectConfirmed,
        EffectProofLevel::DurableSolution,
    };
    for (EffectProofLevel level : levels) {
        const QString text = killai::effectProofLevelToString(level);
        EXPECT_TRUE(killai::effectProofLevelIsKnownString(text));
        EXPECT_EQ(killai::effectProofLevelFromString(text), level);
    }
}

TEST(EffectProofLevelStringConversion, UnknownStringIsNotAKnownLevel) {
    EXPECT_FALSE(killai::effectProofLevelIsKnownString("not_a_real_level"));
    EXPECT_EQ(killai::effectProofLevelFromString("not_a_real_level"), EffectProofLevel::Unverified);
}
