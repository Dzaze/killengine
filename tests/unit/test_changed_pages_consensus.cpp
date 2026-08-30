// PHASE 250 — tests du consensus multi-rounds changed-pages (logique pure,
// aucun processus réel requis). Couvre le scénario SC2 solarite : des
// centaines de hits volatils par round, dont seule une minorité suit TOUTES
// les transitions 140 -> 135 -> 130.

#include "scanner/changed_pages_consensus.h"

#include <gtest/gtest.h>

#include <QVariantList>
#include <QVariantMap>

namespace {

QVariantMap makeHit(uint64_t address, const QString& type = "Int32", double confidence = 0.78) {
    QVariantMap hit;
    hit["address"] = QString("%1").arg(address, 0, 16).toUpper();
    hit["type"] = type;
    hit["variantLabel"] = "";
    hit["confidence"] = confidence;
    hit["lastValueHex"] = "8B 00 00 00";
    hit["lastValueNumber"] = 139.0;
    return hit;
}

QVariantMap makeProbe(uint64_t address, const QString& status, const QString& type = "Int32") {
    QVariantMap probe;
    probe["address"] = QString("%1").arg(address, 0, 16).toUpper();
    probe["type"] = type;
    probe["variantLabel"] = "";
    probe["status"] = status;
    return probe;
}

} // namespace

TEST(ChangedPagesConsensusTest, AccumulatesHitsAcrossRounds) {
    killcore::ChangedPagesConsensus consensus;
    consensus.applyRound({makeHit(0x1000), makeHit(0x2000)});
    consensus.applyRound({makeHit(0x1000), makeHit(0x3000)});
    EXPECT_EQ(consensus.roundsApplied(), 2);
    EXPECT_EQ(consensus.totalEntries(), 3);
    EXPECT_EQ(consensus.eliminatedCount(), 0);
}

TEST(ChangedPagesConsensusTest, ConfirmedProbeRaisesScoreAndCount) {
    killcore::ChangedPagesConsensus consensus;
    consensus.applyRound({makeHit(0x1000)});
    consensus.applyProbeResult(makeProbe(0x1000, "confirmed"));
    consensus.applyProbeResult(makeProbe(0x1000, "confirmed"));
    EXPECT_EQ(consensus.confirmedEntries(2), 1);

    const QVariantList ranked = consensus.rankedEntries(10);
    ASSERT_EQ(ranked.size(), 1);
    const QVariantMap entry = ranked.first().toMap();
    EXPECT_EQ(entry.value("roundsConfirmed").toInt(), 2);
    EXPECT_GE(entry.value("score").toDouble(), 4.0);
}

TEST(ChangedPagesConsensusTest, TwoContradictionsEliminateEntry) {
    killcore::ChangedPagesConsensus consensus;
    consensus.applyRound({makeHit(0x1000)});
    // Une contradiction est pardonnée (copie UI réécrite transitoirement)...
    consensus.applyProbeResult(makeProbe(0x1000, "contradicted"));
    EXPECT_EQ(consensus.eliminatedCount(), 0);
    // ...pas deux.
    consensus.applyProbeResult(makeProbe(0x1000, "contradicted"));
    EXPECT_EQ(consensus.eliminatedCount(), 1);
    EXPECT_EQ(consensus.confirmedEntries(1), 0);
    // Une fois éliminée, plus aucune mise à jour ne la ressuscite.
    consensus.applyProbeResult(makeProbe(0x1000, "confirmed"));
    EXPECT_EQ(consensus.eliminatedCount(), 1);
}

TEST(ChangedPagesConsensusTest, FourStaleProbesEliminateEntry) {
    killcore::ChangedPagesConsensus consensus;
    consensus.applyRound({makeHit(0x1000)});
    for (int i = 0; i < 3; ++i) {
        consensus.applyProbeResult(makeProbe(0x1000, "stale"));
    }
    EXPECT_EQ(consensus.eliminatedCount(), 0);
    consensus.applyProbeResult(makeProbe(0x1000, "stale"));
    EXPECT_EQ(consensus.eliminatedCount(), 1);
}

