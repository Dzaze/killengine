#include "application_controller.h"

#ifdef Q_OS_WIN
#include <windows.h>
#include <shellapi.h>
// objbase.h avant UIAutomation.h : WIN32_LEAN_AND_MEAN (deja actif via
// windows.h ci-dessus) exclut les headers COM/OLE par defaut, or
// UIAutomationCore.h a besoin de la macro "interface" (-> struct) et des
// types COM de base deja definis, sinon ses declarations "interface Ixxx :
// IUnknown" ne parsent pas (erreurs C2146 en cascade).
#include <objbase.h>
#include <UIAutomation.h>
#include <wrl/client.h>
#endif

#include "auto_resolver.h"
#include "automation_pipe_manager.h"
#include "clr_inspector_bridge.h"
#include "code_patch_manager.h"
#include "debug_feature_manager.h"
#include "freeze_hotkey_overlay_manager.h"
#include "investigation_notebook_manager.h"
#include "kernel_driver_manager.h"
#include "profile_manager.h"
#include "save_file_investigator.h"
#include "scanning_core_manager.h"
#include "settings_diagnostics_manager.h"
#include "smart_search_manager.h"
#include "write_freeze_core_manager.h"
#include "query_text_utils.h"
#include "model_locator.h"
#include "display_string_investigator.h"
#include "candidates/candidate_store.h"
#include "crash_handler.h"
#include "debug/hardware_breakpoint.h"
#include "profiles/ghidra_bridge.h"
#include "logging/logger.h"
#include "memory/memory_map.h"
#include "memory/memory_reader.h"
#include "memory/memory_writer.h"
#include "kernel/kernel_driver_bridge.h"
#include "process/export_resolver.h"
#include "process/process_enumerator.h"
#include "process/process_handle.h"
#include "process/process_suspend.h"
#include "pointer/pointer_chain.h"
#include "pointer/pointer_scanner.h"
#include "patch/aob_scanner.h"
#include "patch/code_patch.h"
#include "patch/instruction_patch_suggester.h"
#include "patch/profile_patch_state.h"
#include "scanner/scan_engine.h"
#include "scanner/scan_types.h"
#include "scanner/display_value_tracker.h"
#include "scanner/display_source_classifier.h"
#include "scanner/memory_window_search.h"
#include "scanner/encrypted_scan.h"
#include "scanner/structure_analyzer.h"
#include "scanner/value_variants.h"
#include "snapshot/snapshot_store.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDesktopServices>
#include <QEventLoop>
#include <QFileDialog>
#include <QUrl>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QMetaObject>
#include <QPointer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QWebEnginePage>
#include <QSettings>
#include <QSet>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <QTimer>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <iphlpapi.h>
#endif

namespace killengine {

namespace {

constexpr size_t kAutoWriteCandidateLimit = 4;
constexpr int kDefaultScanMaxResults = 1000000;
constexpr int kDefaultScanChunkSizeMb = 0;
constexpr int kDefaultScanMaxWorkerThreads = 0;
constexpr int kDefaultScanMaxInFlightMb = 0;
constexpr int kDefaultCandidateFileThreshold = 250000;
constexpr int kDefaultUnknownSnapshotMaxMb = 128;
constexpr size_t kCandidateDisplayLimit = 250000;
constexpr qsizetype kUnknownAutoMaxReturnedMatches = 250000;
constexpr int kCandidateHistoryMaxAddresses = 10000;
constexpr int kCandidateHistoryMaxEntriesPerAddress = 12;

QString findLuaExecutable(const QString& overridePath = {}) {
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

/// Résultat interne partagé par executeLuaScript (bloquant) et
/// executeLuaScriptAsync (thread worker) -- une seule implémentation du
/// lancement de lua.exe pour éviter que les deux versions divergent.
struct LuaScriptRunOutcome {
    bool    started{false};
    bool    success{false};
    bool    timedOut{false};
    bool    cancelled{false};
    int     exitCode{-1};
    QString luaPath;
    QString helperPath;
    QString stdoutText;
    QString stderrText;
    QString error;
};

/// Lance scriptText dans lua.exe/luajit.exe et attend sa fin, en pollant
/// cancellation (si fourni) pour pouvoir kill() le process en cours de route
/// -- utilisé par executeLuaScriptAsync depuis son propre thread, où kill()
/// reste appelé depuis le même thread qui possède le QProcess (pas de
/// manipulation cross-thread).
LuaScriptRunOutcome runLuaScriptProcess(const QString& scriptText, const QVariantMap& options, killcore::CancellationToken* cancellation) {
    LuaScriptRunOutcome outcome;

    const QString trimmedScript = scriptText.trimmed();
    if (trimmedScript.isEmpty()) {
        outcome.error = "Script Lua vide.";
        return outcome;
    }

    const QString luaPath = findLuaExecutable(options.value("luaPath").toString());
    outcome.luaPath = luaPath;
    if (luaPath.isEmpty()) {
        outcome.error = "Aucun interpréteur Lua trouvé. Place lua.exe dans runtime\\lua à côté de KillEngine.exe, ajoute Lua au PATH, ou renseigne options.luaPath.";
        return outcome;
    }

    QTemporaryFile scriptFile(QDir::temp().filePath("killengine-lua-XXXXXX.lua"));
    scriptFile.setAutoRemove(true);
    if (!scriptFile.open()) {
        outcome.error = QStringLiteral("Impossible de créer le script temporaire Lua : %1").arg(scriptFile.errorString());
        return outcome;
    }
    scriptFile.write(scriptText.toUtf8());
    scriptFile.flush();
    const QString scriptPath = scriptFile.fileName();
    scriptFile.close();

    const QString helperPath = findKillEngineLuaHelper();
    outcome.helperPath = helperPath;
    const QFileInfo helperFile(helperPath);
    const QString helperDir = helperFile.exists() ? helperFile.absolutePath() : QString();
    const QString rootDir = helperDir.isEmpty()
        ? QDir(QCoreApplication::applicationDirPath()).absolutePath()
        : QDir(helperDir).absoluteFilePath("..");

    QProcess process;
    process.setProgram(luaPath);
    process.setArguments({scriptPath});
    process.setWorkingDirectory(rootDir);
    process.setProcessChannelMode(QProcess::SeparateChannels);

    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("KILLENGINE_ROOT"), QDir(rootDir).absolutePath());
    env.insert(QStringLiteral("KILLENGINE_AUTOMATION_PIPE_NAME"), options.value("pipeName", QStringLiteral("KillEngineAutomationPipe")).toString());
    if (!helperDir.isEmpty()) {
        const QString existingLuaPath = env.value(QStringLiteral("LUA_PATH"));
        const QString helperPattern = QDir(helperDir).filePath("?.lua").replace('\\', '/');
        env.insert(QStringLiteral("LUA_PATH"), helperPattern + QStringLiteral(";") + existingLuaPath);
    }
    process.setProcessEnvironment(env);

    const int timeoutMs = std::clamp(options.value("timeoutMs", 10000).toInt(), 1000, 120000);
    process.start();
    if (!process.waitForStarted(3000)) {
        outcome.error = QStringLiteral("Impossible de démarrer Lua : %1").arg(process.errorString());
        return outcome;
    }
    outcome.started = true;

    QElapsedTimer timer;
    timer.start();
    bool finished = false;
    while (true) {
        finished = process.waitForFinished(100);
        if (finished) {
            break;
        }
        if (cancellation && cancellation->isCancelled()) {
            outcome.cancelled = true;
            break;
        }
        if (timer.hasExpired(timeoutMs)) {
            break;
        }
    }
    if (!finished) {
        process.kill();
        process.waitForFinished(2000);
    }
    outcome.timedOut = !finished && !outcome.cancelled;

    outcome.stdoutText = QString::fromUtf8(process.readAllStandardOutput());
    outcome.stderrText = QString::fromUtf8(process.readAllStandardError());
    outcome.exitCode = process.exitCode();
    outcome.success = finished && process.exitStatus() == QProcess::NormalExit && outcome.exitCode == 0;
    if (!outcome.success) {
        outcome.error = outcome.cancelled
            ? QStringLiteral("Script Lua annulé.")
            : outcome.timedOut
            ? QStringLiteral("Script Lua interrompu après timeout (%1 ms).").arg(timeoutMs)
            : QStringLiteral("Script Lua terminé avec le code %1.").arg(outcome.exitCode);
    }
    return outcome;
}

double ratePerSecond(size_t count, qint64 elapsedMs) {
    if (elapsedMs <= 0) {
        return 0.0;
    }
    return static_cast<double>(count) * 1000.0 / static_cast<double>(elapsedMs);
}

#ifdef Q_OS_WIN
QString windowsErrorMessage(DWORD errorCode) {
    LPWSTR raw = nullptr;
    const DWORD size = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                         FORMAT_MESSAGE_IGNORE_INSERTS,
                                     nullptr,
                                     errorCode,
                                     MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                                     reinterpret_cast<LPWSTR>(&raw),
                                     0,
                                     nullptr);
    QString message = size > 0 && raw ? QString::fromWCharArray(raw).trimmed() : QStringLiteral("Erreur Windows inconnue");
    if (raw) {
        LocalFree(raw);
    }
    return message;
}
#endif

void emitQueuedScanProgress(const QPointer<ApplicationController>& self, int percent) {
    const int clamped = std::clamp(percent, 0, 100);
    if (!self) {
        return;
    }
    QMetaObject::invokeMethod(self.data(), [self, clamped]() {
        if (!self) {
            return;
        }
        emit self->scanProgress(clamped);
    }, Qt::QueuedConnection);
}

enum class SmartSearchIntentKind {
    Unknown,
    ResetContext,
    ExactScan,
    GuidedScan,
    RefineScan,
    ActivateMemoryTargets,
    WriteMemoryTargets,
    FreezeMemoryTargets,
    RewriteLastTargets,
    WriteProfileTargets,
    ClearActiveTargets,
    ReportBadTargets,
    ReportGoodTargets,
    AnswerTraceUiStringPrompt,
    AnswerTraceUiFilterPrompt,
    AnswerWriteTargetPrompt,
};

struct SmartSearchIntent {
    SmartSearchIntentKind kind{SmartSearchIntentKind::Unknown};
    QStringList numbers;
    QStringList addresses;
    bool resetContext{false};
    QString rationale;
};

QString smartSearchIntentKindToString(SmartSearchIntentKind kind) {
    switch (kind) {
        case SmartSearchIntentKind::Unknown:
            return "Unknown";
        case SmartSearchIntentKind::ResetContext:
            return "ResetContext";
        case SmartSearchIntentKind::ExactScan:
            return "ExactScan";
        case SmartSearchIntentKind::GuidedScan:
            return "GuidedScan";
        case SmartSearchIntentKind::RefineScan:
            return "RefineScan";
        case SmartSearchIntentKind::ActivateMemoryTargets:
            return "ActivateMemoryTargets";
        case SmartSearchIntentKind::WriteMemoryTargets:
            return "WriteMemoryTargets";
        case SmartSearchIntentKind::FreezeMemoryTargets:
            return "FreezeMemoryTargets";
        case SmartSearchIntentKind::RewriteLastTargets:
            return "RewriteLastTargets";
        case SmartSearchIntentKind::WriteProfileTargets:
            return "WriteProfileTargets";
        case SmartSearchIntentKind::ClearActiveTargets:
            return "ClearActiveTargets";
        case SmartSearchIntentKind::ReportBadTargets:
            return "ReportBadTargets";
        case SmartSearchIntentKind::ReportGoodTargets:
            return "ReportGoodTargets";
        case SmartSearchIntentKind::AnswerTraceUiStringPrompt:
            return "AnswerTraceUiStringPrompt";
        case SmartSearchIntentKind::AnswerTraceUiFilterPrompt:
            return "AnswerTraceUiFilterPrompt";
        case SmartSearchIntentKind::AnswerWriteTargetPrompt:
            return "AnswerWriteTargetPrompt";
    }
    return "Unknown";
}

QVariantMap moduleToVariantMap(const killcore::ProcessModuleInfo& module) {
    QVariantMap entry;
    entry["name"] = module.name;
    entry["path"] = module.path;
    entry["baseAddress"] = QString::number(module.baseAddress, 16);
    entry["size"] = static_cast<qulonglong>(module.size);
    return entry;
}

QVariantMap memoryRegionToVariantMap(const killcore::MemoryRegion& region) {
    QVariantMap entry;
    entry["baseAddress"] = QString::number(region.baseAddress, 16);
    entry["allocationBase"] = QString::number(region.allocationBase, 16);
    entry["size"] = static_cast<qulonglong>(region.size);
    entry["protection"] = killcore::protectionToString(region.protection);
    entry["state"] = killcore::memoryStateToString(region.state);
    entry["type"] = killcore::memoryTypeToString(region.type);
    entry["readable"] = region.readable;
    entry["writable"] = region.writable;
    entry["executable"] = region.executable;
    entry["guarded"] = region.guarded;
    return entry;
}

QVariantMap memoryStatsToVariantMap(const killcore::MemoryMapStats& stats) {
    QVariantMap entry;
    entry["regionCount"] = stats.regionCount;
    entry["committedCount"] = stats.committedCount;
    entry["readableCount"] = stats.readableCount;
    entry["writableCount"] = stats.writableCount;
    entry["executableCount"] = stats.executableCount;
    entry["totalBytes"] = static_cast<qulonglong>(stats.totalBytes);
    entry["committedBytes"] = static_cast<qulonglong>(stats.committedBytes);
    entry["readableBytes"] = static_cast<qulonglong>(stats.readableBytes);
    entry["writableBytes"] = static_cast<qulonglong>(stats.writableBytes);
    entry["executableBytes"] = static_cast<qulonglong>(stats.executableBytes);
    return entry;
}

double bytesToDouble(const QByteArray& bytes, killcore::ValueType type);
QString bytesToHex(const QByteArray& bytes);

QVariantMap candidateToVariantMap(const killcore::Candidate& candidate) {
    QVariantMap entry;
    entry["address"] = QString::number(candidate.address, 16);
    entry["type"] = killcore::valueTypeToString(candidate.type);
    entry["lastValueHex"] = bytesToHex(candidate.lastValue);
    entry["lastValueNumber"] = bytesToDouble(candidate.lastValue, candidate.type);
    entry["confidence"] = candidate.confidence;
    if (!candidate.variantLabel.isEmpty()) {
        entry["variantLabel"] = candidate.variantLabel;
    }
    return entry;
}

QVariantMap autoResolveStepToVariantMap(const killai::AutoResolveStep& step, int index) {
    QVariantMap entry;
    entry["index"] = index;
    entry["type"] = killai::stepTypeToString(step.type);
    entry["description"] = step.description;
    entry["params"] = step.params;
    entry["completed"] = step.completed;
    entry["success"] = step.success;
    entry["result"] = step.result;
    return entry;
}

QVariantList autoResolveStepsToVariantList(const QList<killai::AutoResolveStep>& steps) {
    QVariantList list;
    for (int i = 0; i < steps.size(); ++i) {
        list.append(autoResolveStepToVariantMap(steps[i], i + 1));
    }
    return list;
}

QVariantMap aobPatternQualityToVariantMap(const killcore::AobPatternQuality& quality) {
    QVariantMap item;
    item["score"] = quality.score;
    item["level"] = quality.level;
    item["warning"] = quality.warning;
    item["patternBytes"] = quality.patternBytes;
    item["fixedBytes"] = quality.fixedBytes;
    item["wildcardBytes"] = quality.wildcardBytes;
    item["uniqueFixedBytes"] = quality.uniqueFixedBytes;
    item["fixedRatio"] = quality.fixedRatio;
    item["trainerSafe"] = quality.trainerSafe;
    return item;
}

QString autoResolverGameKey(QString processName) {
    processName = processName.trimmed().toLower();
    if (processName.isEmpty()) {
        return "unknown";
    }
    processName.replace(QRegularExpression("[^a-z0-9_.-]+"), "_");
    return processName.left(80);
}

bool parseHexAddress(const QString& addressHex, uint64_t* address) {
    if (!address) {
        return false;
    }

    QString normalized = addressHex.trimmed();
    if (normalized.startsWith("0x", Qt::CaseInsensitive)) {
        normalized = normalized.mid(2);
    }

    bool ok = false;
    const uint64_t parsed = normalized.toULongLong(&ok, 16);
    if (!ok || parsed == 0) {
        return false;
    }

    *address = parsed;
    return true;
}

// Resout une adresse absolue en (module, offset relatif) — la seule forme qui
// survit a un redemarrage/ASLR. Utilise pour convertir les adresses brutes que
// remonte le pipeline auto-write (valables uniquement pour la session en
// cours) en quelque chose de reutilisable au prochain lancement du meme jeu
// (cf. logAiAudit / rememberedPatterns).
bool resolveModuleOffset(uint32_t pid, uint64_t address, QString* module, uint64_t* offset) {
    if (!module || !offset || address == 0) {
        return false;
    }
    const auto modules = killcore::ProcessEnumerator::enumerateModules(pid);
    QString bestModule;
    uint64_t bestOffset = 0;
    uint64_t bestSize = 0;
    for (const auto& mod : modules) {
        if (address >= mod.baseAddress && address < mod.baseAddress + mod.size) {
            if (bestModule.isEmpty() || mod.size < bestSize) {
                bestModule = mod.name;
                bestOffset = address - mod.baseAddress;
                bestSize = mod.size;
            }
        }
    }
    if (bestModule.isEmpty()) {
        return false;
    }
    *module = bestModule;
    *offset = bestOffset;
    return true;
}

// SHA-256 du binaire attaché, pour détecter qu'un patch/signature AOB
// sauvegardé dans un Profil a été fait sur une version différente de
// l'exécutable — la cause la plus probable quand une signature AOB qui
// marchait avant ne matche plus rien après une mise à jour du jeu (le
// pattern est fait d'octets exacts, une recompilation peut trivialement les
// changer même à comportement identique). Lecture par blocs pour rester
// raisonnable sur un gros binaire plutôt que de tout charger en mémoire d'un coup.
QString computeExecutableHash(const QString& filePath) {
    if (filePath.isEmpty()) {
        return {};
    }
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file)) {
        return {};
    }
    return QString::fromLatin1(hash.result().toHex());
}

int boundedSettingInt(QSettings& settings, const QString& key, int fallback, int minimum, int maximum) {
    bool ok = false;
    const int value = settings.value(key, fallback).toInt(&ok);
    if (!ok) {
        return fallback;
    }
    return std::clamp(value, minimum, maximum);
}

killcore::ScanOptions scanOptionsFromSettings() {
    QSettings settings;
    killcore::ScanOptions options;
    options.maxResults = static_cast<size_t>(
        boundedSettingInt(settings, "scan/maxResults", kDefaultScanMaxResults, 1000, 10000000));
    const QByteArray modeName = settings.value("scan/performanceMode", "Auto").toString().toLatin1();
    options.performanceMode = killcore::parsePerformanceMode(modeName.constData(), killcore::PerformanceMode::Auto);
    const int chunkSizeMb = boundedSettingInt(settings, "scan/chunkSizeMb", kDefaultScanChunkSizeMb, 0, 64);
    options.chunkSize = chunkSizeMb > 0
        ? static_cast<size_t>(chunkSizeMb) * 1024 * 1024
        : 0;
    options.maxWorkerThreads = static_cast<size_t>(
        boundedSettingInt(settings, "scan/maxWorkerThreads", kDefaultScanMaxWorkerThreads, 0, 128));
    const int maxInFlightMb = boundedSettingInt(settings, "scan/maxInFlightMb", kDefaultScanMaxInFlightMb, 0, 32768);
    options.maxInFlightBytes = maxInFlightMb > 0
        ? static_cast<uint64_t>(maxInFlightMb) * 1024ull * 1024ull
        : 0;
    options.fastScan = settings.value("scan/fastScan", true).toBool();
    return options;
}

