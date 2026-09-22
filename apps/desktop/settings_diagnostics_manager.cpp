#include "settings_diagnostics_manager.h"

#include "application_controller.h"
#include "crash_handler.h"
#include "localization/localization.h"
#include "logging/logger.h"
#include "model_locator.h"
#include "paths/portable_paths.h"
#include "settings/settings_persistence.h"
#include "scan_state_access.h"
#include "scanner/performance_profile.h"
#include "scanner/scan_types.h"
#include "snapshot/snapshot_store.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcessEnvironment>
#include <QSettings>
#include <QSet>
#include <QStandardPaths>
#include <QUrl>
#include <QtGlobal>

#include <algorithm>

namespace killengine {

namespace {

constexpr int kDefaultScanMaxResults = 1000000;
constexpr int kDefaultScanChunkSizeMb = 0;
constexpr int kDefaultScanMaxWorkerThreads = 0;
constexpr int kDefaultScanMaxInFlightMb = 0;
constexpr int kDefaultCandidateFileThreshold = 250000;
constexpr int kDefaultUnknownSnapshotMaxMb = 128;

int boundedSettingInt(QSettings& settings, const QString& key, int fallback, int minimum, int maximum) {
    bool ok = false;
    const int value = settings.value(key, fallback).toInt(&ok);
    if (!ok) {
        return fallback;
    }
    return std::clamp(value, minimum, maximum);
}

int unknownSnapshotMaxMbFromSettings() {
    QSettings settings;
    const int mb = boundedSettingInt(settings, "scan/unknownSnapshotMaxMb", kDefaultUnknownSnapshotMaxMb, -1, 32768);
    return mb == -1 ? -1 : std::max(mb, kDefaultUnknownSnapshotMaxMb);
}

size_t candidateFileBackedThresholdFromSettings() {
    QSettings settings;
    return static_cast<size_t>(boundedSettingInt(
        settings,
        "scan/candidateFileBackedThreshold",
        kDefaultCandidateFileThreshold,
        1,
        5000000));
}

QStringList killengineTemporaryFileNames() {
    QDir dir(QDir::tempPath());
    return dir.entryList(
        QStringList{
            "killengine_candidates_*.kecand",
            "killengine_snapshot_*.kesnap",
        },
        QDir::Files);
}

QVariantMap scanKillengineTemporaryFiles() {
    QDir dir(QDir::tempPath());
    QVariantList files;
    qulonglong bytes = 0;

    for (const auto& name : killengineTemporaryFileNames()) {
        const QFileInfo info(dir.absoluteFilePath(name));
        if (!info.exists() || !info.isFile()) {
            continue;
        }

        QVariantMap file;
        file["name"] = info.fileName();
        file["path"] = info.absoluteFilePath();
        file["bytes"] = static_cast<qulonglong>(std::max<qint64>(0, info.size()));
        file["lastModified"] = info.lastModified().toString(Qt::ISODate);
        files.append(file);
        bytes += file.value("bytes").toULongLong();
    }

    QVariantMap result;
    result["count"] = files.size();
    result["bytes"] = bytes;
    result["files"] = files;
    result["tempPath"] = dir.absolutePath();
    return result;
}

} // namespace

SettingsDiagnosticsManager::SettingsDiagnosticsManager(ApplicationController& controller)
    : m_controller(controller) {
}
QVariantMap SettingsDiagnosticsManager::getTemporaryStorageStatus() const {
    const auto orphan = scanKillengineTemporaryFiles();
    auto state = m_controller.scanState();
    const auto& candidates = state.candidates();
    const auto& previousCandidates = state.previousCandidates();
    const auto& snapshot = state.snapshot();
    const qulonglong candidateBytes = static_cast<qulonglong>(candidates.storageBytes());
    const qulonglong undoBytes = m_controller.m_hasPreviousCandidates
        ? static_cast<qulonglong>(previousCandidates.storageBytes())
        : 0;
    const qulonglong snapshotBytes = static_cast<qulonglong>(snapshot.compressedBytesCaptured());
    const bool hasCandidateFile = candidates.isFileBacked();
    const bool hasUndoFile = m_controller.m_hasPreviousCandidates && previousCandidates.isFileBacked();
    const bool hasSnapshotFile = snapshot.usesMappedStorage();
    const qulonglong activeBytes = candidateBytes + undoBytes + snapshotBytes;

    QVariantMap result;
    result["success"] = true;
    result["tempPath"] = orphan.value("tempPath");
    result["activeBytes"] = activeBytes;
    result["activeFileCount"] = static_cast<int>(hasCandidateFile) + static_cast<int>(hasUndoFile) + static_cast<int>(hasSnapshotFile);
    result["candidateBytes"] = candidateBytes;
    result["candidateFileBacked"] = hasCandidateFile;
    result["undoBytes"] = undoBytes;
    result["undoFileBacked"] = hasUndoFile;
    result["snapshotBytes"] = snapshotBytes;
    result["snapshotFileBacked"] = hasSnapshotFile;
    result["orphanBytes"] = orphan.value("bytes").toULongLong();
    result["orphanFileCount"] = orphan.value("count").toInt();
    result["orphanFiles"] = orphan.value("files").toList();
    result["totalBytes"] = activeBytes + result.value("orphanBytes").toULongLong();
    return result;
}

QVariantMap SettingsDiagnosticsManager::clearTemporaryStorage() {
    QVariantMap result;
    if (m_controller.m_activeScanCancellation) {
        result["success"] = false;
        result["error"] = KE_TXT("Un scan est actif : annule ou attends la fin avant de nettoyer le temporaire.",
            "A scan is active: cancel it or wait for it to finish before clearing the temporary storage.");
        return result;
    }

    const auto before = getTemporaryStorageStatus();
    auto state = m_controller.scanState();
    const qulonglong clearedCandidates = static_cast<qulonglong>(state.candidates().size());
    const bool hadUndo = m_controller.m_hasPreviousCandidates;
    const bool hadSnapshot = !state.snapshot().isEmpty();

    state.clearCandidates();
    m_controller.clearCandidateUndo();
    m_controller.clearCandidateValueHistory();
    state.clearSnapshot();
    m_controller.m_smartSearchActive = false;
    m_controller.m_smartSearchInitialValue.clear();
    m_controller.m_smartSearchTargetValue.clear();

    QDir dir(QDir::tempPath());
    QVariantList removedFiles;
    QVariantList failedFiles;
    qulonglong removedBytes = 0;
    for (const auto& name : killengineTemporaryFileNames()) {
        const QString path = dir.absoluteFilePath(name);
        const QFileInfo info(path);
        const qulonglong bytes = static_cast<qulonglong>(std::max<qint64>(0, info.size()));
        if (QFile::remove(path)) {
            QVariantMap file;
            file["path"] = path;
            file["bytes"] = bytes;
            removedFiles.append(file);
            removedBytes += bytes;
        } else if (info.exists()) {
            failedFiles.append(path);
        }
    }

    result["success"] = failedFiles.isEmpty();
    result["tempPath"] = dir.absolutePath();
    result["beforeBytes"] = before.value("totalBytes").toULongLong();
    result["closedActiveBytes"] = before.value("activeBytes").toULongLong();
    result["removedBytes"] = removedBytes;
    result["removedFileCount"] = removedFiles.size();
    result["removedFiles"] = removedFiles;
    result["failedFiles"] = failedFiles;
    result["clearedCandidates"] = clearedCandidates;
    result["hadUndoReduction"] = hadUndo;
    result["hadUnknownSnapshot"] = hadSnapshot;
    result["message"] = failedFiles.isEmpty()
        ? KE_TXT("Stockage temporaire nettoyé : %1 fichier(s), %2 octet(s) supprimé(s).",
              "Temporary storage cleared: %1 file(s), %2 byte(s) removed.")
              .arg(removedFiles.size())
              .arg(removedBytes)
        : KE_TXT("Nettoyage partiel : %1 fichier(s) supprimé(s), %2 fichier(s) verrouillé(s).",
              "Partial cleanup: %1 file(s) removed, %2 file(s) locked.")
              .arg(removedFiles.size())
              .arg(failedFiles.size());
    this->appendSmartSearchDebug("temporary_storage_cleared", result);
    return result;
}


QVariantMap SettingsDiagnosticsManager::getSettings() const {
    QSettings settings;
    QVariantMap result;
    result["language"] = settings.value("ui/language", "fr").toString();
    result["defaultValueType"] = settings.value("scan/defaultValueType", "Int32").toString();
    result["scanMaxResults"] = boundedSettingInt(
        settings, "scan/maxResults", kDefaultScanMaxResults, 1000, 10000000);
    result["scanChunkSizeMb"] = boundedSettingInt(
        settings, "scan/chunkSizeMb", kDefaultScanChunkSizeMb, 0, 64);
    result["performanceMode"] = settings.value("scan/performanceMode", "Auto").toString();
    result["scanMaxWorkerThreads"] = boundedSettingInt(
        settings, "scan/maxWorkerThreads", kDefaultScanMaxWorkerThreads, 0, 128);
    result["scanMaxInFlightMb"] = boundedSettingInt(
        settings, "scan/maxInFlightMb", kDefaultScanMaxInFlightMb, 0, 32768);
    result["candidateFileBackedThreshold"] = boundedSettingInt(
        settings, "scan/candidateFileBackedThreshold", kDefaultCandidateFileThreshold, 1, 5000000);
    result["unknownSnapshotMaxMb"] = unknownSnapshotMaxMbFromSettings();
    result["fastScan"] = settings.value("scan/fastScan", true).toBool();
    result["smartSearchDebugEnabled"] = settings.value("diagnostics/smartSearchDebugEnabled", true).toBool();
    result["smartSearchDebugMaxEvents"] = boundedSettingInt(
        settings, "diagnostics/smartSearchDebugMaxEvents", 30, 5, 200);
    result["modelPath"] = settings.value("ai/modelPath", "").toString();
    result["modelEnabled"] = settings.value("ai/modelEnabled", true).toBool();
    result["modelThreads"] = boundedSettingInt(settings, "ai/modelThreads", 4, 1, 32);
    // PHASE 271 : Mode discret automatique
    result["stealthAutoEnable"] = settings.value("stealth/autoEnable", false).toBool();
    result["stealthDefaultProfile"] = settings.value("stealth/defaultProfile", "default").toString();
    return result;
}


QVariantMap SettingsDiagnosticsManager::getAiModelStatus() const {
    QSettings settings;
    QVariantMap result;
    QVariantList modelCandidates;
    QVariantList executableCandidates;
    QVariantList embeddedAgents;
    QVariantList embeddedModelFolders;

    // PORT-4 (docs/PORTABILITY_ROADMAP.md, 18/09/2026) : la valeur stockée
    // peut désormais être une référence portable relative -- résolue ici pour
    // comparaison/affichage, jamais contre le répertoire courant du process.
    const QString configuredModelRaw = settings.value("ai/modelPath", "").toString().trimmed();
    const QString configuredModel = killai::ModelLocator::resolveModelReference(configuredModelRaw);
    const bool modelEnabled = settings.value("ai/modelEnabled", true).toBool();
    // UX-PIPE-2 (docs/PHASE_TRACKER.md, 18/09/2026) : desactiveForSession()
    // (bouton "Continuer sans IA locale") n'est jamais persiste -- distinct
    // de modelEnabled (reglage persistant) ET de la presence reelle des
    // fichiers (available ci-dessous). Les trois etats ne doivent jamais se
    // recouvrir dans un seul champ.
    const bool sessionDisabled = m_controller.m_ai.isSessionDisabled();
    const QString envModel = QProcessEnvironment::systemEnvironment().value("KILLENGINE_QWEN_GGUF").trimmed();
    const QString envExe = QProcessEnvironment::systemEnvironment().value("KILLENGINE_LLAMA_CLI").trimmed();
    const auto model = killai::ModelLocator::findQwenGguf();
    // Une référence configurée devenue introuvable ne doit jamais être
    // présentée comme "le choix inchangé" -- le modèle réellement utilisé
    // (s'il y en a un) est rendu visible séparément via configuredModelWarning.
    const bool configuredModelMissing = !configuredModelRaw.isEmpty()
        && !(QFileInfo(configuredModel).exists() && QFileInfo(configuredModel).isFile());

    for (const auto& path : killai::ModelLocator::candidateModelPaths()) {
        QFileInfo file(path);
        QVariantMap item;
        item["path"] = path;
        item["exists"] = file.exists() && file.isFile();
        item["isGguf"] = file.suffix().compare("gguf", Qt::CaseInsensitive) == 0;
        item["source"] = path == configuredModel
            ? QString("settings")
            : (path == envModel ? QString("KILLENGINE_QWEN_GGUF") : QString("candidate"));
        if (file.exists()) {
            item["absolutePath"] = file.absoluteFilePath();
            item["sizeBytes"] = static_cast<qlonglong>(file.size());
        }
        modelCandidates.append(item);
    }

    QString executablePath;
    const QDir appDir(QCoreApplication::applicationDirPath());
    const QStringList exeCandidates = {
        envExe,
        appDir.filePath("llama-cli.exe"),
        appDir.filePath("llama.cpp/llama-cli.exe"),
        appDir.filePath("../../third_party/llama.cpp/llama-cli.exe"),
        appDir.filePath("../../third_party/llama.cpp/build/bin/Release/llama-cli.exe"),
        appDir.filePath("../../third_party/llama.cpp/build/bin/llama-cli.exe"),
        QDir::current().filePath("third_party/llama.cpp/llama-cli.exe"),
        QDir::current().filePath("third_party/llama.cpp/build/bin/Release/llama-cli.exe"),
        QDir::current().filePath("third_party/llama.cpp/build/bin/llama-cli.exe"),
    };
    QSet<QString> seenExeCandidates;
    for (const auto& candidate : exeCandidates) {
        const QString trimmed = candidate.trimmed();
        if (trimmed.isEmpty() || seenExeCandidates.contains(trimmed)) {
            continue;
        }
        seenExeCandidates.insert(trimmed);
        QFileInfo file(trimmed);
        const bool exists = file.exists() && file.isFile();
        if (executablePath.isEmpty() && exists) {
            executablePath = file.absoluteFilePath();
        }
        QVariantMap item;
        item["path"] = trimmed;
        item["exists"] = exists;
        item["source"] = trimmed == envExe ? QString("KILLENGINE_LLAMA_CLI") : QString("candidate");
        if (exists) {
            item["absolutePath"] = file.absoluteFilePath();
            item["sizeBytes"] = static_cast<qlonglong>(file.size());
        }
        executableCandidates.append(item);
    }

    const QStringList modelRootCandidates = {
        appDir.filePath("model"),
        appDir.filePath("../../model"),
        QDir::current().filePath("model"),
    };
    QSet<QString> seenModelRoots;
    for (const auto& rootCandidate : modelRootCandidates) {
        QFileInfo rootInfo(rootCandidate);
        if (!rootInfo.exists() || !rootInfo.isDir()) {
            continue;
        }
        const QString rootPath = rootInfo.canonicalFilePath().isEmpty()
            ? rootInfo.absoluteFilePath()
            : rootInfo.canonicalFilePath();
        if (seenModelRoots.contains(rootPath)) {
            continue;
        }
        seenModelRoots.insert(rootPath);

        const QDir rootDir(rootPath);
        const auto subdirs = rootDir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
        for (const auto& subdirInfo : subdirs) {
            const QDir subdir(subdirInfo.absoluteFilePath());
            const auto ggufFiles = subdir.entryInfoList(QStringList{"*.gguf"}, QDir::Files, QDir::Name);

            QVariantMap modelFolder;
            modelFolder["id"] = subdirInfo.fileName();
            modelFolder["path"] = subdirInfo.absoluteFilePath();
            modelFolder["hasGguf"] = !ggufFiles.isEmpty();
            modelFolder["ggufCount"] = ggufFiles.size();
            if (!ggufFiles.isEmpty()) {
                modelFolder["primaryModelPath"] = ggufFiles.first().absoluteFilePath();
                modelFolder["primaryModelSizeBytes"] = static_cast<qlonglong>(ggufFiles.first().size());
            }
            embeddedModelFolders.append(modelFolder);

            const QFileInfo manifestInfo(subdir.filePath("MODEL_MANIFEST.json"));
            if (!manifestInfo.exists() || !manifestInfo.isFile()) {
                continue;
            }

            QVariantMap agent;
            agent["id"] = subdirInfo.fileName();
            agent["displayName"] = subdirInfo.fileName();
            agent["role"] = QString("agent");
            agent["provider"] = QString("llama.cpp");
            agent["manifestPath"] = manifestInfo.absoluteFilePath();
            agent["valid"] = false;
            agent["modelFound"] = false;

            QFile manifestFile(manifestInfo.absoluteFilePath());
            if (!manifestFile.open(QIODevice::ReadOnly)) {
                agent["error"] = KE_TXT("Manifest illisible.", "Manifest unreadable.");
                embeddedAgents.append(agent);
                continue;
            }

            const QJsonDocument doc = QJsonDocument::fromJson(manifestFile.readAll());
            if (!doc.isObject()) {
                agent["error"] = KE_TXT("Manifest JSON invalide.", "Invalid manifest JSON.");
                embeddedAgents.append(agent);
                continue;
            }

            const QJsonObject object = doc.object();
            agent["valid"] = true;
            agent["id"] = object.value("id").toString(subdirInfo.fileName());
            agent["displayName"] = object.value("displayName").toString(agent.value("id").toString());
            agent["role"] = object.value("role").toString("agent");
            agent["provider"] = object.value("provider").toString("llama.cpp");
            agent["required"] = object.value("required").toBool(true);

            const QString manifestModelPath = object.value("modelPath").toString().trimmed();
            const QFileInfo manifestModelInfo(manifestModelPath.isEmpty()
                ? QString()
                : subdir.filePath(manifestModelPath));
            agent["modelPath"] = manifestModelInfo.absoluteFilePath();
            agent["modelFound"] = manifestModelInfo.exists() && manifestModelInfo.isFile();
            if (manifestModelInfo.exists()) {
                agent["modelSizeBytes"] = static_cast<qlonglong>(manifestModelInfo.size());
            }
            embeddedAgents.append(agent);
        }
    }

    result["success"] = true;
    result["enabled"] = modelEnabled;
    result["sessionDisabled"] = sessionDisabled;
    result["backend"] = modelEnabled && !sessionDisabled && model.found && !executablePath.isEmpty() ? QString("llama.cpp") : QString("deterministic");
    result["ready"] = modelEnabled && !sessionDisabled && model.found && !executablePath.isEmpty();
    // available = fichiers presents, INDEPENDAMMENT de l'activation courante
    // (reglage persistant ou session) -- c'est le seul champ qui doit piloter
    // "installe ?" dans le catalogue Modules (voir getModuleCatalog, piege 1).
    result["available"] = model.found && !executablePath.isEmpty();
    result["configuredModelPath"] = configuredModel;
    result["configuredModelPathRaw"] = configuredModelRaw;
    result["configuredModelMissing"] = configuredModelMissing;
    if (configuredModelMissing) {
        result["configuredModelWarning"] = model.found
            ? KE_TXT("Le modèle configuré est introuvable ; un autre modèle est utilisé à la place : %1",
                     "The configured model could not be found; another model is used instead: %1").arg(model.path)
            : KE_TXT("Le modèle configuré est introuvable et aucun autre modèle n'a été trouvé.",
                     "The configured model could not be found and no other model was found.");
    }
    result["envModelPath"] = envModel;
    result["envExecutablePath"] = envExe;
    result["modelFound"] = model.found;
    result["modelPath"] = model.path;
    result["modelSource"] = model.source;
    result["modelError"] = model.errorMessage;
    result["executableFound"] = !executablePath.isEmpty();
    result["executablePath"] = executablePath;
    result["modelCandidates"] = modelCandidates;
    result["executableCandidates"] = executableCandidates;
    result["embeddedAgents"] = embeddedAgents;
    result["embeddedAgentCount"] = embeddedAgents.size();
    result["embeddedModelFolders"] = embeddedModelFolders;
    result["threads"] = boundedSettingInt(settings, "ai/modelThreads", 4, 1, 32);
    // UX-PIPE-2 : message hiérarchisé -- "prêt" / "désactivée" ne doivent
    // jamais être confondus avec "fichiers manquants", sinon l'UI suggère à
    // tort un téléchargement/réinstallation alors que le modèle est là mais
    // simplement pas autorisé à tourner (session ou réglage persistant).
    const bool available = result.value("available").toBool();
    if (result.value("ready").toBool()) {
        result["message"] = KE_TXT("IA embarquée prête.", "Embedded AI ready.");
    } else if (sessionDisabled && available) {
        result["message"] = KE_TXT("IA locale désactivée pour cette session (fichiers installés, redémarre KillEngine pour la réactiver).",
                                    "Local AI disabled for this session (files installed, restart KillEngine to re-enable it).");
    } else if (!modelEnabled && available) {
        result["message"] = KE_TXT("IA locale désactivée dans les réglages (fichiers installés).",
                                    "Local AI disabled in settings (files installed).");
    } else {
        result["message"] = KE_TXT("IA embarquée indisponible : modèle ou runtime manquant.", "Embedded AI unavailable: model or runtime missing.");
    }
    return result;
}

QVariantMap SettingsDiagnosticsManager::browseForModelFile() {
    QVariantMap result;
    result["success"] = false;

    const QString path = QFileDialog::getOpenFileName(
        nullptr,
        KE_TXT("Choisir un modèle GGUF", "Choose a GGUF model"),
        QString(),
        KE_TXT("Modèles GGUF (*.gguf);;Tous les fichiers (*.*)", "GGUF models (*.gguf);;All files (*.*)"));

    if (path.isEmpty()) {
        result["cancelled"] = true;
        return result;
    }

    result["success"] = true;
    result["path"] = path;
    return result;
}

QVariantMap SettingsDiagnosticsManager::saveSettings(const QVariantMap& incoming) {
    QSettings settings;

    const QString language = incoming.value("language", "fr").toString() == "en" ? "en" : "fr";
    const QString valueType = incoming.value("defaultValueType", "Int32").toString();
    killcore::ValueType parsedType;

    settings.setValue("ui/language", language);
    settings.setValue(
        "scan/defaultValueType",
        killcore::parseValueType(valueType, &parsedType) ? valueType : "Int32");
    settings.setValue(
        "scan/maxResults",
        std::clamp(incoming.value("scanMaxResults", kDefaultScanMaxResults).toInt(), 1000, 10000000));
    settings.setValue(
        "scan/chunkSizeMb",
        std::clamp(incoming.value("scanChunkSizeMb", kDefaultScanChunkSizeMb).toInt(), 0, 64));
    const QByteArray performanceModeName = incoming.value("performanceMode", "Auto").toString().toLatin1();
    settings.setValue(
        "scan/performanceMode",
        killcore::performanceModeToString(killcore::parsePerformanceMode(
            performanceModeName.constData(),
            killcore::PerformanceMode::Auto)));
    settings.setValue(
        "scan/maxWorkerThreads",
        std::clamp(incoming.value("scanMaxWorkerThreads", kDefaultScanMaxWorkerThreads).toInt(), 0, 128));
    settings.setValue(
        "scan/maxInFlightMb",
        std::clamp(incoming.value("scanMaxInFlightMb", kDefaultScanMaxInFlightMb).toInt(), 0, 32768));
    settings.setValue(
        "scan/candidateFileBackedThreshold",
        std::clamp(incoming.value("candidateFileBackedThreshold", kDefaultCandidateFileThreshold).toInt(), 1, 5000000));
    const int unknownSnapshotMaxMb = incoming.value("unknownSnapshotMaxMb", kDefaultUnknownSnapshotMaxMb).toInt();
    settings.setValue(
        "scan/unknownSnapshotMaxMb",
        unknownSnapshotMaxMb == -1 ? -1 : std::clamp(unknownSnapshotMaxMb, 128, 32768));
    settings.setValue("scan/fastScan", incoming.value("fastScan", true).toBool());
    settings.setValue(
        "diagnostics/smartSearchDebugEnabled",
        incoming.value("smartSearchDebugEnabled", true).toBool());
    settings.setValue(
        "diagnostics/smartSearchDebugMaxEvents",
        std::clamp(incoming.value("smartSearchDebugMaxEvents", 30).toInt(), 5, 200));
    // PORT-4 (docs/PORTABILITY_ROADMAP.md, 18/09/2026) : un chemin absolu dont
    // l'appartenance au paquet est démontrée est converti en référence
    // portable (relative à killcore::PortablePaths::root()) avant stockage --
    // sinon il restait figé sur l'ancien emplacement après une copie/déplacement
    // du dossier. Un chemin externe (hors paquet) n'est jamais deviné comme
    // appartenant au paquet : reste absolu tel quel.
    settings.setValue("ai/modelPath",
        killai::ModelLocator::toPortableModelReference(incoming.value("modelPath", "").toString()));
    settings.setValue("ai/modelEnabled", incoming.value("modelEnabled", true).toBool());
    settings.setValue(
        "ai/modelThreads",
        std::clamp(incoming.value("modelThreads", 4).toInt(), 1, 32));
    // PHASE 271 : Mode discret automatique
    settings.setValue("stealth/autoEnable", incoming.value("stealthAutoEnable", false).toBool());
    const QString stealthProfile = incoming.value("stealthDefaultProfile", "default").toString();
    settings.setValue("stealth/defaultProfile",
        (stealthProfile == "sc2" || stealthProfile == "minimal") ? stealthProfile : "default");

    // PORT-3a : le changement ci-dessus est déjà actif en mémoire dans ce
    // QSettings (et scanState() ci-dessous l'applique réellement) que la
    // synchronisation sur disque réussisse ou non -- seul le champ "success"
    // (persistance réelle) doit refléter un échec, jamais l'état en mémoire.
    QString syncError;
    const bool persisted = killcore::commitSettingsSync(settings, &syncError);
    m_controller.scanState().setFileBackedThreshold(candidateFileBackedThresholdFromSettings());

    QVariantMap result = this->getSettings();
    result["success"] = persisted;
    if (!persisted) {
        result["error"] = syncError;
    }
    return result;
}

QVariantMap SettingsDiagnosticsManager::setUiLanguage(const QString& language) {
    const QString normalized = language == "en" ? "en" : "fr";
    QSettings settings;
    settings.setValue("ui/language", normalized);

    QString syncError;
    const bool persisted = killcore::commitSettingsSync(settings, &syncError);

    QVariantMap result;
    result["success"] = persisted;
    result["language"] = normalized;
    if (!persisted) {
        result["error"] = syncError;
    }
    return result;
}

QString SettingsDiagnosticsManager::getLogFilePath() const {
    return killcore::Logger::instance().logFilePath();
}

QString SettingsDiagnosticsManager::getSmartSearchDebugFilePath() const {
    return this->smartSearchDebugFilePath();
}

QString SettingsDiagnosticsManager::getScanTelemetryFilePath() const {
    return this->scanTelemetryFilePath();
}

QVariantMap SettingsDiagnosticsManager::getSmartSearchDebugEvents(int maxEvents) const {
    QVariantMap result;
    QVariantList events;
    result["success"] = false;
    result["path"] = this->smartSearchDebugFilePath();
    result["events"] = events;

    QSettings settings;
    if (maxEvents <= 0) {
        maxEvents = boundedSettingInt(settings, "diagnostics/smartSearchDebugMaxEvents", 30, 5, 200);
    }
    maxEvents = std::clamp(maxEvents, 1, 200);

    QFile file(this->smartSearchDebugFilePath());
    if (!file.exists()) {
        result["success"] = true;
        result["error"] = "";
        return result;
    }

    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        result["error"] = KE_TXT("Impossible de lire le fichier debug Smart Search.", "Unable to read the Smart Search debug file.");
        return result;
    }

