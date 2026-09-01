#include "save_file_investigator.h"

#include "logging/logger.h"
#include "memory/memory_reader.h"
#include "process/file_watch.h"
#include "process/package_storage.h"

#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QMetaObject>
#include <QPointer>
#include <QSet>
#include <QVariantList>

#include <algorithm>
#include <thread>
#include <utility>

namespace killengine {
namespace {
bool isPathUnderPackagesRoot(const QString& path) {
    const QString normalized = QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath()).toLower();
    const QString allowedRoot = QDir::toNativeSeparators(
        QDir(qEnvironmentVariable("LOCALAPPDATA")).filePath("Packages")).toLower();
    return !allowedRoot.isEmpty() && normalized.startsWith(allowedRoot);
}
} // namespace

SaveFileInvestigator::SaveFileInvestigator(
    const killcore::ProcessHandle& handle,
    NextRequestIdCallback nextRequestId,
    ResultCallback saveFileWatchFinished,
    QObject* parent)
    : QObject(parent)
    , m_handle(handle)
    , m_nextRequestId(std::move(nextRequestId))
    , m_saveFileWatchFinished(std::move(saveFileWatchFinished)) {}

QVariantMap SaveFileInvestigator::discoverProcessSaveFiles(int maxResults) const {
    QVariantMap result;
    result["success"] = false;
    QVariantList filesList;
    result["files"] = filesList;

    if (!m_handle.isValid()) {
        result["error"] = "Aucun processus attaché.";
        return result;
    }

    QString familyName;
    QString error;
    if (!killcore::resolvePackageFamilyName(m_handle, &familyName, &error)) {
        result["error"] = error.isEmpty() ? "Résolution du package échouée." : error;
        return result;
    }
    result["familyName"] = familyName;

    QVector<killcore::PackageSaveFileEntry> files;
    if (!killcore::listPackageSaveFiles(familyName, maxResults, /*excludeNoise=*/true, &files, &error)) {
        result["error"] = error.isEmpty() ? "Listage des fichiers échoué." : error;
        return result;
    }

    for (const auto& entry : files) {
        QVariantMap fileMap;
        fileMap["path"] = entry.path;
        fileMap["sizeBytes"] = entry.sizeBytes;
        fileMap["lastWriteTime"] = entry.lastWriteTimeIso;
        filesList.append(fileMap);
    }

    result["success"] = true;
    result["files"] = filesList;
    result["count"] = filesList.size();
    result["error"] = "";
    return result;
}

QVariantMap SaveFileInvestigator::compareSaveFileSnapshots(const QVariantList& before, const QVariantList& after) const {
    // Comparaison pure (pas d'acces disque ici) : les deux snapshots viennent
    // de deux appels a discoverProcessSaveFiles (avant/apres une action
    // utilisateur), voir docs/UWP_STATE_INSPECTOR_SPEC.md. La detection de
    // changement se fait sur (taille, date de derniere ecriture) plutot qu'un
    // hash de contenu : suffisant pour reperer QUEL fichier a change sans
    // relire chaque fichier, et coherent avec les entrees deja retournees par
    // listPackageSaveFiles (path/sizeBytes/lastWriteTime, pas de hash).
    QVariantMap result;
    result["success"] = true;

    QHash<QString, QVariantMap> beforeByPath;
    for (const QVariant& entry : before) {
        const QVariantMap map = entry.toMap();
        const QString path = map.value("path").toString();
        if (!path.isEmpty()) {
            beforeByPath.insert(path, map);
        }
    }

    QSet<QString> seenAfterPaths;
    QVariantList added;
    QVariantList modified;
    int unchangedCount = 0;

    for (const QVariant& entry : after) {
        const QVariantMap afterMap = entry.toMap();
        const QString path = afterMap.value("path").toString();
        if (path.isEmpty()) {
            continue;
        }
        seenAfterPaths.insert(path);

        const auto it = beforeByPath.constFind(path);
        if (it == beforeByPath.constEnd()) {
            added.append(afterMap);
            continue;
        }

        const QVariantMap& beforeMap = it.value();
        const bool sizeChanged = beforeMap.value("sizeBytes").toLongLong() != afterMap.value("sizeBytes").toLongLong();
        const bool timeChanged = beforeMap.value("lastWriteTime").toString() != afterMap.value("lastWriteTime").toString();
        if (sizeChanged || timeChanged) {
            QVariantMap diffEntry;
            diffEntry["path"] = path;
            diffEntry["sizeBytesBefore"] = beforeMap.value("sizeBytes");
            diffEntry["sizeBytesAfter"] = afterMap.value("sizeBytes");
            diffEntry["sizeDeltaBytes"] = afterMap.value("sizeBytes").toLongLong() - beforeMap.value("sizeBytes").toLongLong();
            diffEntry["lastWriteTimeBefore"] = beforeMap.value("lastWriteTime");
            diffEntry["lastWriteTimeAfter"] = afterMap.value("lastWriteTime");
            modified.append(diffEntry);
        } else {
            ++unchangedCount;
        }
    }

    QVariantList removed;
    for (auto it = beforeByPath.constBegin(); it != beforeByPath.constEnd(); ++it) {
        if (!seenAfterPaths.contains(it.key())) {
            removed.append(it.value());
        }
    }

    result["added"] = added;
    result["removed"] = removed;
    result["modified"] = modified;
    result["addedCount"] = added.size();
    result["removedCount"] = removed.size();
    result["modifiedCount"] = modified.size();
    result["unchangedCount"] = unchangedCount;
    result["error"] = "";
    return result;
}

