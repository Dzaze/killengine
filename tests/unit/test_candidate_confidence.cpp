// Vérifie que findRegionForAddress() (utilisée par computeCandidateConfidence,
// bientôt branchée dans le pipeline de scan normal — voir
// core/scanner/candidate_confidence.cpp) donne le même résultat qu'un scan
// linéaire de référence sur un jeu de régions couvrant tous les cas limites.
// Même raisonnement que test_auto_dissect.cpp pour isPointerValid() : c'est
// le seul endroit où une régression de la recherche binaire réintroduirait
// silencieusement un score de confiance faux (région introuvable alors
// qu'elle existe, ou mauvaise région retournée) sans jamais planter.

#include "scanner/candidate_confidence.h"

#include <gtest/gtest.h>

#include <limits>

using namespace killcore;

namespace {

MemoryRegion makeRegion(uint64_t base, uint64_t size, bool committed, bool readable, bool writable) {
    MemoryRegion r;
    r.baseAddress = base;
    r.size = size;
    r.state = committed ? MemoryState::Committed : MemoryState::Reserved;
    r.readable = readable;
    r.writable = writable;
    return r;
}

const MemoryRegion* linearReferenceFind(const QList<MemoryRegion>& regions, uint64_t addr) {
    for (const auto& r : regions) {
        if (addr >= r.baseAddress && addr < r.baseAddress + r.size) {
            return &r;
        }
    }
    return nullptr;
}

// Même jeu "difficile" que test_auto_dissect.cpp : trous, région d'un octet,
// tailles variées.
QList<MemoryRegion> sampleRegions() {
    return {
        makeRegion(0x1000, 0x1000, true, true, true),
        makeRegion(0x2000, 0x1000, false, true, false),
        // trou : [0x3000, 0x5000)
        makeRegion(0x5000, 0x0500, true, false, true),
        makeRegion(0x5500, 0x2000, true, true, true),
        makeRegion(0x7500, 0x0001, true, true, true),
    };
}

} // namespace

TEST(CandidateConfidenceFindRegion, EmptyRegionsReturnsNullptr) {
    QList<MemoryRegion> empty;
    EXPECT_EQ(findRegionForAddress(empty, 0x1000), nullptr);
}

TEST(CandidateConfidenceFindRegion, AddressBeforeFirstRegionReturnsNullptr) {
    EXPECT_EQ(findRegionForAddress(sampleRegions(), 0x0500), nullptr);
}

TEST(CandidateConfidenceFindRegion, AddressAtRegionStartFindsThatRegion) {
    const auto regions = sampleRegions();
    const auto* r = findRegionForAddress(regions, 0x1000);
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->baseAddress, 0x1000ULL);
}

TEST(CandidateConfidenceFindRegion, AddressInGapReturnsNullptr) {
    const auto regions = sampleRegions();
    EXPECT_EQ(findRegionForAddress(regions, 0x3000), nullptr);
    EXPECT_EQ(findRegionForAddress(regions, 0x4FFF), nullptr);
}

TEST(CandidateConfidenceFindRegion, AddressInLastRegionFindsIt) {
    const auto regions = sampleRegions();
    const auto* r = findRegionForAddress(regions, 0x6000);
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->baseAddress, 0x5500ULL);
}

TEST(CandidateConfidenceFindRegion, SingleByteRegionIsHandledPrecisely) {
    const auto regions = sampleRegions();
    EXPECT_NE(findRegionForAddress(regions, 0x7500), nullptr);
    EXPECT_EQ(findRegionForAddress(regions, 0x7501), nullptr);
}

TEST(CandidateConfidenceFindRegion, MatchesLinearReferenceForEveryBoundaryCase) {
    const auto regions = sampleRegions();
    QList<uint64_t> addressesToCheck;
    for (const auto& r : regions) {
        addressesToCheck << r.baseAddress
                          << r.baseAddress + 1
                          << r.baseAddress + r.size / 2
                          << r.baseAddress + r.size - 1
                          << r.baseAddress + r.size
                          << (r.baseAddress > 0 ? r.baseAddress - 1 : 0);
    }
    addressesToCheck << 0 << 0x500 << 0x3000 << 0x4FFF << 0xFFFFFFFFULL;

    for (const uint64_t addr : addressesToCheck) {
        const auto* fast = findRegionForAddress(regions, addr);
        const auto* reference = linearReferenceFind(regions, addr);
        if (reference == nullptr) {
            EXPECT_EQ(fast, nullptr) << "addr=0x" << QString::number(addr, 16).toStdString();
        } else {
            ASSERT_NE(fast, nullptr) << "addr=0x" << QString::number(addr, 16).toStdString();
            EXPECT_EQ(fast->baseAddress, reference->baseAddress)
                << "addr=0x" << QString::number(addr, 16).toStdString();
        }
    }
}

TEST(CandidateConfidenceScore, RegionScoreZeroWhenRegionMissing) {
    CandidateConfidenceContext ctx;
    const auto regions = sampleRegions();
    ctx.regions = &regions;
    // 0x3000 tombe dans le trou : aucune région -> score très bas (région
    // disparue = suspect), pas neutre.
    const double confidence = computeCandidateConfidence(0x3000, ValueType::Int32, ctx);
    EXPECT_LT(confidence, 0.5);
}