killcore::ScanOptions scanOptionsFromSettingsAndExpertOptions(const QVariantMap& expertOptions) {
    killcore::ScanOptions options = scanOptionsFromSettings();
    options.startAddress = expertOptions.value("startAddress", 0).toULongLong();
    options.stopAddress = expertOptions.value("stopAddress", 0).toULongLong();
    options.alignment = static_cast<size_t>(std::max(0, expertOptions.value("alignment", 0).toInt()));
    options.writableOnly = expertOptions.value("writableOnly", false).toBool();
    options.executableOnly = expertOptions.value("executableOnly", false).toBool();
    options.copyOnWriteOnly = expertOptions.value("copyOnWriteOnly", false).toBool();
    options.fastScan = expertOptions.value("fastScan", options.fastScan).toBool();
    return options;
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

int unknownSnapshotMaxMbFromSettings() {
    QSettings settings;
    const int raw = settings.value("scan/unknownSnapshotMaxMb", kDefaultUnknownSnapshotMaxMb).toInt();
    if (raw == -1) {
        return -1;
    }
    return std::clamp(raw, 128, 32768);
}

size_t unknownSnapshotMaxBytesFromSettings() {
    const int mb = unknownSnapshotMaxMbFromSettings();
    if (mb == -1) {
        return static_cast<size_t>(kDefaultUnknownSnapshotMaxMb) * 1024 * 1024;
    }
    return static_cast<size_t>(mb) * 1024 * 1024;
}

int requestedUnknownSnapshotMaxMb(const QVariantMap& options) {
    if (options.contains("unknownSnapshotMaxMb")) {
        const int fromOptions = options.value("unknownSnapshotMaxMb", 0).toInt();
        return fromOptions == -1 ? -1 : std::clamp(fromOptions, 128, 32768);
    }
    return unknownSnapshotMaxMbFromSettings();
}

bool isCopyOnWriteProtection(uint32_t protection) {
    const auto flags = killcore::protectionToString(protection).split('|');
    return flags.contains("WC") || flags.contains("XWC");
}

bool regionMatchesScanOptions(const killcore::MemoryRegion& region, const killcore::ScanOptions& options) {
    if (!region.readable || region.guarded || region.size == 0) {
        return false;
    }
    if (options.writableOnly && !region.writable) return false;
    if (options.executableOnly && !region.executable) return false;
    if (options.copyOnWriteOnly && !isCopyOnWriteProtection(region.protection)) return false;

    const uint64_t regionStart = region.baseAddress;
    const uint64_t regionEnd = region.baseAddress + region.size;
    if (options.stopAddress != 0 && regionStart >= options.stopAddress) return false;
    if (options.startAddress != 0 && options.startAddress >= regionEnd) return false;
    return true;
}

QString uiStringAddress(uint64_t address) {
    return QString::number(address, 16).toUpper();
}

const killcore::MemoryRegion* findRegionContaining(
    const QList<killcore::MemoryRegion>& regions,
    uint64_t address) {
    for (const auto& region : regions) {
        if (address >= region.baseAddress && address < region.baseAddress + region.size) {
            return &region;
        }
    }
    return nullptr;
}

uint64_t relevantBytesForUnknownSnapshotOptions(
    const QList<killcore::MemoryRegion>& regions,
    const killcore::ScanOptions& options) {
    uint64_t relevantBytes = 0;
    for (const auto& region : regions) {
        if (!region.readable || region.guarded || region.size == 0) {
            continue;
        }
        if (options.writableOnly && !region.writable) continue;
        if (options.executableOnly && !region.executable) continue;
        if (options.copyOnWriteOnly && !isCopyOnWriteProtection(region.protection)) continue;

        const uint64_t regionStart = region.baseAddress;
        const uint64_t regionEnd = region.baseAddress + region.size;
        if (options.stopAddress != 0 && regionStart >= options.stopAddress) continue;
        if (options.startAddress != 0 && options.startAddress >= regionEnd) continue;

        const uint64_t effectiveStart = std::max(regionStart, options.startAddress);
        const uint64_t effectiveEnd = options.stopAddress == 0 ? regionEnd : std::min(regionEnd, options.stopAddress);
        if (effectiveEnd > effectiveStart) {
            relevantBytes += effectiveEnd - effectiveStart;
        }
    }
    return relevantBytes;
}

// Suggere une profondeur de snapshot (en Mo) adaptee aux filtres de capture.
// Heuristique : couvrir toute la memoire pertinente, arrondie au 128 Mo
// superieur, plafonnee a 4096 Mo pour limiter l'usage disque du snapshot.
int suggestUnknownSnapshotDepthMb(const killcore::ProcessHandle& handle, const killcore::ScanOptions& options, uint64_t* outRelevantBytes = nullptr) {
    if (!handle.isValid()) {
        return 0;
    }
    const auto regions = killcore::MemoryMap::snapshot(handle);
    const uint64_t relevantBytes = relevantBytesForUnknownSnapshotOptions(regions, options);
    if (outRelevantBytes) {
        *outRelevantBytes = relevantBytes;
    }
    if (relevantBytes == 0) {
        return 0;
    }
    constexpr uint64_t kMb = 1024 * 1024;
    const uint64_t mb = (relevantBytes + kMb - 1) / kMb;
    const uint64_t rounded = ((mb + 127) / 128) * 128;
    return static_cast<int>(std::min<uint64_t>(rounded, 4096));
}

// Mode Auto : unknownSnapshotMaxMb == -1 dans les options expert.
// Utilise la suggestion basee sur les filtres actifs de la capture.
size_t resolveUnknownSnapshotMaxBytes(
    const QVariantMap& options,
    const killcore::ProcessHandle& handle,
    const killcore::ScanOptions& scanOptions,
    int* outSuggestedMb = nullptr,
    uint64_t* outRelevantBytes = nullptr) {
    uint64_t relevantBytes = 0;
    const int suggested = suggestUnknownSnapshotDepthMb(handle, scanOptions, &relevantBytes);
    if (outSuggestedMb) *outSuggestedMb = suggested;
    if (outRelevantBytes) *outRelevantBytes = relevantBytes;

    const int requestedMb = requestedUnknownSnapshotMaxMb(options);
    if (requestedMb == -1 && suggested > 0) {
        return static_cast<size_t>(suggested) * 1024 * 1024;
    }
    if (requestedMb > 0) {
        return static_cast<size_t>(requestedMb) * 1024 * 1024;
    }
    return unknownSnapshotMaxBytesFromSettings();
}

double bytesToDouble(const QByteArray& bytes, killcore::ValueType type) {
    if (bytes.size() < static_cast<qsizetype>(killcore::valueTypeSize(type))) {
        return 0.0;
    }

    switch (type) {
        case killcore::ValueType::Int8: {
            int8_t value = 0;
            std::memcpy(&value, bytes.constData(), sizeof(value));
            return static_cast<double>(value);
        }
        case killcore::ValueType::UInt8: {
            uint8_t value = 0;
            std::memcpy(&value, bytes.constData(), sizeof(value));
            return static_cast<double>(value);
        }
        case killcore::ValueType::Int16: {
            int16_t value = 0;
            std::memcpy(&value, bytes.constData(), sizeof(value));
            return static_cast<double>(value);
        }
        case killcore::ValueType::UInt16: {
            uint16_t value = 0;
            std::memcpy(&value, bytes.constData(), sizeof(value));
            return static_cast<double>(value);
        }
        case killcore::ValueType::Int32: {
            int32_t value = 0;
            std::memcpy(&value, bytes.constData(), sizeof(value));
            return static_cast<double>(value);
        }
        case killcore::ValueType::UInt32: {
            uint32_t value = 0;
            std::memcpy(&value, bytes.constData(), sizeof(value));
            return static_cast<double>(value);
        }
        case killcore::ValueType::Int64: {
            int64_t value = 0;
            std::memcpy(&value, bytes.constData(), sizeof(value));
            return static_cast<double>(value);
        }
        case killcore::ValueType::UInt64: {
            uint64_t value = 0;
            std::memcpy(&value, bytes.constData(), sizeof(value));
            return static_cast<double>(value);
        }
        case killcore::ValueType::Float32: {
            float value = 0;
            std::memcpy(&value, bytes.constData(), sizeof(value));
            return static_cast<double>(value);
        }
        case killcore::ValueType::Float64: {
            double value = 0;
            std::memcpy(&value, bytes.constData(), sizeof(value));
            return value;
        }
    }
    return 0.0;
}

bool bytesEqual(const QByteArray& a, const QByteArray& b, killcore::ValueType type) {
    if (type == killcore::ValueType::Float32 || type == killcore::ValueType::Float64) {
        return std::abs(bytesToDouble(a, type) - bytesToDouble(b, type)) < 0.000001;
    }
    return a == b;
}

// Symetrique de bytesToDouble() : sert a construire une valeur test (original +
// delta) pour testCandidateFieldsAsync() sans passer par le pipeline
// parseScanValue/QString habituel des ecritures normales (pas besoin de
// formatage texte ici, seulement d'un aller-retour bytes<->double interne).
QByteArray doubleToBytes(double value, killcore::ValueType type) {
    QByteArray bytes(static_cast<int>(killcore::valueTypeSize(type)), '\0');
    switch (type) {
        case killcore::ValueType::Int8: { const int8_t v = static_cast<int8_t>(std::llround(value)); std::memcpy(bytes.data(), &v, sizeof(v)); break; }
        case killcore::ValueType::UInt8: { const uint8_t v = static_cast<uint8_t>(std::llround(value)); std::memcpy(bytes.data(), &v, sizeof(v)); break; }
        case killcore::ValueType::Int16: { const int16_t v = static_cast<int16_t>(std::llround(value)); std::memcpy(bytes.data(), &v, sizeof(v)); break; }
        case killcore::ValueType::UInt16: { const uint16_t v = static_cast<uint16_t>(std::llround(value)); std::memcpy(bytes.data(), &v, sizeof(v)); break; }
        case killcore::ValueType::Int32: { const int32_t v = static_cast<int32_t>(std::llround(value)); std::memcpy(bytes.data(), &v, sizeof(v)); break; }
        case killcore::ValueType::UInt32: { const uint32_t v = static_cast<uint32_t>(std::llround(value)); std::memcpy(bytes.data(), &v, sizeof(v)); break; }
        case killcore::ValueType::Int64: { const int64_t v = static_cast<int64_t>(std::llround(value)); std::memcpy(bytes.data(), &v, sizeof(v)); break; }
        case killcore::ValueType::UInt64: { const uint64_t v = static_cast<uint64_t>(std::llround(value)); std::memcpy(bytes.data(), &v, sizeof(v)); break; }
        case killcore::ValueType::Float32: { const float v = static_cast<float>(value); std::memcpy(bytes.data(), &v, sizeof(v)); break; }
        case killcore::ValueType::Float64: { std::memcpy(bytes.data(), &value, sizeof(value)); break; }
    }
    return bytes;
}

QString unknownVariantLabel() {
    return "Unknown current";
}

bool candidateNeedsAutoVariantMatch(const killcore::Candidate& candidate) {
    const QString label = candidate.variantLabel.trimmed();
    return label.isEmpty()
        || label.compare(unknownVariantLabel(), Qt::CaseInsensitive) == 0
        || label.compare("Unknown Auto", Qt::CaseInsensitive) == 0;
}

QString variantKey(killcore::ValueType type, const QString& label) {
    return killcore::valueTypeToString(type) + "|" + label;
}

QHash<QString, QByteArray> variantBytesByKey(const QList<killcore::ValueVariant>& variants) {
    QHash<QString, QByteArray> bytes;
    for (const auto& variant : variants) {
        bytes.insert(variantKey(variant.value.type, variant.label), killcore::scanValueToBytes(variant.value));
    }
    return bytes;
}

QList<killcore::ValueVariant> smartAutoScanVariants(const QString& rawValue) {
    QList<killcore::ValueVariant> variants;
    const QList<killcore::ValueType> fastTypes = {
        killcore::ValueType::UInt16,
        killcore::ValueType::Int32,
        killcore::ValueType::UInt32,
        killcore::ValueType::Int64,
        killcore::ValueType::Float32,
        killcore::ValueType::Float64,
    };

    for (const auto type : fastTypes) {
        killcore::ScanValue value;
        if (!killcore::parseScanValue(rawValue, type, &value)) {
            continue;
        }
        killcore::ValueVariant variant;
        variant.value = value;
        variant.label = killcore::valueTypeToString(type);
        variant.secondary = false;
        variants.append(variant);
    }
    return variants;
}

QList<killcore::ValueType> unknownAutoValueTypes() {
    return {
        killcore::ValueType::Int32,
        killcore::ValueType::UInt32,
        killcore::ValueType::Float32,
        killcore::ValueType::Int16,
        killcore::ValueType::UInt16,
        killcore::ValueType::Int64,
        killcore::ValueType::Float64,
    };
}

QByteArray targetBytesForCandidate(
    const QString& rawValue,
    const killcore::Candidate& candidate,
    QString* error = nullptr) {
    if (!candidate.variantLabel.trimmed().isEmpty()) {
        const auto variants = killcore::generateScanVariants(rawValue, candidate.type, true);
        const auto bytes = variantBytesByKey(variants);
        const QString key = variantKey(candidate.type, candidate.variantLabel);
        if (bytes.contains(key)) {
            return bytes.value(key);
        }
    }

    killcore::ScanValue parsed;
    QString parseError;
    if (!killcore::parseScanValue(rawValue, candidate.type, &parsed, &parseError)) {
        if (error) *error = parseError;
        return {};
    }
    return killcore::scanValueToBytes(parsed);
}

bool matchCandidateExactVariant(
    const QString& rawValue,
    const killcore::Candidate& candidate,
    const QByteArray& current,
    killcore::Candidate* updatedCandidate,
    QString* error) {
    if (!candidateNeedsAutoVariantMatch(candidate)) {
        QString targetError;
        const QByteArray candidateTargetBytes = targetBytesForCandidate(rawValue, candidate, &targetError);
        if (candidateTargetBytes.isEmpty()) {
            if (error) {
                *error = targetError.isEmpty()
                    ? QString("Impossible de construire la valeur cible pour un candidat.")
                    : targetError;
            }
            return false;
        }
        if (!bytesEqual(current, candidateTargetBytes, candidate.type)) {
            return false;
        }
        if (updatedCandidate) {
            *updatedCandidate = candidate;
        }
        return true;
    }

    const auto variants = killcore::generateScanVariants(rawValue, candidate.type, true);
    for (const auto& variant : variants) {
        if (variant.value.type != candidate.type) {
            continue;
        }
        const QByteArray targetBytes = killcore::scanValueToBytes(variant.value);
        if (!targetBytes.isEmpty() && bytesEqual(current, targetBytes, candidate.type)) {
            if (updatedCandidate) {
                *updatedCandidate = candidate;
                updatedCandidate->variantLabel = variant.label;
            }
            return true;
        }
    }

    QString parseError;
    const QByteArray fallbackBytes = targetBytesForCandidate(rawValue, candidate, &parseError);
    if (fallbackBytes.isEmpty()) {
        if (error) {
            *error = parseError.isEmpty()
                ? QString("Impossible de construire la valeur cible pour un candidat.")
                : parseError;
        }
        return false;
    }
    if (!bytesEqual(current, fallbackBytes, candidate.type)) {
        return false;
    }
    if (updatedCandidate) {
        *updatedCandidate = candidate;
    }
    return true;
}

QList<killcore::Candidate> candidatesFromUnknownScan(
    const killcore::ProcessHandle& process,
    const killcore::UnknownScanResult& scan) {
    QList<killcore::Candidate> candidates;
    candidates.reserve(scan.matches.size());
    killcore::MemoryReader reader(process);

    for (const auto& match : scan.matches) {
        const size_t valueSize = killcore::valueTypeSize(match.type);
        const auto read = reader.read(match.address, valueSize);
        if (!(read.success || read.partial) || read.bytesRead != valueSize) {
            continue;
        }

        killcore::Candidate candidate;
        candidate.address = match.address;
        candidate.type = match.type;
        candidate.lastValue = read.data;
        candidate.confidence = match.confidence;
        candidate.variantLabel = match.variantLabel.isEmpty() ? unknownVariantLabel() : match.variantLabel;
        candidates.append(candidate);
    }

    return candidates;
}

QStringList hexAddressesFromText(const QString& text) {
    QStringList addresses;
    const QRegularExpression re(R"(\b0x[0-9a-fA-F]{5,16}\b)");
    auto it = re.globalMatch(text);
    while (it.hasNext()) {
        addresses.append(it.next().captured(0));
    }
    return addresses;
}

QString textWithoutHexAddresses(QString text) {
    const QRegularExpression re(R"(\b0x[0-9a-fA-F]{5,16}\b)");
    return text.replace(re, " ");
}

QString textWithoutTypeTokens(QString text) {
    static const QRegularExpression typeRe(
        R"(\b(?:u?int(?:8|16|32|64)?|float(?:32|64)?|double|long)\b)",
        QRegularExpression::CaseInsensitiveOption);
    return text.replace(typeRe, " ");
}

QStringList numbersFromText(const QString& text) {
    QStringList values;
    // L'ecriture francaise groupe les milliers par espace ("100 000" = cent
    // mille). Sans l'alternative groupee ci-dessous (essayee en premier),
    // "100 000" se lit comme DEUX nombres distincts "100" et "000" : dans
    // "je veux a 100 000", GuidedScan ne voit que numbers.at(1) = "100" et la
    // cible reelle (100000) disparait silencieusement, sans aucune erreur.
    // Espace normal ET insecable (U+00A0, que Windows/le clavier FR produisent
    // parfois) sont acceptes comme separateur de groupe.
    // Compromis assume, non resolu : deux nombres tapes cote a cote sans mot
    // de liaison ni ponctuation, ou le second fait exactement 3 chiffres
    // (ex: "100 200" sans "à" entre les deux), fusionnent en un seul nombre
    // "100200" au lieu de rester deux valeurs distinctes — GuidedScan (qui
    // veut numbers.size() >= 2) verrait alors une seule cible. Un connecteur
    // ("à", "vers", "et"...) entre les deux nombres empeche la fusion (le
    // groupe exige un espace suivi directement de 3 chiffres), donc "de 100 à
    // 200" n'est pas affecte ; seule la forme rare "100 200" sans connecteur
    // l'est. Pas de correctif ici : resoudre parfaitement cette ambiguite
    // demanderait de la vraie comprehension du langage naturel, et le risque
    // inverse (rater "100 000" pour cent mille, bien plus frequent en usage
    // reel) serait pire.
    const QRegularExpression re(
        QStringLiteral("[-+]?\\d{1,3}(?:[ \\x{00A0}]\\d{3})+(?:[.,]\\d+)?|[-+]?\\d+(?:[.,]\\d+)?"));
    auto it = re.globalMatch(textWithoutTypeTokens(textWithoutHexAddresses(text)));
    while (it.hasNext()) {
        QString captured = it.next().captured(0);
        captured.remove(' ');
        captured.remove(QChar(0x00A0));
        values.append(captured.replace(',', '.'));
    }
    return values;
}

QString explicitValueTypeFromText(const QString& text) {
    const QString q = text.toLower();
    if (q.contains("uint8") || q.contains("u8") || q.contains("byte")) return "UInt8";
    if (q.contains("int8") || q.contains("i8")) return "Int8";
    if (q.contains("uint16") || q.contains("u16")) return "UInt16";
    if (q.contains("int16") || q.contains("i16") || q.contains("short")) return "Int16";
    if (q.contains("uint32") || q.contains("u32")) return "UInt32";
    if (q.contains("uint64") || q.contains("u64")) return "UInt64";
    if (q.contains("float64") || q.contains("double")) return "Float64";
    if (q.contains("float32") || q.contains("float")) return "Float32";
    if (q.contains("int64") || q.contains("long")) return "Int64";
    if (q.contains("int32") || q.contains("int")) return "Int32";
    return {};
}

QString bytesToHex(const QByteArray& bytes) {
    return QString::fromLatin1(bytes.toHex(' '));
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

QVariantMap candidateObservationToVariantMap(
    const killcore::Candidate& candidate,
    const QByteArray& current,
    const QString& phase,
    bool kept,
    bool readable) {
    QVariantMap observation;
    observation["address"] = QString::number(candidate.address, 16);
    observation["type"] = killcore::valueTypeToString(candidate.type);
    observation["phase"] = phase;
    observation["previousHex"] = bytesToHex(candidate.lastValue);
    observation["currentHex"] = bytesToHex(current);
    observation["previousNumber"] = bytesToDouble(candidate.lastValue, candidate.type);
    observation["currentNumber"] = bytesToDouble(current, candidate.type);
    observation["kept"] = kept;
    observation["readable"] = readable;
    observation["time"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    return observation;
}

QString noCandidateDiagnosticMessage(const QVariantMap& actionResult, const QString& tool) {
    const qulonglong checked = tool == "next_scan"
        ? actionResult.value("checked").toULongLong()
        : actionResult.value("checkedBytes").toULongLong();
    const qulonglong unreadable = actionResult.value("unreadable").toULongLong();
    const QVariantList samples = actionResult.value("debugSamples").toList();

    QStringList parts;
    if (tool == "next_scan") {
        parts.append(QString("J'ai comparé %1 adresse(s).").arg(checked));
        if (unreadable > 0) {
            parts.append(QString("%1 adresse(s) étaient illisibles.").arg(unreadable));
        }
    } else {
        parts.append(QString("La comparaison unknown a parcouru %1 octet(s).").arg(checked));
    }

    QStringList sampleTexts;
    int unchangedSamples = 0;
    int readableSamples = 0;
    for (int i = 0; i < std::min<int>(static_cast<int>(samples.size()), 3); ++i) {
        const auto sample = samples.at(i).toMap();
        const QString address = sample.value("address").toString();
        if (address.isEmpty()) {
            continue;
        }
        if (sample.value("readable", true).toBool()) {
            ++readableSamples;
            if (sample.value("previousNumber").toString() == sample.value("currentNumber").toString()) {
                ++unchangedSamples;
            }
            sampleTexts.append(QString("0x%1 : %2 -> %3")
                                   .arg(address)
                                   .arg(sample.value("previousNumber").toString())
                                   .arg(sample.value("currentNumber").toString()));
        } else {
            sampleTexts.append(QString("0x%1 : illisible").arg(address));
        }
    }
    if (!sampleTexts.isEmpty()) {
        parts.append(QString("Exemples : %1.").arg(sampleTexts.join(", ")));
    }
    if (readableSamples > 0 && unchangedSamples == readableSamples) {
        parts.append("Les exemples n'ont pas bougé : la première recherche a probablement capturé des copies, une valeur miroir, ou une représentation qui ne suit pas la valeur affichée.");
    }

    parts.append("Restaure la réduction précédente, puis essaie une réduction changed/increased ou une nouvelle recherche en Float32 / valeur x100.");
    return parts.join(' ');
}

QString normalizedProfileText(QString value) {
    return value.toLower().trimmed();
}

QString profileTargetGroupName(QString name) {
    name = normalizedProfileText(name);
    static const QRegularExpression numberedSuffix(R"(\s+\d+$)");
    return name.remove(numberedSuffix).trimmed();
}

bool looksLikeLastAutoWriteRewrite(const QString& query) {
    const QString q = query.toLower();
    return q.contains("plutot")
        || q.contains("plutôt")
        || q.contains("change")
        || q.contains("changer")
        || q.contains("modifie")
        || q.contains("modifier")
        || q.contains("mettre")
        || q.contains("mets")
        || q.contains("met ")
        || q.contains("passe")
        || q.contains("passé")
        || q.contains("passer")
        || q.contains("remet")
        || q.contains("remets")
        || q.contains("veux")
        || q.contains("veut")
        || q.contains("voulais")
        || q.contains("voudrais")
        || q.contains("augmente")
        || q.contains("augmenter")
        || q.contains("remplace")
        || q.contains("remplacer")
        || q.contains("fixe")
        || q.contains("définis")
        || q.contains("definis")
        || q.contains("définir")
        || q.contains("definir")
        || q.contains("ces adresse")
        || q.contains("ces adresses")
        || q.contains("les adresse")
        || q.contains("les adresses")
        || q.contains(" le ")
        || q.contains(" les ")
        || q.contains(" ça ")
        || q.contains(" ca ")
        || q.contains("les mettre");
}

bool looksLikeMemoryTargetWriteRequest(const QString& query) {
    const QString q = query.toLower();
    return looksLikeLastAutoWriteRewrite(q)
        || q.contains("passer")
        || q.contains("mets")
        || q.contains("met ");
}

bool looksLikeNewSearchRequest(const QString& query) {
    const QString q = query.toLower();
    return q.contains("nouvelle recherche")
        || q.contains("autre recherche")
        || q.contains("nouveau scan")
        || q.contains("nouvelle valeur")
        || q.contains("autre valeur")
        || q.contains("autre chose")
        || q.contains("valeur a chercher")
        || q.contains("valeur à chercher")
        || q.contains("autre que")
        || q.contains("pas ces adresse")
        || q.contains("pas ces adresses")
        || q.contains("repart")
        || q.contains("recommence")
        || q.contains("reset")
        // Abandon pur et simple, sans mot-cle "nouvelle recherche" explicite.
        // Sans ca, une reponse comme "laisse tomber" a une relance en attente
        // (trace_ui_string/trace_ui_filter/write_target_value) est traitee
        // comme si c'etait la reponse demandee : le texte litteral finit
        // dans m_smartSearchTargetValue ou comme valeur observee, au lieu
        // d'annuler proprement.
        || q.contains("laisse tomber")
        || q.contains("j'abandonne")
        || q.contains("j abandonne")
        || q.contains("oublie ça")
        || q.contains("oublie ca")
        || q.contains("peu importe");
}

bool looksLikeClearActiveTargetsRequest(const QString& query) {
    const QString q = query.toLower();
    const bool clearVerb = q.contains("oublie")
        || q.contains("oublier")
        || q.contains("efface")
        || q.contains("supprime")
        || q.contains("retire")
        || q.contains("vide");
    const bool targetWord = q.contains("adresse")
        || q.contains("memoire")
        || q.contains("mémoire")
        || q.contains("cible")
        || q.contains("profil")
        || q.contains("contexte");
    return clearVerb && targetWord;
}

bool looksLikeBadTargetReport(const QString& query) {
    const QString q = query.toLower();
    return q.contains("marche pas")
        || q.contains("marché pas")
        || q.contains("pas marche")
        || q.contains("pas marché")
        || q.contains("n'a pas marché")
        || q.contains("n a pas marche")
        || q.contains("ne marche pas")
        // "fonctionne/fonctionné" est le synonyme le plus courant de "marche"
        // et n'etait pas couvert : "ça n'a pas fonctionné" tombait sur Unknown
        // et partait vers l'IA locale au lieu de ReportBadTargets (vu en test
        // reel le 17/08/2026 - ~90s perdues sur un appel LLM qui n'aboutit a
        // rien, puis le message suivant reecrivait sur les adresses jamais
        // invalidees puisque ce chemin n'avait jamais ete declenche).
        || q.contains("fonctionne pas")
        || q.contains("fonctionné pas")
        || q.contains("pas fonctionne")
        || q.contains("pas fonctionné")
        || q.contains("n'a pas fonctionné")
        || q.contains("n a pas fonctionne")
        || q.contains("ne fonctionne pas")
        || q.contains("rien fait")
        || q.contains("aucun effet")
        || q.contains("toujours pareil")
        || q.contains("pas bon")
        || q.contains("pas la bonne")
        || q.contains("mauvaise adresse")
        || q.contains("mauvaises adresses")
        || q.contains("ca change pas")
        || q.contains("ça change pas")
        || q.contains("rien change")
        || q.contains("rien ne change")
        || q.contains("crash")
        || q.contains("crashé")
        || q.contains("crashe")
        || q.contains("planté")
        || q.contains("plante")
        || q.contains("jeu s'est fermé")
        || q.contains("jeu s est ferme");
}

bool looksLikeGoodTargetReport(const QString& query) {
    // "c'est ça" est volontairement absent : c'est une confirmation
    // conversationnelle generique ("d'accord, c'est ça le prochain objectif")
    // qui n'a le plus souvent aucun rapport avec une adresse. La declencher a
    // tort sauvegarde silencieusement l'adresse active dans le Profil et
    // l'immunise pour toujours contre le filtre anti-bruit (voir
    // flagNoisyCandidates / everConfirmed plus haut) : une fausse confirmation
    // ici est quasi irreversible, donc seuls des motifs sans ambiguite
    // raisonnable sont acceptes.
    const QString q = query.toLower();
    return q.contains("ça a marché")
        || q.contains("ca a marche")
        || q.contains("ça marche")
        || q.contains("ca marche")
        || q.contains("ça a fonctionné")
        || q.contains("ca a fonctionne")
        || q.contains("ça fonctionne")
        || q.contains("ca fonctionne")
        || q.contains("c'est la bonne")
        || q.contains("c est la bonne")
        || q.contains("bonne adresse")
        || q.contains("ça a changé")
        || q.contains("ca a change")
        || q.contains("nickel")
        || q.contains("parfait");
}

bool looksLikeFreezeRequest(const QString& query) {
    const QString q = query.toLower();
    const bool negated = q.contains("sans freeze") || q.contains("sans freezer")
        || q.contains("sans geler") || q.contains("sans figer")
        || q.contains("ni freeze") || q.contains("ni freezer")
        || q.contains("ni geler") || q.contains("ni figer")
        || q.contains("pas de freeze") || q.contains("pas freeze")
        || q.contains("ne freeze pas") || q.contains("ne pas freeze")
        || q.contains("ne pas freezer") || q.contains("without freeze")
        || q.contains("without freezing") || q.contains("no freeze")
        || q.contains("do not freeze") || q.contains("don't freeze");
    if (negated) {
        return false;
    }

    return q.contains("freeze")
        || q.contains("freezer")
        || q.contains("fige")
        || q.contains("figer")
        || q.contains("bloque")
        || q.contains("bloquer")
        || q.contains("verrouille")
        || q.contains("verrouiller")
        || q.contains("garde a")
        || q.contains("garde à")
        || q.contains("maintien")
        || q.contains("maintenir");
}

SmartSearchIntent classifySmartSearchIntent(
    const QString& query,
    const QStringList& numbers,
    const QStringList& addresses,
    bool hasChatMemoryTargets,
    bool hasLastAutoWriteTargets,
    bool hasCandidates,
    bool smartSearchActive,
    bool awaitingUiStringTraceValue,
    bool awaitingUiStringFilterValue,
    bool awaitingWriteTargetValue) {
    SmartSearchIntent intent;
    intent.numbers = numbers;
    intent.addresses = addresses;
    intent.resetContext = looksLikeNewSearchRequest(query);

    const bool hasOneNumber = numbers.size() == 1;
    const bool wantsMemoryWrite = looksLikeMemoryTargetWriteRequest(query);
    const bool wantsLastRewrite = looksLikeLastAutoWriteRewrite(query);
    const bool wantsClearTargets = looksLikeClearActiveTargetsRequest(query);
    const bool reportsBadTargets = looksLikeBadTargetReport(query);
    // Verifie reportsBadTargets d'abord dans la branche ci-dessous : certains
    // tours ("ça marche pas") sont un sous-ensemble textuel de motifs positifs
    // ("ça marche"), l'ordre de l'if/else suffit a lever l'ambiguite sans que
    // les deux listes de mots-cles aient besoin d'etre mutuellement exclusives.
    const bool reportsGoodTargets = looksLikeGoodTargetReport(query);
    const bool wantsFreeze = looksLikeFreezeRequest(query);

    if (wantsClearTargets) {
        intent.kind = SmartSearchIntentKind::ClearActiveTargets;
        intent.rationale = "L'utilisateur demande d'oublier les adresses, profils ou cibles actives.";
    } else if (reportsBadTargets && (hasLastAutoWriteTargets || hasChatMemoryTargets)) {
        intent.kind = SmartSearchIntentKind::ReportBadTargets;
        intent.rationale = "L'utilisateur indique que les dernières adresses écrites ne donnent pas le résultat attendu.";
    } else if (reportsGoodTargets && (hasLastAutoWriteTargets || hasChatMemoryTargets)) {
        intent.kind = SmartSearchIntentKind::ReportGoodTargets;
        intent.rationale = "L'utilisateur confirme que les dernières adresses écrites fonctionnent.";
    } else if (awaitingUiStringTraceValue && !intent.resetContext && (hasOneNumber || !query.trimmed().isEmpty())) {
        // L'assistant vient de proposer "Tracer le texte affiché" et attend la
        // valeur affichée en reponse. Sans cette interception, une reponse en
        // langage libre contenant un nombre (ex: "le texte affiche est 180")
        // retombe sur WriteMemoryTargets plus bas et reecrit betement ce
        // nombre sur les adresses deja invalidees, au lieu de tracer.
        intent.kind = SmartSearchIntentKind::AnswerTraceUiStringPrompt;
        intent.rationale = "L'utilisateur répond à la proposition de tracer le texte affiché.";
    } else if (awaitingUiStringFilterValue && !intent.resetContext && (hasOneNumber || !query.trimmed().isEmpty())) {
        // Meme interception pour l'etape 2 du pipeline Trace UI string
        // (filtrer les strings survivantes puis analyser les sources
        // numeriques autour) : sans elle, la reponse "nouvelle valeur
        // affichee" retombe elle aussi sur l'ancien pipeline numerique.
        intent.kind = SmartSearchIntentKind::AnswerTraceUiFilterPrompt;
        intent.rationale = "L'utilisateur répond à la proposition de filtrer les strings suivies.";
    } else if (awaitingWriteTargetValue && !intent.resetContext && (hasOneNumber || !query.trimmed().isEmpty())) {
        // L'assistant a demande la valeur a ecrire (candidats reduits mais
        // aucune cible connue). Sans cette interception, la reponse retombe
        // sur ExactScan et repart sur un scan complet, abandonnant la
        // reduction deja faite.
        intent.kind = SmartSearchIntentKind::AnswerWriteTargetPrompt;
        intent.rationale = "L'utilisateur donne la valeur à écrire sur les candidats déjà réduits.";
    } else if (smartSearchActive && hasCandidates && hasOneNumber) {
        intent.kind = SmartSearchIntentKind::RefineScan;
        intent.rationale = "Un scan guidé est actif et l'utilisateur donne une nouvelle valeur observée.";
    } else if (intent.resetContext && numbers.isEmpty()) {
        intent.kind = SmartSearchIntentKind::ResetContext;
        intent.rationale = "L'utilisateur demande un nouveau contexte sans donner encore de valeur.";
    } else if (!addresses.isEmpty()) {
        intent.kind = hasOneNumber && wantsFreeze
            ? SmartSearchIntentKind::FreezeMemoryTargets
            : (hasOneNumber && wantsMemoryWrite
                ? SmartSearchIntentKind::WriteMemoryTargets
                : SmartSearchIntentKind::ActivateMemoryTargets);
        intent.rationale = "Le message contient une ou plusieurs adresses mémoire explicites.";
    } else if (hasChatMemoryTargets && hasOneNumber && !intent.resetContext && wantsFreeze) {
        intent.kind = SmartSearchIntentKind::FreezeMemoryTargets;
        intent.rationale = "Des adresses mémoire sont actives et l'utilisateur demande de freezer la valeur.";
    } else if (hasLastAutoWriteTargets && hasOneNumber && !intent.resetContext && wantsLastRewrite) {
        intent.kind = SmartSearchIntentKind::RewriteLastTargets;
        intent.rationale = "L'utilisateur demande de modifier les dernières adresses écrites.";
    } else if (hasChatMemoryTargets && hasOneNumber && !intent.resetContext) {
        intent.kind = SmartSearchIntentKind::WriteMemoryTargets;
        intent.rationale = "Des adresses mémoire sont actives dans la conversation.";
    } else if (hasOneNumber && !intent.resetContext && wantsMemoryWrite) {
        intent.kind = SmartSearchIntentKind::WriteProfileTargets;
        intent.rationale = "L'utilisateur formule une intention d'écriture sur une cible nommée.";
    } else if (numbers.size() >= 2) {
        intent.kind = SmartSearchIntentKind::GuidedScan;
        intent.rationale = "Le message contient une valeur actuelle et une valeur cible.";
    } else if (hasOneNumber) {
        intent.kind = SmartSearchIntentKind::ExactScan;
        intent.rationale = intent.resetContext
            ? "Nouvelle recherche demandée avec une valeur."
            : "Recherche exacte depuis une valeur unique.";
    }

    return intent;
}

// Detecte si le processus a des connexions TCP ETABLIES vers un hote distant
// (hors loopback). Utilise l'API Windows standard en lecture seule (aucune
// capture de paquets, aucun droit admin requis) : ce n'est PAS une preuve
// qu'une valeur donnee est synchronisee avec un serveur, juste un indice
// supplementaire a proposer une fois toutes les pistes memoire locales
// epuisees (ex: XP/monnaie lies a un compte en ligne plutot qu'a une simple
// variable de session).
#ifdef _WIN32
bool processHasActiveRemoteConnections(int pid) {
    ULONG size = 0;
    if (GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0) != ERROR_INSUFFICIENT_BUFFER
        || size == 0) {
        return false;
    }
    QByteArray buffer(static_cast<int>(size), 0);
    auto* table = reinterpret_cast<MIB_TCPTABLE_OWNER_PID*>(buffer.data());
    if (GetExtendedTcpTable(table, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0) != NO_ERROR) {
        return false;
    }
    for (DWORD i = 0; i < table->dwNumEntries; ++i) {
        const auto& row = table->table[i];
        if (static_cast<int>(row.dwOwningPid) != pid || row.dwState != MIB_TCP_STATE_ESTAB) {
            continue;
        }
        const uint32_t remote = ntohl(row.dwRemoteAddr);
        const bool isLoopback = (remote >> 24) == 127;
        if (remote != 0 && !isLoopback) {
            return true;
        }
    }
    return false;
}
#else
bool processHasActiveRemoteConnections(int) {
    return false;
}
#endif

QString confidenceLabel(double confidence) {
    if (confidence >= 0.85) {
        return "fiabilité élevée";
    }
    if (confidence >= 0.65) {
        return "fiabilité moyenne";
    }
    return "fiabilité faible";
}

// Historique inter-sessions des adresses ecrites par le pipeline auto-write,
// par jeu. Une adresse qui revient comme "candidat final" sur des recherches
// avec des paires (valeur initiale, cible) differentes et sans rapport est
// tres probablement un compteur interne (timer, animation, allocation
// reutilisee) qui satisfait le test de transition par pure coincidence, pas
// la vraie donnee cherchee. Persiste via QSettings, comme le reste du
// "profil appris" par gameKey (cf. getAutoResolveReport).
constexpr int kCandidateHistoryLimit = 60;

QVariantList loadCandidateHistory(const QString& gameKey) {
    QSettings settings;
    settings.beginGroup(QString("autoResolver/process/%1").arg(gameKey));
    const QByteArray raw = settings.value("candidateHistory").toByteArray();
    settings.endGroup();
    if (raw.isEmpty()) {
        return {};
    }
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(raw, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isArray()) {
        return {};
    }
    return doc.array().toVariantList();
}

void appendCandidateHistory(const QString& gameKey, const QVariantList& newEntries) {
    if (newEntries.isEmpty()) {
        return;
    }
    QVariantList history = loadCandidateHistory(gameKey);
    history.append(newEntries);
    while (history.size() > kCandidateHistoryLimit) {
        history.removeFirst();
    }
    QSettings settings;
    settings.beginGroup(QString("autoResolver/process/%1").arg(gameKey));
    settings.setValue("candidateHistory", QJsonDocument(QJsonArray::fromVariantList(history)).toJson(QJsonDocument::Compact));
    settings.endGroup();
}

// Marque (sans les retirer) les suggestions dont l'adresse apparait deja dans
// l'historique pour une paire de valeurs differente. Retourne le nombre de
// suggestions non suspectes restantes, pour decider s'il faut filtrer.
int flagNoisyCandidates(
    QVariantList* suggestions,
    const QString& gameKey,
    const QString& initialValue,
    const QString& targetValue) {
    if (!suggestions || suggestions->isEmpty()) {
        return suggestions ? suggestions->size() : 0;
    }
    const QVariantList history = loadCandidateHistory(gameKey);
    int cleanCount = 0;
    for (int i = 0; i < suggestions->size(); ++i) {
        QVariantMap suggestion = suggestions->at(i).toMap();
        const QString address = suggestion.value("address").toString();

        // Une adresse explicitement confirmee par l'utilisateur ("ça a
        // marché") un jour n'est plus jamais consideree comme du bruit,
        // meme si elle revient plus tard avec une autre paire de valeurs :
        // une vraie donnee (XP, argent...) est justement testee avec des
        // cibles differentes a chaque farming, ce n'est pas un signe de bruit.
        bool everConfirmed = false;
        for (const auto& entryVariant : history) {
            const QVariantMap entry = entryVariant.toMap();
            if (entry.value("address").toString() == address && entry.value("confirmed").toBool()) {
                everConfirmed = true;
                break;
            }
        }
        if (everConfirmed) {
            ++cleanCount;
            continue;
        }

        int unrelatedHits = 0;
        for (const auto& entryVariant : history) {
            const QVariantMap entry = entryVariant.toMap();
            if (entry.value("address").toString() != address) {
                continue;
            }
            const bool sameSearch = entry.value("initialValue").toString() == initialValue
                && entry.value("targetValue").toString() == targetValue;
            if (!sameSearch) {
                ++unrelatedHits;
            }
        }
        if (unrelatedHits > 0) {
            suggestion["noisyHistoryHits"] = unrelatedHits;
            suggestion["confidenceReason"] = QString("⚠ vue dans %1 recherche(s) différente(s) sans rapport — probablement du bruit · %2")
                .arg(unrelatedHits)
                .arg(suggestion.value("confidenceReason").toString());
        } else {
            ++cleanCount;
        }
        (*suggestions)[i] = suggestion;
    }
    return cleanCount;
}

QVariantList suggestedWritesForCandidates(const killcore::CandidateStore& candidates, const QString& value, size_t limit) {
    QVariantList suggestions;
    if (value.isEmpty() || candidates.isEmpty()) {
        return suggestions;
    }

    const auto& all = candidates.candidates();
    for (qsizetype i = 0; i < all.size() && static_cast<size_t>(suggestions.size()) < limit; ++i) {
        const auto& candidate = all.at(i);

        // Une valeur cible qui ne rentre pas dans le type detecte du candidat
        // (ex: 100000 sur un candidat UInt16, max 65535) echoue a coup sur a
        // l'ecriture. Sans ce filtre, le candidat etait quand meme presente
        // comme suggestion "fiabilite elevee" et l'echec n'apparaissait qu'au
        // moment d'ecrire (auto_write_partial_or_failed) — trop tard pour
        // etre utile a l'utilisateur ou a l'IA.
        killcore::ScanValue parsedValue;
        if (!killcore::parseScanValue(value, candidate.type, &parsedValue)) {
            continue;
        }

        QVariantMap suggestion;
        suggestion["address"] = QString::number(candidate.address, 16);
        suggestion["type"] = killcore::valueTypeToString(candidate.type);
        suggestion["value"] = value;
        suggestion["confidence"] = candidate.confidence;
        suggestion["confidenceLabel"] = confidenceLabel(candidate.confidence);
        suggestion["confidenceReason"] = candidate.variantLabel.isEmpty()
            ? QString("adresse survivante des réductions")
            : QString("%1 · %2").arg(confidenceLabel(candidate.confidence), candidate.variantLabel);
        if (!candidate.variantLabel.isEmpty()) {
            suggestion["variantLabel"] = candidate.variantLabel;
        }
        suggestions.append(suggestion);
    }
    return suggestions;
}

void appendDistinctText(QStringList* values, const QString& value, int maxCount) {
    if (!values) {
        return;
    }
    const QString trimmed = value.trimmed();
    if (trimmed.isEmpty()) {
        return;
    }
    if (values->isEmpty() || values->last() != trimmed) {
        values->append(trimmed);
    }
    while (values->size() > maxCount) {
        values->removeFirst();
    }
}

QVariantList writeHistoryToVariantList(const QStringList& values) {
    QVariantList result;
    for (const auto& value : values) {
        result.append(value);
    }
    return result;
}

#ifdef Q_OS_WIN
struct WindowTextChildPayload {
    QVariantList* children{nullptr};
    int maxChildren{0};
};

struct WindowTextEnumState {
    DWORD pid{0};
    int maxWindows{100};
    int maxChildrenPerWindow{200};
    bool includeAllVisible{false};
    QString titleContains;
    QVariantList windows;
};

BOOL CALLBACK enumChildWindowTextProc(HWND hwnd, LPARAM lParam) {
    auto* payload = reinterpret_cast<WindowTextChildPayload*>(lParam);
    if (!payload || !payload->children || payload->children->size() >= payload->maxChildren) {
        return FALSE;
    }
    wchar_t textBuffer[512]{};
    wchar_t classBuffer[256]{};
    const int textLen = GetWindowTextW(hwnd, textBuffer, static_cast<int>(std::size(textBuffer)));
    const int classLen = GetClassNameW(hwnd, classBuffer, static_cast<int>(std::size(classBuffer)));
    if (textLen <= 0 && classLen <= 0) {
        return TRUE;
    }
    QVariantMap child;
    child["hwnd"] = QString::number(reinterpret_cast<quintptr>(hwnd), 16).toUpper();
    child["text"] = textLen > 0 ? QString::fromWCharArray(textBuffer, textLen) : QString();
    child["className"] = classLen > 0 ? QString::fromWCharArray(classBuffer, classLen) : QString();
    RECT rect{};
    if (GetWindowRect(hwnd, &rect)) {
        child["x"] = static_cast<int>(rect.left);
        child["y"] = static_cast<int>(rect.top);
        child["width"] = static_cast<int>(rect.right - rect.left);
        child["height"] = static_cast<int>(rect.bottom - rect.top);
    }
    payload->children->append(child);
    return TRUE;
}

BOOL CALLBACK enumWindowTextProc(HWND hwnd, LPARAM lParam) {
    auto* state = reinterpret_cast<WindowTextEnumState*>(lParam);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!state) {
        return TRUE;
    }
    wchar_t textBuffer[512]{};
    wchar_t classBuffer[256]{};
    const int textLen = GetWindowTextW(hwnd, textBuffer, static_cast<int>(std::size(textBuffer)));
    const int classLen = GetClassNameW(hwnd, classBuffer, static_cast<int>(std::size(classBuffer)));
    const QString title = textLen > 0 ? QString::fromWCharArray(textBuffer, textLen) : QString();
    const QString className = classLen > 0 ? QString::fromWCharArray(classBuffer, classLen) : QString();
    const bool pidMatches = pid == state->pid;
    const bool titleMatches = !state->titleContains.isEmpty()
        && title.contains(state->titleContains, Qt::CaseInsensitive);
    const bool relatedVisibleFrame = state->includeAllVisible
        && IsWindowVisible(hwnd)
        && (titleMatches || className.compare("ApplicationFrameWindow", Qt::CaseInsensitive) == 0);
    if (!pidMatches && !relatedVisibleFrame) {
        return TRUE;
    }
    if (state->windows.size() >= state->maxWindows) {
        return FALSE;
    }

    QVariantMap window;
    window["hwnd"] = QString::number(reinterpret_cast<quintptr>(hwnd), 16).toUpper();
    window["pid"] = static_cast<qulonglong>(pid);
    window["pidMatchesAttached"] = pidMatches;
    window["visible"] = IsWindowVisible(hwnd) != FALSE;
    window["text"] = title;
    window["className"] = className;
    RECT rect{};
    if (GetWindowRect(hwnd, &rect)) {
        window["x"] = static_cast<int>(rect.left);
        window["y"] = static_cast<int>(rect.top);
        window["width"] = static_cast<int>(rect.right - rect.left);
        window["height"] = static_cast<int>(rect.bottom - rect.top);
    }

    QVariantList children;
    WindowTextChildPayload payload{&children, state->maxChildrenPerWindow};
    EnumChildWindows(hwnd, enumChildWindowTextProc, reinterpret_cast<LPARAM>(&payload));
    window["children"] = children;
    window["childCount"] = children.size();
    state->windows.append(window);
    return TRUE;
}
#endif

} // namespace

