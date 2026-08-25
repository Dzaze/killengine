#include <gtest/gtest.h>

#include "process/package_storage.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QUuid>

namespace {

class PackageStoragePatchTest : public ::testing::Test {
protected:
    void SetUp() override {
        const QString localAppData = qEnvironmentVariable("LOCALAPPDATA");
        ASSERT_FALSE(localAppData.isEmpty());

        m_root = QDir(localAppData).filePath(QStringLiteral("Packages/KillEnginePatchBytesTest_%1")
            .arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
        ASSERT_TRUE(QDir().mkpath(m_root));
    }

    void TearDown() override {
        if (!m_root.isEmpty()) {
            QDir dir(m_root);
            dir.removeRecursively();
        }
    }

    QString writeFile(const QByteArray& content) const {
        const QString path = QDir(m_root).filePath(QStringLiteral("save.bin"));
        QFile file(path);
        EXPECT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        EXPECT_EQ(file.write(content), content.size());
        file.close();
        return path;
    }

    QByteArray readFile(const QString& path) const {
        QFile file(path);
        EXPECT_TRUE(file.open(QIODevice::ReadOnly));
        const QByteArray content = file.readAll();
        file.close();
        return content;
    }

    QString m_root;
};

} // namespace

TEST_F(PackageStoragePatchTest, RejectsDifferentDecodedLengths) {
    const QString path = writeFile("value=58");
    QString error;

    const bool ok = killcore::patchPackageSaveFileBytes(path, QStringLiteral("35 38"), QStringLiteral("39"), &error);

    EXPECT_FALSE(ok);
    EXPECT_TRUE(error.contains(QStringLiteral("longueur"), Qt::CaseInsensitive));
    EXPECT_EQ(readFile(path), QByteArray("value=58"));
}

TEST_F(PackageStoragePatchTest, RejectsMissingSequence) {
    const QString path = writeFile("value=58");
    QString error;

    const bool ok = killcore::patchPackageSaveFileBytes(path, QStringLiteral("39 39"), QStringLiteral("30 30"), &error);

    EXPECT_FALSE(ok);
    EXPECT_TRUE(error.contains(QStringLiteral("introuvable"), Qt::CaseInsensitive));
    EXPECT_EQ(readFile(path), QByteArray("value=58"));
}

TEST_F(PackageStoragePatchTest, RejectsAmbiguousSequence) {
    const QString path = writeFile("a=58;b=58");
    QString error;

    const bool ok = killcore::patchPackageSaveFileBytes(path, QStringLiteral("35 38"), QStringLiteral("39 39"), &error);

    EXPECT_FALSE(ok);
    EXPECT_TRUE(error.contains(QStringLiteral("occurrences"), Qt::CaseInsensitive));
    EXPECT_EQ(readFile(path), QByteArray("a=58;b=58"));
}

TEST_F(PackageStoragePatchTest, PatchesSingleOccurrenceInPlace) {
    const QByteArray original("prefix value=58 suffix");
    const QString path = writeFile(original);
    const qint64 originalSize = QFileInfo(path).size();
    QString error;

    const bool ok = killcore::patchPackageSaveFileBytes(path, QStringLiteral("35-38"), QStringLiteral("39 39"), &error);

    EXPECT_TRUE(ok) << error.toStdString();
    EXPECT_EQ(QFileInfo(path).size(), originalSize);
    EXPECT_EQ(readFile(path), QByteArray("prefix value=99 suffix"));
}