TEST(CandidateConfidenceScore, PreferWritablePrivateOverNonWritable) {
    const auto regions = sampleRegions();
    CandidateConfidenceContext ctx;
    ctx.regions = &regions;
    ctx.preferWritable = true;

    // 0x1500 -> région writable+readable+committed ([0x1000,0x2000)).
    const double writableScore = computeCandidateConfidence(0x1500, ValueType::Int32, ctx);
    // 0x5600 -> aussi writable+readable ([0x5500,0x7500)) : les deux devraient
    // être également favorisées, contrairement à une région non-writable.
    const double writableScore2 = computeCandidateConfidence(0x5600, ValueType::Int32, ctx);
    EXPECT_GT(writableScore, 0.5);
    EXPECT_GT(writableScore2, 0.5);
}

TEST(CandidateConfidenceScore, ScanTimeConfidenceReachesHighForIdealRegionAndPrimaryVariant) {
    // Sans historique de valeurs (cas de tout scan classique aujourd'hui),
    // une région idéale (writable+private, non exécutable) avec une variante
    // primaire doit atteindre "fiabilité élevée" (>= 0.80) dès le premier
    // scan — pas seulement plafonner à ~0.75 en diluant avec une stabilité
    // neutre. Voir le commentaire de computeScanTimeConfidence().
    MemoryRegion idealRegion;
    idealRegion.baseAddress = 0x1000;
    idealRegion.size = 0x1000;
    idealRegion.state = MemoryState::Committed;
    idealRegion.readable = true;
    idealRegion.writable = true;
    idealRegion.executable = false;
    idealRegion.type = MemoryType::Private;

    const double regionScore = computeRegionScore(idealRegion, CandidateConfidenceContext{});
    const double confidence = computeScanTimeConfidence(regionScore, /*secondaryVariant=*/false);
    EXPECT_GE(confidence, 0.80);

    const double secondaryConfidence = computeScanTimeConfidence(regionScore, /*secondaryVariant=*/true);
    EXPECT_LT(secondaryConfidence, confidence);
}

TEST(CandidateConfidenceScore, NonReadableRegionScoresLow) {
    const auto regions = sampleRegions();
    const MemoryRegion* region = findRegionForAddress(regions, 0x5200);
    ASSERT_NE(region, nullptr);
    EXPECT_FALSE(region->readable);

    CandidateConfidenceContext ctx;
    const double regionScore = computeRegionScore(*region, ctx);
    EXPECT_LT(regionScore, 0.3);
}

namespace {

ConfidenceObservation makeObservation(double previous, double current, bool kept = true) {
    ConfidenceObservation obs;
    obs.previousValue = previous;
    obs.currentValue = current;
    obs.kept = kept;
    return obs;
}

} // namespace

TEST(CandidateConfidenceScore, HistoryIncreasesConfidenceAsRoundsSurvived) {
    // Un candidat qui a survécu à plusieurs next_scan doit inspirer plus
    // confiance qu'un candidat qui vient d'être trouvé (aucun historique) —
    // c'est tout le point de brancher valueHistory dans le scan.
    const auto regions = sampleRegions();
    CandidateConfidenceContext noHistoryCtx;
    noHistoryCtx.regions = &regions;
    const double noHistoryConfidence = computeCandidateConfidence(0x1500, ValueType::Int32, noHistoryCtx);

    QHash<uint64_t, QList<ConfidenceObservation>> history;
    history.insert(0x1500, {makeObservation(100, 100), makeObservation(100, 100), makeObservation(100, 100)});
    CandidateConfidenceContext withHistoryCtx;
    withHistoryCtx.regions = &regions;
    withHistoryCtx.valueHistory = &history;
    const double withHistoryConfidence = computeCandidateConfidence(0x1500, ValueType::Int32, withHistoryCtx);

    EXPECT_GT(withHistoryConfidence, noHistoryConfidence);
}

TEST(CandidateConfidenceScore, MoreRoundsSurvivedIsAtLeastAsConfidentAsFewer) {
    const auto regions = sampleRegions();
    QHash<uint64_t, QList<ConfidenceObservation>> shortHistory;
    shortHistory.insert(0x1500, {makeObservation(100, 100)});
    QHash<uint64_t, QList<ConfidenceObservation>> longHistory;
    longHistory.insert(0x1500, {makeObservation(100, 100), makeObservation(100, 100),
                                 makeObservation(100, 100), makeObservation(100, 100),
                                 makeObservation(100, 100)});

    CandidateConfidenceContext shortCtx;
    shortCtx.regions = &regions;
    shortCtx.valueHistory = &shortHistory;
    CandidateConfidenceContext longCtx;
    longCtx.regions = &regions;
    longCtx.valueHistory = &longHistory;

    EXPECT_GE(computeCandidateConfidence(0x1500, ValueType::Int32, longCtx),
              computeCandidateConfidence(0x1500, ValueType::Int32, shortCtx));
}

TEST(CandidateConfidenceScore, NonFiniteHistoryValueTanksConfidence) {
    // Une valeur non finie (NaN/Inf) apparue dans l'historique est un signe
    // de mémoire corrompue ou mal typée (ex. des octets aléatoires réinterprétés
    // en Float32) : ça doit faire chuter la confiance, pas juste rester neutre.
    const auto regions = sampleRegions();
    QHash<uint64_t, QList<ConfidenceObservation>> history;
    history.insert(0x1500, {makeObservation(100, 100),
                             makeObservation(std::numeric_limits<double>::quiet_NaN(), 100)});

    CandidateConfidenceContext ctx;
    ctx.regions = &regions;
    ctx.valueHistory = &history;
    EXPECT_LT(computeCandidateConfidence(0x1500, ValueType::Int32, ctx), 0.3);
}
