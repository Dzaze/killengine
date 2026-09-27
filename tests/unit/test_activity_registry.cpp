// Tests unitaires du registre d'activité (UX-PRODUIT-12 -- Centre d'activité
// permanent). Logique pure, sans QObject ni thread : voir
// core/activity/activity_registry.h pour l'invariant de thread-safety
// (aucun mutex, appel depuis le thread Qt uniquement).

#include "activity/activity_registry.h"

#include <gtest/gtest.h>

using namespace killcore;

namespace {
ActivityTarget makeTarget(int pid = 1234) {
    ActivityTarget target;
    target.pid = QString::number(pid);
    target.processName = "test.exe";
    target.attachmentGeneration = "1";
    return target;
}
}

TEST(ActivityRegistry, BeginTransitionsToRunning) {
    ActivityRegistry registry;
    const QString id = registry.beginActivity(ActivityKind::ScanExact, "Scan exact", true, makeTarget());
    ASSERT_FALSE(id.isEmpty());

    const auto entry = registry.find(id);
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(entry->state, ActivityState::Running);
    EXPECT_EQ(entry->kind, ActivityKind::ScanExact);
    EXPECT_TRUE(entry->canCancel);
    EXPECT_EQ(registry.runningCount(), 1);
}

TEST(ActivityRegistry, RefusedStartProducesNoRunningEntry) {
    // Un refus de démarrage ne doit jamais appeler beginActivity -- ce test
    // documente juste qu'un registre vide reste vide tant que rien ne le peuple.
    ActivityRegistry registry;
    EXPECT_EQ(registry.runningCount(), 0);
    EXPECT_TRUE(registry.snapshot().isEmpty());
}

TEST(ActivityRegistry, ProgressUpdateBumpsRevisionAndValue) {
    ActivityRegistry registry;
    const QString id = registry.beginActivity(ActivityKind::ScanExact, "Scan exact", true, std::nullopt);
    const qint64 revisionAfterBegin = registry.find(id)->revision;

    ASSERT_TRUE(registry.updateProgress(id, 42));
    const auto entry = registry.find(id);
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(entry->progress.toInt(), 42);
    EXPECT_GT(entry->revision, revisionAfterBegin);
}

TEST(ActivityRegistry, FinishBeforeAnyProgressUpdateStillTerminal) {
    ActivityRegistry registry;
    const QString id = registry.beginActivity(ActivityKind::ScanExact, "Scan exact", true, std::nullopt);
    ASSERT_TRUE(registry.finish(id, ActivityState::Completed, "done", QString()));

    const auto entry = registry.find(id);
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(entry->state, ActivityState::Completed);
    EXPECT_NE(entry->finishedAtMs, 0);
    EXPECT_EQ(registry.runningCount(), 0);
}

TEST(ActivityRegistry, ProgressAfterFinishIsIgnored) {
    ActivityRegistry registry;
    const QString id = registry.beginActivity(ActivityKind::ScanExact, "Scan exact", true, std::nullopt);
    ASSERT_TRUE(registry.finish(id, ActivityState::Completed, QString(), QString()));

    EXPECT_FALSE(registry.updateProgress(id, 99));
    const auto entry = registry.find(id);
    ASSERT_TRUE(entry.has_value());
    EXPECT_NE(entry->progress.toInt(), 99);
    EXPECT_EQ(entry->state, ActivityState::Completed);
}

TEST(ActivityRegistry, DoubleFinishDoesNotOverwriteFirstResult) {
    ActivityRegistry registry;
    const QString id = registry.beginActivity(ActivityKind::ScanExact, "Scan exact", true, std::nullopt);
    ASSERT_TRUE(registry.finish(id, ActivityState::Completed, "first", QString()));
    EXPECT_FALSE(registry.finish(id, ActivityState::Failed, "second", "error"));

    const auto entry = registry.find(id);
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(entry->state, ActivityState::Completed);
    EXPECT_EQ(entry->summary, QString("first"));
}

TEST(ActivityRegistry, CancelRequestedOnlyBecomesTerminalAfterFinish) {
    ActivityRegistry registry;
    const QString id = registry.beginActivity(ActivityKind::ScanExact, "Scan exact", true, std::nullopt);
    ASSERT_TRUE(registry.markCancelRequested(id));

    auto entry = registry.find(id);
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(entry->state, ActivityState::CancelRequested);
    EXPECT_EQ(registry.runningCount(), 1); // CancelRequested n'est pas terminal

    ASSERT_TRUE(registry.finish(id, ActivityState::Cancelled, QString(), QString()));
    entry = registry.find(id);
    EXPECT_EQ(entry->state, ActivityState::Cancelled);
    EXPECT_EQ(registry.runningCount(), 0);
}

TEST(ActivityRegistry, CancelOfTerminalEntryIsIdempotentNoOp) {
    ActivityRegistry registry;
    const QString id = registry.beginActivity(ActivityKind::ScanExact, "Scan exact", true, std::nullopt);
    ASSERT_TRUE(registry.finish(id, ActivityState::Completed, QString(), QString()));

    EXPECT_TRUE(registry.markCancelRequested(id)); // idempotent, pas une erreur
    EXPECT_EQ(registry.find(id)->state, ActivityState::Completed); // inchangé
}

