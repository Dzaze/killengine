#include <gtest/gtest.h>

#include "debug/breakpoint_arbiter.h"

using killcore::HwBreakpointArbiter;
using killcore::HwBreakpointLifecycle;
using killcore::HwBreakpointOwner;
using killcore::HwBreakpointOwnershipGuard;

namespace {
// PIDs dediees par test pour eviter toute interference (le singleton
// HwBreakpointArbiter::instance() est process-wide et partage entre tests).
constexpr uint32_t kPidA = 900001;
constexpr uint32_t kPidB = 900002;
constexpr uint32_t kPidC = 900003;
constexpr uint32_t kPidD = 900004;
constexpr uint32_t kPidE = 900005;
} // namespace

TEST(BreakpointArbiter, TryAcquireSucceedsWhenIdle) {
    QString error;
    ASSERT_TRUE(HwBreakpointArbiter::instance().tryAcquire(
        kPidA, HwBreakpointOwner::InProcess, "test", 0x1000, 0, &error));
    EXPECT_TRUE(error.isEmpty());
    HwBreakpointArbiter::instance().resetForPid(kPidA);
}

TEST(BreakpointArbiter, SecondTryAcquireRefusedWhileFirstStillOwns) {
    QString error1;
    ASSERT_TRUE(HwBreakpointArbiter::instance().tryAcquire(
        kPidB, HwBreakpointOwner::InProcess, "in-process capture", 0x2000, 0, &error1));

    QString error2;
    const bool second = HwBreakpointArbiter::instance().tryAcquire(
        kPidB, HwBreakpointOwner::ExternalDebug, "findWhatWrites", 0x2000, -1, &error2);

    EXPECT_FALSE(second);
    EXPECT_FALSE(error2.isEmpty());

    HwBreakpointArbiter::instance().resetForPid(kPidB);
}

TEST(BreakpointArbiter, TryAcquireSucceedsAgainAfterCleanRelease) {
    QString error;
    ASSERT_TRUE(HwBreakpointArbiter::instance().tryAcquire(
        kPidC, HwBreakpointOwner::InProcess, "test", 0x3000, 0, &error));
    HwBreakpointArbiter::instance().markActive(kPidC);
    HwBreakpointArbiter::instance().markDisarming(kPidC);
    HwBreakpointArbiter::instance().release(kPidC, /*disarmConfirmed=*/true);

    QString error2;
    EXPECT_TRUE(HwBreakpointArbiter::instance().tryAcquire(
        kPidC, HwBreakpointOwner::ExternalDebug, "second", 0x3000, -1, &error2));

    HwBreakpointArbiter::instance().resetForPid(kPidC);
}

TEST(BreakpointArbiter, ReleaseWithoutConfirmedDisarmPoisonsThePidUntilReset) {
    QString error;
    ASSERT_TRUE(HwBreakpointArbiter::instance().tryAcquire(
        kPidD, HwBreakpointOwner::InProcess, "test", 0x4000, 0, &error));
    HwBreakpointArbiter::instance().markActive(kPidD);
    HwBreakpointArbiter::instance().markDisarming(kPidD);
    // Simule un timeout de desarmement (piege corrige le 20/08/2026) : on ne
    // peut PAS confirmer que DR7 est revenu a 0.
    HwBreakpointArbiter::instance().release(kPidD, /*disarmConfirmed=*/false);

    EXPECT_TRUE(HwBreakpointArbiter::instance().isPoisoned(kPidD));

    QString error2;
    EXPECT_FALSE(HwBreakpointArbiter::instance().tryAcquire(
        kPidD, HwBreakpointOwner::ExternalDebug, "second", 0x4000, -1, &error2));
    EXPECT_FALSE(error2.isEmpty());

    // resetForPid() est le seul moyen de sortir de l'etat Error -- reserve au
    // cas ou la cible elle-meme a disparu (PID mort/redemarre).
    HwBreakpointArbiter::instance().resetForPid(kPidD);
    EXPECT_FALSE(HwBreakpointArbiter::instance().isPoisoned(kPidD));

    QString error3;
    EXPECT_TRUE(HwBreakpointArbiter::instance().tryAcquire(
        kPidD, HwBreakpointOwner::ExternalDebug, "third", 0x4000, -1, &error3));
    HwBreakpointArbiter::instance().resetForPid(kPidD);
}

TEST(BreakpointArbiter, DifferentPidsDoNotInterfere) {
    QString errorA;
    QString errorB;
    ASSERT_TRUE(HwBreakpointArbiter::instance().tryAcquire(
        kPidA, HwBreakpointOwner::InProcess, "a", 0x1000, 0, &errorA));
    // kPidE est independant de kPidA -- doit reussir meme si kPidA est occupe.
    EXPECT_TRUE(HwBreakpointArbiter::instance().tryAcquire(
        kPidE, HwBreakpointOwner::ExternalDebug, "e", 0x5000, -1, &errorB));

    HwBreakpointArbiter::instance().resetForPid(kPidA);
    HwBreakpointArbiter::instance().resetForPid(kPidE);
}

TEST(BreakpointArbiter, OwnershipGuardReleasesAutomaticallyOnScopeExitByDefaultConfirmed) {
    {
        HwBreakpointOwnershipGuard guard(kPidA, HwBreakpointOwner::InProcess, "raii-test", 0x1000, 0);
        ASSERT_TRUE(guard.acquired());
        // Ne jamais appeler confirmDisarmed() -- rien n'a ete arme, le
        // defaut (true) doit s'appliquer et liberer proprement au destructeur.
    }
    QString error;
    EXPECT_TRUE(HwBreakpointArbiter::instance().tryAcquire(
        kPidA, HwBreakpointOwner::ExternalDebug, "after-raii", 0x1000, -1, &error));
    HwBreakpointArbiter::instance().resetForPid(kPidA);
}

TEST(BreakpointArbiter, OwnershipGuardPoisonsPidIfDestroyedWithoutConfirmingDisarm) {
    {
        HwBreakpointOwnershipGuard guard(kPidB, HwBreakpointOwner::InProcess, "raii-armed", 0x2000, 0);
        ASSERT_TRUE(guard.acquired());
        guard.markActive();
        guard.confirmDisarmed(false); // arme, jamais confirme desarme avant destruction
    }
    EXPECT_TRUE(HwBreakpointArbiter::instance().isPoisoned(kPidB));
    HwBreakpointArbiter::instance().resetForPid(kPidB);
}

TEST(BreakpointArbiter, TryAcquireFailsWhenGuardConstructionAlreadyRefused) {
    HwBreakpointOwnershipGuard first(kPidC, HwBreakpointOwner::InProcess, "first", 0x3000, 0);
    ASSERT_TRUE(first.acquired());

    HwBreakpointOwnershipGuard second(kPidC, HwBreakpointOwner::ExternalDebug, "second", 0x3000, -1);
    EXPECT_FALSE(second.acquired());
    EXPECT_FALSE(second.error().isEmpty());

    HwBreakpointArbiter::instance().resetForPid(kPidC);
}