TEST(ChangedPagesConsensusTest, CheckpointsReturnTopActiveSortedByScore) {
    killcore::ChangedPagesConsensus consensus;
    consensus.applyRound({makeHit(0x1000), makeHit(0x2000), makeHit(0x3000)});
    consensus.applyProbeResult(makeProbe(0x2000, "confirmed"));
    consensus.applyProbeResult(makeProbe(0x2000, "confirmed"));
    consensus.applyProbeResult(makeProbe(0x1000, "confirmed"));
    consensus.applyProbeResult(makeProbe(0x3000, "contradicted"));
    consensus.applyProbeResult(makeProbe(0x3000, "contradicted"));

    const QVariantList checkpoints = consensus.checkpoints(10);
    ASSERT_EQ(checkpoints.size(), 2);
    EXPECT_EQ(checkpoints.first().toMap().value("address").toString().toStdString(),
              QString("%1").arg(0x2000, 0, 16).toUpper().toStdString());
    EXPECT_EQ(checkpoints.last().toMap().value("address").toString().toStdString(),
              QString("%1").arg(0x1000, 0, 16).toUpper().toStdString());
}

TEST(ChangedPagesConsensusTest, InvalidProbeDataIsIgnored) {
    killcore::ChangedPagesConsensus consensus;
    consensus.applyRound({makeHit(0x1000)});

    QVariantMap badAddress;
    badAddress["address"] = "not-an-address";
    badAddress["type"] = "Int32";
    badAddress["status"] = "confirmed";
    consensus.applyProbeResult(badAddress);

    QVariantMap unknownType;
    unknownType["address"] = "1000";
    unknownType["type"] = "NotAType";
    unknownType["status"] = "confirmed";
    consensus.applyProbeResult(unknownType);

    QVariantMap unknownAddress;
    unknownAddress["address"] = "9999";
    unknownAddress["type"] = "Int32";
    unknownAddress["status"] = "confirmed";
    consensus.applyProbeResult(unknownAddress);

    QVariantMap unknownStatus = makeProbe(0x1000, "something-else");
    consensus.applyProbeResult(unknownStatus);

    EXPECT_EQ(consensus.totalEntries(), 1);
    EXPECT_EQ(consensus.confirmedEntries(1), 0);
}

TEST(ChangedPagesConsensusTest, ResetClearsEverything) {
    killcore::ChangedPagesConsensus consensus;
    consensus.applyRound({makeHit(0x1000)});
    consensus.applyProbeResult(makeProbe(0x1000, "confirmed"));
    consensus.reset();
    EXPECT_EQ(consensus.roundsApplied(), 0);
    EXPECT_EQ(consensus.totalEntries(), 0);
    EXPECT_EQ(consensus.rankedEntries(10).size(), 0);
}

TEST(ChangedPagesConsensusTest, RoundWithInvalidHitsSkipsThem) {
    killcore::ChangedPagesConsensus consensus;
    QVariantMap invalid;
    invalid["address"] = "";
    invalid["type"] = "Int32";
    consensus.applyRound({invalid, makeHit(0x1000)});
    EXPECT_EQ(consensus.totalEntries(), 1);
}

TEST(ChangedPagesConsensusTest, DistinctVariantsAreDistinctEntries) {
    killcore::ChangedPagesConsensus consensus;
    QVariantMap plain = makeHit(0x1000, "Int32");
    QVariantMap scaled = makeHit(0x1000, "Int32");
    scaled["variantLabel"] = "Int32 x100";
    consensus.applyRound({plain, scaled});
    EXPECT_EQ(consensus.totalEntries(), 2);
    EXPECT_EQ(consensus.roundsApplied(), 1);
}