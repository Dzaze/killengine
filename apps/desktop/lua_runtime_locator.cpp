#include "lua_runtime_locator.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QStringList>

namespace killengine {

QString findLuaExecutable(const QString& overridePath) {
    const QString trimmedOverride = overridePath.trimmed();
    if (!trimmedOverride.isEmpty()) {
        const QFileInfo overrideFile(trimmedOverride);
        if (overrideFile.exists() && overrideFile.isFile()) {
            return overrideFile.absoluteFilePath();
        }
    }

    const QDir appDir(QCoreApplication::applicationDirPath());
    const QStringList executableNames = {
        QStringLiteral("lua.exe"),
        QStringLiteral("lua54.exe"),
        QStringLiteral("lua5.4.exe"),
        QStringLiteral("luajit.exe"),
    };
    const QStringList bundledDirectories = {
        appDir.filePath("runtime/lua"),
        appDir.filePath("lua"),
        appDir.absolutePath(),
        appDir.filePath("../runtime/lua"),
        appDir.filePath("../../runtime/lua"),
        QDir::current().filePath("runtime/lua"),
        QDir::current().filePath("third_party/lua"),
        QDir::current().filePath("third_party/lua/bin"),
        QDir::current().filePath("tools/lua"),
    };
    for (const auto& directory : bundledDirectories) {
        const QDir dir(directory);
        for (const auto& name : executableNames) {
            const QFileInfo file(dir.filePath(name));
            if (file.exists() && file.isFile()) {
                return file.absoluteFilePath();
            }
        }
    }

    for (const auto& name : executableNames) {
        const QString found = QStandardPaths::findExecutable(name);
        if (!found.isEmpty()) {
            return found;
        }
    }

    return {};
}

QString findKillEngineLuaHelper() {
    const QDir appDir(QCoreApplication::applicationDirPath());
    const QStringList candidates = {
        appDir.filePath("scripts/killengine.lua"),
        appDir.filePath("../scripts/killengine.lua"),
        appDir.filePath("../../scripts/killengine.lua"),
        QDir::current().filePath("scripts/killengine.lua"),
    };
    for (const auto& candidate : candidates) {
        const QFileInfo file(candidate);
        if (file.exists() && file.isFile()) {
            return file.absoluteFilePath();
        }
    }
    return {};
}

} // namespace killengine
