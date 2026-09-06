// Vérifie que le passage de isPointerValid() d'un scan linéaire O(R) à une
// recherche binaire O(log R) (PHASE perf, 06/09/2026) ne change AUCUN
// résultat — voir core/scanner/auto_dissect.cpp/.h. C'est le seul endroit où
// une régression réintroduirait silencieusement des faux négatifs (une
// structure valide ignorée par Auto-Dissect) sans jamais planter ni logger
// d'erreur.
//
// Stratégie : une référence par scan linéaire (mirroir de l'ancien code,
// trivialement correcte par construction) comparée à isPointerValid() sur un
// jeu de régions couvrant délibérément tous les cas limites (avant la
// première région, à la frontière exacte d'une région, dans un trou entre
// deux régions, dans la dernière région, etc.) plutôt que quelques exemples
// choisis au hasard.

#include "scanner/auto_dissect.h"

#include <gtest/gtest.h>

using namespace killcore;

namespace {

MemoryRegion makeRegion(uint64_t base, uint64_t size, bool committed, bool readable) {
    MemoryRegion r;
    r.baseAddress = base;
    r.size = size;
    r.state = committed ? MemoryState::Committed : MemoryState::Reserved;
    r.readable = readable;
    r.writable = true;
    return r;
}

// Référence délibérément naïve (scan linéaire) — c'est exactement le code
// remplacé dans auto_dissect.cpp avant le passage à la recherche binaire.
bool linearReferenceIsPointerValid(const QList<MemoryRegion>& regions, uint64_t addr) {
    if (addr == 0) return false;
    for (const auto& r : regions) {
        if (addr >= r.baseAddress && addr < r.baseAddress + r.size) {
            return r.state == MemoryState::Committed && r.readable;
        }
    }
    return false;
}

// Un jeu de régions volontairement "difficile" : tailles différentes, trous
// entre certaines régions, un mélange de committed/readable et non-valides,
// pour que la recherche binaire ne puisse pas accidentellement "avoir bon"
// sur un cas trivial à une seule région.
QList<MemoryRegion> sampleRegions() {
    return {
        makeRegion(0x1000, 0x1000, true, true),   // [0x1000, 0x2000) valide
        makeRegion(0x2000, 0x1000, false, true),  // [0x2000, 0x3000) reserved (pas committed)
        // trou : [0x3000, 0x5000) — aucune région ne couvre cette plage
        makeRegion(0x5000, 0x0500, true, false),  // [0x5000, 0x5500) committed mais pas readable
        makeRegion(0x5500, 0x2000, true, true),   // [0x5500, 0x7500) valide
        makeRegion(0x7500, 0x0001, true, true),   // [0x7500, 0x7501) région d'un seul octet
    };
}

} // namespace

TEST(AutoDissectIsPointerValid, ZeroAddressIsAlwaysInvalid) {
    EXPECT_FALSE(isPointerValid(sampleRegions(), 0));
}

TEST(AutoDissectIsPointerValid, EmptyRegionListIsAlwaysInvalid) {
    QList<MemoryRegion> empty;
    EXPECT_FALSE(isPointerValid(empty, 0x1000));
    EXPECT_FALSE(isPointerValid(empty, 0));
}

TEST(AutoDissectIsPointerValid, AddressBeforeFirstRegionIsInvalid) {
    EXPECT_FALSE(isPointerValid(sampleRegions(), 0x0500));
}

TEST(AutoDissectIsPointerValid, AddressAtExactRegionStartIsValidWhenCommittedAndReadable) {
    EXPECT_TRUE(isPointerValid(sampleRegions(), 0x1000));
}

TEST(AutoDissectIsPointerValid, AddressInMiddleOfFirstRegionIsValid) {
    EXPECT_TRUE(isPointerValid(sampleRegions(), 0x1800));
}

TEST(AutoDissectIsPointerValid, AddressAtLastByteOfRegionIsValid) {
    // 0x1FFF est le dernier octet de [0x1000, 0x2000) — 0x2000 lui-même
    // appartient déjà à la région suivante (reserved, testé séparément).
    EXPECT_TRUE(isPointerValid(sampleRegions(), 0x1FFF));
}

TEST(AutoDissectIsPointerValid, AddressInReservedRegionIsInvalid) {
    // [0x2000, 0x3000) existe mais n'est pas "Committed".
    EXPECT_FALSE(isPointerValid(sampleRegions(), 0x2500));
}

TEST(AutoDissectIsPointerValid, AddressInGapBetweenRegionsIsInvalid) {
    // [0x3000, 0x5000) n'est couvert par AUCUNE région — c'est le cas qui
    // casserait silencieusement si la recherche binaire se contentait de
    // "la dernière région dont baseAddress <= addr" sans vérifier la borne
    // haute (size) de cette région.
    EXPECT_FALSE(isPointerValid(sampleRegions(), 0x3000));
    EXPECT_FALSE(isPointerValid(sampleRegions(), 0x4500));
    EXPECT_FALSE(isPointerValid(sampleRegions(), 0x4FFF));
}

TEST(AutoDissectIsPointerValid, AddressInCommittedButNotReadableRegionIsInvalid) {
    EXPECT_FALSE(isPointerValid(sampleRegions(), 0x5200));
}

TEST(AutoDissectIsPointerValid, AddressInLastRegionIsValid) {
    // Exerce le cas où upper_bound doit avancer jusqu'au tout dernier
    // élément de la liste triée.
    EXPECT_TRUE(isPointerValid(sampleRegions(), 0x6000));
}

TEST(AutoDissectIsPointerValid, SingleByteRegionIsHandledPrecisely) {
    EXPECT_TRUE(isPointerValid(sampleRegions(), 0x7500));
    EXPECT_FALSE(isPointerValid(sampleRegions(), 0x7501)); // un octet après la fin
}

TEST(AutoDissectIsPointerValid, AddressAfterLastRegionIsInvalid) {
    EXPECT_FALSE(isPointerValid(sampleRegions(), 0x9000));
}

TEST(AutoDissectIsPointerValid, MatchesLinearReferenceForEveryBoundaryCase) {
    // Le vrai test de non-régression : compare exhaustivement contre la
    // référence linéaire (l'ancien algorithme) sur toutes les adresses
    // "intéressantes" du jeu de régions ci-dessus, plutôt que de faire
    // confiance à quelques cas choisis à la main un par un.
    const auto regions = sampleRegions();
    QList<uint64_t> addressesToCheck;
    for (const auto& r : regions) {
        addressesToCheck << r.baseAddress
                          << r.baseAddress + 1
                          << r.baseAddress + r.size / 2
                          << r.baseAddress + r.size - 1
                          << r.baseAddress + r.size       // un octet après la fin
                          << (r.baseAddress > 0 ? r.baseAddress - 1 : 0); // un octet avant le début
    }
    addressesToCheck << 0 << 0x500 << 0x3000 << 0x4FFF << 0xFFFFFFFFULL;

    for (const uint64_t addr : addressesToCheck) {
        EXPECT_EQ(isPointerValid(regions, addr), linearReferenceIsPointerValid(regions, addr))
            << "Désaccord entre recherche binaire et scan linéaire pour addr=0x"
            << QString::number(addr, 16).toStdString();
    }
}