    constexpr qint64 kMaxTailBytes = 1024 * 1024;
    if (file.size() > kMaxTailBytes) {
        file.seek(file.size() - kMaxTailBytes);
        file.readLine();
    }

    QList<QByteArray> lines;
    while (!file.atEnd()) {
        const QByteArray line = file.readLine().trimmed();
        if (line.isEmpty()) {
            continue;
        }
        lines.append(line);
        if (lines.size() > maxEvents) {
            lines.removeFirst();
        }
    }

    for (const auto& line : lines) {
        QJsonParseError parseError;
        const QJsonDocument doc = QJsonDocument::fromJson(line, &parseError);
        if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
            QVariantMap parseEntry;
            parseEntry["event"] = "parse_error";
            parseEntry["raw"] = QString::fromUtf8(line);
            parseEntry["error"] = parseError.errorString();
            events.append(parseEntry);
            continue;
        }
        events.append(doc.object().toVariantMap());
    }

    result["success"] = true;
    result["error"] = "";
    result["events"] = events;
    return result;
}

QVariantMap SettingsDiagnosticsManager::clearSmartSearchDebugEvents() {
    QVariantMap result;
    result["success"] = false;
    result["path"] = this->smartSearchDebugFilePath();

    QFile file(this->smartSearchDebugFilePath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        result["error"] = KE_TXT("Impossible de vider le fichier debug Smart Search.", "Unable to clear the Smart Search debug file.");
        return result;
    }

    result["success"] = true;
    result["error"] = "";
    return result;
}