QVariantMap SaveFileInvestigator::inspectProcessLocalSettings(int maxValues) const {
    QVariantMap result;
    result["success"] = false;
    QVariantList valuesList;
    result["values"] = valuesList;

    if (!m_handle.isValid()) {
        result["error"] = "Aucun processus attaché.";
        return result;
    }

    QString familyName;
    QString error;
    if (!killcore::resolvePackageFamilyName(m_handle, &familyName, &error)) {
        result["error"] = error.isEmpty() ? "Résolution du package échouée." : error;
        return result;
    }
    result["familyName"] = familyName;

    QVector<killcore::PackageLocalSettingsEntry> values;
    QString settingsPath;
    if (!killcore::inspectPackageLocalSettings(familyName, maxValues, &values, &settingsPath, &error)) {
        result["settingsPath"] = settingsPath;
        result["error"] = error.isEmpty() ? "Inspection LocalSettings échouée." : error;
        return result;
    }

    for (const auto& entry : values) {
        QVariantMap valueMap;
        valueMap["keyPath"] = entry.keyPath;
        valueMap["name"] = entry.name;
        valueMap["type"] = entry.type;
        valueMap["preview"] = entry.preview;
        valueMap["dataSizeBytes"] = entry.dataSizeBytes;
        valuesList.append(valueMap);
    }

    result["success"] = true;
    result["settingsPath"] = settingsPath;
    result["values"] = valuesList;
    result["count"] = valuesList.size();
    result["error"] = "";
    return result;
}

QVariantMap SaveFileInvestigator::readProcessSaveFileText(const QString& path, int maxBytes) const {
    QVariantMap result;
    result["success"] = false;
    result["path"] = path;

    if (!m_handle.isValid()) {
        result["error"] = "Aucun processus attaché.";
        return result;
    }

    if (!isPathUnderPackagesRoot(path)) {
        result["error"] = "Chemin refusé : doit être sous %LOCALAPPDATA%\\Packages\\ (utilise discoverProcessSaveFiles pour lister les chemins valides).";
        return result;
    }

    QString text;
    bool truncated = false;
    QString error;
    if (!killcore::readPackageSaveFileText(path, maxBytes, &text, &truncated, &error)) {
        result["error"] = error.isEmpty() ? "Lecture du fichier échouée." : error;
        return result;
    }

    result["success"] = true;
    result["text"] = text;
    result["truncated"] = truncated;
    result["error"] = "";
    return result;
}

QVariantMap SaveFileInvestigator::watchSaveFileForChanges(const QString& path, const QVariantMap& options) {
    QVariantMap result;
    result["success"] = false;
    result["path"] = path;

    if (!m_handle.isValid()) {
        result["error"] = "Aucun processus attaché.";
        return result;
    }
    if (!isPathUnderPackagesRoot(path)) {
        result["error"] = "Chemin refusé : doit être sous %LOCALAPPDATA%\\Packages\\ (utilise discoverProcessSaveFiles pour lister les chemins valides).";
        return result;
    }
    if (m_saveFileWatchInProgress) {
        result["error"] = "Une surveillance de fichier est déjà en cours.";
        return result;
    }

    const int timeoutMs = std::clamp(options.value("timeoutMs", 5000).toInt(), 250, 60000);

    m_saveFileWatchInProgress = true;
    auto cancellation = std::make_shared<killcore::CancellationToken>();
    m_activeSaveFileWatchCancellation = cancellation;

    killcore::FileWatchOutcome outcome;
    QString error;
    const bool started = killcore::watchFileForChanges(path, timeoutMs, cancellation.get(), &outcome, &error);

    m_saveFileWatchInProgress = false;
    m_activeSaveFileWatchCancellation.reset();

    if (!started) {
        result["error"] = error.isEmpty() ? "Surveillance du fichier échouée." : error;
        return result;
    }

    result["success"] = true;
    result["changed"] = outcome.changed;
    result["changeType"] = outcome.changeType;
    result["cancelled"] = outcome.cancelled;
    result["error"] = outcome.changed
        ? ""
        : (outcome.cancelled ? "Surveillance annulée." : "Aucun changement détecté avant le timeout.");
    return result;
}