ApplicationController::ApplicationController(QObject* parent)
    : QObject(parent) {
    const size_t candidateThreshold = candidateFileBackedThresholdFromSettings();
    scanState().setFileBackedThreshold(candidateThreshold);
    m_scanningCoreManager = std::make_unique<ScanningCoreManager>(*this, this);
    m_settingsDiagnosticsManager = std::make_unique<SettingsDiagnosticsManager>(*this);
    m_smartSearchManager = std::make_unique<SmartSearchManager>(*this);
    m_uiStringInvestigator = std::make_unique<UiStringInvestigator>(
        m_handle,
        [this](const QString& event, const QVariantMap& payload) {
            appendScanTelemetry(event, payload);
        },
        [](const QVariantMap& expertOptions) {
            return scanOptionsFromSettingsAndExpertOptions(expertOptions);
        },
        [this]() {
            emit scanStarted();
        },
        [this](int percent) {
            emit scanProgress(percent);
        });
    m_clrInspectorBridge = std::make_unique<ClrInspectorBridge>(
        m_handle,
        [this](const QString& event, const QVariantMap& payload) {
            appendScanTelemetry(event, payload);
        },
        [this]() {
            return m_attached;
        },
        [this]() {
            return m_pid;
        },
        [this]() {
            return m_processName;
        });
    m_codePatchManager = std::make_unique<CodePatchManager>(
        m_handle,
        [this](const QString& event, const QVariantMap& payload) {
            appendScanTelemetry(event, payload);
        },
        [this]() {
            return m_attached;
        },
        [this]() {
            return m_pid;
        });
    m_profileManager = std::make_unique<ProfileManager>(*this);
    m_freezeHotkeyOverlayManager = std::make_unique<FreezeHotkeyOverlayManager>(
        m_handle,
        [this](const QString& event, const QVariantMap& payload) {
            appendScanTelemetry(event, payload);
        },
        [this]() {
            return m_pid;
        },
        [this](uint64_t address) {
            return hasAddressBeenWriteVerified(address);
        },
        [this](killcore::BreakpointFreezeMode mode, QString* error) {
            return m_debugFeatureManager->restartBreakpointFreezeFromRegistry(mode, error);
        },
        [this](const QVariantMap& event) {
            emit globalHotkeyTriggered(event);
        },
        [this](const QVariantMap& info) {
            emit freezeInstabilityDetected(info);
        },
        this);
    m_automationPipeManager = std::make_unique<AutomationPipeManager>(this);
    m_investigationNotebookManager = std::make_unique<InvestigationNotebookManager>();
    m_kernelDriverManager = std::make_unique<KernelDriverManager>(
        m_handle,
        [this](const QString& event, const QVariantMap& payload) {
            appendScanTelemetry(event, payload);
        });
    m_saveFileInvestigator = std::make_unique<SaveFileInvestigator>(
        m_handle,
        [this]() {
            return m_nextDebugRequestId++;
        },
        [this](const QVariantMap& result) {
            emit saveFileWatchFinished(result);
        },
        this);
    m_debugFeatureManager = std::make_unique<DebugFeatureManager>(
        m_handle,
        m_freezeHotkeyOverlayManager->freezeManager(),
        [this](const QString& event, const QVariantMap& payload) {
            appendScanTelemetry(event, payload);
        },
        [this]() {
            return m_attached;
        },
        [this]() {
            return m_pid;
        },
        [this]() {
            return m_nextDebugRequestId++;
        },
        [this](const QVariantMap& result) {
            emit findWhatWritesFinished(result);
        },
        [this](const QVariantMap& result) {
            emit findWhatAccessesFinished(result);
        },
        [this](const QVariantMap& result) {
            emit pageGuardWatchFinished(result);
        },
        [this](const QVariantMap& result) {
            emit inProcessBreakpointWatchFinished(result);
        });
    m_writeFreezeCoreManager = std::make_unique<WriteFreezeCoreManager>(
        m_handle,
        [this]() {
            return autoWriteState();
        },
        [this]() {
            return m_attached;
        },
        [this]() {
            return m_pid;
        },
        [this](const QString& event, const QVariantMap& payload) {
            appendScanTelemetry(event, payload);
        },
        [this](uint64_t address, killcore::ValueType type, const QString& valueText) {
            persistWriteHistorySequenceEntry(address, type, valueText);
        },
        [this](const QVariantMap& info) {
            emit writeDidNotHold(info);
        },
        m_lastBatchStartIndex,
        m_lastBatchEndIndex,
        this);
    m_ai.init();
    KE_LOG_INFO() << "ApplicationController initialized";
}

ApplicationController::~ApplicationController() {
    m_clrInspectorBridge.reset();
    m_debugFeatureManager.reset();
    KE_LOG_INFO() << "ApplicationController destroyed";
}

ScanStateAccess ApplicationController::scanState() {
    return ScanStateAccess(m_candidates, m_previousCandidates, m_snapshot);
}

ScanStateAccess ApplicationController::scanState() const {
    return const_cast<ApplicationController*>(this)->scanState();
}

AutoWriteStateAccess ApplicationController::autoWriteState() {
    return AutoWriteStateAccess(m_writeHistory, m_lastAutoWriteTargets, m_chatMemoryTargets);
}

bool ApplicationController::hasAddressBeenWriteVerified(uint64_t address) const {
    for (const auto& record : m_writeHistory) {
        if (record.address == address) {
            return true;
        }
    }
    return false;
}