QVariantMap SettingsDiagnosticsManager::getLogTail(int maxLines) const {
    QVariantMap result;
    QVariantList lines;
    result["success"] = false;
    result["path"] = this->getLogFilePath();
    result["lines"] = lines;

    maxLines = std::clamp(maxLines, 1, 1000);

    QFile file(this->getLogFilePath());
    if (!file.exists()) {
        result["success"] = true;
        result["error"] = "";
        return result;
    }
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        result["error"] = KE_TXT("Impossible de lire le fichier log.", "Unable to read the log file.");
        return result;
    }

    constexpr qint64 kMaxTailBytes = 2 * 1024 * 1024;
    if (file.size() > kMaxTailBytes) {
        file.seek(file.size() - kMaxTailBytes);
        file.readLine();
    }

    QList<QByteArray> tail;
    while (!file.atEnd()) {
        const QByteArray line = file.readLine().trimmed();
        if (line.isEmpty()) {
            continue;
        }
        tail.append(line);
        if (tail.size() > maxLines) {
            tail.removeFirst();
        }
    }

    for (const auto& line : tail) {
        lines.append(QString::fromUtf8(line));
    }

    result["success"] = true;
    result["error"] = "";
    result["lines"] = lines;
    return result;
}

QVariantMap SettingsDiagnosticsManager::exportDiagnostics() {
    QVariantMap result;
    result["success"] = false;

    const QString exportDir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation).isEmpty()
        ? QDir::currentPath()
        : QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    const QString timestamp = QDateTime::currentDateTime().toString("yyyy-MM-dd_HH-mm-ss");
    const QString exportPath = QDir(exportDir).filePath("KillEngine-diagnostics-" + timestamp + ".kezdiag");

    QVariantMap manifest;
    manifest["createdAt"] = QDateTime::currentDateTime().toString(Qt::ISODateWithMs);
    manifest["version"] = m_controller.getVersion();
    manifest["pid"] = m_controller.m_pid;
    manifest["processName"] = m_controller.m_processName;
    manifest["attached"] = m_controller.m_attached;
    const auto& candidateStore = m_controller.scanState().candidates();
    manifest["candidateCount"] = static_cast<qulonglong>(candidateStore.size());
    manifest["candidateStoreFileBacked"] = candidateStore.isFileBacked();
    manifest["candidateStorePath"] = candidateStore.backingFilePath();
    manifest["candidateStoreBytes"] = static_cast<qulonglong>(candidateStore.storageBytes());
    manifest["candidateStoreMemoryBytes"] = static_cast<qulonglong>(candidateStore.estimatedMemoryBytes());
    manifest["logFilePath"] = this->getLogFilePath();
    manifest["smartSearchDebugFilePath"] = this->smartSearchDebugFilePath();
    manifest["scanTelemetryFilePath"] = this->scanTelemetryFilePath();
    manifest["crashDirectory"] = CrashHandler::crashDirectory();
    manifest["settings"] = this->getSettings();

    QDir crashDir(CrashHandler::crashDirectory());
    const auto crashFiles = crashDir.entryInfoList(QStringList{"*.crash.txt"}, QDir::Files, QDir::Time);
    // Les .dmp (minidump binaire, WinDbg/Visual Studio) ne rentrent pas dans
    // ce bundle texte compressé comme les .crash.txt embarqués plus bas —
    // juste listés dans le manifeste pour que la personne qui traite le
    // diagnostic sache qu'ils existent et où les récupérer séparément
    // (chaque .crash.txt référence aussi son .dmp pairé via "minidump=").
    const auto dumpFiles = crashDir.entryInfoList(QStringList{"*.dmp"}, QDir::Files, QDir::Time);
    QStringList recentDumpPaths;
    for (qsizetype i = 0; i < std::min<qsizetype>(dumpFiles.size(), 5); ++i) {
        recentDumpPaths.append(dumpFiles.at(i).absoluteFilePath());
    }
    manifest["recentMinidumps"] = recentDumpPaths;

    QByteArray payload;
    auto appendSection = [&payload](const QString& name, const QByteArray& data) {
        payload.append("\n===== ");
        payload.append(name.toUtf8());
        payload.append(" =====\n");
        payload.append(data);
        if (!payload.endsWith('\n')) {
            payload.append('\n');
        }
    };

    appendSection("manifest.json", QJsonDocument(QJsonObject::fromVariantMap(manifest)).toJson(QJsonDocument::Indented));

    QFile logFile(this->getLogFilePath());
    if (logFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        appendSection(QFileInfo(logFile).fileName(), logFile.readAll());
    }

    QFile debugFile(this->smartSearchDebugFilePath());
    if (debugFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        appendSection(QFileInfo(debugFile).fileName(), debugFile.readAll());
    }

    QFile scanTelemetryFile(this->scanTelemetryFilePath());
    if (scanTelemetryFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        appendSection(QFileInfo(scanTelemetryFile).fileName(), scanTelemetryFile.readAll());
    }

    const qsizetype crashFileCount = std::min<qsizetype>(crashFiles.size(), 5);
    for (qsizetype i = 0; i < crashFileCount; ++i) {
        QFile crashFile(crashFiles.at(i).absoluteFilePath());
        if (crashFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
            appendSection("crashes/" + crashFiles.at(i).fileName(), crashFile.readAll());
        }
    }

    QFile out(exportPath);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        result["error"] = KE_TXT("Impossible de créer le fichier diagnostic.", "Unable to create the diagnostic file.");
        result["path"] = exportPath;
        return result;
    }
    out.write(qCompress(payload, 9));
    out.close();

    result["success"] = true;
    result["path"] = exportPath;
    result["bytesWritten"] = static_cast<qulonglong>(QFileInfo(exportPath).size());
    result["error"] = "";

    // Ouvre l'explorateur Windows sur le dossier contenant l'export.
    // L'échec de l'ouverture ne doit pas invalider l'export.
    const QString folderPath = QFileInfo(exportPath).absolutePath();
    const bool folderOpened = QDesktopServices::openUrl(QUrl::fromLocalFile(folderPath));
    result["folderOpened"] = folderOpened;
    if (!folderOpened) {
        result["openFolderError"] = KE_TXT("Le dossier de l'export n'a pas pu être ouvert automatiquement. Chemin : %1",
            "The export folder could not be opened automatically. Path: %1").arg(folderPath);
    } else {
        result["openFolderError"] = "";
    }

    return result;
}

