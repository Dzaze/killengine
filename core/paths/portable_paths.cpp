#include "portable_paths.h"

#include <QCoreApplication>
#include <QDir>

namespace killcore {

namespace {

QString& testRootOverrideStorage() {
    static QString value;
    return value;
}

// UX-PRODUIT-15 : storage séparé de testRootOverrideStorage() -- volontairement
// distinct pour ne jamais mélanger un override de test (fixture GTest,
// réinitialisé souvent dans un même process) avec un override de production
// (posé une seule fois pour toute la durée de vie d'un processus enfant
// tutoriel).
QString& productionRootOverrideStorage() {
    static QString value;
    return value;
}

} // namespace

QString PortablePaths::root() {
    const QString& testOverride = testRootOverrideStorage();
    if (!testOverride.isEmpty()) {
        return testOverride;
    }
    const QString& productionOverride = productionRootOverrideStorage();
    if (!productionOverride.isEmpty()) {
        return productionOverride;
    }
    return QCoreApplication::applicationDirPath();
}

QString PortablePaths::ensureSubdir(const QString& relativeSubdir) {
    const QString dir = QDir(root()).filePath(relativeSubdir);
    QDir().mkpath(dir);
    return dir;
}

QString PortablePaths::filePath(const QString& relativeSubdir, const QString& fileName) {
    return QDir(ensureSubdir(relativeSubdir)).filePath(fileName);
}

void PortablePaths::setTestRootOverride(const QString& root) {
    testRootOverrideStorage() = root;
}

void PortablePaths::setProductionRootOverride(const QString& root) {
    productionRootOverrideStorage() = root;
}

} // namespace killcore