void ApplicationController::detectStableCandidateGroup(
    killcore::NextScanMode mode,
    const QList<killcore::Candidate>& survivors,
    QVariantMap* result) {
    constexpr int kStableGroupMaxSize = 5;
    constexpr int kStableGroupCycleThreshold = 2;

    if (mode == killcore::NextScanMode::Exact
        && !survivors.isEmpty()
        && survivors.size() <= kStableGroupMaxSize) {
        QSet<uint64_t> currentGroup;
        for (const auto& candidate : survivors) {
            currentGroup.insert(candidate.address);
        }
        if (currentGroup == m_stableCandidateGroup) {
            ++m_stableCandidateGroupCycles;
        } else {
            m_stableCandidateGroup = currentGroup;
            m_stableCandidateGroupCycles = 1;
        }
        if (m_stableCandidateGroupCycles >= kStableGroupCycleThreshold) {
            QVariantList stableAddresses;
            for (uint64_t addr : currentGroup) {
                stableAddresses.append(QString::number(addr, 16).toUpper());
            }
            (*result)["stableGroupCycles"] = m_stableCandidateGroupCycles;
            (*result)["stableGroupAddresses"] = stableAddresses;
            (*result)["stableGroupHint"] = QString(
                "%1 candidat(s) restent identiques depuis %2 cycles de next scan — "
                "probablement des copies redondantes de la même valeur. Une écriture "
                "isolée sur un seul risque d'être annulée silencieusement ; essaie "
                "writeMemoryValuesAtomic() pour les écrire tous en même temps.")
                .arg(currentGroup.size())
                .arg(m_stableCandidateGroupCycles);
        }
    } else {
        m_stableCandidateGroup.clear();
        m_stableCandidateGroupCycles = 0;
    }
}

void ApplicationController::persistWriteHistorySequenceEntry(uint64_t address, killcore::ValueType type, const QString& valueText) {
    if (m_pid <= 0) {
        return;
    }
    QString module;
    uint64_t moduleOffset = 0;
    if (!resolveModuleOffset(static_cast<uint32_t>(m_pid), address, &module, &moduleOffset)) {
        return;
    }

    QSettings settings;
    const QString gameKey = autoResolverGameKey(processName());
    settings.beginGroup(QString("writeHistory/process/%1").arg(gameKey));

    QJsonArray sequence = QJsonDocument::fromJson(settings.value("sequence").toByteArray()).array();

    QJsonObject entry;
    entry["module"] = module;
    entry["moduleOffset"] = QString::number(moduleOffset, 16);
    entry["valueType"] = killcore::valueTypeToString(type);
    entry["value"] = valueText;
    entry["writtenAt"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    sequence.append(entry);

    constexpr int kWriteHistorySequenceLimit = 50;
    while (sequence.size() > kWriteHistorySequenceLimit) {
        sequence.removeAt(0);
    }
    settings.setValue("sequence", QJsonDocument(sequence).toJson(QJsonDocument::Compact));
    settings.endGroup();
}

// ---------------------------------------------------------------------------
// Propriétés
// ---------------------------------------------------------------------------
QString ApplicationController::version() const {
    return QString("%1.%2.%3")
        .arg(KILLENGINE_VERSION_MAJOR)
        .arg(KILLENGINE_VERSION_MINOR)
        .arg(KILLENGINE_VERSION_PATCH);
}

bool ApplicationController::isAttached() const {
    return m_attached;
}

QString ApplicationController::processName() const {
    return m_processName;
}

// ---------------------------------------------------------------------------
// Slots
// ---------------------------------------------------------------------------
QString ApplicationController::getVersion() const {
    return version();
}

QVariantList ApplicationController::getProcesses() const {
    QVariantList result;

    auto processes = killcore::ProcessEnumerator::enumerate();
    std::sort(processes.begin(), processes.end(),
              [](const killcore::ProcessInfo& a, const killcore::ProcessInfo& b) {
                  if (a.hasWindow != b.hasWindow) {
                      return a.hasWindow;
                  }
                  return a.name.toLower() < b.name.toLower();
              });

    for (const auto& p : processes) {
        QVariantMap entry;
        entry["pid"] = static_cast<int>(p.pid);
        entry["name"] = p.name;
        entry["path"] = p.executablePath;
        entry["arch"] = killcore::ProcessEnumerator::architectureToString(p.arch);
        entry["hasWindow"] = p.hasWindow;
        entry["moduleCount"] = p.modules.size();
        result.append(entry);
    }

    KE_LOG_DEBUG() << "getProcesses() returned " << result.size() << " processes";
    return result;
}

QVariantList ApplicationController::getProcessModules(int pid) const {
    QVariantList result;

    if (pid <= 0) {
        return result;
    }

    auto modules = killcore::ProcessEnumerator::enumerateModules(static_cast<uint32_t>(pid));
    for (const auto& module : modules) {
        result.append(moduleToVariantMap(module));
    }

    KE_LOG_DEBUG() << "getProcessModules(pid=" << pid << ") returned "
                   << result.size() << " modules";
    return result;
}

QVariantMap ApplicationController::resolveSymbolAddress(const QString& moduleName, const QString& functionName) const {
    QVariantMap result;
    result["success"] = false;
    result["module"] = moduleName;
    result["function"] = functionName;

    if (!m_handle.isValid()) {
        result["error"] = "Aucun processus attaché.";
        return result;
    }
    if (moduleName.trimmed().isEmpty() || functionName.trimmed().isEmpty()) {
        result["error"] = "Module et fonction requis.";
        return result;
    }

    uint64_t address = 0;
    QString error;
    if (!killcore::resolveRemoteExportAddress(m_handle, moduleName, functionName, &address, &error)) {
        result["error"] = error.isEmpty() ? "Résolution de symbole échouée." : error;
        return result;
    }

    result["success"] = true;
    result["address"] = QString::number(address, 16);
    result["error"] = "";
    return result;
}

QVariantMap ApplicationController::listModuleExports(const QString& moduleName, const QString& filterSubstring, int maxNames) const {
    QVariantMap result;
    result["success"] = false;
    result["module"] = moduleName;
    QVariantList namesList;
    result["names"] = namesList;

    if (!m_handle.isValid()) {
        result["error"] = "Aucun processus attaché.";
        return result;
    }
    if (moduleName.trimmed().isEmpty()) {
        result["error"] = "Module requis.";
        return result;
    }

    QStringList names;
    QString error;
    if (!killcore::listRemoteExportNames(m_handle, moduleName, filterSubstring, maxNames, &names, &error)) {
        result["error"] = error.isEmpty() ? "Listage des exports échoué." : error;
        return result;
    }

    for (const auto& name : names) {
        namesList.append(name);
    }

    result["success"] = true;
    result["names"] = namesList;
    result["count"] = namesList.size();
    result["error"] = "";
    return result;
}

QVariantMap ApplicationController::discoverProcessSaveFiles(int maxResults) const {
    return m_saveFileInvestigator->discoverProcessSaveFiles(maxResults);
}

QVariantMap ApplicationController::inspectProcessLocalSettings(int maxValues) const {
    return m_saveFileInvestigator->inspectProcessLocalSettings(maxValues);
}

QVariantMap ApplicationController::readProcessSaveFileText(const QString& path, int maxBytes) const {
    return m_saveFileInvestigator->readProcessSaveFileText(path, maxBytes);
}

QVariantMap ApplicationController::watchSaveFileForChanges(const QString& path, const QVariantMap& options) {
    return m_saveFileInvestigator->watchSaveFileForChanges(path, options);
}

QVariantMap ApplicationController::startSaveFileWatchAsync(const QString& path, const QVariantMap& options) {
    return m_saveFileInvestigator->startSaveFileWatchAsync(path, options);
}

QVariantMap ApplicationController::cancelSaveFileWatch() {
    return m_saveFileInvestigator->cancelSaveFileWatch();
}

QVariantMap ApplicationController::patchProcessSaveFileBytes(const QString& path, const QString& findHex, const QString& replaceHex) {
    return m_saveFileInvestigator->patchProcessSaveFileBytes(path, findHex, replaceHex);
}

bool ApplicationController::attachProcess(int pid) {
    KE_LOG_INFO() << "attachProcess(pid=" << pid << ")";

    m_debugFeatureManager->stopBreakpointFreeze();
    m_debugFeatureManager->resetHardwareBreakpointStateForPreviousTarget(m_pid);

    // Close any existing handle
    m_handle.close();

    if (!m_handle.open(static_cast<uint32_t>(pid), killcore::ProcessAccess::ReadOnly)) {
        KE_LOG_ERROR() << "Failed to open process PID " << pid;
        emit errorOccurred("Accès insuffisant au processus.");
        return false;
    }

    m_pid = pid;
    m_attached = true;
    m_processName = m_handle.executableName();
    scanState().clearCandidates();
    clearCandidateUndo();
    clearCandidateValueHistory();
    scanState().clearSnapshot();
    m_writeFreezeCoreManager->clearSessionState();
    m_codePatchManager->clearSessionState();
    m_activeProfileTargets.clear();
    m_autoWriteValueHistory.clear();

    KE_LOG_INFO() << "Attached to PID " << pid << " (" << m_processName.toStdString() << ")";

    emit attachmentChanged();
    return true;
}

void ApplicationController::detachProcess() {
    KE_LOG_INFO() << "detachProcess()";

    if (m_scanningCoreManager->isScanInProgress()) {
        m_scanningCoreManager->requestCancelActiveScan();
        KE_LOG_INFO() << "Detach deferred because a scan is still running.";
        return;
    }
    if (m_debugFeatureManager->deferDetachIfBusy()) {
        return;
    }

    m_debugFeatureManager->stopBreakpointFreeze();
    m_debugFeatureManager->stopInjectionSessions();
    detachClrInspector();
    m_debugFeatureManager->resetHardwareBreakpointStateForPreviousTarget(m_pid);

    m_handle.close();
    m_pid = 0;
    m_attached = false;
    m_processName.clear();
    scanState().clearCandidates();
    clearCandidateUndo();
    clearCandidateValueHistory();
    scanState().clearSnapshot();
    m_freezeHotkeyOverlayManager->clearFreezeState();
    m_writeFreezeCoreManager->clearSessionState();
    m_activeProfileTargets.clear();
    m_autoWriteValueHistory.clear();

    emit attachmentChanged();
}

bool ApplicationController::rememberCandidatesForUndo(QString* error) {
    auto state = scanState();
    auto& candidates = state.candidates();
    auto& previousCandidates = state.previousCandidates();
    if (candidates.isEmpty()) {
        clearCandidateUndo();
        return true;
    }

    previousCandidates = candidates.clone(error);
    m_hasPreviousCandidates = previousCandidates.size() > 0;
    return m_hasPreviousCandidates;
}

void ApplicationController::clearCandidateUndo() {
    scanState().clearPreviousCandidates();
    m_hasPreviousCandidates = false;
}

void ApplicationController::clearCandidateValueHistory() {
    m_candidateValueHistory.clear();
}

void ApplicationController::recordCandidateObservations(const QVariantList& observations) {
    for (const auto& item : observations) {
        const QVariantMap observation = item.toMap();
        uint64_t address = 0;
        if (!parseHexAddress(observation.value("address").toString(), &address)) {
            continue;
        }

        if (!m_candidateValueHistory.contains(address)
            && m_candidateValueHistory.size() >= kCandidateHistoryMaxAddresses) {
            continue;
        }

        auto history = m_candidateValueHistory.value(address);
        history.append(observation);
        while (history.size() > kCandidateHistoryMaxEntriesPerAddress) {
            history.removeFirst();
        }
        m_candidateValueHistory.insert(address, history);
    }
}

QVariantList ApplicationController::candidateValueHistory(uint64_t address) const {
    return m_candidateValueHistory.value(address);
}

void ApplicationController::enrichSuggestedWritesWithHistory(QVariantList* suggestions) const {
    if (!suggestions) {
        return;
    }

    for (int i = 0; i < suggestions->size(); ++i) {
        QVariantMap suggestion = suggestions->at(i).toMap();
        uint64_t address = 0;
        if (parseHexAddress(suggestion.value("address").toString(), &address)) {
            const auto history = candidateValueHistory(address);
            if (!history.isEmpty()) {
                suggestion["valueHistory"] = history;
            }
        }
        (*suggestions)[i] = suggestion;
    }
}

QVariantList ApplicationController::filterAutoWriteSuggestionsByRegion(
    const QVariantList& suggestions,
    QVariantList* rejected) const {
    QVariantList accepted;
    const auto regions = killcore::MemoryMap::snapshot(m_handle);

    for (const auto& item : suggestions) {
        QVariantMap suggestion = item.toMap();
        uint64_t address = 0;
        if (!parseHexAddress(suggestion.value("address").toString(), &address)) {
            suggestion["noiseFilterReason"] = "Adresse invalide.";
            if (rejected) rejected->append(suggestion);
            continue;
        }

        bool foundRegion = false;
        killcore::MemoryRegion matchedRegion;
        for (const auto& region : regions) {
            const uint64_t end = region.baseAddress + region.size;
            if (address >= region.baseAddress && address < end) {
                matchedRegion = region;
                foundRegion = true;
                break;
            }
        }

        if (!foundRegion) {
            suggestion["noiseFilterReason"] = "Région mémoire introuvable.";
            if (rejected) rejected->append(suggestion);
            continue;
        }

        suggestion["regionBase"] = QString::number(matchedRegion.baseAddress, 16);
        suggestion["regionType"] = killcore::memoryTypeToString(matchedRegion.type);
        suggestion["regionWritable"] = matchedRegion.writable;
        suggestion["regionReadable"] = matchedRegion.readable;
        suggestion["regionGuarded"] = matchedRegion.guarded;

        const bool relevantType = matchedRegion.type == killcore::MemoryType::Private
            || matchedRegion.type == killcore::MemoryType::Mapped;
        if (!matchedRegion.readable || !matchedRegion.writable || matchedRegion.guarded || !relevantType) {
            suggestion["noiseFilterReason"] = "Région peu pertinente pour une valeur de jeu.";
            if (rejected) rejected->append(suggestion);
            continue;
        }

        accepted.append(suggestion);
    }

    return accepted;
}

QVariantMap ApplicationController::getMemoryMap() const {
    QVariantMap result;
    QVariantList regionList;

    if (!m_handle.isValid()) {
        result["attached"] = false;
        result["regions"] = regionList;
        result["stats"] = memoryStatsToVariantMap({});
        return result;
    }

    const auto regions = killcore::MemoryMap::snapshot(m_handle);
    for (const auto& region : regions) {
        regionList.append(memoryRegionToVariantMap(region));
    }

    result["attached"] = true;
    result["processName"] = m_processName;
    result["pid"] = m_pid;
    result["regions"] = regionList;
    result["stats"] = memoryStatsToVariantMap(killcore::MemoryMap::stats(regions));

    KE_LOG_DEBUG() << "getMemoryMap() returned " << regionList.size()
                   << " regions for PID " << m_pid;
    return result;
}

QVariantMap ApplicationController::readMemoryPreview(const QString& addressHex, int size) const {
    QVariantMap result;
    result["success"] = false;

    if (!m_handle.isValid()) {
        result["error"] = "Aucun processus attaché.";
        return result;
    }

    bool ok = false;
    QString normalized = addressHex.trimmed();
    if (normalized.startsWith("0x", Qt::CaseInsensitive)) {
        normalized = normalized.mid(2);
    }
    const uint64_t address = normalized.toULongLong(&ok, 16);
    if (!ok || address == 0) {
        result["error"] = "Adresse invalide.";
        return result;
    }

    const int boundedSize = std::clamp(size, 1, 256);
    killcore::MemoryReader reader(m_handle);
    const auto read = reader.readChunked(address, static_cast<size_t>(boundedSize), 4096);

    result["success"] = read.success || read.partial;
    result["partial"] = read.partial;
    result["cancelled"] = read.cancelled;
    result["bytesRead"] = static_cast<int>(read.bytesRead);
    result["requestedBytes"] = static_cast<int>(read.requestedBytes);
    result["error"] = read.errorMessage;
    result["hex"] = QString::fromLatin1(read.data.toHex(' ').toUpper());
    return result;
}

QVariantMap ApplicationController::readMemoryBlock(const QString& addressHex, int size) const {
    QVariantMap result;
    result["success"] = false;

    if (!m_handle.isValid()) {
        result["error"] = "Aucun processus attaché.";
        return result;
    }

    bool ok = false;
    QString normalized = addressHex.trimmed();
    if (normalized.startsWith("0x", Qt::CaseInsensitive)) {
        normalized = normalized.mid(2);
    }
    const uint64_t address = normalized.toULongLong(&ok, 16);
    if (!ok || address == 0) {
        result["error"] = "Adresse invalide.";
        return result;
    }

    const int boundedSize = std::clamp(size, 16, 64 * 1024);
    killcore::MemoryReader reader(m_handle);
    const auto read = reader.readChunked(address, static_cast<size_t>(boundedSize), 4096);

    result["success"] = read.success || read.partial;
    result["partial"] = read.partial;
    result["cancelled"] = read.cancelled;
    result["bytesRead"] = static_cast<int>(read.bytesRead);
    result["requestedBytes"] = static_cast<int>(read.requestedBytes);
    result["error"] = read.errorMessage;
    result["hex"] = QString::fromLatin1(read.data.toHex(' ').toUpper());
    return result;
}

QVariantMap ApplicationController::analyzeStructureMemory(const QString& addressHex, int size) const {
    QVariantMap result;
    QVariantList fields;
    result["success"] = false;
    result["fields"] = fields;

    if (!m_handle.isValid()) {
        result["error"] = "Aucun processus attaché.";
        return result;
    }

    bool ok = false;
    QString normalized = addressHex.trimmed();
    if (normalized.startsWith("0x", Qt::CaseInsensitive)) {
        normalized = normalized.mid(2);
    }
    const uint64_t address = normalized.toULongLong(&ok, 16);
    if (!ok || address == 0) {
        result["error"] = "Adresse invalide.";
        return result;
    }

    const int boundedSize = std::clamp(size, 16, 1024);
    killcore::MemoryReader reader(m_handle);
    const auto read = reader.readChunked(address, static_cast<size_t>(boundedSize), 4096);
    if (!read.success && !read.partial) {
        result["error"] = read.errorMessage.isEmpty() ? QStringLiteral("Lecture structure impossible.") : read.errorMessage;
        result["bytesRead"] = static_cast<int>(read.bytesRead);
        result["requestedBytes"] = static_cast<int>(read.requestedBytes);
        appendScanTelemetry("structure_analyze", result);
        return result;
    }

    killcore::StructureAnalysisOptions options;
    options.windowSize = static_cast<int>(read.bytesRead);
    const auto analysis = killcore::analyzeStructure(read.data, address, options);
    if (!analysis.success) {
        result["error"] = analysis.error;
        appendScanTelemetry("structure_analyze", result);
        return result;
    }

    for (const auto& field : analysis.fields) {
        QVariantMap item;
        item["offset"] = field.offset;
        item["address"] = QStringLiteral("%1").arg(address + static_cast<uint64_t>(field.offset), 0, 16).toUpper();
        item["type"] = killcore::fieldTypeToString(field.type);
        item["value"] = field.interpretedValue;
        item["valueText"] = killcore::fieldToReadable(field);
        item["rawHex"] = field.rawHex;
        item["changed"] = field.changed;
        if (!field.label.isEmpty()) {
            item["label"] = field.label;
        }
        fields.append(item);
    }

    result["success"] = true;
    result["partial"] = read.partial;
    result["cancelled"] = read.cancelled;
    result["baseAddress"] = QStringLiteral("%1").arg(address, 0, 16).toUpper();
    result["bytesRead"] = static_cast<int>(read.bytesRead);
    result["requestedBytes"] = static_cast<int>(read.requestedBytes);
    result["fieldCount"] = fields.size();
    result["fields"] = fields;
    result["error"] = read.errorMessage;

    appendScanTelemetry("structure_analyze", {
        {"success", true},
        {"address", result.value("baseAddress")},
        {"bytesRead", result.value("bytesRead")},
        {"fieldCount", result.value("fieldCount")},
        {"partial", result.value("partial")},
    });
    return result;
}

QVariantMap ApplicationController::inferStructureInstanceDelta(
    const QString& baseAddressAHex,
    const QString& fieldAddressAHex,
    const QString& baseAddressBHex,
    const QString& fieldAddressBHex,
    const QVariantMap& optionsMap) const {

    auto hexAddress = [](uint64_t address) {
        return QStringLiteral("%1").arg(address, 0, 16).toUpper();
    };

    QVariantMap result;
    result["success"] = false;
    result["candidates"] = QVariantList{};

    uint64_t baseA = 0;
    uint64_t fieldA = 0;
    uint64_t baseB = 0;
    uint64_t fieldB = 0;
    if (!parseHexAddress(baseAddressAHex, &baseA) ||
        !parseHexAddress(fieldAddressAHex, &fieldA) ||
        !parseHexAddress(baseAddressBHex, &baseB) ||
        !parseHexAddress(fieldAddressBHex, &fieldB)) {
        result["error"] = QStringLiteral("Adresses invalides.");
        return result;
    }

    killcore::StructureInstanceDeltaOptions options;
    options.beforeCount = std::clamp(optionsMap.value("beforeCount", 2).toInt(), 0, 16);
    options.afterCount = std::clamp(optionsMap.value("afterCount", 4).toInt(), 0, 16);

    const auto inference = killcore::inferStructureInstanceDelta(baseA, fieldA, baseB, fieldB, options);
    result["success"] = inference.success;
    result["error"] = inference.error;
    result["warning"] = inference.warning;
    result["compatibleLayout"] = inference.compatibleLayout;
    result["baseAddressA"] = hexAddress(inference.baseAddressA);
    result["baseAddressB"] = hexAddress(inference.baseAddressB);
    result["fieldAddressA"] = hexAddress(inference.fieldAddressA);
    result["fieldAddressB"] = hexAddress(inference.fieldAddressB);
    result["fieldOffsetA"] = static_cast<qlonglong>(inference.fieldOffsetA);
    result["fieldOffsetB"] = static_cast<qlonglong>(inference.fieldOffsetB);
    result["fieldOffsetDelta"] = static_cast<qlonglong>(inference.fieldOffsetDelta);
    result["instanceDelta"] = static_cast<qlonglong>(inference.instanceDelta);
    result["fieldAddressDelta"] = static_cast<qlonglong>(inference.fieldAddressDelta);

    QVariantList candidates;
    for (const auto& candidate : inference.candidates) {
        QVariantMap item;
        item["relativeIndex"] = candidate.relativeIndex;
        item["baseAddress"] = hexAddress(candidate.baseAddress);
        item["fieldAddress"] = hexAddress(candidate.fieldAddress);
        item["inputInstance"] = candidate.inputInstance;
        candidates.append(item);
    }
    result["candidates"] = candidates;
    result["candidateCount"] = candidates.size();

    appendScanTelemetry("structure_instance_delta", {
        {"success", result.value("success")},
        {"compatibleLayout", result.value("compatibleLayout")},
        {"baseAddressA", result.value("baseAddressA")},
        {"baseAddressB", result.value("baseAddressB")},
        {"fieldOffsetA", result.value("fieldOffsetA")},
        {"fieldOffsetB", result.value("fieldOffsetB")},
        {"instanceDelta", result.value("instanceDelta")},
        {"candidateCount", result.value("candidateCount")},
    });

    return result;
}

QVariantMap ApplicationController::scanUiStrings(const QString& value, const QVariantMap& optionsMap) const {
    return m_uiStringInvestigator->scanUiStrings(value, optionsMap);
}

QVariantMap ApplicationController::trackUiStringCandidates(const QVariantList& candidates, const QString& value) const {
    return m_uiStringInvestigator->trackUiStringCandidates(candidates, value);
}

QVariantMap ApplicationController::analyzeUiStringSources(
    const QVariantMap& stringCandidate,
    const QString& value,
    const QVariantMap& optionsMap) const {
    return m_uiStringInvestigator->analyzeUiStringSources(stringCandidate, value, optionsMap);
}

QVariantMap ApplicationController::scanMemoryWindow(
    const QString& addressHex,
    const QString& value,
    const QVariantMap& optionsMap) const {
    return m_uiStringInvestigator->scanMemoryWindow(addressHex, value, optionsMap);
}

QVariantMap ApplicationController::trackUiStringSources(const QVariantList& sourceCandidates, const QString& value) const {
    return m_uiStringInvestigator->trackUiStringSources(sourceCandidates, value);
}

QVariantMap ApplicationController::inspectUiStringOrigins(
    const QVariantList& stringCandidates,
    const QVariantMap& optionsMap) const {
    return m_uiStringInvestigator->inspectUiStringOrigins(stringCandidates, optionsMap);
}

QVariantMap ApplicationController::startUiStringInvestigation(
    const QVariantList& stringCandidates,
    const QVariantList& sourceCandidates,
    const QVariantMap& optionsMap) {
    return m_uiStringInvestigator->startUiStringInvestigation(stringCandidates, sourceCandidates, optionsMap);
}

QVariantMap ApplicationController::finishUiStringInvestigation(const QVariantMap& optionsMap) {
    return m_uiStringInvestigator->finishUiStringInvestigation(optionsMap);
}

QVariantMap ApplicationController::startChangedPagesDiff(const QVariantMap& options) {
    return m_uiStringInvestigator->startChangedPagesDiff(options);
}

QVariantMap ApplicationController::finishChangedPagesDiff(
    const QString& previousValue,
    const QString& currentValue,
    const QVariantMap& options) {
    return m_uiStringInvestigator->finishChangedPagesDiff(previousValue, currentValue, options);
}

QVariantMap ApplicationController::startChangedPagesSession(const QVariantMap& options) {
    return m_uiStringInvestigator->startChangedPagesSession(options);
}

QVariantMap ApplicationController::applyChangedPagesRound(
    const QString& previousValue,
    const QString& currentValue,
    const QVariantMap& options) {
    return m_uiStringInvestigator->applyChangedPagesRound(previousValue, currentValue, options);
}

QVariantMap ApplicationController::getChangedPagesConsensus(const QVariantMap& options) const {
    return m_uiStringInvestigator->getChangedPagesConsensus(options);
}

QVariantMap ApplicationController::stopChangedPagesSession() {
    return m_uiStringInvestigator->stopChangedPagesSession();
}

QVariantMap ApplicationController::startExactScan(const QString& value, const QString& valueType) {
    return m_scanningCoreManager->startExactScan(value, valueType);
}

QVariantMap ApplicationController::startExactScanMultiType(const QString& value, const QString& valueType) {
    return m_scanningCoreManager->startExactScanMultiType(value, valueType);
}

QVariantMap ApplicationController::startExactScanExpert(const QString& value, const QString& valueType, const QVariantMap& expertOptions) {
    return m_scanningCoreManager->startExactScanExpert(value, valueType, expertOptions);
}

QVariantMap ApplicationController::scanEncryptedValue(const QString& value, const QString& valueType, const QVariantMap& options) {
    return m_scanningCoreManager->scanEncryptedValue(value, valueType, options);
}

QVariantMap ApplicationController::startExactScanAsync(const QString& value, const QString& valueType, const QVariantMap& expertOptions) {
    return m_scanningCoreManager->startExactScanAsync(value, valueType, expertOptions);
}

QVariantMap ApplicationController::cancelActiveScan() {
    return m_scanningCoreManager->cancelActiveScan();
}

QVariantMap ApplicationController::nextScanAsync(const QString& mode, const QString& value) {
    return m_scanningCoreManager->nextScanAsync(mode, value);
}

QVariantMap ApplicationController::nextScan(const QString& mode, const QString& value) {
    return m_scanningCoreManager->nextScan(mode, value);
}

QVariantMap ApplicationController::undoCandidateScan() {
    return m_scanningCoreManager->undoCandidateScan();
}

QVariantMap ApplicationController::getCandidates(int pageIndex, int pageSize, const QString& addressFilter) const {
    return m_scanningCoreManager->getCandidates(pageIndex, pageSize, addressFilter);
}

QVariantMap ApplicationController::captureUnknownSnapshot() {
    return m_scanningCoreManager->captureUnknownSnapshot();
}

QVariantMap ApplicationController::captureUnknownSnapshotWithOptions(const QVariantMap& expertOptions) {
    return m_scanningCoreManager->captureUnknownSnapshotWithOptions(expertOptions);
}

QVariantMap ApplicationController::captureUnknownSnapshotAsync() {
    return m_scanningCoreManager->captureUnknownSnapshotAsync();
}

QVariantMap ApplicationController::captureUnknownSnapshotAsyncWithOptions(const QVariantMap& expertOptions) {
    return m_scanningCoreManager->captureUnknownSnapshotAsyncWithOptions(expertOptions);
}

QVariantMap ApplicationController::unknownNextScan(const QString& mode, const QString& valueType) {
    return m_scanningCoreManager->unknownNextScan(mode, valueType);
}

QVariantMap ApplicationController::unknownNextScanAsync(const QString& mode, const QString& valueType) {
    return m_scanningCoreManager->unknownNextScanAsync(mode, valueType);
}

QVariantMap ApplicationController::writeMemoryValue(const QString& addressHex, const QString& valueType, const QString& value) {
    return m_writeFreezeCoreManager->writeMemoryValue(addressHex, valueType, value);
}

// H3 (docs/STRATEGY_ROOM.md, session Solitaire du 19/08/2026) : certaines
// cibles maintiennent plusieurs copies redondantes d'une meme valeur et
// resynchronisent silencieusement celle qui diverge d'une ecriture isolee.
// Deux appels writeMemoryValue separes (meme rapproches dans le temps)
// laissent une fenetre a l'ordonnanceur Windows pour que la cible detecte le
// desaccord entre-temps. Suspendre toutes les threads de la cible pendant la
// rafale d'ecritures ferme cette fenetre — pas un vrai atomique CPU, mais
// suffisant dans le cas reel qui a motive cette fonction (voir
// docs/PHASE_TRACKER.md PHASE 26).
QVariantMap ApplicationController::writeMemoryValuesAtomic(const QVariantList& targets, const QVariantMap& options) {
    return m_writeFreezeCoreManager->writeMemoryValuesAtomic(targets, options);
}

QVariantMap ApplicationController::writeMemoryValuesWithVariants(const QVariantList& targets, const QString& value) {
    return m_writeFreezeCoreManager->writeMemoryValuesWithVariants(targets, value);
}

QVariantMap ApplicationController::writeMemoryValueConfirmed(
    const QString& addressHex,
    const QString& valueType,
    const QString& value,
    bool persistHistory) {
    return m_writeFreezeCoreManager->writeMemoryValueConfirmed(addressHex, valueType, value, persistHistory);
}

QVariantMap ApplicationController::rollbackLastWriteBatch() {
    return m_writeFreezeCoreManager->rollbackLastWriteBatch();
}

QVariantMap ApplicationController::rollbackLastWrite() {
    return m_writeFreezeCoreManager->rollbackLastWrite();
}

QVariantMap ApplicationController::findWhatWrites(const QString& addressHex, const QVariantMap& options) {
    return m_debugFeatureManager->findWhatWrites(addressHex, options);
}

QVariantMap ApplicationController::analyzeFieldStability(const QString& addressHex, const QVariantMap& options) {
    QVariantMap result;
    result["success"] = false;
    result["address"] = addressHex;

    if (!m_attached || m_pid <= 0) {
        result["error"] = "Aucun processus attaché.";
        return result;
    }

    uint64_t address = 0;
    if (!parseHexAddress(addressHex, &address)) {
        result["error"] = "Adresse invalide.";
        return result;
    }

    const int sizeBytes = std::clamp(options.value("size", 4).toInt(), 1, 8);
    killcore::BreakpointSize breakpointSize = killcore::BreakpointSize::DWord;
    if (sizeBytes <= 1) {
        breakpointSize = killcore::BreakpointSize::Byte;
    } else if (sizeBytes <= 2) {
        breakpointSize = killcore::BreakpointSize::Word;
    } else if (sizeBytes <= 4) {
        breakpointSize = killcore::BreakpointSize::DWord;
    } else {
        breakpointSize = killcore::BreakpointSize::QWord;
    }

    const int captureWindowMs = std::clamp(options.value("captureWindowMs", 800).toInt(), 250, 15000);
    const int maxHitsInt = std::clamp(options.value("maxHits", 12).toInt(), 1, 100);

    KE_LOG_INFO() << "analyzeFieldStability(address=0x" << std::hex << address
                  << ", pid=" << std::dec << m_pid
                  << ", size=" << sizeBytes
                  << ", captureWindowMs=" << captureWindowMs
                  << ", maxHits=" << maxHitsInt << ")";

    const auto classification = killcore::classifyFieldStabilityLive(
        static_cast<uint32_t>(m_pid),
        address,
        breakpointSize,
        captureWindowMs,
        static_cast<size_t>(maxHitsInt));

    QString verdictLabel;
    switch (classification.verdict) {
        case killcore::FieldStabilityVerdict::NoWritesObserved: verdictLabel = "no_writes_observed"; break;
        case killcore::FieldStabilityVerdict::InsufficientData: verdictLabel = "insufficient_data"; break;
        case killcore::FieldStabilityVerdict::LikelyDerivedDisplay: verdictLabel = "likely_derived_display"; break;
        case killcore::FieldStabilityVerdict::LikelyEventDriven: verdictLabel = "likely_event_driven"; break;
    }

    result["success"] = true;
    result["verdict"] = verdictLabel;
    result["writeCount"] = static_cast<int>(classification.writeCount);
    result["distinctInstructionCount"] = static_cast<int>(classification.distinctInstructionCount);
    result["dominantInstructionPointer"] = classification.dominantInstructionPointer != 0
        ? QString::number(classification.dominantInstructionPointer, 16).toUpper()
        : QString();
    result["dominantInstructionShare"] = classification.dominantInstructionShare;
    result["meanIntervalMs"] = classification.meanIntervalMs;
    result["intervalCoefficientOfVariation"] = classification.intervalCoefficientOfVariation;
    result["rationale"] = classification.rationale;
    result["warning"] = "Cette fonction attache KillEngine comme debugger au processus cible pendant la capture (lecture seule, aucune écriture).";
    return result;
}

QVariantMap ApplicationController::findWhatAccesses(const QString& addressHex, const QVariantMap& options) {
    return m_debugFeatureManager->findWhatAccesses(addressHex, options);
}

QVariantMap ApplicationController::findWhatExecutes(const QString& instructionAddressHex, const QVariantMap& options) {
    return m_debugFeatureManager->findWhatExecutes(instructionAddressHex, options);
}

QVariantMap ApplicationController::readAttachedWindowText(const QVariantMap& options) const {
    QVariantMap result;
    QVariantList windows;
    result["success"] = false;
    result["windows"] = windows;

    if (!m_attached || m_pid <= 0) {
        result["error"] = "Aucun processus attaché.";
        return result;
    }

    const int maxWindows = std::clamp(options.value("maxWindows", 100).toInt(), 1, 500);
    const int maxChildrenPerWindow = std::clamp(options.value("maxChildrenPerWindow", 200).toInt(), 0, 1000);
    const bool includeAllVisible = options.value("includeAllVisible", true).toBool();
    QString titleContains = options.value("titleContains", "Solitaire").toString().trimmed();

#ifdef Q_OS_WIN
    WindowTextEnumState state;
    state.pid = static_cast<DWORD>(m_pid);
    state.maxWindows = maxWindows;
    state.maxChildrenPerWindow = maxChildrenPerWindow;
    state.includeAllVisible = includeAllVisible;
    state.titleContains = titleContains;

    EnumWindows(enumWindowTextProc, reinterpret_cast<LPARAM>(&state));
    windows = state.windows;
    result["success"] = true;
    result["windows"] = windows;
    result["windowCount"] = windows.size();
    result["includeAllVisible"] = includeAllVisible;
    result["titleContains"] = titleContains;
    result["error"] = "";
#else
    Q_UNUSED(options);
    Q_UNUSED(includeAllVisible);
    Q_UNUSED(titleContains);
    result["error"] = "Lecture de texte de fenêtre disponible seulement sous Windows.";
#endif

    appendScanTelemetry("attached_window_text_read", {
        {"success", result.value("success")},
        {"windowCount", result.value("windowCount", 0)},
        {"error", result.value("error")},
    });
    return result;
}

QVariantMap ApplicationController::readUiAutomationTree(const QVariantMap& options) const {
    QVariantMap result;
    QVariantList elements;
    result["success"] = false;
    result["elements"] = elements;

    if (!m_attached || m_pid <= 0) {
        result["error"] = "Aucun processus attaché.";
        return result;
    }

#ifdef Q_OS_WIN
    HWND targetHwnd = nullptr;
    const QString hwndHexOption = options.value("hwndHex").toString().trimmed();
    if (!hwndHexOption.isEmpty()) {
        bool ok = false;
        const quintptr hwndValue = static_cast<quintptr>(hwndHexOption.toULongLong(&ok, 16));
        if (ok) {
            targetHwnd = reinterpret_cast<HWND>(hwndValue);
        }
    }
    if (!targetHwnd) {
        // Meme heuristique que readAttachedWindowText : un process UWP
        // n'expose que des fenetres IME cachees sous son propre PID, la
        // vraie fenetre visible (ApplicationFrameWindow) appartient a un
        // PID different.
        WindowTextEnumState state;
        state.pid = static_cast<DWORD>(m_pid);
        state.maxWindows = 20;
        state.maxChildrenPerWindow = 0;
        state.includeAllVisible = true;
        state.titleContains = options.value("titleContains", "Solitaire").toString();
        EnumWindows(enumWindowTextProc, reinterpret_cast<LPARAM>(&state));
        for (const auto& winVariant : state.windows) {
            const auto win = winVariant.toMap();
            if (win.value("visible").toBool() && !win.value("text").toString().isEmpty()) {
                bool ok = false;
                const quintptr hwndValue = static_cast<quintptr>(win.value("hwnd").toString().toULongLong(&ok, 16));
                if (ok && hwndValue) {
                    targetHwnd = reinterpret_cast<HWND>(hwndValue);
                    break;
                }
            }
        }
    }
    if (!targetHwnd) {
        result["error"] = "Aucune fenêtre visible trouvée pour ce processus (essaie hwndHex explicite ou ajuste titleContains).";
        return result;
    }

    const int maxElements = std::clamp(options.value("maxElements", 500).toInt(), 1, 5000);
    const QString filterText = options.value("filterText").toString();

    const HRESULT initHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    // S_FALSE = deja initialise sur ce thread (Qt le fait typiquement) --
    // toujours SUCCEEDED, on doit quand meme equilibrer par CoUninitialize.
    // RPC_E_CHANGED_MODE = deja initialise dans un AUTRE mode par quelqu'un
    // d'autre : pas notre init a nous, mais COM reste utilisable.
    const bool comInitializedHere = SUCCEEDED(initHr);
    const bool comUsable = comInitializedHere || initHr == RPC_E_CHANGED_MODE;
    if (!comUsable) {
        result["error"] = QStringLiteral("CoInitializeEx a échoué (0x%1).").arg(static_cast<uint32_t>(initHr), 0, 16);
        return result;
    }

    auto cleanupCom = [comInitializedHere]() {
        if (comInitializedHere) {
            CoUninitialize();
        }
    };

    Microsoft::WRL::ComPtr<IUIAutomation> automation;
    HRESULT hr = CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER,
                                   __uuidof(IUIAutomation), &automation);
    if (FAILED(hr) || !automation) {
        cleanupCom();
        result["error"] = QStringLiteral("CoCreateInstance(CUIAutomation) a échoué (0x%1).").arg(static_cast<uint32_t>(hr), 0, 16);
        return result;
    }

    Microsoft::WRL::ComPtr<IUIAutomationElement> root;
    hr = automation->ElementFromHandle(targetHwnd, &root);
    if (FAILED(hr) || !root) {
        cleanupCom();
        result["error"] = QStringLiteral("ElementFromHandle (UIA) a échoué pour cette fenêtre (0x%1).").arg(static_cast<uint32_t>(hr), 0, 16);
        return result;
    }

    // Diagnostic : nom/type du root lui-meme, meme si FindAll ne remonte
    // ensuite aucun descendant -- permet de distinguer "mauvais element
    // trouve" de "element correct mais arbre d'accessibilite vide/inactif".
    BSTR rootNameBstr = nullptr;
    root->get_CurrentName(&rootNameBstr);
    result["rootName"] = rootNameBstr ? QString::fromWCharArray(rootNameBstr) : QString();
    if (rootNameBstr) SysFreeString(rootNameBstr);
    CONTROLTYPEID rootControlType = 0;
    root->get_CurrentControlType(&rootControlType);
    result["rootControlType"] = static_cast<int>(rootControlType);

    Microsoft::WRL::ComPtr<IUIAutomationCondition> trueCondition;
    automation->CreateTrueCondition(&trueCondition);

    Microsoft::WRL::ComPtr<IUIAutomationElementArray> found;
    hr = root->FindAll(TreeScope_Subtree, trueCondition.Get(), &found);
    result["findAllHr"] = QStringLiteral("0x%1").arg(static_cast<uint32_t>(hr), 0, 16);
    if (FAILED(hr) || !found) {
        cleanupCom();
        result["error"] = "FindAll (UIA) a échoué -- la cible ne repond peut-etre pas a l'arbre d'accessibilite.";
        return result;
    }

    int count = 0;
    found->get_Length(&count);
    for (int i = 0; i < count && elements.size() < maxElements; ++i) {
        Microsoft::WRL::ComPtr<IUIAutomationElement> element;
        if (FAILED(found->GetElement(i, &element)) || !element) {
            continue;
        }

        BSTR nameBstr = nullptr;
        element->get_CurrentName(&nameBstr);
        const QString nameStr = nameBstr ? QString::fromWCharArray(nameBstr) : QString();
        if (nameBstr) SysFreeString(nameBstr);

        CONTROLTYPEID controlType = 0;
        element->get_CurrentControlType(&controlType);

        // ValuePattern : souvent porteur du texte reel pour les controles
        // texte/edit (un TextBlock XAML expose generalement deja sa valeur
        // via Name, mais ValuePattern couvre les cas ou ce n'est pas le cas).
        QString valueStr;
        Microsoft::WRL::ComPtr<IUnknown> valuePatternUnknown;
        if (SUCCEEDED(element->GetCurrentPattern(UIA_ValuePatternId, &valuePatternUnknown)) && valuePatternUnknown) {
            Microsoft::WRL::ComPtr<IUIAutomationValuePattern> valuePattern;
            if (SUCCEEDED(valuePatternUnknown.As(&valuePattern)) && valuePattern) {
                BSTR valBstr = nullptr;
                if (SUCCEEDED(valuePattern->get_CurrentValue(&valBstr)) && valBstr) {
                    valueStr = QString::fromWCharArray(valBstr);
                    SysFreeString(valBstr);
                }
            }
        }

        if (nameStr.isEmpty() && valueStr.isEmpty()) {
            continue;
        }
        if (!filterText.isEmpty()
            && !nameStr.contains(filterText, Qt::CaseInsensitive)
            && !valueStr.contains(filterText, Qt::CaseInsensitive)) {
            continue;
        }

        RECT rect{};
        element->get_CurrentBoundingRectangle(&rect);

        QVariantMap item;
        item["name"] = nameStr;
        item["value"] = valueStr;
        item["controlType"] = static_cast<int>(controlType);
        item["x"] = static_cast<int>(rect.left);
        item["y"] = static_cast<int>(rect.top);
        item["width"] = static_cast<int>(rect.right - rect.left);
        item["height"] = static_cast<int>(rect.bottom - rect.top);
        elements.append(item);
    }

    cleanupCom();

    result["success"] = true;
    result["elements"] = elements;
    result["elementCount"] = elements.size();
    result["scannedCount"] = count;
    result["hwnd"] = QString::number(reinterpret_cast<quintptr>(targetHwnd), 16).toUpper();
#else
    Q_UNUSED(options);
    result["error"] = "Lecture UI Automation disponible seulement sous Windows.";
#endif

    return result;
}