QString SettingsDiagnosticsManager::smartSearchDebugFilePath() const {
    // PORT-2b (docs/PORTABILITY_ROADMAP.md, 17/09/2026) : AppLocalDataLocation
    // (%LOCALAPPDATA%) cassait le mode portable -- copier le dossier de l'app
    // perdait silencieusement ce journal. Redirige vers le meme dossier "logs"
    // deja portable (killcore::Logger, voir main.cpp) au lieu d'un dossier a
    // part, via le resolveur commun killcore::PortablePaths.
    return killcore::PortablePaths::filePath("logs", "smart_search_debug.jsonl");
}

QString SettingsDiagnosticsManager::scanTelemetryFilePath() const {
    // PORT-2b : meme raisonnement que smartSearchDebugFilePath() ci-dessus.
    return killcore::PortablePaths::filePath("logs", "scan_telemetry.jsonl");
}

void SettingsDiagnosticsManager::appendSmartSearchDebug(const QString& event, const QVariantMap& payload) const {
    QSettings settings;
    if (!settings.value("diagnostics/smartSearchDebugEnabled", true).toBool()) {
        return;
    }

    QVariantMap entry = payload;
    entry["event"] = event;
    entry["timestamp"] = QDateTime::currentDateTime().toString(Qt::ISODateWithMs);
    entry["pid"] = m_controller.m_pid;
    entry["processName"] = m_controller.m_processName;

    const QString path = this->smartSearchDebugFilePath();
    QFileInfo debugInfo(path);
    if (debugInfo.exists() && debugInfo.size() > 8 * 1024 * 1024) {
        QFile::remove(path + ".old");
        QFile::rename(path, path + ".old");
    }

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        KE_LOG_WARN() << "Unable to open Smart Search debug file: "
                      << path.toStdString();
        return;
    }

    file.write(QJsonDocument(QJsonObject::fromVariantMap(entry)).toJson(QJsonDocument::Compact));
    file.write("\n");
}

