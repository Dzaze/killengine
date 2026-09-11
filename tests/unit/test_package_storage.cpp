#include <gtest/gtest.h>

#include "process/package_storage.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QVector>
#include <QUuid>

namespace {

// Sauvegarde/restaure "ui/language" autour du test, meme pattern que
// test_localization.cpp/test_ai_tools.cpp/test_auto_resolver.cpp/
// test_aob_scanner.cpp : les messages verifies ici sont desormais traduits
// via KE_TXT (docs/BACKEND_UI_LOCALIZATION_ROADMAP.md, candidat B8, 11/09/2026).
class ScopedUiLanguage {
public:
    explicit ScopedUiLanguage(const QString& language) {
        QSettings settings;
        m_previous = settings.value("ui/language");
        settings.setValue("ui/language", language);
        settings.sync();
    }

    ~ScopedUiLanguage() {
        QSettings settings;
        if (m_previous.isValid()) {
            settings.setValue("ui/language", m_previous);
        } else {
            settings.remove("ui/language");
        }
        settings.sync();
    }

private:
    QVariant m_previous;
};

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
    ScopedUiLanguage lang("fr");
    const QString path = writeFile("value=58");
    QString error;

    const bool ok = killcore::patchPackageSaveFileBytes(path, QStringLiteral("35 38"), QStringLiteral("39"), &error);

    EXPECT_FALSE(ok);
    EXPECT_TRUE(error.contains(QStringLiteral("longueur"), Qt::CaseInsensitive));
    EXPECT_EQ(readFile(path), QByteArray("value=58"));
}

TEST_F(PackageStoragePatchTest, RejectsMissingSequence) {
    ScopedUiLanguage lang("fr");
    const QString path = writeFile("value=58");
    QString error;

    const bool ok = killcore::patchPackageSaveFileBytes(path, QStringLiteral("39 39"), QStringLiteral("30 30"), &error);

    EXPECT_FALSE(ok);
    EXPECT_TRUE(error.contains(QStringLiteral("introuvable"), Qt::CaseInsensitive));
    EXPECT_EQ(readFile(path), QByteArray("value=58"));
}

TEST_F(PackageStoragePatchTest, RejectsAmbiguousSequence) {
    ScopedUiLanguage lang("fr");
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

TEST_F(PackageStoragePatchTest, LocalSettingsReportsMissingHiveUnderPackageRoot) {
    ScopedUiLanguage lang("fr");
    QVector<killcore::PackageLocalSettingsEntry> entries;
    QString settingsPath;
    QString error;
    const QString familyName = QFileInfo(m_root).fileName();

    const bool ok = killcore::inspectPackageLocalSettings(familyName, 25, &entries, &settingsPath, &error);

    EXPECT_FALSE(ok);
    EXPECT_TRUE(entries.isEmpty());
    EXPECT_TRUE(settingsPath.endsWith(QStringLiteral("Settings/settings.dat"))
                || settingsPath.endsWith(QStringLiteral("Settings\\settings.dat")));
    EXPECT_TRUE(error.contains(QStringLiteral("introuvable"), Qt::CaseInsensitive));
}

TEST_F(PackageStoragePatchTest, LocalSettingsRejectsInvalidHiveFileCleanly) {
    ScopedUiLanguage lang("fr");
    ASSERT_TRUE(QDir().mkpath(QDir(m_root).filePath(QStringLiteral("Settings"))));
    const QString hivePath = QDir(m_root).filePath(QStringLiteral("Settings/settings.dat"));
    QFile fakeHive(hivePath);
    ASSERT_TRUE(fakeHive.open(QIODevice::WriteOnly | QIODevice::Truncate));
    ASSERT_EQ(fakeHive.write("not a registry hive"), qint64(19));
    fakeHive.close();

    QVector<killcore::PackageLocalSettingsEntry> entries;
    QString settingsPath;
    QString error;
    const QString familyName = QFileInfo(m_root).fileName();

    const bool ok = killcore::inspectPackageLocalSettings(familyName, 25, &entries, &settingsPath, &error);

    EXPECT_FALSE(ok);
    EXPECT_TRUE(entries.isEmpty());
    EXPECT_EQ(QFileInfo(settingsPath).absoluteFilePath(), QFileInfo(hivePath).absoluteFilePath());
    EXPECT_TRUE(error.contains(QStringLiteral("RegLoadAppKeyW"), Qt::CaseInsensitive)
                || error.contains(QStringLiteral("Non supporté"), Qt::CaseInsensitive));
}