QVariantMap ApplicationController::findWhatWritesAsync(const QString& addressHex, const QVariantMap& options) {
    return m_debugFeatureManager->findWhatWritesAsync(addressHex, options);
}

QVariantMap ApplicationController::cancelFindWhatWrites() {
    return m_debugFeatureManager->cancelFindWhatWrites();
}

QVariantMap ApplicationController::startPageGuardWatchAsync(const QString& addressHex, const QVariantMap& options) {
    return m_debugFeatureManager->startPageGuardWatchAsync(addressHex, options);
}

QVariantMap ApplicationController::validatePageStability(const QString& addressHex, const QVariantMap& options) {
    return m_debugFeatureManager->validatePageStability(addressHex, options);
}

QVariantMap ApplicationController::cancelPageGuardWatch() {
    return m_debugFeatureManager->cancelPageGuardWatch();
}

QVariantMap ApplicationController::startInProcessBreakpointWatchAsync(const QString& addressHex, const QVariantMap& options) {
    return m_debugFeatureManager->startInProcessBreakpointWatchAsync(addressHex, options);
}

QVariantMap ApplicationController::startInProcessExecuteWatchAsync(const QString& instructionAddressHex, const QVariantMap& options) {
    return m_debugFeatureManager->startInProcessExecuteWatchAsync(instructionAddressHex, options);
}

QVariantMap ApplicationController::startInProcessExecuteWatch(const QString& instructionAddressHex, const QVariantMap& options) {
    return m_debugFeatureManager->startInProcessExecuteWatch(instructionAddressHex, options);
}

QVariantMap ApplicationController::cancelInProcessBreakpointWatch() {
    return m_debugFeatureManager->cancelInProcessBreakpointWatch();
}

QVariantMap ApplicationController::startInProcessBreakpointFreeze(const QString& addressHex, const QString& valueType, const QString& value, const QVariantMap& options) {
    return m_debugFeatureManager->startInProcessBreakpointFreeze(addressHex, valueType, value, options);
}

QVariantMap ApplicationController::stopInProcessBreakpointFreeze() {
    return m_debugFeatureManager->stopInProcessBreakpointFreeze();
}

QVariantMap ApplicationController::getInProcessBreakpointFreezeStats() const {
    return m_debugFeatureManager->getInProcessBreakpointFreezeStats();
}

QVariantMap ApplicationController::startSpeedhack(double factor) {
    return m_debugFeatureManager->startSpeedhack(factor);
}

QVariantMap ApplicationController::setSpeedhackFactor(double factor) {
    return m_debugFeatureManager->setSpeedhackFactor(factor);
}

QVariantMap ApplicationController::stopSpeedhack() {
    return m_debugFeatureManager->stopSpeedhack();
}

QVariantMap ApplicationController::startApiHook(const QString& moduleName, const QString& functionName,
                                                int mode, qlonglong forcedReturnValue) {
    return m_debugFeatureManager->startApiHook(moduleName, functionName, mode, forcedReturnValue);
}

QVariantMap ApplicationController::stopApiHook() {
    return m_debugFeatureManager->stopApiHook();
}

QVariantMap ApplicationController::getApiHookStatus() const {
    return m_debugFeatureManager->getApiHookStatus();
}

QVariantMap ApplicationController::getSpeedhackStatus() const {
    return m_debugFeatureManager->getSpeedhackStatus();
}

QVariantMap ApplicationController::findWhatAccessesAsync(const QString& addressHex, const QVariantMap& options) {
    return m_debugFeatureManager->findWhatAccessesAsync(addressHex, options);
}

QVariantMap ApplicationController::scanGroupScan(const QVariantList& entriesList, const QVariantMap& optionsMap) {
    return m_scanningCoreManager->scanGroupScan(entriesList, optionsMap);
}

QVariantMap ApplicationController::writeMemoryHex(const QString& addressHex, const QString& hexString) {
    QVariantMap result;
    result["success"] = false;

    if (!m_handle.isValid()) {
        result["error"] = "Aucun processus attache.";
        return result;
    }

    uint64_t address = 0;
    if (!parseHexAddress(addressHex, &address)) {
        result["error"] = "Adresse invalide.";
        return result;
    }

    // Parser la chaine hex : accepte "48 8B 00", "488B00", "48 8b 00"
    QString cleaned = hexString.simplified().remove(' ').remove('\t').remove('\n').remove('\r').remove(',');
    if (cleaned.size() % 2 != 0) {
        result["error"] = "Chaine hexadecimale invalide : nombre impair de caracteres.";
        return result;
    }
    if (cleaned.isEmpty()) {
        result["error"] = "Chaine hexadecimale vide.";
        return result;
    }
    if (cleaned.size() > 4096) {
        result["error"] = "Chaine hexadecimale trop longue (max 2048 octets).";
        return result;
    }
    const QByteArray bytes = QByteArray::fromHex(cleaned.toLatin1());
    if (bytes.isEmpty()) {
        result["error"] = "Chaine hexadecimale invalide.";
        return result;
    }

    killcore::MemoryWriter writer(m_handle);
    const auto writeResult = writer.write(address, bytes, true);

    result["success"] = writeResult.success;
    result["verified"] = writeResult.verified;
    result["address"] = QString::number(address, 16).toUpper();
    result["bytesWritten"] = static_cast<int>(writeResult.bytesWritten);
    result["requestedBytes"] = bytes.size();
    result["protectionChanged"] = writeResult.protectionChanged;
    result["previousHex"] = QString::fromLatin1(writeResult.previousValue.toHex(' ').toUpper());
    result["newHex"] = QString::fromLatin1(bytes.toHex(' ').toUpper());
    result["error"] = writeResult.errorMessage;

    if (writeResult.success) {
        KE_LOG_INFO() << "writeMemoryHex: " << writeResult.bytesWritten << " octets ecrits a 0x" << std::hex << address;
    }
    return result;
}

