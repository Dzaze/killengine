#include "portable_paths.h"

#include <QCoreApplication>
#include <QDir>

namespace killcore {

namespace {

QString& testRootOverrideStorage() {
    static QString value;
    return value;
}

} // namespace

QString PortablePaths::root() {
    const QString& override = testRootOverrideStorage();
    if (!override.isEmpty()) {
        return override;
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

} // namespace killcore
