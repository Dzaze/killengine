#include <gtest/gtest.h>

#include "process/file_watch.h"
#include "memory/memory_reader.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QThread>
#include <QUuid>

#include <thread>

namespace {

class FileWatchTest : public ::testing::Test {
protected:
    void SetUp() override {
        const QString localAppData = qEnvironmentVariable("LOCALAPPDATA");
        ASSERT_FALSE(localAppData.isEmpty());

        m_root = QDir(localAppData).filePath(QStringLiteral("Packages/KillEngineFileWatchTest_%1")
            .arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
        ASSERT_TRUE(QDir().mkpath(m_root));

        m_path = QDir(m_root).filePath(QStringLiteral("watched.bin"));
        writeContent("initial");
    }

    void TearDown() override {
        if (!m_root.isEmpty()) {
            QDir dir(m_root);
            dir.removeRecursively();
        }
    }

    void writeContent(const QByteArray& content) const {
        QFile file(m_path);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write(content);
        file.close();
    }

    QString m_root;
    QString m_path;
};

} // namespace

TEST_F(FileWatchTest, TimesOutWithoutErrorWhenNothingChanges) {
    killcore::FileWatchOutcome outcome;
    QString error;

    const bool started = killcore::watchFileForChanges(m_path, 400, nullptr, &outcome, &error);

    EXPECT_TRUE(started) << error.toStdString();
    EXPECT_FALSE(outcome.changed);
    EXPECT_FALSE(outcome.cancelled);
    EXPECT_TRUE(error.isEmpty());
}

TEST_F(FileWatchTest, DetectsModificationFromAnotherThread) {
    std::thread writer([this]() {
        QThread::msleep(150);
        writeContent("modified");
    });

    killcore::FileWatchOutcome outcome;
    QString error;
    const bool started = killcore::watchFileForChanges(m_path, 5000, nullptr, &outcome, &error);
    writer.join();

    EXPECT_TRUE(started) << error.toStdString();
    EXPECT_TRUE(outcome.changed);
    EXPECT_FALSE(outcome.changeType.isEmpty());
}

TEST_F(FileWatchTest, CancellationStopsTheWaitBeforeTimeout) {
    killcore::CancellationToken cancellation;
    std::thread canceller([&cancellation]() {
        QThread::msleep(150);
        cancellation.cancel();
    });

    QElapsedTimer elapsed;
    elapsed.start();

    killcore::FileWatchOutcome outcome;
    QString error;
    const bool started = killcore::watchFileForChanges(m_path, 30000, &cancellation, &outcome, &error);
    canceller.join();

    EXPECT_TRUE(started) << error.toStdString();
    EXPECT_TRUE(outcome.cancelled);
    EXPECT_FALSE(outcome.changed);
    // La boucle poll interne re-arme toutes les ~200ms : l'annulation doit
    // etre vue bien avant le timeout de 30s, pas juste "avant timeout".
    EXPECT_LT(elapsed.elapsed(), 5000);
}