TEST(ActivityRegistry, CancelOfRunningADoesNotAffectRunningB) {
    ActivityRegistry registry;
    const QString idA = registry.beginActivity(ActivityKind::ScanExact, "A", true, std::nullopt);
    const QString idB = registry.beginActivity(ActivityKind::ScanNext, "B", true, std::nullopt);

    ASSERT_TRUE(registry.markCancelRequested(idA));
    ASSERT_TRUE(registry.finish(idA, ActivityState::Cancelled, QString(), QString()));

    const auto entryB = registry.find(idB);
    ASSERT_TRUE(entryB.has_value());
    EXPECT_EQ(entryB->state, ActivityState::Running);
    EXPECT_EQ(registry.runningCount(), 1);
}

TEST(ActivityRegistry, EvictsOldestTerminalAt200RunningNeverEvicted) {
    ActivityRegistry registry;
    const QString keepRunning = registry.beginActivity(ActivityKind::TimelineCollection, "long running", true, std::nullopt);

    QString firstTerminalId;
    for (int i = 0; i < kActivityTerminalCap + 10; ++i) {
        const QString id = registry.beginActivity(ActivityKind::ScanExact, QString::number(i), false, std::nullopt);
        if (i == 0) {
            firstTerminalId = id;
        }
        ASSERT_TRUE(registry.finish(id, ActivityState::Completed, QString(), QString()));
    }

    // La toute première entrée terminale doit avoir été évincée (plus vieille).
    EXPECT_FALSE(registry.find(firstTerminalId).has_value());
    // L'entrée running n'a jamais été touchée par l'éviction.
    EXPECT_TRUE(registry.find(keepRunning).has_value());
    EXPECT_EQ(registry.runningCount(), 1);

    int terminalCount = 0;
    for (const auto& entry : registry.snapshot()) {
        if (entry.state == ActivityState::Completed) {
            ++terminalCount;
        }
    }
    EXPECT_EQ(terminalCount, kActivityTerminalCap);
}

TEST(ActivityRegistry, SnapshotOrdersRunningFirstThenTerminalNewestFirst) {
    ActivityRegistry registry;
    const QString running = registry.beginActivity(ActivityKind::ScanExact, "running", true, std::nullopt);
    const QString terminal1 = registry.beginActivity(ActivityKind::ScanNext, "t1", false, std::nullopt);
    registry.finish(terminal1, ActivityState::Completed, QString(), QString());
    const QString terminal2 = registry.beginActivity(ActivityKind::ScanNext, "t2", false, std::nullopt);
    registry.finish(terminal2, ActivityState::Completed, QString(), QString());

    const auto snapshot = registry.snapshot();
    ASSERT_EQ(snapshot.size(), 3);
    EXPECT_EQ(snapshot[0].operationId, running);
    EXPECT_EQ(snapshot[1].operationId, terminal2); // plus récente d'abord
    EXPECT_EQ(snapshot[2].operationId, terminal1);
}

TEST(ActivityRegistry, GlobalRevisionIncreasesMonotonicallyAcrossEntries) {
    ActivityRegistry registry;
    const QString idA = registry.beginActivity(ActivityKind::ScanExact, "A", true, std::nullopt);
    const qint64 revAfterBegin = registry.globalRevision();

    registry.updateProgress(idA, 10);
    const qint64 revAfterProgress = registry.globalRevision();
    EXPECT_GT(revAfterProgress, revAfterBegin);

    const QString idB = registry.beginActivity(ActivityKind::ScanNext, "B", true, std::nullopt);
    EXPECT_GT(registry.globalRevision(), revAfterProgress);
    EXPECT_NE(idA, idB);
}

TEST(ActivityRegistry, AcknowledgeMarksEntryAndClearsUnacknowledgedCount) {
    ActivityRegistry registry;
    const QString id = registry.beginActivity(ActivityKind::ScanExact, "A", true, std::nullopt);
    registry.finish(id, ActivityState::Completed, QString(), QString());

    EXPECT_EQ(registry.unacknowledgedTerminalCount(), 1);
    ASSERT_TRUE(registry.acknowledge(id));
    EXPECT_EQ(registry.unacknowledgedTerminalCount(), 0);
    EXPECT_TRUE(registry.find(id)->acknowledged);
}

TEST(ActivityRegistry, UnknownOperationIdIsHandledGracefully) {
    ActivityRegistry registry;
    EXPECT_FALSE(registry.find("does-not-exist").has_value());
    EXPECT_FALSE(registry.updateProgress("does-not-exist", 1));
    EXPECT_FALSE(registry.markCancelRequested("does-not-exist"));
    EXPECT_FALSE(registry.finish("does-not-exist", ActivityState::Completed, QString(), QString()));
    EXPECT_FALSE(registry.acknowledge("does-not-exist"));
}

TEST(ActivityRegistry, FinishRejectsNonTerminalTargetState) {
    ActivityRegistry registry;
    const QString id = registry.beginActivity(ActivityKind::ScanExact, "A", true, std::nullopt);
    EXPECT_FALSE(registry.finish(id, ActivityState::Running, QString(), QString()));
    EXPECT_FALSE(registry.finish(id, ActivityState::CancelRequested, QString(), QString()));
    EXPECT_EQ(registry.find(id)->state, ActivityState::Running);
}
