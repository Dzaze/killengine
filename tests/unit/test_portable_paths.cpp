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

// UX-PRODUIT-15 : même patron, pour l'override de production (storage
// statique séparé de l'override de test, voir portable_paths.cpp).
class ScopedProductionPortableRoot {
public:
    explicit ScopedProductionPortableRoot(const QString& root) {
        killcore::PortablePaths::setProductionRootOverride(root);
    }
    ~ScopedProductionPortableRoot() {
        killcore::PortablePaths::setProductionRootOverride(QString());
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

// --- UX-PRODUIT-15 : override de production, storage séparé de l'override de test ---

TEST(PortablePathsTest, ProductionOverrideChangesRootAndClearingRestoresDefault) {
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());

    {
        ScopedProductionPortableRoot scoped(tmpDir.path());
        EXPECT_EQ(killcore::PortablePaths::root(), tmpDir.path());
    }

    EXPECT_EQ(killcore::PortablePaths::root(), QCoreApplication::applicationDirPath());
}

TEST(PortablePathsTest, TestOverrideTakesPrecedenceOverProductionOverride) {
    QTemporaryDir testDir;
    QTemporaryDir prodDir;
    ASSERT_TRUE(testDir.isValid());
    ASSERT_TRUE(prodDir.isValid());

    ScopedProductionPortableRoot scopedProd(prodDir.path());
    ScopedPortableRoot scopedTest(testDir.path());

    // L'override de test (fixture GTest) ne doit jamais être masqué par un
    // override de production qui traînerait -- comportement des tests
    // existants garanti inchangé même si les deux sont actifs simultanément
    // (ne devrait jamais arriver en usage réel, mais l'ordre de vérification
    // dans root() doit rester déterministe).
    EXPECT_EQ(killcore::PortablePaths::root(), testDir.path());
}

TEST(PortablePathsTest, ProductionOverrideAppliesWhenNoTestOverrideActive) {
    QTemporaryDir prodDir;
    ASSERT_TRUE(prodDir.isValid());
    ScopedProductionPortableRoot scopedProd(prodDir.path());

    EXPECT_EQ(killcore::PortablePaths::root(), prodDir.path());

    const QString dir = killcore::PortablePaths::ensureSubdir("data/tutorial-sessions/fake-uuid");
    EXPECT_EQ(dir, QDir(prodDir.path()).filePath("data/tutorial-sessions/fake-uuid"));
    EXPECT_TRUE(QFileInfo(dir).isDir());
}

TEST(PortablePathsTest, ProductionAndTestOverrideStoragesAreIndependent) {
    QTemporaryDir prodDir;
    ASSERT_TRUE(prodDir.isValid());

    // Poser puis effacer l'override de PRODUCTION ne doit pas affecter
    // l'override de TEST -- deux slots de storage réellement distincts, pas
    // un seul partagé sous deux noms de méthode.
    killcore::PortablePaths::setProductionRootOverride(prodDir.path());
    killcore::PortablePaths::setProductionRootOverride(QString());
    EXPECT_EQ(killcore::PortablePaths::root(), QCoreApplication::applicationDirPath());

    QTemporaryDir testDir;
    ASSERT_TRUE(testDir.isValid());
    ScopedPortableRoot scopedTest(testDir.path());
    EXPECT_EQ(killcore::PortablePaths::root(), testDir.path());
}
