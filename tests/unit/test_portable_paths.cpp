#include <gtest/gtest.h>
#include "paths/portable_paths.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QTemporaryDir>

namespace {

// Sauvegarde/restaure l'override de test autour du test, meme pattern que
// ScopedUiLanguage (tests/unit/test_localization.cpp) pour ne pas laisser de
// racine de fixture active pour un test suivant qui l'ignore.
class ScopedPortableRoot {
public:
    explicit ScopedPortableRoot(const QString& root) {
        killcore::PortablePaths::setTestRootOverride(root);
    }
    ~ScopedPortableRoot() {
        killcore::PortablePaths::setTestRootOverride(QString());
    }
};

} // namespace

TEST(PortablePathsTest, RootDefaultsToApplicationDirPathWithoutOverride) {
    EXPECT_EQ(killcore::PortablePaths::root(), QCoreApplication::applicationDirPath());
}

TEST(PortablePathsTest, TestOverrideChangesRootAndClearingRestoresDefault) {
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());

    {
        ScopedPortableRoot scoped(tmpDir.path());
        EXPECT_EQ(killcore::PortablePaths::root(), tmpDir.path());
    }

    EXPECT_EQ(killcore::PortablePaths::root(), QCoreApplication::applicationDirPath());
}

TEST(PortablePathsTest, EnsureSubdirCreatesDirectoryUnderRoot) {
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());
    ScopedPortableRoot scoped(tmpDir.path());

    const QString dir = killcore::PortablePaths::ensureSubdir("logs");
    EXPECT_EQ(dir, QDir(tmpDir.path()).filePath("logs"));
    EXPECT_TRUE(QFileInfo(dir).isDir());
}

TEST(PortablePathsTest, EnsureSubdirSupportsMultiSegmentPath) {
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());
    ScopedPortableRoot scoped(tmpDir.path());

    const QString dir = killcore::PortablePaths::ensureSubdir("data/profiles");
    EXPECT_EQ(dir, QDir(tmpDir.path()).filePath("data/profiles"));
    EXPECT_TRUE(QFileInfo(dir).isDir());
}

TEST(PortablePathsTest, FilePathReturnsNestedPathAndCreatesParentSubdir) {
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());
    ScopedPortableRoot scoped(tmpDir.path());

    const QString path = killcore::PortablePaths::filePath("logs", "app.log");
    EXPECT_EQ(path, QDir(tmpDir.path()).filePath("logs/app.log"));
    EXPECT_TRUE(QFileInfo(QDir(tmpDir.path()).filePath("logs")).isDir());
}

TEST(PortablePathsTest, TwoIndependentOverriddenRootsDoNotShareSubdirs) {
    QTemporaryDir tmpDirA;
    QTemporaryDir tmpDirB;
    ASSERT_TRUE(tmpDirA.isValid());
    ASSERT_TRUE(tmpDirB.isValid());

    QString pathInA;
    {
        ScopedPortableRoot scopedA(tmpDirA.path());
        pathInA = killcore::PortablePaths::filePath("data/profiles", "fixture.keprofile");
        QFile file(pathInA);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write("test");
    }

    {
        ScopedPortableRoot scopedB(tmpDirB.path());
        const QString pathInB = killcore::PortablePaths::filePath("data/profiles", "fixture.keprofile");
        EXPECT_NE(pathInA, pathInB);
        EXPECT_FALSE(QFileInfo(pathInB).exists());
    }
}