void SettingsDiagnosticsManager::appendScanTelemetry(const QString& event, const QVariantMap& payload) const {
    QSettings settings;

    QVariantMap entry = payload;
    entry["event"] = event;
    entry["timestamp"] = QDateTime::currentDateTime().toString(Qt::ISODateWithMs);
    entry["pid"] = m_controller.m_pid;
    entry["processName"] = m_controller.m_processName;
    entry["performanceMode"] = settings.value("scan/performanceMode", "Auto").toString();
    entry["scanChunkSizeMb"] = boundedSettingInt(settings, "scan/chunkSizeMb", kDefaultScanChunkSizeMb, 0, 64);
    entry["scanMaxWorkerThreads"] = boundedSettingInt(settings, "scan/maxWorkerThreads", kDefaultScanMaxWorkerThreads, 0, 128);
    entry["scanMaxInFlightMb"] = boundedSettingInt(settings, "scan/maxInFlightMb", kDefaultScanMaxInFlightMb, 0, 32768);
    entry["scanMaxResults"] = boundedSettingInt(settings, "scan/maxResults", kDefaultScanMaxResults, 1000, 10000000);
    entry["fastScan"] = settings.value("scan/fastScan", true).toBool();

    const QString path = this->scanTelemetryFilePath();
    QFileInfo telemetryInfo(path);
    if (telemetryInfo.exists() && telemetryInfo.size() > 16 * 1024 * 1024) {
        QFile::remove(path + ".old");
        QFile::rename(path, path + ".old");
    }

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        KE_LOG_WARN() << "Unable to open scan telemetry file: "
                      << path.toStdString();
        return;
    }

    file.write(QJsonDocument(QJsonObject::fromVariantMap(entry)).toJson(QJsonDocument::Compact));
    file.write("\n");
}


} // namespace killengine
