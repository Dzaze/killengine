// PORT-4 (docs/PORTABILITY_ROADMAP.md, 18/09/2026) : un chemin absolu choisi
// par l'utilisateur (sélecteur de fichier) devait rester stocké tel quel --
// après copie/déplacement du paquet, il continuait de pointer vers l'ancien
// emplacement au lieu de suivre le paquet. Ce fichier teste la conversion
// vers/depuis une référence portable relative à killcore::PortablePaths::root().

#include "model_locator.h"
#include "paths/portable_paths.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QTemporaryDir>

using killai::ModelLocator;
using killcore::PortablePaths;

namespace {

// Même pattern que ScopedUiLanguage (tests/unit/test_localization.cpp).
class ScopedPortableRoot {
public:
    explicit ScopedPortableRoot(const QString& root) {
        PortablePaths::setTestRootOverride(root);
    }
    ~ScopedPortableRoot() {
        PortablePaths::setTestRootOverride(QString());
    }
};

} // namespace

TEST(ModelLocatorPortableReferenceTest, ToPortableReferenceReturnsEmptyForEmptyInput) {
    EXPECT_TRUE(ModelLocator::toPortableModelReference("").isEmpty());
    EXPECT_TRUE(ModelLocator::toPortableModelReference("   ").isEmpty());
}

TEST(ModelLocatorPortableReferenceTest, ToPortableReferenceLeavesAlreadyRelativePathUnchanged) {
    EXPECT_EQ(ModelLocator::toPortableModelReference("model/qwen/x.gguf"), "model/qwen/x.gguf");
}

TEST(ModelLocatorPortableReferenceTest, ToPortableReferenceConvertsPathUnderRootToRelative) {
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    ScopedPortableRoot scoped(root.path());

    const QString absolute = QDir(root.path()).filePath("model/qwen/Qwen_test.gguf");
    const QString reference = ModelLocator::toPortableModelReference(absolute);

    EXPECT_EQ(reference, "model/qwen/Qwen_test.gguf");
}

TEST(ModelLocatorPortableReferenceTest, ToPortableReferenceLeavesExternalAbsolutePathUnchanged) {
    QTemporaryDir root;
    QTemporaryDir elsewhere;
    ASSERT_TRUE(root.isValid());
    ASSERT_TRUE(elsewhere.isValid());
    ScopedPortableRoot scoped(root.path());

    const QString externalAbsolute = QDir(elsewhere.path()).filePath("MyModels/custom.gguf");
    EXPECT_EQ(ModelLocator::toPortableModelReference(externalAbsolute), externalAbsolute);
}

TEST(ModelLocatorPortableReferenceTest, ToPortableReferenceDoesNotMatchSimilarlyNamedSiblingRoot) {
    // Piège de comparaison par préfixe textuel naïf : "root" ne doit jamais
    // matcher "rootFoo" juste parce que la chaîne commence pareil.
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    ScopedPortableRoot scoped(root.path());

    const QString siblingWithSimilarPrefix = root.path() + "Foo/model/x.gguf";
    EXPECT_EQ(ModelLocator::toPortableModelReference(siblingWithSimilarPrefix), siblingWithSimilarPrefix);
}

TEST(ModelLocatorPortableReferenceTest, ResolveReferenceReturnsEmptyForEmptyInput) {
    EXPECT_TRUE(ModelLocator::resolveModelReference("").isEmpty());
}

TEST(ModelLocatorPortableReferenceTest, ResolveReferenceLeavesAbsolutePathUnchanged) {
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    ScopedPortableRoot scoped(root.path());

    const QString absolute = QDir(root.path()).filePath("external.gguf");
    EXPECT_EQ(ModelLocator::resolveModelReference(absolute), absolute);
}

TEST(ModelLocatorPortableReferenceTest, ResolveReferenceResolvesRelativeAgainstPortableRootNotCurrentPath) {
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    ScopedPortableRoot scoped(root.path());

    const QString resolved = ModelLocator::resolveModelReference("model/qwen/x.gguf");
    EXPECT_EQ(resolved, QDir::cleanPath(QDir(root.path()).filePath("model/qwen/x.gguf")));
}

TEST(ModelLocatorPortableReferenceTest, HandlesWindowsPathsWithSpacesAndAccents) {
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    // Sous-dossier avec espace + caractères accentués -- reproduit
    // "MES APPS DEV" / noms de dossiers utilisateur francophones réels.
    const QString accentedRoot = QDir(root.path()).filePath(QString::fromUtf8("Mes Jeux Préférés"));
    ASSERT_TRUE(QDir().mkpath(accentedRoot));
    ScopedPortableRoot scoped(accentedRoot);

    const QString absolute = QDir(accentedRoot).filePath(QString::fromUtf8("modèle/qwen/Qwen_test.gguf"));
    const QString reference = ModelLocator::toPortableModelReference(absolute);
    EXPECT_EQ(reference, QString::fromUtf8("modèle/qwen/Qwen_test.gguf"));

    const QString resolved = ModelLocator::resolveModelReference(reference);
    EXPECT_EQ(resolved, QDir::cleanPath(absolute));
}

TEST(ModelLocatorPortableReferenceTest, RoundTripThroughDifferentRootFollowsThePackageMove) {
    // Le scenario reel de PORT-4 : copier/deplacer le paquet vers un nouvel
    // emplacement (voire un autre lecteur) doit faire suivre la reference
    // portable, jamais rester figee sur l'ancien chemin absolu.
    QTemporaryDir rootA;
    QTemporaryDir rootB;
    ASSERT_TRUE(rootA.isValid());
    ASSERT_TRUE(rootB.isValid());

    QString reference;
    {
        ScopedPortableRoot scopedA(rootA.path());
        const QString absoluteInA = QDir(rootA.path()).filePath("model/qwen/Qwen_test.gguf");
        reference = ModelLocator::toPortableModelReference(absoluteInA);
        ASSERT_EQ(reference, "model/qwen/Qwen_test.gguf");
    }

    {
        ScopedPortableRoot scopedB(rootB.path());
        const QString resolvedInB = ModelLocator::resolveModelReference(reference);
        EXPECT_EQ(resolvedInB, QDir::cleanPath(QDir(rootB.path()).filePath("model/qwen/Qwen_test.gguf")));
    }
}
