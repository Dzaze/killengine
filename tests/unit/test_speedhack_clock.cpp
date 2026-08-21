#include <gtest/gtest.h>

#include "debug/speedhack_clock.h"

using killcore::scaleClockDelta;

TEST(SpeedhackClock, FactorOneIsExactPassthrough) {
    // factor=1.0 (vitesse normale) doit reproduire exactement le compteur reel,
    // quel que soit le point de depart — c'est l'etat de repos apres stop().
    const int64_t virtualNow = scaleClockDelta(/*realBase=*/1000, /*realNow=*/1500, /*virtualBase=*/1000, /*factor=*/1.0);
    EXPECT_EQ(virtualNow, 1500);
}

TEST(SpeedhackClock, FactorTwoDoublesTheDelta) {
    const int64_t virtualNow = scaleClockDelta(1000, 1100, 1000, 2.0);
    EXPECT_EQ(virtualNow, 1200); // delta reel 100 -> delta virtuel 200
}

TEST(SpeedhackClock, FactorHalfSlowsTheDelta) {
    const int64_t virtualNow = scaleClockDelta(1000, 1100, 1000, 0.5);
    EXPECT_EQ(virtualNow, 1050); // delta reel 100 -> delta virtuel 50
}

TEST(SpeedhackClock, FactorZeroFreezesTheClock) {
    // Preset "pause" : l'horloge virtuelle n'avance plus, quel que soit le temps
    // reel ecoule.
    const int64_t virtualNow = scaleClockDelta(1000, 5000, 2500, 0.0);
    EXPECT_EQ(virtualNow, 2500);
}

TEST(SpeedhackClock, NoJumpAtActivation) {
    // Au moment de l'activation, realBase == realNow == virtualBase (etat
    // initial pose par le site d'appel, voir speedhack_handler.cpp) : le tout
    // premier appel ne doit produire aucun saut, quel que soit le facteur.
    const int64_t virtualNow = scaleClockDelta(42, 42, 42, 4.0);
    EXPECT_EQ(virtualNow, 42);
}

TEST(SpeedhackClock, FactorChangeMidStreamHasNoDiscontinuity) {
    // Simule le site d'appel reel : chaque appel met a jour realBase/virtualBase
    // avec le resultat du precedent (voir le commentaire de scaleClockDelta).
    // Un changement de facteur en cours de route ne doit affecter QUE le
    // prochain delta, jamais recalculer/sauter le passe deja ecoule.
    int64_t realBase = 0;
    int64_t virtualBase = 0;

    // Premiere seconde a facteur 1.0 (vitesse normale).
    int64_t virtualNow = scaleClockDelta(realBase, 1000, virtualBase, 1.0);
    EXPECT_EQ(virtualNow, 1000);
    realBase = 1000;
    virtualBase = virtualNow;

    // Changement de facteur a 2.0 : le point de depart du prochain delta est
    // exactement la ou l'horloge virtuelle s'est arretee (1000), pas un
    // recalcul depuis l'origine (qui aurait donne 2000 et un saut visible).
    virtualNow = scaleClockDelta(realBase, 1500, virtualBase, 2.0);
    EXPECT_EQ(virtualNow, 2000); // 1000 + (1500-1000)*2
}
