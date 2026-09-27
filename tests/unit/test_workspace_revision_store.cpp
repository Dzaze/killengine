// Tests unitaires du registre de révisions du workspace (UX-PRODUIT-13 --
// Historique automatique et récupération). Écrit réellement sur disque dans
// un répertoire temporaire par test (pas de mock du système de fichiers) --
// même approche que test_profile_store.cpp pour son propre store.

#include "workspace/workspace_revision_store.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QThread>
#include <QUuid>

using namespace killcore;

class WorkspaceRevisionStoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        tempDir = std::make_unique<QTemporaryDir>();
        ASSERT_TRUE(tempDir->isValid());
        store = std::make_unique<WorkspaceRevisionStore>(tempDir->path());
    }

    QString samplePayload(const QString& marker = "x") const {
        return QStringLiteral("{\"investigation\":{\"marker\":\"%1\"}}").arg(marker);
    }

    std::unique_ptr<QTemporaryDir> tempDir;
    std::unique_ptr<WorkspaceRevisionStore> store;
};

TEST_F(WorkspaceRevisionStoreTest, RoundTripPreservesPayloadExactly) {
    const auto created = store->create(WorkspaceRevisionReason::Manual, "ctxA", "game.exe", samplePayload("hello"));
    ASSERT_TRUE(created.success);
    ASSERT_FALSE(created.metadata.id.isEmpty());

    const auto read = store->read(created.metadata.id);
    ASSERT_TRUE(read.success);
    EXPECT_EQ(read.payloadJson, samplePayload("hello"));
    EXPECT_EQ(read.metadata.reason, WorkspaceRevisionReason::Manual);
    EXPECT_EQ(read.metadata.projectContext, QString("ctxA"));
    EXPECT_EQ(read.metadata.targetName, QString("game.exe"));
    EXPECT_EQ(read.metadata.payloadSha256, created.metadata.payloadSha256);
}

TEST_F(WorkspaceRevisionStoreTest, ListReturnsNewestFirst) {
    const auto first = store->create(WorkspaceRevisionReason::Manual, "", "", samplePayload("1"));
    ASSERT_TRUE(first.success);
    // Force un createdAtUtc strictement postérieur (résolution ms) pour un tri déterministe.
    QThread::msleep(5);
    const auto second = store->create(WorkspaceRevisionReason::Manual, "", "", samplePayload("2"));
    ASSERT_TRUE(second.success);

    const auto entries = store->list(0, 20);
    ASSERT_EQ(entries.size(), 2);
    EXPECT_EQ(entries[0].metadata.id, second.metadata.id);
    EXPECT_EQ(entries[1].metadata.id, first.metadata.id);
}

TEST_F(WorkspaceRevisionStoreTest, TruncatedFileIsIsolatedWithoutBreakingOtherEntries) {
    const auto good = store->create(WorkspaceRevisionReason::Manual, "", "", samplePayload("good"));
    ASSERT_TRUE(good.success);

    // Fichier tronqué directement sur disque : JSON invalide.
    QFile bad(tempDir->filePath("00000000-0000-0000-0000-000000000000.kwrev"));
    ASSERT_TRUE(bad.open(QIODevice::WriteOnly));
    bad.write("{\"schemaVersion\":1,\"id\":\"00000000-0000-0000-0000-000000000000\"");   // JSON coupé
    bad.close();

    const auto entries = store->list(0, 20);
    ASSERT_EQ(entries.size(), 2);
    int validCount = 0, invalidCount = 0;
    for (const auto& entry : entries) {
        if (entry.valid) { ++validCount; EXPECT_EQ(entry.metadata.id, good.metadata.id); }
        else { ++invalidCount; EXPECT_FALSE(entry.error.isEmpty()); }
    }
    EXPECT_EQ(validCount, 1);
    EXPECT_EQ(invalidCount, 1);

    // La révision valide reste lisible malgré la voisine corrompue.
    const auto read = store->read(good.metadata.id);
    EXPECT_TRUE(read.success);
}

TEST_F(WorkspaceRevisionStoreTest, TamperedShaIsDetectedAsInvalid) {
    const auto created = store->create(WorkspaceRevisionReason::Manual, "", "", samplePayload("original"));
    ASSERT_TRUE(created.success);

    // Modifie le payload sur disque sans mettre à jour payloadSha256 --
    // simule une corruption/altération partielle.
    const QString path = tempDir->filePath(created.metadata.id + ".kwrev");
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    QByteArray content = file.readAll();
    file.close();
    content.replace("original", "tampered!");
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write(content);
    file.close();

    const auto entries = store->list(0, 20);
    ASSERT_EQ(entries.size(), 1);
    EXPECT_FALSE(entries[0].valid);
    EXPECT_FALSE(entries[0].error.isEmpty());

    const auto read = store->read(created.metadata.id);
    EXPECT_FALSE(read.success);
}

TEST_F(WorkspaceRevisionStoreTest, UnknownSchemaVersionIsRejected) {
    QFile file(tempDir->filePath("11111111-1111-1111-1111-111111111111.kwrev"));
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write("{\"schemaVersion\":99,\"id\":\"11111111-1111-1111-1111-111111111111\",\"payload\":\"{}\",\"payloadSha256\":\"x\"}");
    file.close();

    const auto entries = store->list(0, 20);
    ASSERT_EQ(entries.size(), 1);
    EXPECT_FALSE(entries[0].valid);
}

TEST_F(WorkspaceRevisionStoreTest, ReadRejectsPathTraversalId) {
    const auto read = store->read("../../etc/passwd");
    EXPECT_FALSE(read.success);
    EXPECT_FALSE(read.error.isEmpty());
}