QVariantMap SaveFileInvestigator::startSaveFileWatchAsync(const QString& path, const QVariantMap& options) {
    QVariantMap result;
    result["success"] = false;
    result["started"] = false;
    result["path"] = path;

    if (m_saveFileWatchInProgress) {
        result["error"] = "Une surveillance de fichier est déjà en cours.";
        return result;
    }
    if (!m_handle.isValid()) {
        result["error"] = "Aucun processus attaché.";
        return result;
    }
    if (!isPathUnderPackagesRoot(path)) {
        result["error"] = "Chemin refusé : doit être sous %LOCALAPPDATA%\\Packages\\ (utilise discoverProcessSaveFiles pour lister les chemins valides).";
        return result;
    }

    const int timeoutMs = std::clamp(options.value("timeoutMs", 5000).toInt(), 250, 60000);
    const int requestId = m_nextRequestId();
    const QString requestedPath = path;
    const QPointer<SaveFileInvestigator> self(this);
    auto cancellation = std::make_shared<killcore::CancellationToken>();

    m_saveFileWatchInProgress = true;
    m_activeSaveFileWatchCancellation = cancellation;

    KE_LOG_INFO() << "startSaveFileWatchAsync(path=" << requestedPath.toStdString()
                  << ", timeoutMs=" << timeoutMs
                  << ", requestId=" << requestId << ")";

    std::thread([self, requestId, requestedPath, timeoutMs, cancellation]() {
        killcore::FileWatchOutcome outcome;
        QString error;
        const bool started = killcore::watchFileForChanges(requestedPath, timeoutMs, cancellation.get(), &outcome, &error);

        if (!self) {
            return;
        }

        QMetaObject::invokeMethod(self.data(), [self, requestId, requestedPath, started, outcome, error]() {
            if (!self) {
                return;
            }

            QVariantMap finished;
            finished["requestId"] = requestId;
            finished["kind"] = "save_file_watch";
            finished["success"] = started;
            finished["path"] = requestedPath;
            finished["changed"] = outcome.changed;
            finished["changeType"] = outcome.changeType;
            finished["cancelled"] = outcome.cancelled;
            finished["error"] = !started
                ? (error.isEmpty() ? "Surveillance du fichier échouée." : error)
                : (outcome.changed
                       ? ""
                       : (outcome.cancelled ? "Surveillance annulée." : "Aucun changement détecté avant le timeout."));

            self->m_saveFileWatchInProgress = false;
            self->m_activeSaveFileWatchCancellation.reset();
            self->m_saveFileWatchFinished(finished);
        }, Qt::QueuedConnection);
    }).detach();

    result["success"] = true;
    result["started"] = true;
    result["requestId"] = requestId;
    result["error"] = "";
    return result;
}

QVariantMap SaveFileInvestigator::cancelSaveFileWatch() {
    QVariantMap result;
    result["success"] = false;
    if (!m_saveFileWatchInProgress || !m_activeSaveFileWatchCancellation) {
        result["error"] = "Aucune surveillance de fichier active à annuler.";
        return result;
    }

    m_activeSaveFileWatchCancellation->cancel();
    result["success"] = true;
    result["error"] = "";
    return result;
}

QVariantMap SaveFileInvestigator::patchProcessSaveFileBytes(const QString& path, const QString& findHex, const QString& replaceHex) {
    QVariantMap result;
    result["success"] = false;
    result["path"] = path;
    result["occurrencesFound"] = 0;

    if (!m_handle.isValid()) {
        result["error"] = "Aucun processus attaché.";
        return result;
    }

    QString error;
    int occurrencesFound = 0;
    if (!killcore::patchPackageSaveFileBytes(path, findHex, replaceHex, &error, &occurrencesFound)) {
        result["occurrencesFound"] = occurrencesFound;
        result["error"] = error.isEmpty() ? "Patch du fichier échoué." : error;
        return result;
    }

    result["success"] = true;
    result["occurrencesFound"] = 1;
    result["error"] = "";
    return result;
}

} // namespace killengine