QVariantMap ApplicationController::dumpMemoryRegion(const QString& addressHex, int size, const QString& fileName) {
    QVariantMap result;
    result["success"] = false;

    if (!m_handle.isValid()) {
        result["error"] = "Aucun processus attache.";
        return result;
    }

    uint64_t address = 0;
    if (!parseHexAddress(addressHex, &address)) {
        result["error"] = "Adresse invalide.";
        return result;
    }

    const int boundedSize = std::clamp(size, 1, 16 * 1024 * 1024);

    QString safeName = fileName.simplified();
    if (safeName.isEmpty()) {
        safeName = QString("dump_%1_%2.bin")
            .arg(QString::number(address, 16).toUpper())
            .arg(QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss"));
    }
    safeName.remove('<').remove('>').remove(':').remove('"').remove('/').remove('\\').remove('|').remove('?').remove('*');
    if (!safeName.endsWith(".bin", Qt::CaseInsensitive)) {
        safeName += ".bin";
    }

    const QString dumpDir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)
        + "/KillEngine/dumps";
    QDir().mkpath(dumpDir);
    const QString filePath = dumpDir + "/" + safeName;

    killcore::MemoryReader reader(m_handle);
    const auto read = reader.readChunked(address, static_cast<size_t>(boundedSize), 1024 * 1024);
    if (!read.success && !read.partial) {
        result["error"] = read.errorMessage.isEmpty() ? QString("Lecture memoire echouee.") : read.errorMessage;
        return result;
    }

    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly)) {
        result["error"] = QString("Impossible de creer le fichier : %1").arg(filePath);
        return result;
    }
    const qint64 written = file.write(read.data);
    file.close();
    if (written != read.data.size()) {
        result["error"] = QString("Ecriture fichier incomplete : %1/%2 octets").arg(written).arg(read.data.size());
        return result;
    }

    result["success"] = true;
    result["address"] = QString::number(address, 16).toUpper();
    result["size"] = static_cast<int>(read.bytesRead);
    result["partial"] = read.partial;
    result["filePath"] = filePath;
    result["fileName"] = safeName;
    result["error"] = "";
    KE_LOG_INFO() << "dumpMemoryRegion: " << read.bytesRead << " octets dumps depuis 0x" << std::hex << address;
    return result;
}
QVariantMap ApplicationController::scanAobPattern(const QString& patternText, const QVariantMap& optionsMap) {
    return m_codePatchManager->scanAobPattern(patternText, optionsMap);
}
QVariantMap ApplicationController::generateAobSignature(const QString& addressHex, const QVariantMap& options) {
    return m_codePatchManager->generateAobSignature(addressHex, options);
}
QVariantMap ApplicationController::applyCodePatch(const QString& addressHex, const QString& bytesText, const QVariantMap& options) {
    return m_codePatchManager->applyCodePatch(addressHex, bytesText, options);
}
QVariantMap ApplicationController::suggestCodePatches(const QString& addressHex, const QVariantMap& options) {
    return m_codePatchManager->suggestCodePatches(addressHex, options);
}
QVariantMap ApplicationController::disassembleBackward(const QString& addressHex, const QVariantMap& options) const {
    return m_debugFeatureManager->disassembleBackward(addressHex, options);
}


// Cadence et garde-fous du sondage "tient/repart" ci-dessous : mêmes valeurs que
// WriteFreezeCoreManager (1.5s/tick, ~12s, 2 mismatches consecutifs pour confirmer
// une reversion), mais etat et logique dedies — m_activeDebugCancellation reste
// reserve a l'attach debugger, voir commentaire sur
// m_activeCandidateFieldTestCancellation dans le header.
constexpr int kCandidateTestTicks = 8;
constexpr int kCandidateTestIntervalMs = 1500;
constexpr int kCandidateTestConfirmMismatches = 2;
constexpr int kCandidateTestMaxCount = 5;
// Delta distinctif ajoute a la valeur d'origine pour la valeur test : assez
// caracteristique pour qu'un "tient" soit sans ambiguite, assez petit pour
// rester inoffensif et visible quelques secondes avant restauration.
constexpr double kCandidateProbeDelta = 777.0;

QVariantMap ApplicationController::testCandidateFieldsAsync(
    const QString& writeInstructionAddressHex,
    const QString& knownWriteTargetAddressHex,
    const QVariantMap& options) {
    QVariantMap result;
    result["success"] = false;
    result["started"] = false;

    if (m_candidateFieldTestInProgress) {
        result["error"] = "Un test de champs candidats est déjà en cours.";
        return result;
    }
    if (!m_attached || !m_handle.isValid() || m_pid <= 0) {
        result["error"] = "Aucun processus attaché.";
        return result;
    }

    uint64_t writeInstructionAddress = 0;
    if (!parseHexAddress(writeInstructionAddressHex, &writeInstructionAddress)) {
        result["error"] = "Adresse d'instruction invalide.";
        return result;
    }
    uint64_t knownWriteTargetAddress = 0;
    if (!parseHexAddress(knownWriteTargetAddressHex, &knownWriteTargetAddress)) {
        result["error"] = "Adresse écrite connue invalide.";
        return result;
    }

    const int windowBytes = std::clamp(options.value("windowBytes", 64).toInt(), 16, 128);
    const int trailingBytes = 16; // Meme marge que disassembleBackward, pour decoder l'instruction cible entierement.
    const uint64_t start = writeInstructionAddress >= static_cast<uint64_t>(windowBytes)
        ? writeInstructionAddress - static_cast<uint64_t>(windowBytes)
        : 0;
    const int targetOffsetInWindow = static_cast<int>(writeInstructionAddress - start);

    killcore::MemoryReader windowReader(m_handle);
    const auto windowRead = windowReader.readChunked(start, static_cast<size_t>(targetOffsetInWindow + trailingBytes), 4096);
    if (!windowRead.success && windowRead.bytesRead == 0) {
        result["error"] = windowRead.errorMessage.isEmpty() ? QString("Lecture mémoire impossible.") : windowRead.errorMessage;
        return result;
    }
    if (targetOffsetInWindow > windowRead.data.size()) {
        result["error"] = "Lecture mémoire trop courte pour atteindre l'adresse cible.";
        return result;
    }

    const auto backward = killcore::disassembleBackwardWindow(windowRead.data, targetOffsetInWindow);
    if (!backward.success) {
        result["error"] = backward.error;
        return result;
    }

    auto resolved = killcore::resolveCandidateFieldAddresses(backward.instructions, knownWriteTargetAddress);
    if (resolved.isEmpty()) {
        result["error"] = "Aucun champ candidat résolvable (registre de base différent de celui de l'instruction "
                           "d'écriture, ou aucun champ mémoire simple en amont).";
        return result;
    }
    // resolveCandidateFieldAddresses() rend les candidats dans l'ordre d'execution :
    // les plus proches de l'instruction d'ecriture (les plus vraisemblables) sont en
    // fin de liste — on inverse pour les tester en premier, puis on borne le nombre.
    std::reverse(resolved.begin(), resolved.end());
    if (resolved.size() > kCandidateTestMaxCount) {
        resolved.resize(kCandidateTestMaxCount);
    }

    const int requestId = m_nextDebugRequestId++;
    const int pid = m_pid;
    const QPointer<ApplicationController> self(this);
    auto cancellation = std::make_shared<killcore::CancellationToken>();

    m_candidateFieldTestInProgress = true;
    m_activeCandidateFieldTestCancellation = cancellation;

    KE_LOG_INFO() << "testCandidateFieldsAsync(pid=" << pid
                  << ", candidates=" << resolved.size()
                  << ", requestId=" << requestId << ")";

    std::thread([self, requestId, pid, resolved, cancellation]() {
        killcore::ProcessHandle testHandle(static_cast<uint32_t>(pid), killcore::ProcessAccess::ReadWrite);
        QVariantList outcomes;

        if (!testHandle.isValid()) {
            for (const auto& candidate : resolved) {
                QVariantMap outcome;
                outcome["address"] = QString::number(candidate.address, 16).toUpper();
                outcome["memBaseRegister"] = candidate.memBaseRegister;
                outcome["memDisplacement"] = static_cast<qlonglong>(candidate.memDisplacement);
                outcome["valueType"] = killcore::valueTypeToString(candidate.inferredType);
                outcome["verdict"] = "error";
                outcome["error"] = "Impossible d'ouvrir le processus en écriture.";
                outcomes.append(outcome);
            }
        } else {
            killcore::MemoryReader reader(testHandle);
            killcore::MemoryWriter writer(testHandle);

            for (const auto& candidate : resolved) {
                if (cancellation->isCancelled()) {
                    break;
                }

                QVariantMap outcome;
                outcome["address"] = QString::number(candidate.address, 16).toUpper();
                outcome["memBaseRegister"] = candidate.memBaseRegister;
                outcome["memDisplacement"] = static_cast<qlonglong>(candidate.memDisplacement);
                outcome["valueType"] = killcore::valueTypeToString(candidate.inferredType);

                const size_t typeSize = killcore::valueTypeSize(candidate.inferredType);
                const auto originalRead = reader.read(candidate.address, typeSize);
                if ((!originalRead.success && !originalRead.partial) || originalRead.bytesRead != typeSize) {
                    outcome["verdict"] = "error";
                    outcome["error"] = "Lecture de la valeur d'origine impossible.";
                    outcomes.append(outcome);
                    continue;
                }

                const double originalValue = bytesToDouble(originalRead.data, candidate.inferredType);
                const QByteArray testBytes = doubleToBytes(originalValue + kCandidateProbeDelta, candidate.inferredType);

                const auto probeWrite = writer.write(candidate.address, testBytes, true);
                if (!probeWrite.success || !probeWrite.verified) {
                    outcome["verdict"] = "error";
                    outcome["error"] = probeWrite.errorMessage.isEmpty() ? "Écriture test impossible." : probeWrite.errorMessage;
                    outcomes.append(outcome);
                    continue;
                }
                const QByteArray previousValue = probeWrite.previousValue;

                int consecutiveMismatches = 0;
                int ticksSurvived = 0;
                bool reverted = false;
                for (int tick = 0; tick < kCandidateTestTicks; ++tick) {
                    if (cancellation->isCancelled()) {
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(kCandidateTestIntervalMs));
                    const auto tickRead = reader.read(candidate.address, typeSize);
                    const bool matches = (tickRead.success || tickRead.partial)
                        && tickRead.bytesRead == typeSize
                        && bytesEqual(tickRead.data, testBytes, candidate.inferredType);
                    if (matches) {
                        consecutiveMismatches = 0;
                        ++ticksSurvived;
                    } else {
                        ++consecutiveMismatches;
                        if (consecutiveMismatches >= kCandidateTestConfirmMismatches) {
                            reverted = true;
                            break;
                        }
                    }
                }

                outcome["verdict"] = reverted ? "reverts" : "holds";
                outcome["ticksSurvived"] = ticksSurvived;

                const auto restore = writer.write(candidate.address, previousValue, true);
                const bool restored = restore.success && restore.verified;
                outcome["restored"] = restored;
                if (!restored) {
                    outcome["error"] = "Valeur test écrite mais restauration échouée — vérifie manuellement cette adresse.";
                }

                outcomes.append(outcome);
            }
        }

        if (!self) {
            return;
        }

        const bool cancelled = cancellation->isCancelled();
        QMetaObject::invokeMethod(self.data(), [self, requestId, outcomes, cancelled]() {
            if (!self) {
                return;
            }
            QVariantMap finished;
            finished["requestId"] = requestId;
            finished["kind"] = "test_candidate_fields";
            finished["success"] = true;
            finished["results"] = outcomes;
            finished["cancelled"] = cancelled;
            self->m_candidateFieldTestInProgress = false;
            self->m_activeCandidateFieldTestCancellation.reset();
            emit self->candidateFieldTestFinished(finished);
        }, Qt::QueuedConnection);
    }).detach();

    result["success"] = true;
    result["started"] = true;
    result["requestId"] = requestId;
    result["candidateCount"] = resolved.size();
    result["warning"] = QString(
        "Écrit une valeur test transitoire sur chaque champ candidat (jusqu'à %1s par champ), "
        "puis restaure systématiquement la valeur d'origine.")
        .arg(kCandidateTestTicks * kCandidateTestIntervalMs / 1000);
    return result;
}

QVariantMap ApplicationController::cancelCandidateFieldTest() {
    QVariantMap result;
    result["success"] = false;
    if (!m_candidateFieldTestInProgress || !m_activeCandidateFieldTestCancellation) {
        result["error"] = "Aucun test de champs candidats actif à annuler.";
        return result;
    }
    m_activeCandidateFieldTestCancellation->cancel();
    result["success"] = true;
    result["error"] = "";
    return result;
}

QVariantMap ApplicationController::restoreCodePatch(const QString& addressHex) {
    return m_codePatchManager->restoreCodePatch(addressHex);
}
QVariantMap ApplicationController::injectDllIntoProcess(const QString& dllPath) {
    QVariantMap result;
    result["success"] = false;
    result["dllPath"] = dllPath;

    if (!m_attached || !m_handle.isValid()) {
        result["error"] = "Aucun processus attaché.";
        return result;
    }
    if (dllPath.trimmed().isEmpty()) {
        result["error"] = "Chemin DLL vide.";
        return result;
    }

    const auto injected = killcore::injectDll(m_handle, dllPath);
    result["success"] = injected.success;
    result["error"] = injected.error;
    result["moduleBase"] = QString::number(injected.moduleBase, 16).toUpper();

    if (injected.remoteThreadHandle) {
        CloseHandle(reinterpret_cast<HANDLE>(injected.remoteThreadHandle));
    }

    appendScanTelemetry("dll_injection", result);
    if (injected.success) {
        KE_LOG_WARN() << "DLL injected: " << dllPath.toStdString() << " moduleBase=0x" << std::hex << injected.moduleBase;
    }
    return result;
}

QVariantMap ApplicationController::installFunctionHook(const QString& targetAddressHex, const QString& hookAddressHex) {
    return m_codePatchManager->installFunctionHook(targetAddressHex, hookAddressHex);
}
QVariantMap ApplicationController::removeFunctionHook(const QString& targetAddressHex) {
    return m_codePatchManager->removeFunctionHook(targetAddressHex);
}
QVariantMap ApplicationController::parseAutoAssemblerScript(const QString& scriptText) const {
    return m_codePatchManager->parseAutoAssemblerScript(scriptText);
}

QVariantMap ApplicationController::executeAutoAssemblerScript(const QString& scriptText) {
    return m_codePatchManager->executeAutoAssemblerScript(scriptText);
}

QVariantMap ApplicationController::restoreAutoAssemblerScript() {
    return m_codePatchManager->restoreAutoAssemblerScript();
}
QVariantMap ApplicationController::forceWriteInstructionValue(
    const QString& ripHex,
    int instructionLength,
    const QString& memBaseRegister,
    qlonglong memDisplacement,
    const QString& valueType,
    const QString& value) {
    QVariantMap result;
    result["success"] = false;

    if (!m_attached || !m_handle.isValid()) {
        result["error"] = "Aucun processus attaché.";
        return result;
    }
    if (memBaseRegister.trimmed().isEmpty()) {
        result["error"] = "Cette instruction n'a pas de destination mémoire exploitable (adressage indexé ou RIP-relatif, non supporté).";
        return result;
    }
    if (instructionLength < 5) {
        result["error"] = QString("Instruction trop courte (%1 octet(s)) pour y poser un saut de redirection (5 minimum).").arg(instructionLength);
        return result;
    }

    uint64_t rip = 0;
    if (!parseHexAddress(ripHex, &rip)) {
        result["error"] = "Adresse invalide.";
        return result;
    }

    killcore::ValueType type;
    if (!killcore::parseValueType(valueType, &type)) {
        result["error"] = "Type invalide.";
        return result;
    }
    // encodeMemImmMov (core/scripting/auto_assembler.cpp) n'ecrit qu'un
    // immediat 32 bits (dword) : memes limites que "Forcer une valeur".
    if (type == killcore::ValueType::Int64 || type == killcore::ValueType::UInt64
        || type == killcore::ValueType::Float32 || type == killcore::ValueType::Float64) {
        result["error"] = "Seuls les types entiers jusqu'à 32 bits sont supportés pour forcer une valeur ici.";
        return result;
    }
    killcore::ScanValue scanValue;
    QString parseError;
    if (!killcore::parseScanValue(value, type, &scanValue, &parseError)) {
        result["error"] = parseError;
        return result;
    }
    const qint64 immediateValue = scanValue.value.toLongLong();

    // Le pattern "module"+offset: du compilateur auto-assembleur a besoin
    // d'un nom de module, pas d'une adresse absolue brute — retrouve le
    // module qui contient RIP (meme demarche que generateAobSignature).
    QString moduleName;
    uint64_t moduleOffset = 0;
    const auto modules = killcore::ProcessEnumerator::enumerateModules(static_cast<uint32_t>(m_pid));
    for (const auto& module : modules) {
        if (rip >= module.baseAddress && rip < module.baseAddress + module.size) {
            moduleName = module.name;
            moduleOffset = rip - module.baseAddress;
            break;
        }
    }
    if (moduleName.isEmpty()) {
        result["error"] = "Impossible de déterminer le module contenant cette adresse (mémoire allouée dynamiquement, hors d'un module chargé ?).";
        return result;
    }

    QString destinationOperand = memBaseRegister;
    if (memDisplacement > 0) {
        destinationOperand += QString("+%1").arg(memDisplacement);
    } else if (memDisplacement < 0) {
        destinationOperand += QString("-%1").arg(-memDisplacement);
    }

    // Redirige le site existant (RIP) vers un trampoline alloue qui ecrit la
    // valeur choisie a la meme destination memoire que l'instruction
    // d'origine, puis reprend juste apres — l'instruction d'origine
    // n'execute donc plus jamais (contrairement a un inline hook classique
    // qui rejouerait les bytes originaux dans son propre trampoline).
    QString nopLines;
    for (int i = 0; i < instructionLength - 5; ++i) {
        nopLines += "nop\n";
    }
    const QString scriptText = QStringLiteral(
        "alloc(newmem, 64)\n"
        "label(returnhere)\n"
        "\n"
        "newmem:\n"
        "mov [%1], %2\n"
        "jmp returnhere\n"
        "\n"
        "\"%3\"+0x%4:\n"
        "jmp newmem\n"
        "%5"
        "returnhere:\n")
        .arg(destinationOperand)
        .arg(immediateValue)
        .arg(moduleName)
        .arg(QString::number(moduleOffset, 16))
        .arg(nopLines);

    result = executeAutoAssemblerScript(scriptText);
    result["generatedScript"] = scriptText;
    result["targetAddress"] = ripHex;
    return result;
}

QVariantMap ApplicationController::setFreezeValue(const QString& addressHex, const QString& valueType, const QString& value, bool enabled) {
    return m_freezeHotkeyOverlayManager->setFreezeValue(addressHex, valueType, value, enabled);
}

QVariantMap ApplicationController::freezeWithBreakpoint(const QString& addressHex, const QString& valueType, const QString& value, const QVariantMap& options) {
    return m_debugFeatureManager->freezeWithBreakpoint(addressHex, valueType, value, options);
}

QVariantMap ApplicationController::escalatePollingFreezeToBreakpoint(const QString& addressHex) {
    return m_debugFeatureManager->escalatePollingFreezeToBreakpoint(addressHex);
}

QVariantMap ApplicationController::stopBreakpointFreeze() {
    return m_debugFeatureManager->stopBreakpointFreezeCommand();
}

QVariantMap ApplicationController::getBreakpointFreezeStats() const {
    return m_debugFeatureManager->getBreakpointFreezeStats();
}

QVariantMap ApplicationController::setFreezeInterval(int intervalMs) {
    return m_freezeHotkeyOverlayManager->setFreezeInterval(intervalMs);
}

QVariantMap ApplicationController::registerGlobalHotkey(const QString& comboText, const QVariantMap& actionMap) {
    return m_freezeHotkeyOverlayManager->registerGlobalHotkey(comboText, actionMap);
}

QVariantMap ApplicationController::unregisterGlobalHotkey(int id) {
    return m_freezeHotkeyOverlayManager->unregisterGlobalHotkey(id);
}

QVariantMap ApplicationController::getGlobalHotkeys() const {
    return m_freezeHotkeyOverlayManager->getGlobalHotkeys();
}

QVariantMap ApplicationController::clearGlobalHotkeys() {
    return m_freezeHotkeyOverlayManager->clearGlobalHotkeys();
}

QVariantMap ApplicationController::setTrainerOverlayVisible(bool visible, const QVariantMap& options) {
    return m_freezeHotkeyOverlayManager->setTrainerOverlayVisible(visible, options);
}

QVariantMap ApplicationController::updateTrainerOverlay(const QVariantMap& state) {
    return m_freezeHotkeyOverlayManager->updateTrainerOverlay(state);
}

QVariantMap ApplicationController::rewriteLastAutoWriteTargets(const QString& value, const QString& query) {
    QVariantMap result;
    QVariantList suggestions;
    QVariantList writeResults;

    result["query"] = query;
    result["aiReady"] = m_ai.isReady();
    result["status"] = "tool_call";
    result["tool"] = "rewrite_last_auto_write";
    result["actionStatus"] = "executed";
    result["workflowStatus"] = "auto_write_done";
    result["targetValue"] = value;

    bool allWritesOk = true;
    const QString previousTargetValue = m_smartSearchTargetValue;
    m_smartSearchTargetValue = value;
    auto writeState = autoWriteState();
    m_lastBatchStartIndex = writeState.writeHistorySize();

    for (const auto& target : writeState.lastTargets()) {
        QVariantMap suggestion;
        suggestion["address"] = QString::number(target.address, 16);
        suggestion["type"] = killcore::valueTypeToString(target.type);
        suggestion["value"] = value;
        const auto history = candidateValueHistory(target.address);
        if (!history.isEmpty()) {
            suggestion["valueHistory"] = history;
        }
        suggestions.append(suggestion);

        auto writeResult = writeMemoryValueConfirmed(
            suggestion.value("address").toString(),
            suggestion.value("type").toString(),
            value);
        writeResult.insert("address", suggestion.value("address"));
        writeResult.insert("value", value);
        writeResult.insert("type", suggestion.value("type"));
        if (suggestion.contains("valueHistory")) {
            writeResult.insert("valueHistory", suggestion.value("valueHistory"));
        }
        allWritesOk = allWritesOk && writeResult.value("success").toBool();
        writeResults.append(writeResult);
    }

    m_lastBatchEndIndex = writeState.writeHistorySize();
    if (m_lastBatchEndIndex == m_lastBatchStartIndex) {
        m_lastBatchStartIndex = -1;
        m_lastBatchEndIndex = -1;
    }
    if (allWritesOk && writeState.hasLastTargets()) {
        m_smartSearchActive = false;
        writeState.replaceChatTargetsWithLastTargets();
        resetFailureEscalationState();
        appendDistinctText(&m_autoWriteValueHistory, value, 12);
    }

    QVariantMap actionResult;
    actionResult["success"] = allWritesOk;
    actionResult["remaining"] = static_cast<qulonglong>(writeState.lastTargetCount());
    actionResult["error"] = allWritesOk ? QString() : QString("Au moins une réécriture a échoué.");

    result["actionResult"] = actionResult;
    result["suggestedWrites"] = suggestions;
    result["suggestedWrite"] = suggestions.isEmpty() ? QVariantMap{} : suggestions.first().toMap();
    result["autoWriteResults"] = writeResults;
    result["autoWriteResult"] = writeResults.isEmpty() ? QVariantMap{} : writeResults.last().toMap();
    result["autoWriteCount"] = writeResults.size();
    result["activeTargetCount"] = writeState.chatTargetCount();
    result["previousTargetValue"] = previousTargetValue;
    result["writeHistory"] = writeHistoryToVariantList(m_autoWriteValueHistory);
    result["rollbackNote"] = "Tu peux annuler cette réécriture via le bouton rollback batch dans l'assistant.";
    result["message"] = allWritesOk
        ? QString("J'ai repris les %1 dernière(s) adresse(s) auto-écrite(s) et j'ai mis %2 dessus. Je garde ces adresses actives pour les prochaines modifications.")
              .arg(writeState.lastTargetCount())
              .arg(value)
        : QString("J'ai repris les dernières adresses auto-écrites, mais au moins une réécriture vers %1 a échoué.")
              .arg(value);
    return result;
}