TEST_F(WorkspaceRevisionStoreTest, RemoveRejectsPathTraversalId) {
    QString error;
    EXPECT_FALSE(store->remove("../outside", &error));
    EXPECT_FALSE(error.isEmpty());
}

TEST_F(WorkspaceRevisionStoreTest, ReadUnknownIdFails) {
    const auto read = store->read(QUuid::createUuid().toString(QUuid::WithoutBraces));
    EXPECT_FALSE(read.success);
}

TEST_F(WorkspaceRevisionStoreTest, PayloadOverTenMibIsRejectedBeforeWrite) {
    const QString oversized(11 * 1024 * 1024, QChar('a'));
    const auto created = store->create(WorkspaceRevisionReason::Manual, "", "", oversized);
    EXPECT_FALSE(created.success);
    EXPECT_TRUE(store->list(0, 100).isEmpty());
}

TEST_F(WorkspaceRevisionStoreTest, AutomaticCapEvictsOldestAutomaticOnly) {
    for (int i = 0; i < kWorkspaceRevisionAutomaticCap + 5; ++i) {
        const auto created = store->create(WorkspaceRevisionReason::Automatic, "", "", samplePayload(QString::number(i)));
        ASSERT_TRUE(created.success);
        QThread::msleep(2);
    }
    const auto manual = store->create(WorkspaceRevisionReason::Manual, "", "", samplePayload("keep-me"));
    ASSERT_TRUE(manual.success);

    const auto entries = store->list(0, 100);
    int automaticCount = 0;
    bool manualStillPresent = false;
    for (const auto& entry : entries) {
        if (entry.metadata.reason == WorkspaceRevisionReason::Automatic) ++automaticCount;
        if (entry.metadata.id == manual.metadata.id) manualStillPresent = true;
    }
    EXPECT_EQ(automaticCount, kWorkspaceRevisionAutomaticCap);
    EXPECT_TRUE(manualStillPresent);
}

TEST_F(WorkspaceRevisionStoreTest, ProtectionCapCombinesBeforeImportAndBeforeRestore) {
    for (int i = 0; i < kWorkspaceRevisionProtectionCap + 3; ++i) {
        const auto reason = (i % 2 == 0) ? WorkspaceRevisionReason::BeforeImport : WorkspaceRevisionReason::BeforeRestore;
        const auto created = store->create(reason, "", "", samplePayload(QString::number(i)));
        ASSERT_TRUE(created.success);
        QThread::msleep(2);
    }
    const auto entries = store->list(0, 100);
    int protectionCount = 0;
    for (const auto& entry : entries) {
        if (entry.metadata.reason == WorkspaceRevisionReason::BeforeImport
            || entry.metadata.reason == WorkspaceRevisionReason::BeforeRestore) {
            ++protectionCount;
        }
    }
    EXPECT_EQ(protectionCount, kWorkspaceRevisionProtectionCap);
}

TEST_F(WorkspaceRevisionStoreTest, ManualCapRefusesCreationRatherThanEvicting) {
    QStringList createdIds;
    for (int i = 0; i < kWorkspaceRevisionManualCap; ++i) {
        const auto created = store->create(WorkspaceRevisionReason::Manual, "", "", samplePayload(QString::number(i)));
        ASSERT_TRUE(created.success);
        createdIds.append(created.metadata.id);
        QThread::msleep(2);
    }

    // La fiche est explicite : "jamais les manuelles automatiquement" -- donc
    // le cap de 10 est appliqué en REFUSANT la 11e création, pas en évinçant
    // silencieusement une sauvegarde intentionnelle de l'utilisateur.
    const auto refused = store->create(WorkspaceRevisionReason::Manual, "", "", samplePayload("overflow"));
    EXPECT_FALSE(refused.success);
    EXPECT_FALSE(refused.error.isEmpty());

    const auto entries = store->list(0, 100);
    int manualCount = 0;
    for (const auto& entry : entries) {
        if (entry.metadata.reason == WorkspaceRevisionReason::Manual) ++manualCount;
    }
    EXPECT_EQ(manualCount, kWorkspaceRevisionManualCap);
    // Toutes les 10 révisions d'origine sont toujours présentes, aucune évincée.
    for (const auto& id : createdIds) {
        EXPECT_TRUE(store->read(id).success);
    }
}

TEST_F(WorkspaceRevisionStoreTest, ExplicitRemoveDeletesManualRevision) {
    const auto created = store->create(WorkspaceRevisionReason::Manual, "", "", samplePayload("to-delete"));
    ASSERT_TRUE(created.success);

    QString error;
    EXPECT_TRUE(store->remove(created.metadata.id, &error));
    EXPECT_TRUE(error.isEmpty());
    EXPECT_TRUE(store->list(0, 100).isEmpty());
    EXPECT_FALSE(store->read(created.metadata.id).success);
}

TEST_F(WorkspaceRevisionStoreTest, RefusedWriteLeavesExistingRevisionsIntact) {
    const auto first = store->create(WorkspaceRevisionReason::Manual, "", "", samplePayload("safe"));
    ASSERT_TRUE(first.success);

    // Un create() refusé (payload > 10 Mio, vérifié avant toute écriture
    // disque -- voir create()) ne doit jamais toucher les révisions
    // existantes ("un disque plein laisse les anciennes intactes").
    const QString oversized(11 * 1024 * 1024, QChar('a'));
    const auto failed = store->create(WorkspaceRevisionReason::Manual, "", "", oversized);
    EXPECT_FALSE(failed.success);

    const auto entries = store->list(0, 100);
    ASSERT_EQ(entries.size(), 1);
    EXPECT_EQ(entries[0].metadata.id, first.metadata.id);
    EXPECT_TRUE(store->read(first.metadata.id).success);
}