QVariantMap ApplicationController::activateChatMemoryTargetsFromQuery(const QString& query) {
    return m_smartSearchManager->activateChatMemoryTargetsFromQuery(query);
}

QVariantMap ApplicationController::writeChatMemoryTargetsFromQuery(const QString& query, const QString& value) {
    return m_smartSearchManager->writeChatMemoryTargetsFromQuery(query, value);
}

QVariantMap ApplicationController::freezeChatMemoryTargetsFromQuery(const QString& query, const QString& value) {
    return m_smartSearchManager->freezeChatMemoryTargetsFromQuery(query, value);
}

QVariantMap ApplicationController::confirmChatMemoryWrite(const QString& value) {
    return m_smartSearchManager->confirmChatMemoryWrite(value);
}

QVariantMap ApplicationController::confirmChatMemoryFreeze(const QString& value) {
    return m_smartSearchManager->confirmChatMemoryFreeze(value);
}

QVariantMap ApplicationController::confirmRewriteLastAutoWrite(const QString& value) {
    return m_smartSearchManager->confirmRewriteLastAutoWrite(value);
}

QVariantMap ApplicationController::getActiveChatMemoryTargets() const {
    return m_smartSearchManager->getActiveChatMemoryTargets();
}

QVariantMap ApplicationController::clearActiveChatMemoryTargets() {
    return m_smartSearchManager->clearActiveChatMemoryTargets();
}

QVariantMap ApplicationController::clearScanContext() {
    return m_smartSearchManager->clearScanContext();
}

void ApplicationController::acknowledgePendingSmartSearchRecovery() {
    m_smartSearchManager->acknowledgePendingSmartSearchRecovery();
}

QVariantMap ApplicationController::getTemporaryStorageStatus() const {
    return m_settingsDiagnosticsManager->getTemporaryStorageStatus();
}

QVariantMap ApplicationController::clearTemporaryStorage() {
    return m_settingsDiagnosticsManager->clearTemporaryStorage();
}

QVariantMap ApplicationController::getSmartSearchContext() const {
    return m_smartSearchManager->getSmartSearchContext();
}

QVariantMap ApplicationController::getAutoResolveReport(int maxEvents) const {
    return m_smartSearchManager->getAutoResolveReport(maxEvents);
}

QVariantMap ApplicationController::clearAutoResolveMemory(bool allProcesses) {
    return m_smartSearchManager->clearAutoResolveMemory(allProcesses);
}

QVariantMap ApplicationController::addInvestigationHypothesis(const QString& description, int baselineScore) {
    return m_investigationNotebookManager->addHypothesis(description, baselineScore);
}

QVariantMap ApplicationController::recordInvestigationTestResult(const QString& hypothesisId, bool confirmed, const QString& evidenceNote) {
    return m_investigationNotebookManager->recordTestResult(hypothesisId, confirmed, evidenceNote);
}

QVariantMap ApplicationController::getInvestigationNotebookSynthesis() const {
    return m_investigationNotebookManager->getSynthesis();
}

QVariantMap ApplicationController::proposeInvestigationNotebookPlan(const QString& symptom, const QVariantMap& options) {
    QVariantMap plan = m_ai.proposeInvestigationNotebookPlan(symptom, options);
    if (plan.value("success").toBool() != true) {
        return plan;
    }

    if (options.value("resetFirst", false).toBool()) {
        m_investigationNotebookManager->resetNotebook();
    }

    const int baselineScore = options.value("baselineScore", 50).toInt();
    QVariantList added;
    const QVariantList hypotheses = plan.value("hypotheses").toList();
    for (const QVariant& item : hypotheses) {
        const QString description = item.toString().trimmed();
        if (description.isEmpty()) {
            continue;
        }
        const QVariantMap addedResult = m_investigationNotebookManager->addHypothesis(description, baselineScore);
        if (addedResult.value("success").toBool()) {
            added.append(addedResult.value("hypothesis"));
        }
    }

    QVariantMap synthesis = m_investigationNotebookManager->getSynthesis();
    plan["addedHypotheses"] = added;
    plan["confirmed"] = synthesis.value("confirmed");
    plan["active"] = synthesis.value("active");
    plan["refuted"] = synthesis.value("refuted");
    return plan;
}

QVariantMap ApplicationController::resetInvestigationNotebook() {
    return m_investigationNotebookManager->resetNotebook();
}

QVariantMap ApplicationController::logAiAudit(const QString& event, const QVariantMap& payload) {
    return m_smartSearchManager->logAiAudit(event, payload);
}

QVariantMap ApplicationController::getRememberedPatterns() const {
    return m_smartSearchManager->getRememberedPatterns();
}

QVariantMap ApplicationController::getWriteHistorySequence() const {
    return m_smartSearchManager->getWriteHistorySequence();
}

QVariantMap ApplicationController::replayWriteHistorySequence() {
    return m_smartSearchManager->replayWriteHistorySequence();
}

QVariantMap ApplicationController::clearWriteHistorySequence() {
    return m_smartSearchManager->clearWriteHistorySequence();
}

QVariantMap ApplicationController::writeProfileTargetsFromQuery(const QString& query, const QString& value) {
    return m_smartSearchManager->writeProfileTargetsFromQuery(query, value);
}

QVariantMap ApplicationController::startAutoResolve(const QString& query, const QVariantMap& options) {
    return m_smartSearchManager->startAutoResolve(query, options);
}

void ApplicationController::resetFailureEscalationState() {
    m_smartSearchManager->resetFailureEscalationState();
}

QVariantMap ApplicationController::buildFailureEscalationRecovery(const QString& query, const QStringList& numbers) {
    return m_smartSearchManager->buildFailureEscalationRecovery(query, numbers);
}

QVariantMap ApplicationController::startSmartSearch(const QString& query) {
    return m_smartSearchManager->startSmartSearch(query);
}

QString ApplicationController::ping(const QString& message) {
    QString response = QString("pong: %1 @ %2")
                           .arg(message)
                           .arg(QDateTime::currentDateTime().toString("HH:mm:ss.zzz"));
    KE_LOG_DEBUG() << "ping(\"" << message.toStdString() << "\") → \"" << response.toStdString() << "\"";
    return response;
}

QVariantMap ApplicationController::getSettings() const {
    return m_settingsDiagnosticsManager->getSettings();
}
bool ApplicationController::hasSeenOnboarding() const {
    return QSettings().value("ui/hasSeenOnboarding", false).toBool();
}

void ApplicationController::setOnboardingSeen(bool seen) {
    QSettings().setValue("ui/hasSeenOnboarding", seen);
}

bool ApplicationController::openUserGuide() const {
    const QDir appDir(QCoreApplication::applicationDirPath());
    const QStringList candidates = {
        appDir.filePath("USER_GUIDE.md"),
        appDir.filePath("../../docs/USER_GUIDE.md"),
        QDir::current().filePath("docs/USER_GUIDE.md"),
    };
    for (const auto& candidate : candidates) {
        QFileInfo file(candidate);
        if (file.exists() && file.isFile()) {
            return QDesktopServices::openUrl(QUrl::fromLocalFile(file.absoluteFilePath()));
        }
    }
    return false;
}

QVariantMap ApplicationController::requestWindowsDefenderExclusion() {
    QVariantMap result;
    result["success"] = false;
    result["cancelled"] = false;

#ifdef Q_OS_WIN
    const QString installDir = QDir::toNativeSeparators(QCoreApplication::applicationDirPath());
    const QString exeName = QFileInfo(QCoreApplication::applicationFilePath()).fileName();

    // Guillemets simples PowerShell pour le chemin : les guillemets doubles
    // seraient interpretes par PowerShell, pas juste par le shell qui lance
    // ShellExecute. Un chemin contenant une apostrophe casserait cette
    // commande — cas limite volontairement non gere ici (rare sur Windows,
    // et l'echec serait visible/explicite plutot que silencieux).
    const QString psCommand = QStringLiteral(
        "Add-MpPreference -ExclusionPath '%1' -ExclusionProcess '%2'")
        .arg(installDir, exeName);

    const std::wstring parameters =
        L"-NoProfile -ExecutionPolicy Bypass -Command \"" + psCommand.toStdWString() + L"\"";

    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.hwnd = nullptr;
    sei.lpVerb = L"runas"; // declenche l'invite UAC visible -- jamais silencieux
    sei.lpFile = L"powershell.exe";
    sei.lpParameters = parameters.c_str();
    sei.nShow = SW_HIDE;

    if (!ShellExecuteExW(&sei)) {
        const DWORD err = GetLastError();
        if (err == ERROR_CANCELLED) {
            result["cancelled"] = true;
            result["error"] = "Invite d'élévation refusée par l'utilisateur.";
        } else {
            result["error"] = QStringLiteral("Impossible de lancer PowerShell élevé (error=%1).").arg(err);
        }
        KE_LOG_WARN() << "requestWindowsDefenderExclusion: ShellExecuteExW failed, error=" << err;
        return result;
    }

    if (sei.hProcess) {
        WaitForSingleObject(sei.hProcess, 15000);
        DWORD exitCode = 1;
        GetExitCodeProcess(sei.hProcess, &exitCode);
        CloseHandle(sei.hProcess);
        result["success"] = (exitCode == 0);
        if (exitCode != 0) {
            result["error"] = QStringLiteral(
                "Add-MpPreference a échoué (code %1) — l'exclusion est peut-être gérée de façon centralisée "
                "par une politique d'entreprise (Tamper Protection) et ne peut pas être modifiée localement.")
                .arg(exitCode);
        }
    } else {
        // Pas de handle de process a attendre -- best-effort, on suppose que
        // l'invite s'est affichee correctement.
        result["success"] = true;
    }

    KE_LOG_INFO() << "requestWindowsDefenderExclusion: success=" << result.value("success").toBool()
                  << " path=" << installDir.toStdString() << " process=" << exeName.toStdString();
#else
    result["error"] = "Fonctionnalité Windows uniquement.";
#endif

    return result;
}

namespace {
// Nom de regle pare-feu derive du nom d'executable : caracteres hors
// [A-Za-z0-9_.-] remplaces par '_' pour eviter tout probleme de quoting dans
// la commande PowerShell generee plus bas.
QString sanitizeFirewallRuleToken(const QString& exeName) {
    QString sanitized = exeName;
    static const QRegularExpression kUnsafeChars("[^A-Za-z0-9_.-]");
    sanitized.replace(kUnsafeChars, "_");
    return sanitized.isEmpty() ? QStringLiteral("process") : sanitized;
}

QString firewallRuleNameOut(const QString& token) {
    return QStringLiteral("KillEngine-NetBlock-%1-Out").arg(token);
}

QString firewallRuleNameIn(const QString& token) {
    return QStringLiteral("KillEngine-NetBlock-%1-In").arg(token);
}
} // namespace

QVariantMap ApplicationController::blockProcessNetwork() {
    QVariantMap result;
    result["success"] = false;
    result["cancelled"] = false;

    if (!m_attached || m_pid <= 0) {
        result["error"] = "Aucun processus attaché.";
        return result;
    }

#ifdef Q_OS_WIN
    const QString exePath = QDir::toNativeSeparators(m_handle.executablePath());
    if (exePath.isEmpty()) {
        result["error"] = "Chemin de l'exécutable introuvable pour le processus attaché.";
        return result;
    }
    const QString ruleToken = sanitizeFirewallRuleToken(QFileInfo(exePath).fileName());
    const QString ruleOut = firewallRuleNameOut(ruleToken);
    const QString ruleIn = firewallRuleNameIn(ruleToken);

    // Guillemets simples PowerShell pour le chemin — meme convention que
    // requestWindowsDefenderExclusion() ci-dessus (un chemin contenant une
    // apostrophe casserait cette commande, cas limite non gere ici).
    const QString psCommand = QStringLiteral(
        "New-NetFirewallRule -DisplayName '%1' -Direction Outbound -Program '%2' -Action Block -Profile Any -ErrorAction SilentlyContinue | Out-Null; "
        "New-NetFirewallRule -DisplayName '%3' -Direction Inbound -Program '%2' -Action Block -Profile Any -ErrorAction SilentlyContinue | Out-Null")
        .arg(ruleOut, exePath, ruleIn);

    const std::wstring parameters =
        L"-NoProfile -ExecutionPolicy Bypass -Command \"" + psCommand.toStdWString() + L"\"";

    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.hwnd = nullptr;
    sei.lpVerb = L"runas"; // declenche l'invite UAC visible -- jamais silencieux
    sei.lpFile = L"powershell.exe";
    sei.lpParameters = parameters.c_str();
    sei.nShow = SW_HIDE;

    if (!ShellExecuteExW(&sei)) {
        const DWORD err = GetLastError();
        if (err == ERROR_CANCELLED) {
            result["cancelled"] = true;
            result["error"] = "Invite d'élévation refusée par l'utilisateur.";
        } else {
            result["error"] = QStringLiteral("Impossible de lancer PowerShell élevé (error=%1).").arg(err);
        }
        KE_LOG_WARN() << "blockProcessNetwork: ShellExecuteExW failed, error=" << err;
        return result;
    }

    if (sei.hProcess) {
        WaitForSingleObject(sei.hProcess, 15000);
        DWORD exitCode = 1;
        GetExitCodeProcess(sei.hProcess, &exitCode);
        CloseHandle(sei.hProcess);
        result["success"] = (exitCode == 0);
        if (exitCode != 0) {
            result["error"] = QStringLiteral("New-NetFirewallRule a échoué (code %1).").arg(exitCode);
        }
    } else {
        // Pas de handle de process a attendre -- best-effort, meme logique
        // que requestWindowsDefenderExclusion().
        result["success"] = true;
    }

    if (result.value("success").toBool()) {
        m_networkBlockRuleToken = ruleToken;
        m_networkBlockExePath = exePath;
        result["ruleOutbound"] = ruleOut;
        result["ruleInbound"] = ruleIn;
        result["exePath"] = exePath;
    }

    KE_LOG_INFO() << "blockProcessNetwork: success=" << result.value("success").toBool()
                  << " exe=" << exePath.toStdString();
#else
    result["error"] = "Fonctionnalité Windows uniquement.";
#endif

    return result;
}

QVariantMap ApplicationController::unblockProcessNetwork() {
    QVariantMap result;
    result["success"] = false;
    result["cancelled"] = false;

#ifdef Q_OS_WIN
    QString ruleToken = m_networkBlockRuleToken;
    QString exePath = m_networkBlockExePath;
    if (ruleToken.isEmpty() && m_attached) {
        exePath = QDir::toNativeSeparators(m_handle.executablePath());
        if (!exePath.isEmpty()) {
            ruleToken = sanitizeFirewallRuleToken(QFileInfo(exePath).fileName());
        }
    }
    if (ruleToken.isEmpty()) {
        result["error"] = "Aucune règle de blocage réseau KillEngine connue à retirer.";
        return result;
    }

    const QString ruleOut = firewallRuleNameOut(ruleToken);
    const QString ruleIn = firewallRuleNameIn(ruleToken);
    const QString psCommand = QStringLiteral(
        "Remove-NetFirewallRule -DisplayName '%1' -ErrorAction SilentlyContinue; "
        "Remove-NetFirewallRule -DisplayName '%2' -ErrorAction SilentlyContinue")
        .arg(ruleOut, ruleIn);

    const std::wstring parameters =
        L"-NoProfile -ExecutionPolicy Bypass -Command \"" + psCommand.toStdWString() + L"\"";

    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.hwnd = nullptr;
    sei.lpVerb = L"runas";
    sei.lpFile = L"powershell.exe";
    sei.lpParameters = parameters.c_str();
    sei.nShow = SW_HIDE;

    if (!ShellExecuteExW(&sei)) {
        const DWORD err = GetLastError();
        if (err == ERROR_CANCELLED) {
            result["cancelled"] = true;
            result["error"] = "Invite d'élévation refusée par l'utilisateur.";
        } else {
            result["error"] = QStringLiteral("Impossible de lancer PowerShell élevé (error=%1).").arg(err);
        }
        KE_LOG_WARN() << "unblockProcessNetwork: ShellExecuteExW failed, error=" << err;
        return result;
    }

    if (sei.hProcess) {
        WaitForSingleObject(sei.hProcess, 15000);
        DWORD exitCode = 1;
        GetExitCodeProcess(sei.hProcess, &exitCode);
        CloseHandle(sei.hProcess);
        result["success"] = (exitCode == 0);
        if (exitCode != 0) {
            result["error"] = QStringLiteral("Remove-NetFirewallRule a échoué (code %1).").arg(exitCode);
        }
    } else {
        result["success"] = true;
    }

    if (result.value("success").toBool()) {
        m_networkBlockRuleToken.clear();
        m_networkBlockExePath.clear();
    }

    KE_LOG_INFO() << "unblockProcessNetwork: success=" << result.value("success").toBool();
#else
    result["error"] = "Fonctionnalité Windows uniquement.";
#endif

    return result;
}

QVariantMap ApplicationController::getProcessNetworkBlockStatus() const {
    QVariantMap result;
    result["success"] = true;
    result["blocked"] = false;

#ifdef Q_OS_WIN
    QString ruleToken = m_networkBlockRuleToken;
    QString exePath = m_networkBlockExePath;
    if (ruleToken.isEmpty() && m_attached) {
        exePath = QDir::toNativeSeparators(m_handle.executablePath());
        if (!exePath.isEmpty()) {
            ruleToken = sanitizeFirewallRuleToken(QFileInfo(exePath).fileName());
        }
    }
    if (ruleToken.isEmpty()) {
        return result;
    }
    const QString ruleOut = firewallRuleNameOut(ruleToken);

    QProcess check;
    check.setProgram(QStringLiteral("powershell.exe"));
    check.setArguments({
        "-NoProfile", "-ExecutionPolicy", "Bypass", "-Command",
        QStringLiteral("if (Get-NetFirewallRule -DisplayName '%1' -ErrorAction SilentlyContinue) { 'yes' } else { 'no' }").arg(ruleOut)
    });
    check.start();
    // 5000ms initial etait trop court : le premier chargement du module
    // PowerShell NetSecurity (Get-NetFirewallRule) peut a lui seul depasser
    // 5s, observe en pratique (PHASE 84, smoke test 24/08/2026).
    if (check.waitForFinished(10000)) {
        const QString output = QString::fromLocal8Bit(check.readAllStandardOutput()).trimmed();
        result["blocked"] = (output == QStringLiteral("yes"));
        result["ruleName"] = ruleOut;
        result["exePath"] = exePath;
    } else {
        result["error"] = "Impossible d'interroger le pare-feu (timeout).";
        check.kill();
    }
#endif

    return result;
}

QVariantMap ApplicationController::getClrInspectorStatus() const {
    return m_clrInspectorBridge->getClrInspectorStatus();
}

QVariantMap ApplicationController::attachClrInspector() {
    return m_clrInspectorBridge->attachClrInspector();
}

QVariantMap ApplicationController::detachClrInspector() {
    return m_clrInspectorBridge->detachClrInspector();
}

QVariantMap ApplicationController::shutdownClrInspector() {
    return m_clrInspectorBridge->shutdownClrInspector();
}

QVariantMap ApplicationController::flushClrInspectorCache() {
    return m_clrInspectorBridge->flushClrInspectorCache();
}

QVariantMap ApplicationController::findClrObjectsByType(const QString& typeSubstring) {
    return m_clrInspectorBridge->findClrObjectsByType(typeSubstring);
}

QVariantMap ApplicationController::findClrObjectsByFieldValue(const QString& typeSubstring, const QString& fieldName, const QString& expectedValue, int maxResults) {
    return m_clrInspectorBridge->findClrObjectsByFieldValue(typeSubstring, fieldName, expectedValue, maxResults);
}

QVariantMap ApplicationController::readClrObject(const QString& addressHex) {
    return m_clrInspectorBridge->readClrObject(addressHex);
}

QVariantMap ApplicationController::writeClrPrimitiveField(const QString& objectAddressHex, const QString& fieldName, const QString& value) {
    return m_clrInspectorBridge->writeClrPrimitiveField(objectAddressHex, fieldName, value);
}

QVariantMap ApplicationController::writeClrPrimitivePath(const QString& objectAddressHex, const QString& path, const QString& value) {
    return m_clrInspectorBridge->writeClrPrimitivePath(objectAddressHex, path, value);
}

QVariantMap ApplicationController::writeClrPrimitivePathBatch(const QString& objectAddressHex, const QVariantList& operations) {
    return m_clrInspectorBridge->writeClrPrimitivePathBatch(objectAddressHex, operations);
}

QVariantMap ApplicationController::writeClrPrimitivePathByLocator(const QString& typeSubstring, const QString& identityField, const QString& identityValue, const QString& path, const QString& value) {
    return m_clrInspectorBridge->writeClrPrimitivePathByLocator(typeSubstring, identityField, identityValue, path, value);
}

QVariantMap ApplicationController::writeClrPrimitivePathBatchByLocator(const QString& typeSubstring, const QString& identityField, const QString& identityValue, const QVariantList& operations) {
    return m_clrInspectorBridge->writeClrPrimitivePathBatchByLocator(typeSubstring, identityField, identityValue, operations);
}

QVariantMap ApplicationController::writeClrPrimitivePathBatchAtomic(const QString& objectAddressHex, const QVariantList& operations) {
    return m_clrInspectorBridge->writeClrPrimitivePathBatchAtomic(objectAddressHex, operations);
}

QVariantMap ApplicationController::enumerateClrRoots(const QString& typeSubstring) {
    return m_clrInspectorBridge->enumerateClrRoots(typeSubstring);
}

QVariantMap ApplicationController::callClrInstanceMethod(const QString& objectAddressHex, const QString& methodName, const QString& valueText, const QString& valueType) {
    return m_clrInspectorBridge->callClrInstanceMethod(objectAddressHex, methodName, valueText, valueType);
}

QVariantMap ApplicationController::findClrGcRootPath(const QString& targetObjectAddressHex, int maxDepth, int maxRootsScanned) {
    return m_clrInspectorBridge->findClrGcRootPath(targetObjectAddressHex, maxDepth, maxRootsScanned);
}

QVariantMap ApplicationController::generateClrObjectReport(const QString& objectAddressHex, int maxDepth, int maxNodes, bool includeGcRootChain) {
    return m_clrInspectorBridge->generateClrObjectReport(objectAddressHex, maxDepth, maxNodes, includeGcRootChain);
}

QVariantMap ApplicationController::disassembleClrMethod(const QString& objectAddressHex, const QString& methodName, int instructionCount) {
    return m_clrInspectorBridge->disassembleClrMethod(objectAddressHex, methodName, instructionCount);
}
QVariantMap ApplicationController::probeKernelDriver() const {
    return m_kernelDriverManager->probeKernelDriver();
}

QVariantMap ApplicationController::startKernelDriver() const {
    return m_kernelDriverManager->startKernelDriver();
}

QVariantMap ApplicationController::readMemoryKernel(const QString& addressHex, int size) const {
    return m_kernelDriverManager->readMemoryKernel(addressHex, size);
}

QVariantMap ApplicationController::writeMemoryKernel(const QString& addressHex, const QString& hexBytes) {
    return m_kernelDriverManager->writeMemoryKernel(addressHex, hexBytes);
}

QVariantMap ApplicationController::writeMemoryValueKernel(const QString& addressHex, const QString& valueType, const QString& value) {
    QVariantMap result;
    result["success"] = false;

    if (!m_handle.isValid()) {
        result["error"] = "Aucun processus attaché.";
        return result;
    }

    uint64_t address = 0;
    if (!parseHexAddress(addressHex, &address)) {
        result["error"] = "Adresse invalide.";
        return result;
    }

    killcore::ValueType type;
    if (!killcore::parseValueType(valueType, &type)) {
        result["error"] = "Type invalide.";
        return result;
    }

    killcore::ScanValue scanValue;
    QString parseError;
    if (!killcore::parseScanValue(value, type, &scanValue, &parseError)) {
        result["error"] = parseError;
        return result;
    }

    const QByteArray data = killcore::scanValueToBytes(scanValue);

#ifdef Q_OS_WIN
    const killcore::KernelDriverBridge bridge;
    const auto targetPid = reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(m_handle.pid()));
    const bool written = bridge.writeMemory(targetPid, address, data);
    result["success"] = written;
    if (written) {
        result["bytesWritten"] = data.size();
        // Meme choke point de detection "l'ecriture est repartie toute
        // seule" que writeMemoryValue (H2, docs/STRATEGY_ROOM.md) : une
        // ecriture kernel qui ne tient pas est justement le signal le plus
        // fort qu'il s'agit d'un compteur anime (cf. disassembleBackward),
        // pas d'une simple protection usermode contournable.
        m_writeFreezeCoreManager->watchSuccessfulWrite(address, type, data);
        KE_LOG_INFO() << "writeMemoryValueKernel: pid=" << m_handle.pid() << " address=0x" << QString::number(address, 16).toStdString()
                      << " bytesWritten=" << data.size();
    } else {
        result["error"] = "Écriture kernel échouée (driver non chargé/non connecté, adresse invalide côté cible, ou accès refusé).";
        KE_LOG_WARN() << "writeMemoryValueKernel: échec pid=" << m_handle.pid() << " address=0x" << QString::number(address, 16).toStdString();
    }
#else
    result["error"] = "Fonctionnalité Windows uniquement.";
#endif
    return result;
}

QVariantMap ApplicationController::getAiModelStatus() const {
    return m_settingsDiagnosticsManager->getAiModelStatus();
}

QVariantMap ApplicationController::browseForModelFile() {
    return m_settingsDiagnosticsManager->browseForModelFile();
}

QVariantMap ApplicationController::saveSettings(const QVariantMap& settings) {
    return m_settingsDiagnosticsManager->saveSettings(settings);
}

QString ApplicationController::getLogFilePath() const {
    return m_settingsDiagnosticsManager->getLogFilePath();
}

QString ApplicationController::getSmartSearchDebugFilePath() const {
    return m_settingsDiagnosticsManager->getSmartSearchDebugFilePath();
}

QString ApplicationController::getScanTelemetryFilePath() const {
    return m_settingsDiagnosticsManager->getScanTelemetryFilePath();
}

QVariantMap ApplicationController::getSmartSearchDebugEvents(int maxEvents) const {
    return m_settingsDiagnosticsManager->getSmartSearchDebugEvents(maxEvents);
}

QVariantMap ApplicationController::clearSmartSearchDebugEvents() {
    return m_settingsDiagnosticsManager->clearSmartSearchDebugEvents();
}

QVariantMap ApplicationController::getLogTail(int maxLines) const {
    return m_settingsDiagnosticsManager->getLogTail(maxLines);
}

QVariantMap ApplicationController::exportDiagnostics() {
    return m_settingsDiagnosticsManager->exportDiagnostics();
}

QString ApplicationController::smartSearchDebugFilePath() const {
    return m_settingsDiagnosticsManager->smartSearchDebugFilePath();
}

QString ApplicationController::scanTelemetryFilePath() const {
    return m_settingsDiagnosticsManager->scanTelemetryFilePath();
}

void ApplicationController::appendSmartSearchDebug(const QString& event, const QVariantMap& payload) const {
    m_settingsDiagnosticsManager->appendSmartSearchDebug(event, payload);
}

void ApplicationController::appendScanTelemetry(const QString& event, const QVariantMap& payload) const {
    m_settingsDiagnosticsManager->appendScanTelemetry(event, payload);
}
// ---------------------------------------------------------------------------
// Phase 11 — Profils
// ---------------------------------------------------------------------------

QVariantMap ApplicationController::saveProfileTarget(
    const QString& profileName,
    const QString& targetName,
    const QString& addressHex,
    const QString& valueType,
    const QString& description) {
    return m_profileManager->saveProfileTarget(profileName, targetName, addressHex, valueType, description);
}

QVariantMap ApplicationController::saveClrFieldProfileTarget(
    const QString& profileName,
    const QString& targetName,
    const QString& typeSubstring,
    const QString& identityField,
    const QString& identityValue,
    const QString& targetField,
    const QString& valueType,
    const QString& description) {
    return m_profileManager->saveClrFieldProfileTarget(profileName, targetName, typeSubstring, identityField, identityValue, targetField, valueType, description);
}

QVariantList ApplicationController::listProfiles() {
    return m_profileManager->listProfiles();
}

QVariantMap ApplicationController::loadProfile(const QString& profileName) {
    return m_profileManager->loadProfile(profileName);
}

bool ApplicationController::deleteProfile(const QString& profileName) {
    return m_profileManager->deleteProfile(profileName);
}

QVariantMap ApplicationController::resolveProfileTarget(const QString& profileName, const QString& targetName) {
    return m_profileManager->resolveProfileTarget(profileName, targetName);
}

QVariantMap ApplicationController::comparePointerMapAcrossRestart(const QString& profileName) {
    return m_profileManager->comparePointerMapAcrossRestart(profileName);
}

QVariantMap ApplicationController::exportPointerMap(const QString& profileName) {
    return m_profileManager->exportPointerMap(profileName);
}

QVariantMap ApplicationController::importPointerMap(
    const QString& profileName,
    const QString& pointerMapJson,
    const QVariantMap& options) {
    return m_profileManager->importPointerMap(profileName, pointerMapJson, options);
}

QVariantMap ApplicationController::setProfileTargetDependencies(
    const QString& profileName,
    const QString& targetName,
    const QVariantList& dependencyNames) {
    return m_profileManager->setProfileTargetDependencies(profileName, targetName, dependencyNames);
}

QVariantMap ApplicationController::exportGhidraArtifacts(const QString& profileName) {
    return m_profileManager->exportGhidraArtifacts(profileName);
}

QVariantMap ApplicationController::importGhidraSymbols(const QString& profileName, const QString& symbolsText) {
    return m_profileManager->importGhidraSymbols(profileName, symbolsText);
}

QVariantMap ApplicationController::activateProfileTarget(const QString& profileName, const QString& targetName) {
    return m_profileManager->activateProfileTarget(profileName, targetName);
}

QVariantMap ApplicationController::saveProfileCodePatch(
    const QString& profileName,
    const QString& patchName,
    const QString& addressHex,
    const QString& aobPattern,
    const QString& patchBytes,
    const QVariantMap& metadata) {
    return m_profileManager->saveProfileCodePatch(profileName, patchName, addressHex, aobPattern, patchBytes, metadata);
}

QVariantMap ApplicationController::applyProfileCodePatch(const QString& profileName, const QString& patchName) {
    return m_profileManager->applyProfileCodePatch(profileName, patchName);
}

QVariantMap ApplicationController::restoreProfileCodePatch(const QString& profileName, const QString& patchName) {
    return m_profileManager->restoreProfileCodePatch(profileName, patchName);
}

QVariantMap ApplicationController::applyAllProfileCodePatches(const QString& profileName) {
    return m_profileManager->applyAllProfileCodePatches(profileName);
}

QVariantMap ApplicationController::restoreAllProfileCodePatches(const QString& profileName) {
    return m_profileManager->restoreAllProfileCodePatches(profileName);
}

QVariantMap ApplicationController::inspectProfileCodePatches(const QString& profileName) {
    return m_profileManager->inspectProfileCodePatches(profileName);
}

QVariantMap ApplicationController::saveProfileAutoAsmScript(
    const QString& profileName,
    const QString& scriptName,
    const QString& scriptText,
    const QVariantMap& metadata) {
    return m_profileManager->saveProfileAutoAsmScript(profileName, scriptName, scriptText, metadata);
}

QVariantMap ApplicationController::applyProfileAutoAsmScript(const QString& profileName, const QString& scriptName) {
    return m_profileManager->applyProfileAutoAsmScript(profileName, scriptName);
}

QVariantMap ApplicationController::deleteProfileAutoAsmScript(const QString& profileName, const QString& scriptName) {
    return m_profileManager->deleteProfileAutoAsmScript(profileName, scriptName);
}
QVariantMap ApplicationController::getLuaScriptingStatus() const {
    const QString luaPath = findLuaExecutable();
    const QString helperPath = findKillEngineLuaHelper();
    const QFileInfo helperFile(helperPath);

    QVariantMap result;
    result["success"] = true;
    result["available"] = !luaPath.isEmpty();
    result["luaPath"] = luaPath;
    result["helperAvailable"] = !helperPath.isEmpty();
    result["helperPath"] = helperPath;
    result["helperDirectory"] = helperFile.exists() ? helperFile.absolutePath() : QString();
    result["pipeName"] = QStringLiteral("KillEngineAutomationPipe");
    // PHASE Automation (29/08/2026) : reflete l'etat REEL du pipe (variable
    // d'environnement dev OU mode Automation persistant active depuis
    // Settings), pas seulement l'ancien chemin env var -- sinon ce bandeau
    // resterait affiche a tort apres activation via le toggle.
    result["automationPipeOptIn"] = m_automationPipeManager->isRunning();
    result["message"] = luaPath.isEmpty()
        ? QStringLiteral("Aucun interpréteur Lua trouvé dans runtime/lua, lua, le dossier de l'application ou le PATH.")
        : QStringLiteral("Lua externe prêt. Les appels KillEngine passent par le pipe d'automatisation local.");
    return result;
}

namespace {
QVariantMap luaScriptRunOutcomeToVariant(const LuaScriptRunOutcome& outcome) {
    QVariantMap result;
    result["success"] = outcome.success;
    result["started"] = outcome.started;
    result["timedOut"] = outcome.timedOut;
    result["cancelled"] = outcome.cancelled;
    result["exitCode"] = outcome.exitCode;
    result["luaPath"] = outcome.luaPath;
    result["helperPath"] = outcome.helperPath;
    result["stdout"] = outcome.stdoutText;
    result["stderr"] = outcome.stderrText;
    result["error"] = outcome.error;
    return result;
}
} // namespace

QVariantMap ApplicationController::executeLuaScript(const QString& scriptText, const QVariantMap& options) {
    const LuaScriptRunOutcome outcome = runLuaScriptProcess(scriptText, options, nullptr);
    QVariantMap result = luaScriptRunOutcomeToVariant(outcome);

    appendScanTelemetry(QStringLiteral("lua_script_execute"), {
        {"success", outcome.success},
        {"exitCode", outcome.exitCode},
        {"timedOut", outcome.timedOut},
        {"stdoutBytes", outcome.stdoutText.toUtf8().size()},
        {"stderrBytes", outcome.stderrText.toUtf8().size()},
    });
    return result;
}

QVariantMap ApplicationController::executeLuaScriptAsync(const QString& scriptText, const QVariantMap& options) {
    QVariantMap result;
    result["success"] = false;
    result["started"] = false;

    if (m_luaScriptInProgress) {
        result["error"] = "Un script Lua est déjà en cours d'exécution.";
        return result;
    }
    if (scriptText.trimmed().isEmpty()) {
        result["error"] = "Script Lua vide.";
        return result;
    }

    const int requestId = m_nextDebugRequestId++;
    const QString requestedScript = scriptText;
    const QVariantMap requestedOptions = options;
    const QPointer<ApplicationController> self(this);
    auto cancellation = std::make_shared<killcore::CancellationToken>();

    m_luaScriptInProgress = true;
    m_activeLuaScriptCancellation = cancellation;

    KE_LOG_INFO() << "executeLuaScriptAsync(requestId=" << requestId
                  << ", scriptBytes=" << requestedScript.toUtf8().size() << ")";

    std::thread([self, requestId, requestedScript, requestedOptions, cancellation]() {
        const LuaScriptRunOutcome outcome = runLuaScriptProcess(requestedScript, requestedOptions, cancellation.get());

        if (!self) {
            return;
        }

        QMetaObject::invokeMethod(self.data(), [self, requestId, outcome]() {
            if (!self) {
                return;
            }

            QVariantMap finished = luaScriptRunOutcomeToVariant(outcome);
            finished["requestId"] = requestId;
            finished["kind"] = "lua_script_execute";

            self->m_luaScriptInProgress = false;
            self->m_activeLuaScriptCancellation.reset();
            self->appendScanTelemetry(QStringLiteral("lua_script_execute"), {
                {"success", outcome.success},
                {"exitCode", outcome.exitCode},
                {"timedOut", outcome.timedOut},
                {"cancelled", outcome.cancelled},
                {"stdoutBytes", outcome.stdoutText.toUtf8().size()},
                {"stderrBytes", outcome.stderrText.toUtf8().size()},
            });
            emit self->luaScriptExecutionFinished(finished);
        }, Qt::QueuedConnection);
    }).detach();

    result["success"] = true;
    result["started"] = true;
    result["requestId"] = requestId;
    result["error"] = "";
    return result;
}

QVariantMap ApplicationController::cancelLuaScriptExecution() {
    QVariantMap result;
    result["success"] = false;
    if (!m_luaScriptInProgress || !m_activeLuaScriptCancellation) {
        result["error"] = "Aucun script Lua actif à annuler.";
        return result;
    }

    m_activeLuaScriptCancellation->cancel();
    result["success"] = true;
    result["error"] = "";
    return result;
}

QVariantMap ApplicationController::saveProfileLuaScript(
    const QString& profileName,
    const QString& scriptName,
    const QString& scriptText,
    const QVariantMap& metadata) {
    return m_profileManager->saveProfileLuaScript(profileName, scriptName, scriptText, metadata);
}

QVariantMap ApplicationController::deleteProfileLuaScript(const QString& profileName, const QString& scriptName) {
    return m_profileManager->deleteProfileLuaScript(profileName, scriptName);
}
// ---------------------------------------------------------------------------
// Phase 14 — Pointer Chains (jeux modernes / applications dynamiques)
// ---------------------------------------------------------------------------

QVariantMap ApplicationController::scanPointerChains(
    const QString& addressHex,
    const QVariantMap& scanOptions) {
    return m_profileManager->scanPointerChains(addressHex, scanOptions);
}

QVariantMap ApplicationController::resolvePointerChain(const QVariantMap& chain) {
    return m_profileManager->resolvePointerChain(chain);
}

QVariantMap ApplicationController::suggestStableLocatorForAddress(
    const QString& addressHex,
    const QVariantMap& options) {
    return m_profileManager->suggestStableLocatorForAddress(addressHex, options);
}
void ApplicationController::setWebEnginePage(QWebEnginePage* page) {
    m_webEnginePage = page;
}

namespace {
// PHASE 119 -- liste blanche cote C++ des actions de store Pinia autorisees
// via callVueStoreAction(). Doit rester intentionnellement petite : chaque
// entree ajoutee ici est une fonction de ui/src/stores/app.ts qui devient
// executable depuis le pipe d'automatisation sans confirmRiskAction (le
// RiskGate frontend vit cote UI, pas dans ces fonctions elles-memes -- voir
// la note en tete d'automation_pipe_server.h sur le bypass volontaire du
// RiskGate). La meme liste doit exister independamment cote JS
// (window.__killengineAutomationBridge dans app.ts) : les deux doivent
// matcher pour qu'un appel aboutisse.
const QSet<QString>& allowedVueStoreActions() {
    static const QSet<QString> kAllowed = {
        QStringLiteral("keepCandidate"),
        QStringLiteral("ignoreCandidate"),
        QStringLiteral("addAddressToWatch"),
        QStringLiteral("writeSelectedValue"),
        QStringLiteral("writeSelectedAddresses"),
        QStringLiteral("writeSelectedTargets"),
        QStringLiteral("writeSelectedAtomic"),
        QStringLiteral("rollbackLastWrite"),
        QStringLiteral("rollbackLastWriteBatch"),
        QStringLiteral("freezeCandidateCurrent"),
        QStringLiteral("toggleFreeze"),
        QStringLiteral("startBreakpointFreeze"),
        QStringLiteral("createTrainerFeature"),
        QStringLiteral("deleteTrainerFeature"),
        QStringLiteral("applyTrainerFeature"),
        QStringLiteral("restoreTrainerFeature"),
        QStringLiteral("applyAllTrainerFeatures"),
        QStringLiteral("restoreAllTrainerFeatures"),
        QStringLiteral("getTrainerFeaturesSnapshot"),
        QStringLiteral("generateTrainerFeaturePointerChain"),
    };
    return kAllowed;
}
} // namespace

QVariantMap ApplicationController::callVueStoreAction(const QString& action, const QVariantList& args) {
    QVariantMap result;
    result["success"] = false;
    result["action"] = action;

    if (!allowedVueStoreActions().contains(action)) {
        result["error"] = "Action non autorisee (liste blanche C++ callVueStoreAction) : " + action;
        return result;
    }
    if (!m_webEnginePage) {
        result["error"] = "QWebEnginePage non initialisee (setWebEnginePage jamais appele).";
        return result;
    }

    // Serialise [action, args] en JSON via QJsonDocument -- jamais de
    // concatenation de string dans le script JS ci-dessous, pour qu'une
    // valeur d'argument contenant des guillemets/backslashes ne puisse pas
    // casser hors de son contexte de valeur JSON.
    QJsonArray callArgs;
    callArgs.append(action);
    callArgs.append(QJsonArray::fromVariantList(args));
    const QByteArray callArgsJson = QJsonDocument(callArgs).toJson(QJsonDocument::Compact);

    const QString script = QStringLiteral(
        "(function(){"
        "try{"
        "var call=%1;"
        "var bridge=window.__killengineAutomationBridge;"
        "if(!bridge||typeof bridge.dispatch!=='function'){"
        "return {success:false,error:'bridge indisponible (page pas encore chargee ou store pas initialise)'};"
        "}"
        "var r=bridge.dispatch(call[0],call[1]);"
        "return {success:true,result:(r===undefined?null:r)};"
        "}catch(e){"
        "return {success:false,error:String(e&&e.message?e.message:e)};"
        "}"
        "})()"
    ).arg(QString::fromUtf8(callArgsJson));

    // Etat partage sur le tas (pas de capture par reference sur des
    // variables locales) : runJavaScript() peut invoquer son callback bien
    // apres l'expiration du timeout ci-dessous si la reponse IPC du
    // renderer est en retard (observe en direct : le callback pour
    // getTrainerFeaturesSnapshot pouvait arriver ~5s+ apres l'appel). Si la
    // fonction avait deja retourne (timeout ecoule), l'ancienne capture
    // [&] ecrivait alors dans une pile deja depilee/reutilisee par un appel
    // suivant -> corruption memoire, crash SIGSEGV reproduit sur "liste le
    // trainer" (PHASE 168). QPointer detecte automatiquement la destruction
    // de la QEventLoop locale, donc le callback tardif devient un no-op sur.
    struct JsCallState {
        QVariant jsResult;
        bool finished = false;
        QPointer<QEventLoop> loop;
    };
    auto state = std::make_shared<JsCallState>();

    QEventLoop loop;
    state->loop = &loop;
    QTimer timeoutTimer;
    timeoutTimer.setSingleShot(true);
    QObject::connect(&timeoutTimer, &QTimer::timeout, &loop, &QEventLoop::quit);
    m_webEnginePage->runJavaScript(script, [state](const QVariant& value) {
        state->jsResult = value;
        state->finished = true;
        if (state->loop) {
            state->loop->quit();
        }
    });
    timeoutTimer.start(5000);
    loop.exec();
    state->loop = nullptr;

    if (!state->finished) {
        result["error"] = "Timeout (5s) en attendant la reponse JS -- la page a-t-elle bien fini de charger le store ?";
        return result;
    }

    const QVariantMap jsMap = state->jsResult.toMap();
    const bool jsSuccess = jsMap.value("success", false).toBool();
    result["success"] = jsSuccess;
    if (jsSuccess) {
        result["result"] = jsMap.value("result");
    } else {
        result["error"] = jsMap.value("error", "Erreur JS inconnue (reponse non reconnue).").toString();
    }
    return result;
}

void ApplicationController::ensureAutomationPipeStartedIfConfigured() {
    m_automationPipeManager->ensureStartedIfConfigured();
}

QVariantMap ApplicationController::enableAutomationMode() {
    return m_automationPipeManager->enableAutomationMode();
}

QVariantMap ApplicationController::disableAutomationMode() {
    return m_automationPipeManager->disableAutomationMode();
}

QVariantMap ApplicationController::getAutomationPipeStatus() {
    return m_automationPipeManager->getAutomationPipeStatus();
}

QVariantMap ApplicationController::savePointerChainProfileTarget(
    const QString& profileName,
    const QString& targetName,
    const QVariantMap& chain,
    const QString& valueType,
    const QString& description) {
    return m_profileManager->savePointerChainProfileTarget(profileName, targetName, chain, valueType, description);
}

QVariantMap ApplicationController::applyStealthMode(const QString& profile) {
    QVariantMap result;
    if (!m_attached) {
        result["success"] = false;
        result["error"] = "Not attached to a process";
        return result;
    }
    if (m_stealthActive) {
        result["success"] = false;
        result["error"] = "Stealth mode already active (profile: " + m_stealthProfile + ")";
        return result;
    }

    bool antiDebug = false;
    bool processMask = false;
    bool dllMask = false;

    if (profile == "sc2") {
        antiDebug = true;
        processMask = true;
        dllMask = true;
    } else if (profile == "default") {
        antiDebug = true;
    } else if (profile == "minimal") {
        processMask = true;
    } else {
        result["success"] = false;
        result["error"] = "Unknown profile: " + profile + ". Supported: sc2, default, minimal";
        return result;
    }

    QStringList errors;
    int modulesActivated = 0;

    if (antiDebug) {
        auto antiResult = m_antiDebugSession.start(m_handle);
        if (antiResult.success) {
            modulesActivated++;
        } else {
            errors << "AntiDebug: " + antiResult.error;
        }
    }

    if (processMask) {
        auto maskResult = killcore::ProcessMask::maskCurrentProcess("svchost.exe");
        if (maskResult.success) {
            modulesActivated++;
            m_stealthProcessMaskActive = true;
        } else {
            errors << "ProcessMask: " + maskResult.error;
        }
    }

    if (dllMask) {
        auto dllResult = killcore::DllMask::maskDll(static_cast<uint32_t>(m_pid), "KillEnginePageGuardHandler.dll");
        if (dllResult.success) {
            modulesActivated++;
            m_stealthDllMaskActive = true;
        } else {
            errors << "DllMask: " + dllResult.error;
        }
    }

    m_stealthActive = m_antiDebugSession.isActive() || m_stealthProcessMaskActive || m_stealthDllMaskActive;
    m_stealthProfile = m_stealthActive ? profile : QString();

    result["success"] = m_stealthActive;
    result["profile"] = m_stealthProfile;
    result["modulesActivated"] = modulesActivated;
    result["modules"] = QVariantMap{
        {"antiDebug", m_antiDebugSession.isActive()},
        {"processMask", m_stealthProcessMaskActive},
        {"dllMask", m_stealthDllMaskActive}
    };
    if (!m_stealthActive) {
        result["error"] = "No stealth modules activated";
    }
    if (!errors.isEmpty()) {
        result["warnings"] = errors;
    }

    return result;
}

QVariantMap ApplicationController::restoreStealthMode() {
    QVariantMap result;
    if (!m_stealthActive) {
        result["success"] = false;
        result["error"] = "Stealth mode is not active";
        return result;
    }

    m_antiDebugSession.stop();
    QStringList warnings;
    if (m_stealthProcessMaskActive) {
        auto restoreResult = killcore::ProcessMask::restoreOriginalName();
        if (!restoreResult.success) {
            warnings << restoreResult.error;
        }
    }
    if (m_stealthDllMaskActive) {
        auto restoreResult = killcore::DllMask::restoreDll(static_cast<uint32_t>(m_pid), "KillEnginePageGuardHandler.dll");
        if (!restoreResult.success) {
            warnings << restoreResult.error;
        }
    }
    m_stealthActive = false;
    m_stealthProfile.clear();
    m_stealthProcessMaskActive = false;
    m_stealthDllMaskActive = false;

    result["success"] = true;
    result["restored"] = true;
    if (!warnings.isEmpty()) {
        result["warnings"] = warnings;
    }

    return result;
}

QVariantMap ApplicationController::getStealthModeStatus() const {
    QVariantMap result;
    result["active"] = m_stealthActive;
    result["profile"] = m_stealthProfile;
    result["modules"] = QVariantMap{
        {"antiDebug", m_antiDebugSession.isActive()},
        {"processMask", m_stealthProcessMaskActive},
        {"dllMask", m_stealthDllMaskActive}
    };
    return result;
}

} // namespace killengine
