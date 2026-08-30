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
#include "kernel_driver_manager.h"
#include "profile_manager.h"
#include "save_file_investigator.h"
#include "scanning_core_manager.h"
#include "settings_diagnostics_manager.h"
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
    QVariantMap result;
    QVariantList suggestions;
    const QStringList addressTexts = hexAddressesFromText(query);

    result["query"] = query;
    result["aiReady"] = m_ai.isReady();
    result["status"] = "memory_targets_activated";
    result["actionStatus"] = "not_executed";
    result["workflowStatus"] = "memory_targets_ready";

    auto writeState = autoWriteState();
    writeState.clearChatTargets();
    writeState.clearLastTargets();
    m_autoWriteValueHistory.clear();
    m_smartSearchActive = false;
    m_smartSearchInitialValue.clear();
    m_smartSearchTargetValue.clear();

    for (const auto& addressText : addressTexts) {
        uint64_t address = 0;
        if (!parseHexAddress(addressText, &address)) {
            continue;
        }

        bool alreadyAdded = false;
        for (const auto& target : writeState.chatTargets()) {
            if (target.address == address) {
                alreadyAdded = true;
                break;
            }
        }
        if (alreadyAdded) {
            continue;
        }

        const AutoWriteTarget target{address, killcore::ValueType::Int32, /*chatOrigin=*/true};
        writeState.appendChatTarget(target);
        writeState.appendLastTarget(target);

        QVariantMap suggestion;
        suggestion["source"] = "chat_address";
        suggestion["address"] = QString::number(address, 16);
        suggestion["type"] = killcore::valueTypeToString(target.type);
        suggestions.append(suggestion);
    }

    result["success"] = writeState.hasChatTargets();
    result["targetCount"] = writeState.chatTargetCount();
    result["suggestedWrites"] = suggestions;
    result["message"] = !writeState.hasChatTargets()
        ? QString("Je n'ai pas reconnu d'adresse mémoire valide dans ton message.")
        : QString("J'ai sélectionné %1 adresse(s) mémoire depuis ton message. Donne-moi maintenant la valeur à écrire dessus.")
              .arg(writeState.chatTargetCount());
    appendSmartSearchDebug("chat_memory_targets_activated", result);
    return result;
}

QVariantMap ApplicationController::writeChatMemoryTargetsFromQuery(const QString& query, const QString& value) {
    QVariantMap result;
    QVariantList suggestions;
    QVariantList writeResults;

    result["query"] = query;
    result["aiReady"] = m_ai.isReady();
    result["status"] = "tool_call";
    result["tool"] = "chat_memory_write";
    result["actionStatus"] = "executed";
    result["workflowStatus"] = "auto_write_done";
    result["targetValue"] = value;

    bool allWritesOk = true;
    const QString previousTargetValue = m_smartSearchTargetValue;
    m_smartSearchTargetValue = value;
    auto writeState = autoWriteState();
    m_lastBatchStartIndex = writeState.writeHistorySize();
    writeState.clearLastTargets();

    for (const auto& target : writeState.chatTargets()) {
        QVariantMap suggestion;
        suggestion["source"] = "chat_address";
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
        writeResult.insert("source", suggestion.value("source"));
        writeResult.insert("address", suggestion.value("address"));
        writeResult.insert("value", value);
        writeResult.insert("type", suggestion.value("type"));
        if (suggestion.contains("valueHistory")) {
            writeResult.insert("valueHistory", suggestion.value("valueHistory"));
        }
        allWritesOk = allWritesOk && writeResult.value("success").toBool();
        writeResults.append(writeResult);

        if (writeResult.value("success").toBool()) {
            writeState.appendLastTarget(target);
        }
    }

    m_lastBatchEndIndex = writeState.writeHistorySize();
    if (m_lastBatchEndIndex == m_lastBatchStartIndex) {
        m_lastBatchStartIndex = -1;
        m_lastBatchEndIndex = -1;
        writeState.clearLastTargets();
    }
    if (allWritesOk && writeState.hasLastTargets()) {
        m_smartSearchActive = false;
        writeState.replaceChatTargetsWithLastTargets();
        resetFailureEscalationState();
        if (m_autoWriteValueHistory.isEmpty() && !previousTargetValue.isEmpty()) {
            appendDistinctText(&m_autoWriteValueHistory, previousTargetValue, 12);
        }
        appendDistinctText(&m_autoWriteValueHistory, value, 12);
    }

    QVariantMap actionResult;
    actionResult["success"] = allWritesOk;
    actionResult["remaining"] = static_cast<qulonglong>(writeState.chatTargetCount());
    actionResult["error"] = allWritesOk ? QString() : QString("Au moins une écriture sur adresse donnée a échoué.");

    result["success"] = allWritesOk;
    result["actionResult"] = actionResult;
    result["suggestedWrites"] = suggestions;
    result["suggestedWrite"] = suggestions.isEmpty() ? QVariantMap{} : suggestions.first().toMap();
    result["autoWriteResults"] = writeResults;
    result["autoWriteResult"] = writeResults.isEmpty() ? QVariantMap{} : writeResults.last().toMap();
    result["autoWriteCount"] = writeResults.size();
    result["activeTargetCount"] = writeState.chatTargetCount();
    result["previousTargetValue"] = previousTargetValue;
    result["writeHistory"] = writeHistoryToVariantList(m_autoWriteValueHistory);
    result["rollbackNote"] = "Tu peux annuler cette écriture via le bouton rollback batch dans l'assistant.";
    result["message"] = allWritesOk
        ? QString("J'ai écrit %1 sur %2 adresse(s) mémoire sélectionnée(s) dans la conversation. Je garde ces adresses actives pour les prochaines modifications.")
              .arg(value)
              .arg(writeState.chatTargetCount())
        : QString("J'ai essayé d'écrire %1 sur les adresses mémoire sélectionnées, mais au moins une écriture a échoué.")
              .arg(value);
    appendSmartSearchDebug("chat_memory_write", result);
    return result;
}

QVariantMap ApplicationController::freezeChatMemoryTargetsFromQuery(const QString& query, const QString& value) {
    QVariantMap result;
    QVariantList suggestions;
    QVariantList freezeResults;

    result["query"] = query;
    result["aiReady"] = m_ai.isReady();
    result["status"] = "tool_call";
    result["tool"] = "chat_memory_freeze";
    result["actionStatus"] = "executed";
    result["workflowStatus"] = "freeze_done";
    result["targetValue"] = value;

    bool allFreezeOk = true;
    int frozenCount = 0;

    for (const auto& target : m_chatMemoryTargets) {
        QVariantMap suggestion;
        suggestion["source"] = "chat_address";
        suggestion["address"] = QString::number(target.address, 16);
        suggestion["type"] = killcore::valueTypeToString(target.type);
        suggestion["value"] = value;
        suggestions.append(suggestion);

        auto freezeResult = setFreezeValue(
            suggestion.value("address").toString(),
            suggestion.value("type").toString(),
            value,
            true);
        freezeResult.insert("source", suggestion.value("source"));
        freezeResult.insert("address", suggestion.value("address"));
        freezeResult.insert("value", value);
        freezeResult.insert("type", suggestion.value("type"));
        freezeResults.append(freezeResult);

        allFreezeOk = allFreezeOk && freezeResult.value("success").toBool();
        if (freezeResult.value("success").toBool()) {
            ++frozenCount;
        }
    }

    result["success"] = allFreezeOk && frozenCount > 0;
    result["suggestedWrites"] = suggestions;
    result["freezeResults"] = freezeResults;
    result["activeTargetCount"] = m_chatMemoryTargets.size();
    result["message"] = frozenCount > 0
        ? QString("Freeze activé sur %1/%2 adresse(s) active(s) à %3. Je garde ces adresses actives pour pouvoir modifier ensuite.")
              .arg(frozenCount)
              .arg(m_chatMemoryTargets.size())
              .arg(value)
        : QString("Je n'ai pas pu activer le freeze sur les adresses actives.");
    if (!allFreezeOk) {
        result["workflowStatus"] = "freeze_partial_or_failed";
        result["error"] = "Au moins un freeze a échoué.";
    }

    appendSmartSearchDebug("chat_memory_freeze", result);
    return result;
}

// RiskGate chat (29/08/2026) : points d'entree publics, appeles UNIQUEMENT
// apres un clic explicite sur le recoveryAction renvoye par startSmartSearch
// (le clic EST la confirmation, pas de second modal confirmRiskAction --
// retire a la demande du proprietaire, juge redondant avec la carte chat qui
// affiche deja l'avertissement + le libelle exact de l'action). N'ajoutent
// aucune logique d'ecriture : appellent directement les fonctions privees
// existantes, qui n'ont pas change. Query vide car ces fonctions ne
// re-parsent pas d'adresse depuis la query -- elles utilisent
// m_chatMemoryTargets/m_lastAutoWriteTargets deja peuples cote serveur.
QVariantMap ApplicationController::confirmChatMemoryWrite(const QString& value) {
    return writeChatMemoryTargetsFromQuery(QString(), value);
}

QVariantMap ApplicationController::confirmChatMemoryFreeze(const QString& value) {
    return freezeChatMemoryTargetsFromQuery(QString(), value);
}

QVariantMap ApplicationController::confirmRewriteLastAutoWrite(const QString& value) {
    return rewriteLastAutoWriteTargets(value, QString());
}

QVariantMap ApplicationController::getActiveChatMemoryTargets() const {
    QVariantMap result;
    QVariantList targets;

    for (const auto& target : m_chatMemoryTargets) {
        QVariantMap entry;
        entry["address"] = QString::number(target.address, 16);
        entry["type"] = killcore::valueTypeToString(target.type);
        targets.append(entry);
    }

    result["success"] = true;
    result["count"] = targets.size();
    result["targets"] = targets;
    return result;
}

QVariantMap ApplicationController::clearActiveChatMemoryTargets() {
    const int cleared = m_chatMemoryTargets.size();
    m_chatMemoryTargets.clear();
    m_lastAutoWriteTargets.clear();
    m_autoWriteValueHistory.clear();
    // Bouton "clear_targets" de l'echelle de secours (buildFailureEscalationRecovery) :
    // appele en direct depuis le frontend, ne passe pas par startSmartSearch,
    // donc rien d'autre ne remet a zero l'etat d'attente d'une relance en
    // cours. Sans ca, le prochain message sans rapport de l'utilisateur est
    // intercepte a tort par AnswerTraceUiStringPrompt/AnswerTraceUiFilterPrompt.
    resetFailureEscalationState();
    m_pendingUiStringCandidates.clear();

    QVariantMap result;
    result["success"] = true;
    result["cleared"] = cleared;
    result["targets"] = QVariantList{};
    appendSmartSearchDebug("chat_memory_targets_cleared", result);
    return result;
}

QVariantMap ApplicationController::clearScanContext() {
    QVariantMap result;
    auto state = scanState();
    const auto candidateCount = static_cast<qulonglong>(state.candidates().size());
    const bool hadUndo = m_hasPreviousCandidates;
    const bool hadSnapshot = !state.snapshot().isEmpty();
    const bool wasSmartSearchActive = m_smartSearchActive;

    state.clearCandidates();
    clearCandidateUndo();
    clearCandidateValueHistory();
    state.clearSnapshot();
    m_smartSearchActive = false;
    m_smartSearchInitialValue.clear();
    m_smartSearchTargetValue.clear();
    m_smartSearchValueType = "Int32";
    m_lastBatchStartIndex = -1;
    m_lastBatchEndIndex = -1;

    result["success"] = true;
    result["clearedCandidates"] = candidateCount;
    result["hadUndoReduction"] = hadUndo;
    result["hadUnknownSnapshot"] = hadSnapshot;
    result["wasSmartSearchActive"] = wasSmartSearchActive;
    result["message"] = QString("Contexte de scan vidé : %1 candidat(s) supprimé(s).").arg(candidateCount);
    appendSmartSearchDebug("scan_context_cleared", result);
    return result;
}

void ApplicationController::acknowledgePendingSmartSearchRecovery() {
    if (m_pendingRecoveryAction.isEmpty() && m_pendingUiStringCandidates.isEmpty()) {
        return;
    }
    appendSmartSearchDebug("smart_search_recovery_acknowledged_via_button", {
        {"pendingRecoveryAction", m_pendingRecoveryAction},
    });
    m_pendingRecoveryAction.clear();
    m_pendingUiStringCandidates.clear();
}

QVariantMap ApplicationController::getTemporaryStorageStatus() const {
    return m_settingsDiagnosticsManager->getTemporaryStorageStatus();
}

QVariantMap ApplicationController::clearTemporaryStorage() {
    return m_settingsDiagnosticsManager->clearTemporaryStorage();
}
QVariantMap ApplicationController::getSmartSearchContext() const {
    QVariantMap result;
    QVariantList chatTargets;
    QVariantList profileTargets;

    for (const auto& target : m_chatMemoryTargets) {
        QVariantMap entry;
        entry["address"] = QString::number(target.address, 16);
        entry["type"] = killcore::valueTypeToString(target.type);
        chatTargets.append(entry);
    }

    for (const auto& target : m_activeProfileTargets) {
        QVariantMap entry;
        entry["profile"] = target.profileName;
        entry["target"] = target.targetName;
        entry["group"] = target.groupName;
        entry["address"] = QString::number(target.address, 16);
        entry["type"] = killcore::valueTypeToString(target.type);
        entry["locatorKind"] = target.locatorKind == killcore::LocatorKind::ClrField ? "clr_field" : "memory";
        if (target.locatorKind == killcore::LocatorKind::ClrField) {
            entry["clrTypeSubstring"] = target.clrTypeSubstring;
            entry["clrIdentityField"] = target.clrIdentityField;
            entry["clrIdentityValue"] = target.clrIdentityValue;
            entry["clrFieldName"] = target.clrFieldName;
        }
        profileTargets.append(entry);
    }

    result["success"] = true;
    result["active"] = m_smartSearchActive || !m_chatMemoryTargets.isEmpty() || !m_activeProfileTargets.isEmpty();
    result["workflow"] = m_smartSearchActive
        ? "guided_scan"
        : (!m_chatMemoryTargets.isEmpty() ? "active_addresses" : (!m_activeProfileTargets.isEmpty() ? "active_profile" : "idle"));
    result["initialValue"] = m_smartSearchInitialValue;
    result["targetValue"] = m_smartSearchTargetValue;
    result["valueType"] = m_smartSearchValueType;
    result["candidateCount"] = static_cast<qulonglong>(scanState().candidates().size());
    result["hasUndoReduction"] = m_hasPreviousCandidates;
    result["chatTargets"] = chatTargets;
    result["profileTargets"] = profileTargets;
    result["lastAutoWriteCount"] = m_lastAutoWriteTargets.size();
    result["writeHistory"] = writeHistoryToVariantList(m_autoWriteValueHistory);
    return result;
}

QVariantMap ApplicationController::getAutoResolveReport(int maxEvents) const {
    QVariantMap result;
    result["success"] = true;
    result["attached"] = m_handle.isValid();
    result["processName"] = processName();
    result["candidateCount"] = static_cast<qulonglong>(scanState().candidates().size());
    result["workflow"] = m_smartSearchActive ? QString("guided_scan") : QString("idle");
    result["initialValue"] = m_smartSearchInitialValue;
    result["targetValue"] = m_smartSearchTargetValue;
    result["valueType"] = m_smartSearchValueType;
    result["activeChatTargetCount"] = m_chatMemoryTargets.size();
    result["activeProfileTargetCount"] = m_activeProfileTargets.size();

    QSettings settings;
    const QString gameKey = autoResolverGameKey(processName());
    settings.beginGroup(QString("autoResolver/process/%1").arg(gameKey));
    QVariantMap learnedProfile;
    learnedProfile["gameKey"] = gameKey;
    learnedProfile["starts"] = settings.value("starts", 0).toInt();
    learnedProfile["reductions"] = settings.value("reductions", 0).toInt();
    learnedProfile["noCandidateCount"] = settings.value("noCandidateCount", 0).toInt();
    learnedProfile["lowCandidateCheckpoints"] = settings.value("lowCandidateCheckpoints", 0).toInt();
    learnedProfile["lastWorkflow"] = settings.value("lastWorkflow").toString();
    learnedProfile["lastCandidateCount"] = settings.value("lastCandidateCount", 0).toULongLong();
    learnedProfile["lastUpdated"] = settings.value("lastUpdated").toString();
    learnedProfile["lastSuccessfulAuditEvent"] = settings.value("lastSuccessfulAuditEvent").toString();
    learnedProfile["lastSuccessfulAuditAt"] = settings.value("lastSuccessfulAuditAt").toString();
    learnedProfile["lastSuccessfulAddress"] = settings.value("lastSuccessfulAddress").toString();
    learnedProfile["lastSuccessfulValueType"] = settings.value("lastSuccessfulValueType").toString();
    learnedProfile["lastSuccessfulAobPattern"] = settings.value("lastSuccessfulAobPattern").toString();
    QVariantMap strategyWins;
    settings.beginGroup("strategyWins");
    const QStringList strategyKeys = settings.childKeys();
    for (const QString& key : strategyKeys) {
        strategyWins[key] = settings.value(key, 0).toInt();
    }
    settings.endGroup();
    learnedProfile["strategyWins"] = strategyWins;
    settings.endGroup();
    result["learnedProfile"] = learnedProfile;

    const int boundedMaxEvents = std::clamp(maxEvents, 5, 200);
    auto readEvents = [](const QString& path, int limit) {
        QVariantList events;
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            return events;
        }

        QList<QByteArray> lines;
        while (!file.atEnd()) {
            const QByteArray line = file.readLine().trimmed();
            if (!line.isEmpty()) {
                lines.append(line);
                if (lines.size() > limit) {
                    lines.removeFirst();
                }
            }
        }

        for (const QByteArray& line : lines) {
            QJsonParseError parseError;
            const QJsonDocument doc = QJsonDocument::fromJson(line, &parseError);
            if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
                continue;
            }
            events.append(doc.object().toVariantMap());
        }
        return events;
    };

    QVariantList recentEvents = readEvents(scanTelemetryFilePath(), boundedMaxEvents);
    const QVariantList debugEvents = readEvents(smartSearchDebugFilePath(), boundedMaxEvents);
    for (const QVariant& event : debugEvents) {
        recentEvents.append(event);
    }

    QVariantMap eventCounts;
    QVariantList lastSignals;
    for (const QVariant& item : recentEvents) {
        const QVariantMap event = item.toMap();
        const QString name = event.value("event").toString();
        if (!name.isEmpty()) {
            eventCounts[name] = eventCounts.value(name).toInt() + 1;
        }

        if (lastSignals.size() >= 12) {
            lastSignals.removeFirst();
        }

        QVariantMap signal;
        signal["event"] = name;
        signal["timestamp"] = event.value("timestamp").toString();
        signal["candidateStoreSize"] = event.value("candidateStoreSize", event.value("candidateCount"));
        signal["matchesFound"] = event.value("matchesFound", event.value("matchCount"));
        signal["remaining"] = event.value("remaining", event.value("stored"));
        signal["globalValueHits"] = event.value("globalValueHits");
        signal["error"] = event.value("error").toString();
        lastSignals.append(signal);
    }

    // Analyse pure de la telemetry (insights + rapport valeur affichee),
    // testee independamment dans tests/unit/test_auto_resolver.cpp. Seule
    // implementation de cette logique — voir la note d'architecture dans
    // ai/auto_resolver.h.
    QList<QVariantMap> eventMaps;
    eventMaps.reserve(recentEvents.size());
    for (const QVariant& item : recentEvents) {
        eventMaps.append(item.toMap());
    }
    const auto telemetryReport = killai::computeAutoResolveTelemetryReport(eventMaps);

    QVariantList telemetryInsights;
    for (const auto& insight : telemetryReport.insights) {
        telemetryInsights.append(QVariantMap{
            {"id", insight.id},
            {"label", insight.label},
            {"reason", insight.reason},
            {"nextAction", insight.nextAction},
            {"safe", insight.safe},
            {"requiresConfirmation", !insight.safe},
        });
    }

    QVariantMap displayValueReport;
    displayValueReport["enabled"] = telemetryReport.displayValueSignals;
    displayValueReport["pattern"] = telemetryReport.displayValuePattern;
    displayValueReport["traceUiSourceCount"] = telemetryReport.traceUiSourceCount;
    displayValueReport["globalValueHits"] = telemetryReport.traceUiGlobalHits;
    displayValueReport["exactZeroCount"] = telemetryReport.exactZeroCount;
    displayValueReport["recommendation"] = telemetryReport.displayValueRecommendation;
    displayValueReport["warnings"] = QVariantList{
        "Les strings UI peuvent etre des copies d'affichage, pas la source gameplay.",
        "Ne pas ecrire globalValueHits en masse; tester Top 5/Top 25 seulement.",
        "Si exact=0 et Trace UI donne des strings, privilegier source analysis avant patch/debug."
    };

    QVariantList recommendations;
    QVariantList guardrails;
    guardrails.append(QVariantMap{{"id", "no_auto_write"}, {"label", "Aucune écriture automatique sans confirmation"}, {"risk", "write"}});
    guardrails.append(QVariantMap{{"id", "no_auto_debug"}, {"label", "Aucun debugger/hardware breakpoint sans confirmation"}, {"risk", "debug"}});
    guardrails.append(QVariantMap{{"id", "no_auto_patch"}, {"label", "Aucun patch/injection sans confirmation"}, {"risk", "patch"}});

    const auto& candidates = scanState().candidates();

    if (!m_handle.isValid()) {
        recommendations.append(QVariantMap{{"id", "attach_process"}, {"label", "Attacher un processus"}, {"safe", true}, {"reason", "Aucun processus actif."}});
    } else if (!m_chatMemoryTargets.isEmpty() || !m_activeProfileTargets.isEmpty()) {
        recommendations.append(QVariantMap{{"id", "guarded_write"}, {"label", "Proposer une écriture confirmée"}, {"safe", false}, {"reason", "Des cibles mémoire sont déjà actives."}});
        recommendations.append(QVariantMap{{"id", "guarded_freeze"}, {"label", "Proposer un freeze confirmé"}, {"safe", false}, {"reason", "Des cibles mémoire sont déjà actives."}});
    } else if (m_smartSearchActive && !candidates.isEmpty()) {
        recommendations.append(QVariantMap{{"id", "reduce_with_new_value"}, {"label", "Réduire avec la nouvelle valeur observée"}, {"safe", true}, {"reason", "Une recherche guidée contient encore des candidats."}});
        if (candidates.size() <= kAutoWriteCandidateLimit) {
            recommendations.append(QVariantMap{{"id", "review_top_candidates"}, {"label", "Préparer un test d'écriture confirmé"}, {"safe", false}, {"reason", "Le nombre de candidats est assez bas."}});
        }
    } else {
        recommendations.append(QVariantMap{{"id", "exact_or_multitype"}, {"label", "Lancer un scan exact multi-type"}, {"safe", true}, {"reason", "Aucun contexte actif exploitable."}});
        recommendations.append(QVariantMap{{"id", "encrypted_scan"}, {"label", "Essayer un scan chiffré borné"}, {"safe", true}, {"reason", "Utile si le scan exact ne trouve rien."}});
        recommendations.append(QVariantMap{{"id", "unknown_capture"}, {"label", "Capturer unknown initial value"}, {"safe", true}, {"reason", "Utile quand la valeur réelle n'est pas connue ou transformée."}});
    }

    if (eventCounts.value("ui_string_investigation_finish").toInt() > 0 || eventCounts.value("ui_string_sources_analyze").toInt() > 0) {
        recommendations.append(QVariantMap{{"id", "trace_ui_sources"}, {"label", "Exploiter les sources Trace UI string"}, {"safe", true}, {"reason", "La télémétrie récente contient des pistes UI/string."}});
    }
    if (eventCounts.value("find_what_writes").toInt() > 0 || eventCounts.value("aob_signature").toInt() > 0) {
        recommendations.append(QVariantMap{{"id", "trainer_checkpoint"}, {"label", "Préparer checkpoint AOB/patch"}, {"safe", false}, {"reason", "Des signaux debugger/AOB existent déjà."}});
    }
    if (learnedProfile.value("noCandidateCount").toInt() >= 2) {
        recommendations.prepend(QVariantMap{{"id", "trace_ui_string"}, {"label", "Privilégier Trace UI string"}, {"safe", true}, {"reason", "Les scans exacts récents de ce processus ont souvent fini sans candidat."}});
        recommendations.prepend(QVariantMap{{"id", "encrypted_scan"}, {"label", "Privilégier scan chiffré"}, {"safe", true}, {"reason", "Mémoire locale : plusieurs scans sans candidat sur ce processus."}});
    }
    if (learnedProfile.value("noCandidateCount").toInt() >= 4) {
        // Signal fort de réallocation/instabilité mémoire persistante malgré
        // plusieurs stratégies déjà tentées (scan classique, Trace UI string,
        // scan chiffré) : suggérer d'isoler une éventuelle synchro serveur en
        // arrière-plan comme cause, avant de conclure à une réallocation
        // purement locale — voir blockProcessNetwork() et
        // docs/STRATEGY_ROOM.md, 24/08/2026 (cas Solitaire "Bulles").
        recommendations.prepend(QVariantMap{
            {"id", "block_process_network"},
            {"label", "Couper le réseau du processus (diagnostic)"},
            {"safe", false},
            {"requiresConfirmation", true},
            {"reason", "Plusieurs stratégies de scan ont échoué sur ce processus — la valeur est peut-être resynchronisée depuis un serveur en arrière-plan plutôt que purement locale."}
        });
    }
    if (learnedProfile.value("noCandidateCount").toInt() >= 6) {
        // PHASE 91 : au-dela de la coupure reseau (deja suggeree ci-dessus a
        // 4 echecs), une instabilite memoire qui persiste encore apres
        // isolation reseau suggere que la valeur affichee n'est peut-etre
        // meme pas fiablement en memoire — voir docs/PHASE_TRACKER.md
        // PHASE 90 (investigation Solitaire "Bulles") ou la vraie percee a
        // ete de chercher un fichier de sauvegarde sur disque apres l'echec
        // de toutes les pistes memoire.
        recommendations.prepend(QVariantMap{
            {"id", "discover_save_files"},
            {"label", "Chercher fichiers et paramètres UWP sur le disque"},
            {"safe", true},
            {"requiresConfirmation", false},
            {"reason", "De nombreuses strategies memoire ont echoue meme apres isolation reseau — la valeur affichee vient peut-etre d'un fichier de sauvegarde ou de LocalSettings plutot que d'une adresse memoire stable."}
        });
    }
    const QString lastSuccessfulAudit = learnedProfile.value("lastSuccessfulAuditEvent").toString();
    if (!lastSuccessfulAudit.isEmpty()) {
        recommendations.prepend(QVariantMap{
            {"id", "reuse_successful_strategy"},
            {"label", "Réutiliser la dernière stratégie gagnante"},
            {"safe", lastSuccessfulAudit.contains("write") || lastSuccessfulAudit.contains("freeze") ? false : true},
            {"requiresConfirmation", lastSuccessfulAudit.contains("write") || lastSuccessfulAudit.contains("freeze")},
            {"reason", QString("Dernière action validée pour ce processus : %1.").arg(lastSuccessfulAudit)}
        });
    }

    QVariantList strategyScores;
    auto addStrategyScore = [&strategyScores](const QString& id, const QString& label, int score, const QString& reason) {
        strategyScores.append(QVariantMap{{"id", id}, {"label", label}, {"score", score}, {"reason", reason}});
    };

    int exactScore = m_handle.isValid() ? 55 : 0;
    int reduceScore = (m_smartSearchActive && !candidates.isEmpty()) ? 95 : 0;
    int unknownScore = m_handle.isValid() ? 45 : 0;
    int encryptedScore = m_handle.isValid() ? 35 : 0;
    int traceUiScore = m_handle.isValid() ? 30 : 0;

    const QString processLower = processName().toLower();
    if (processLower.contains("sc2") || processLower.contains("starcraft")) {
        traceUiScore += 25;
        encryptedScore += 10;
    }
    if (learnedProfile.value("noCandidateCount").toInt() >= 2) {
        encryptedScore += 35;
        traceUiScore += 30;
        exactScore -= 20;
    }
    if (lastSuccessfulAudit.contains("aob")) {
        traceUiScore += 10;
    }
    if (lastSuccessfulAudit.contains("write") || lastSuccessfulAudit.contains("freeze")) {
        reduceScore += 10;
    }
    if (eventCounts.value("ui_string_scan").toInt() > 0 || eventCounts.value("ui_string_sources_analyze").toInt() > 0) {
        traceUiScore += 25;
    }
    if (candidates.size() > 50000) {
        unknownScore += 10;
        traceUiScore += 10;
    }

    addStrategyScore("reduce_with_new_value", "Réduire candidats existants", reduceScore, "Meilleur choix quand une recherche guidée est active.");
    addStrategyScore("exact_or_multitype", "Scan exact multi-type", exactScore, "Point d'entrée le plus rapide quand la valeur réelle est connue.");
    addStrategyScore("unknown_capture", "Unknown initial value", unknownScore, "Bon choix si la valeur bouge mais la représentation mémoire est inconnue.");
    addStrategyScore("encrypted_scan", "Scan chiffré borné", encryptedScore, "Bon choix si le scan exact échoue souvent.");
    addStrategyScore("trace_ui_string", "Trace UI string", traceUiScore, "Bon choix si le jeu affiche une copie UI plutôt que la source gameplay.");

    std::sort(strategyScores.begin(), strategyScores.end(), [](const QVariant& a, const QVariant& b) {
        return a.toMap().value("score").toInt() > b.toMap().value("score").toInt();
    });

    const QVariantMap preferredStrategy = strategyScores.isEmpty() ? QVariantMap{} : strategyScores.first().toMap();
    auto proactiveAction = [](const QString& id,
                              const QString& label,
                              const QString& tool,
                              int confidence,
                              bool safe,
                              const QString& risk,
                              const QString& reason) {
        QVariantMap action;
        action["id"] = id;
        action["label"] = label;
        action["tool"] = tool;
        action["confidence"] = std::clamp(confidence, 0, 100);
        action["safe"] = safe;
        action["requiresConfirmation"] = !safe;
        action["risk"] = risk;
        action["reason"] = reason;
        action["proactive"] = true;
        return action;
    };

    QVariantMap nextBestAction;
    if (!m_handle.isValid()) {
        nextBestAction = proactiveAction(
            "attach_process",
            "Attacher un processus",
            "attach_process",
            100,
            true,
            "safe",
            "Aucun processus actif : l'assistant ne peut pas scanner tant qu'une cible autorisée n'est pas attachée.");
    } else if (!m_chatMemoryTargets.isEmpty() || !m_activeProfileTargets.isEmpty()) {
        nextBestAction = proactiveAction(
            "guarded_write",
            "Préparer une écriture confirmée",
            "chat_memory_write",
            88,
            false,
            "write",
            "Des cibles mémoire sont déjà actives; la prochaine action utile est un test confirmé, pas une nouvelle recherche.");
    } else if (m_smartSearchActive && !candidates.isEmpty()) {
        const int confidence = candidates.size() <= kAutoWriteCandidateLimit ? 90 : 82;
        nextBestAction = proactiveAction(
            candidates.size() <= kAutoWriteCandidateLimit ? "review_top_candidates" : "reduce_with_new_value",
            candidates.size() <= kAutoWriteCandidateLimit ? "Préparer test d'écriture confirmé" : "Réduire avec nouvelle valeur",
            candidates.size() <= kAutoWriteCandidateLimit ? "prepare_guarded_write" : "next_scan",
            confidence,
            candidates.size() > kAutoWriteCandidateLimit,
            candidates.size() <= kAutoWriteCandidateLimit ? "write" : "safe",
            candidates.size() <= kAutoWriteCandidateLimit
                ? "Le nombre de candidats est assez bas; il faut passer par une confirmation avant écriture/freeze."
                : "Une recherche est déjà active; refaire varier la valeur donnera la réduction la plus rentable.");
    } else {
        const QString strategyId = preferredStrategy.value("id").toString();
        const int confidence = preferredStrategy.value("score", 50).toInt();
        if (strategyId == "trace_ui_string") {
            nextBestAction = proactiveAction("trace_ui_string", "Lancer Trace UI string", "scan_ui_strings", confidence, true, "safe", preferredStrategy.value("reason").toString());
        } else if (strategyId == "encrypted_scan") {
            nextBestAction = proactiveAction("encrypted_scan", "Lancer scan chiffré borné", "scan_encrypted_value", confidence, true, "safe", preferredStrategy.value("reason").toString());
        } else if (strategyId == "unknown_capture") {
            nextBestAction = proactiveAction("unknown_capture", "Capturer Unknown initial value", "unknown_capture", confidence, true, "safe", preferredStrategy.value("reason").toString());
        } else {
            nextBestAction = proactiveAction("exact_or_multitype", "Lancer scan exact multi-type", "exact_scan_multi_type", confidence, true, "safe", preferredStrategy.value("reason").toString());
        }
    }

    if (!lastSuccessfulAudit.isEmpty()) {
        nextBestAction["learnedFrom"] = lastSuccessfulAudit;
    }
    if (telemetryReport.displayValueSignals) {
        nextBestAction["displayValueAware"] = true;
    }

    result["strategyScores"] = strategyScores;
    result["preferredStrategy"] = preferredStrategy;
    result["nextBestAction"] = nextBestAction;

    result["eventCounts"] = eventCounts;
    result["recentSignals"] = lastSignals;
    result["telemetryInsights"] = telemetryInsights;
    result["displayValueReport"] = displayValueReport;
    QVariantMap aobReport;
    aobReport["multiMatchCount"] = telemetryReport.aobMultiMatchCount;
    aobReport["weakQualityCount"] = telemetryReport.aobWeakQualityCount;
    aobReport["trainerBlockedCount"] = telemetryReport.trainerBlockedCount;
    if (telemetryReport.aobMultiMatchCount > 0) {
        aobReport["matchesFound"] = 2;
    } else if (eventCounts.value("aob_scan").toInt() > 0 || eventCounts.value("aob_signature").toInt() > 0) {
        aobReport["matchesFound"] = 1;
    }
    aobReport["qualityReady"] = telemetryReport.aobWeakQualityCount == 0 && telemetryReport.trainerBlockedCount == 0 && telemetryReport.aobMultiMatchCount == 0;
    result["aob"] = aobReport;
    result["recommendations"] = recommendations;
    result["guardrails"] = guardrails;
    result["summary"] = QString("%1 candidat(s), %2 cible(s) active(s), %3 événement(s) récent(s).")
        .arg(candidates.size())
        .arg(m_chatMemoryTargets.size() + m_activeProfileTargets.size())
        .arg(recentEvents.size());
    return result;
}

QVariantMap ApplicationController::clearAutoResolveMemory(bool allProcesses) {
    QSettings settings;
    QVariantMap result;
    result["success"] = true;
    result["allProcesses"] = allProcesses;
    if (allProcesses) {
        settings.remove("autoResolver");
        result["message"] = "Mémoire Auto vidée pour tous les processus.";
    } else {
        const QString gameKey = autoResolverGameKey(processName());
        settings.remove(QString("autoResolver/process/%1").arg(gameKey));
        result["gameKey"] = gameKey;
        result["message"] = QString("Mémoire Auto vidée pour %1.").arg(gameKey);
    }
    appendSmartSearchDebug("auto_resolve_memory_cleared", result);
    return result;
}

QVariantMap ApplicationController::logAiAudit(const QString& event, const QVariantMap& payload) {
    const QString cleanEvent = event.trimmed().isEmpty()
        ? QString("ai_audit")
        : event.trimmed().left(80);
    QVariantMap entry = payload;
    entry["processName"] = processName();
    entry["pid"] = m_pid;
    entry["auditEvent"] = cleanEvent;
    appendScanTelemetry("ai_audit", entry);

    const bool successfulAction = payload.value("success").toBool()
        || cleanEvent == "risk_confirmed"
        || cleanEvent.endsWith("_prepared");
    if (successfulAction) {
        QSettings settings;
        const QString gameKey = autoResolverGameKey(processName());
        settings.beginGroup(QString("autoResolver/process/%1").arg(gameKey));
        settings.setValue("lastSuccessfulAuditEvent", cleanEvent);
        settings.setValue("lastSuccessfulAuditAt", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
        settings.setValue(
            QString("strategyWins/%1").arg(cleanEvent),
            settings.value(QString("strategyWins/%1").arg(cleanEvent), 0).toInt() + 1);
        const QString address = payload.value("address").toString();
        if (!address.isEmpty()) {
            settings.setValue("lastSuccessfulAddress", address);
        }
        const QString type = payload.value("type").toString();
        if (!type.isEmpty()) {
            settings.setValue("lastSuccessfulValueType", type);
        }
        const QString aobPattern = payload.value("aobPattern").toString();
        if (!aobPattern.isEmpty()) {
            settings.setValue("lastSuccessfulAobPattern", aobPattern.left(512));
        }

        // Memoire de pattern structuree (module + offset relatif, pas
        // l'adresse absolue ci-dessus qui ne survit pas a l'ASLR) — plusieurs
        // entrees distinctes par jeu au lieu d'un seul "dernier succes"
        // ecrase a chaque fois. Reutilisable au prochain lancement du meme
        // executable via getRememberedPatterns().
        uint64_t rawAddress = 0;
        if (!address.isEmpty() && m_pid > 0 && parseHexAddress(address, &rawAddress)) {
            QString module;
            uint64_t moduleOffset = 0;
            if (resolveModuleOffset(static_cast<uint32_t>(m_pid), rawAddress, &module, &moduleOffset)) {
                QJsonArray patterns = QJsonDocument::fromJson(
                    settings.value("rememberedPatterns").toByteArray()).array();

                const QString offsetHex = QString::number(moduleOffset, 16);
                int existingIndex = -1;
                for (int i = 0; i < patterns.size(); ++i) {
                    const QJsonObject entry = patterns.at(i).toObject();
                    if (entry.value("module").toString().compare(module, Qt::CaseInsensitive) == 0 &&
                        entry.value("moduleOffset").toString() == offsetHex) {
                        existingIndex = i;
                        break;
                    }
                }

                // Role semantique "infere" (roadmap H.2 point 3 / STRATEGY_ROOM.md,
                // tranche le 19/08/2026 en faveur de l'option 2 deja recommandee) :
                // reutilise "objective" — deja transmis a CHAQUE appel logAiAudit
                // via le wrapper frontend (ui/src/stores/app.ts, searchQuery.value ou
                // activeInvestigation.objective) — comme libelle lisible, plutot que
                // d'ajouter une question explicite qui casserait le flux sans friction.
                // Quelques objectifs generiques (placeholders de reset de contexte,
                // pas une vraie phrase utilisateur) sont exclus pour ne pas figer un
                // faux "role" du type "nouvelle recherche" a la place d'un vrai libelle.
                const QString objective = payload.value("objective").toString().trimmed();
                const QString objectiveLower = objective.toLower();
                const bool objectiveIsGeneric = objective.isEmpty()
                    || objectiveLower == "investigation manuelle"
                    || objectiveLower == "nouvelle recherche"
                    || objectiveLower.startsWith("nouvelle recherche ")
                    || objectiveLower.startsWith("j'utilise ces mémoires");
                const QString previousQueryLabel = existingIndex >= 0
                    ? patterns.at(existingIndex).toObject().value("queryLabel").toString()
                    : QString();

                QJsonObject entry;
                entry["module"] = module;
                entry["moduleOffset"] = offsetHex;
                entry["valueType"] = type;
                entry["aobPattern"] = aobPattern.left(512);
                entry["auditEvent"] = cleanEvent;
                entry["queryLabel"] = objectiveIsGeneric ? previousQueryLabel : objective.left(120);
                entry["confirmedAt"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
                entry["confirmCount"] = (existingIndex >= 0
                    ? patterns.at(existingIndex).toObject().value("confirmCount").toInt(0)
                    : 0) + 1;

                if (existingIndex >= 0) {
                    patterns.removeAt(existingIndex);
                }
                patterns.append(entry);
                constexpr int kRememberedPatternLimit = 20;
                while (patterns.size() > kRememberedPatternLimit) {
                    patterns.removeAt(0);
                }
                settings.setValue("rememberedPatterns", QJsonDocument(patterns).toJson(QJsonDocument::Compact));
            }
        }

        settings.endGroup();
    }

    QVariantMap result;
    result["success"] = true;
    result["event"] = cleanEvent;
    return result;
}

QVariantMap ApplicationController::getRememberedPatterns() const {
    QVariantMap result;
    result["success"] = true;

    const QString gameKey = autoResolverGameKey(processName());
    result["gameKey"] = gameKey;

    QSettings settings;
    settings.beginGroup(QString("autoResolver/process/%1").arg(gameKey));
    const QJsonArray patterns = QJsonDocument::fromJson(settings.value("rememberedPatterns").toByteArray()).array();
    settings.endGroup();

    // Modules du processus attache, pour resoudre module+offset -> adresse
    // live sans redemander un scan. Vide si rien n'est attache : la liste
    // reste utile en lecture seule (voir ce qui a deja marche sur ce jeu).
    QHash<QString, uint64_t> moduleBases;
    if (m_handle.isValid() && m_pid > 0) {
        for (const auto& mod : killcore::ProcessEnumerator::enumerateModules(static_cast<uint32_t>(m_pid))) {
            moduleBases.insert(mod.name.toLower(), mod.baseAddress);
        }
    }

    QVariantList entries;
    for (const auto& item : patterns) {
        const QJsonObject obj = item.toObject();
        QVariantMap entry;
        const QString module = obj.value("module").toString();
        const QString offsetHex = obj.value("moduleOffset").toString();
        entry["module"] = module;
        entry["moduleOffset"] = offsetHex;
        entry["valueType"] = obj.value("valueType").toString();
        entry["aobPattern"] = obj.value("aobPattern").toString();
        entry["auditEvent"] = obj.value("auditEvent").toString();
        entry["queryLabel"] = obj.value("queryLabel").toString();
        entry["confirmedAt"] = obj.value("confirmedAt").toString();
        entry["confirmCount"] = obj.value("confirmCount").toInt(1);

        const auto baseIt = moduleBases.constFind(module.toLower());
        if (baseIt != moduleBases.constEnd()) {
            bool ok = false;
            const uint64_t offset = offsetHex.toULongLong(&ok, 16);
            if (ok) {
                entry["resolved"] = true;
                entry["liveAddress"] = QString::number(baseIt.value() + offset, 16).toUpper();
            } else {
                entry["resolved"] = false;
            }
        } else {
            entry["resolved"] = false;
        }
        entries.append(entry);
    }
    // Les plus recemment confirmes en premier — les plus susceptibles d'etre
    // encore pertinents (un role peut avoir change d'offset entre deux
    // versions du jeu, la confirmation la plus fraiche est le meilleur signal).
    std::sort(entries.begin(), entries.end(), [](const QVariant& a, const QVariant& b) {
        return a.toMap().value("confirmedAt").toString() > b.toMap().value("confirmedAt").toString();
    });

    result["patterns"] = entries;
    result["patternCount"] = entries.size();
    return result;
}

// Historique d'ecritures persistant/replay inter-session (roadmap I) : distinct
// de rememberedPatterns (dedupliqué par cible, un seul "dernier succès" par
// module+offset) — ici l'ordre chronologique et les doublons sont volontairement
// conservés pour permettre de rejouer une séquence exacte d'écritures plus tard,
// utile en QA/test répétitif plutôt qu'en trainer classique.
// H5 (docs/STRATEGY_ROOM.md, session Solitaire du 19/08/2026) : reutilise
// m_writeHistory (deja alimente par writeMemoryValue ET writeMemoryValueConfirmed,
// donc par tous les chemins d'ecriture "normaux") pour savoir si une adresse a
// deja ete ecrite avec succes au moins une fois avant d'activer un freeze
// dessus — un freeze a haute frequence sur une adresse jamais testee en
// ecriture a fait crasher une cible reelle pendant cette session (voir
// STRATEGY_ROOM.md), d'ou l'avertissement non-bloquant ci-dessous.
bool ApplicationController::hasAddressBeenWriteVerified(uint64_t address) const {
    for (const auto& record : m_writeHistory) {
        if (record.address == address) {
            return true;
        }
    }
    return false;
}

// H1 (docs/STRATEGY_ROOM.md, session Solitaire du 19/08/2026) : un petit
// groupe de candidats identique sur plusieurs next scan "exact" d'affilée est
// un signal statistique de copies redondantes (observé concrètement : 2
// adresses restées identiques sur 8 cycles, qui n'acceptaient une écriture
// qu'en étant modifiées ensemble). Partagé entre nextScan (sync, utilisé par
// le connecteur d'automatisation) et nextScanAsync (worker thread, utilisé
// par l'UI) pour que les deux bénéficient du même signal. Scope
// volontairement limité au mode Exact : dans les autres modes
// (changed/increased/...), les survivants n'ont pas forcément la même valeur
// entre eux, la comparaison n'aurait pas de sens.
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
        // Mode différent d'Exact, aucun survivant, ou groupe encore trop
        // grand pour être significatif : pas de continuité possible avec un
        // éventuel groupe stable précédent, on repart de zéro.
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
        // Adresse hors d'un module chargé (allocation dynamique) : pas
        // réutilisable après un redémarrage, on ne persiste pas cette entrée.
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

QVariantMap ApplicationController::getWriteHistorySequence() const {
    QVariantMap result;
    result["success"] = true;

    const QString gameKey = autoResolverGameKey(processName());
    result["gameKey"] = gameKey;

    QSettings settings;
    settings.beginGroup(QString("writeHistory/process/%1").arg(gameKey));
    const QJsonArray sequence = QJsonDocument::fromJson(settings.value("sequence").toByteArray()).array();
    settings.endGroup();

    // Modules du processus attaché, pour résoudre module+offset -> adresse
    // live sans redemander un scan — même démarche que getRememberedPatterns.
    QHash<QString, uint64_t> moduleBases;
    if (m_handle.isValid() && m_pid > 0) {
        for (const auto& mod : killcore::ProcessEnumerator::enumerateModules(static_cast<uint32_t>(m_pid))) {
            moduleBases.insert(mod.name.toLower(), mod.baseAddress);
        }
    }

    QVariantList entries;
    for (const auto& item : sequence) {
        const QJsonObject obj = item.toObject();
        QVariantMap entry;
        const QString module = obj.value("module").toString();
        const QString offsetHex = obj.value("moduleOffset").toString();
        entry["module"] = module;
        entry["moduleOffset"] = offsetHex;
        entry["valueType"] = obj.value("valueType").toString();
        entry["value"] = obj.value("value").toString();
        entry["writtenAt"] = obj.value("writtenAt").toString();

        const auto baseIt = moduleBases.constFind(module.toLower());
        bool resolved = false;
        if (baseIt != moduleBases.constEnd()) {
            bool ok = false;
            const uint64_t offset = offsetHex.toULongLong(&ok, 16);
            if (ok) {
                resolved = true;
                entry["liveAddress"] = QString::number(baseIt.value() + offset, 16).toUpper();
            }
        }
        entry["resolved"] = resolved;
        entries.append(entry);
    }

    result["sequence"] = entries;
    result["sequenceCount"] = entries.size();
    return result;
}

QVariantMap ApplicationController::replayWriteHistorySequence() {
    QVariantMap result;
    result["success"] = false;

    if (!m_attached || !m_handle.isValid()) {
        result["error"] = "Aucun processus attaché.";
        return result;
    }

    const QVariantMap sequenceResult = getWriteHistorySequence();
    const QVariantList entries = sequenceResult.value("sequence").toList();
    if (entries.isEmpty()) {
        result["error"] = "Aucune séquence d'écritures à rejouer pour cet exécutable.";
        return result;
    }

    QVariantList details;
    int replayedCount = 0;
    int skippedCount = 0;
    int failedCount = 0;
    for (const auto& item : entries) {
        const QVariantMap entry = item.toMap();
        QVariantMap detail;
        detail["module"] = entry.value("module");
        detail["moduleOffset"] = entry.value("moduleOffset");
        detail["valueType"] = entry.value("valueType");
        detail["value"] = entry.value("value");
        if (!entry.value("resolved").toBool()) {
            detail["success"] = false;
            detail["error"] = "Module non chargé dans le processus attaché.";
            skippedCount++;
            details.append(detail);
            continue;
        }
        const QString liveAddress = entry.value("liveAddress").toString();
        // persistHistory=false : on rejoue une sequence deja persistee, ne pas
        // la re-logger a chaque replay (sinon croissance/duplication a chaque appel).
        const auto writeResult = writeMemoryValueConfirmed(
            liveAddress, entry.value("valueType").toString(), entry.value("value").toString(), false);
        detail["liveAddress"] = liveAddress;
        detail["success"] = writeResult.value("success").toBool();
        if (writeResult.value("success").toBool()) {
            replayedCount++;
        } else {
            detail["error"] = writeResult.value("error");
            failedCount++;
        }
        details.append(detail);
    }

    result["success"] = replayedCount > 0;
    result["replayedCount"] = replayedCount;
    result["skippedCount"] = skippedCount;
    result["failedCount"] = failedCount;
    result["details"] = details;
    appendScanTelemetry("write_history_replay", result);
    return result;
}

QVariantMap ApplicationController::clearWriteHistorySequence() {
    const QString gameKey = autoResolverGameKey(processName());
    QSettings settings;
    settings.beginGroup(QString("writeHistory/process/%1").arg(gameKey));
    settings.remove("sequence");
    settings.endGroup();

    QVariantMap result;
    result["success"] = true;
    result["gameKey"] = gameKey;
    return result;
}

QVariantMap ApplicationController::writeProfileTargetsFromQuery(const QString& query, const QString& value) {
    QVariantMap result;
    result["success"] = false;

    if (!m_handle.isValid()) {
        result["error"] = "Aucun processus attaché.";
        return result;
    }

    const QString normalizedQuery = normalizedProfileText(query);
    const auto profileNames = killcore::ProfileStore::listProfiles();
    QList<ActiveProfileTarget> resolvedTargets;
    QString matchedGroupName;

    for (const auto& target : m_activeProfileTargets) {
        if (!target.groupName.isEmpty() && normalizedQuery.contains(target.groupName)) {
            matchedGroupName = target.groupName;
            resolvedTargets.append(target);
        }
    }

    for (const auto& profileName : profileNames) {
        killcore::Profile profile;
        if (!killcore::ProfileStore::load(killcore::ProfileStore::profilePath(profileName), &profile)) {
            continue;
        }

        if (!profile.executableName.isEmpty()
            && !m_processName.isEmpty()
            && profile.executableName.compare(m_processName, Qt::CaseInsensitive) != 0) {
            continue;
        }

        for (const auto& target : profile.targets) {
            const QString groupName = profileTargetGroupName(target.name);
            if (groupName.isEmpty() || !normalizedQuery.contains(groupName)) {
                continue;
            }

            ActiveProfileTarget active{
                profileName,
                target.name,
                groupName,
                0,
                target.type,
                target.locator.kind,
            };

            uint64_t address = 0;
            if (target.locator.kind == killcore::LocatorKind::ClrField) {
                const auto& locator = target.locator.clrField;
                auto locatorResult = findClrObjectsByFieldValue(
                    locator.typeSubstring,
                    locator.identityField,
                    locator.identityValue,
                    1);
                const QVariantMap payload = locatorResult.value("result").toMap();
                const QVariantList matches = payload.value("matches").toList();
                if (!locatorResult.value("success").toBool() || matches.isEmpty()) {
                    continue;
                }
                const QString objectAddress = matches.first().toMap().value("address").toString();
                if (!parseHexAddress(objectAddress, &address)) {
                    continue;
                }
                active.clrTypeSubstring = locator.typeSubstring;
                active.clrIdentityField = locator.identityField;
                active.clrIdentityValue = locator.identityValue;
                active.clrFieldName = locator.targetField;
            } else if (!killcore::resolveLocatorAddress(m_handle, target.locator, &address)) {
                continue;
            }
            active.address = address;

            matchedGroupName = groupName;
            bool alreadyResolved = false;
            for (const auto& existing : resolvedTargets) {
                if (existing.profileName == profileName && existing.targetName == target.name) {
                    alreadyResolved = true;
                    break;
                }
            }
            if (!alreadyResolved) {
                resolvedTargets.append(active);
            }
        }
    }

    if (resolvedTargets.isEmpty()) {
        return result;
    }

    QVariantList suggestions;
    QVariantList writeResults;
    bool allWritesOk = true;
    const QString previousTargetValue = m_smartSearchTargetValue;
    m_smartSearchTargetValue = value;
    m_lastBatchStartIndex = m_writeHistory.size();
    m_lastAutoWriteTargets.clear();
    bool wroteRawMemoryTarget = false;

    for (const auto& target : resolvedTargets) {
        QVariantMap suggestion;
        suggestion["profile"] = target.profileName;
        suggestion["target"] = target.targetName;
        suggestion["address"] = QString::number(target.address, 16);
        suggestion["type"] = killcore::valueTypeToString(target.type);
        suggestion["value"] = value;
        suggestion["locatorKind"] = target.locatorKind == killcore::LocatorKind::ClrField ? "clr_field" : "memory";
        if (target.locatorKind == killcore::LocatorKind::ClrField) {
            suggestion["clrTypeSubstring"] = target.clrTypeSubstring;
            suggestion["clrIdentityField"] = target.clrIdentityField;
            suggestion["clrIdentityValue"] = target.clrIdentityValue;
            suggestion["clrFieldName"] = target.clrFieldName;
        }
        const QVariantList history = target.locatorKind == killcore::LocatorKind::ClrField
            ? QVariantList{}
            : candidateValueHistory(target.address);
        if (!history.isEmpty()) {
            suggestion["valueHistory"] = history;
        }
        suggestions.append(suggestion);

        QVariantMap writeResult;
        if (target.locatorKind == killcore::LocatorKind::ClrField) {
            writeResult = writeClrPrimitiveField(
                suggestion.value("address").toString(),
                target.clrFieldName,
                value);
        } else {
            writeResult = writeMemoryValueConfirmed(
                suggestion.value("address").toString(),
                suggestion.value("type").toString(),
                value);
        }
        writeResult.insert("profile", suggestion.value("profile"));
        writeResult.insert("target", suggestion.value("target"));
        writeResult.insert("address", suggestion.value("address"));
        writeResult.insert("value", value);
        writeResult.insert("type", suggestion.value("type"));
        writeResult.insert("locatorKind", suggestion.value("locatorKind"));
        if (target.locatorKind == killcore::LocatorKind::ClrField) {
            writeResult.insert("clrFieldName", target.clrFieldName);
            writeResult.insert("clrTypeSubstring", target.clrTypeSubstring);
            writeResult.insert("clrIdentityField", target.clrIdentityField);
            writeResult.insert("clrIdentityValue", target.clrIdentityValue);
        }
        if (suggestion.contains("valueHistory")) {
            writeResult.insert("valueHistory", suggestion.value("valueHistory"));
        }
        allWritesOk = allWritesOk && writeResult.value("success").toBool();
        writeResults.append(writeResult);

        if (writeResult.value("success").toBool()) {
            if (target.locatorKind != killcore::LocatorKind::ClrField) {
                m_lastAutoWriteTargets.append({target.address, target.type});
                wroteRawMemoryTarget = true;
            }
            bool updatedActiveTarget = false;
            for (auto& activeTarget : m_activeProfileTargets) {
                if (activeTarget.profileName == target.profileName && activeTarget.targetName == target.targetName) {
                    activeTarget = target;
                    updatedActiveTarget = true;
                    break;
                }
            }
            if (!updatedActiveTarget) {
                m_activeProfileTargets.append(target);
            }
        }
    }

    m_lastBatchEndIndex = m_writeHistory.size();
    if (m_lastBatchEndIndex == m_lastBatchStartIndex) {
        m_lastBatchStartIndex = -1;
        m_lastBatchEndIndex = -1;
        m_lastAutoWriteTargets.clear();
    }
    if (allWritesOk) {
        m_smartSearchActive = false;
        if (wroteRawMemoryTarget) {
            m_chatMemoryTargets = m_lastAutoWriteTargets;
        }
        resetFailureEscalationState();
        if (m_autoWriteValueHistory.isEmpty() && !previousTargetValue.isEmpty()) {
            appendDistinctText(&m_autoWriteValueHistory, previousTargetValue, 12);
        }
        appendDistinctText(&m_autoWriteValueHistory, value, 12);
    }

    QVariantMap actionResult;
    actionResult["success"] = allWritesOk;
    actionResult["remaining"] = static_cast<qulonglong>(resolvedTargets.size());
    actionResult["error"] = allWritesOk ? QString() : QString("Au moins une écriture depuis le profil a échoué.");

    result["success"] = allWritesOk;
    result["query"] = query;
    result["aiReady"] = m_ai.isReady();
    result["status"] = "tool_call";
    result["tool"] = "profile_write";
    result["actionStatus"] = "executed";
    result["workflowStatus"] = allWritesOk ? "auto_write_done" : "auto_write_partial_or_failed";
    result["targetValue"] = value;
    result["actionResult"] = actionResult;
    result["suggestedWrites"] = suggestions;
    result["suggestedWrite"] = suggestions.isEmpty() ? QVariantMap{} : suggestions.first().toMap();
    result["autoWriteResults"] = writeResults;
    result["autoWriteResult"] = writeResults.isEmpty() ? QVariantMap{} : writeResults.last().toMap();
    result["autoWriteCount"] = writeResults.size();
    result["activeTargetCount"] = m_chatMemoryTargets.size() + m_activeProfileTargets.size();
    result["previousTargetValue"] = previousTargetValue;
    result["writeHistory"] = writeHistoryToVariantList(m_autoWriteValueHistory);
    result["rollbackNote"] = wroteRawMemoryTarget
        ? "Tu peux annuler les écritures mémoire brutes via le bouton rollback batch dans l'assistant. Les champs CLR passent par ClrMD et ne sont pas ajoutés au rollback mémoire."
        : "Écriture CLR effectuée via ClrMD : aucun rollback mémoire brut n'a été ajouté.";
    result["message"] = allWritesOk
        ? QString("J'ai utilisé le profil et j'ai mis %1 sur %2 cible(s) \"%3\". Les cibles CLR restent reliées à leur locator logique.")
              .arg(value)
              .arg(resolvedTargets.size())
              .arg(matchedGroupName)
        : QString("J'ai trouvé %1 cible(s) \"%2\" dans le profil, mais au moins une écriture vers %3 a échoué.")
              .arg(resolvedTargets.size())
              .arg(matchedGroupName)
              .arg(value);
    appendSmartSearchDebug("profile_write", result);
    return result;
}

QVariantMap ApplicationController::startAutoResolve(const QString& query, const QVariantMap& options) {
    QVariantMap result;
    result["success"] = false;
    result["query"] = query;
    result["status"] = "auto_resolve";
    result["workflowStatus"] = "auto_resolve_planned";
    result["aiReady"] = m_ai.isReady();

    const QString trimmed = query.trimmed();
    if (trimmed.isEmpty()) {
        result["error"] = "Objectif vide.";
        result["message"] = "Donne-moi un objectif avec une valeur, par exemple : minéraux 41250 vers 99999.";
        return result;
    }
    if (!m_handle.isValid()) {
        result["error"] = "Aucun processus attaché.";
        result["message"] = "Attache d'abord un processus, puis relance l'auto-résolution.";
        return result;
    }

    const QStringList numbers = numbersFromText(trimmed);
    if (numbers.isEmpty()) {
        result["error"] = "Aucune valeur numérique détectée.";
        result["message"] = "J'ai besoin au minimum d'une valeur actuelle pour démarrer le plan.";
        return result;
    }

    const QString explicitType = explicitValueTypeFromText(trimmed);
    const QString valueType = options.value("valueType", explicitType.isEmpty() ? QString("Int32") : explicitType).toString();
    const bool executeSafe = options.value("executeSafe", true).toBool();

    bool targetOk = false;
    const qlonglong targetValue = numbers.first().toLongLong(&targetOk);
    if (!targetOk) {
        result["error"] = "Valeur numérique invalide.";
        return result;
    }

    killai::AutoResolveGoal goal;
    goal.description = trimmed;
    goal.targetValue = targetValue;
    goal.valueType = valueType;
    goal.gameContext = processName();

    killai::AutoResolver resolver;
    const auto plan = resolver.planForGoal(goal);
    result["plan"] = autoResolveStepsToVariantList(plan);
    result["planStepCount"] = plan.size();
    result["targetValue"] = numbers.size() > 1 ? numbers.at(1) : numbers.first();
    result["initialValue"] = numbers.first();
    result["valueType"] = valueType;

    auto rememberAutoResolverProgress = [this](const QString& workflow, qulonglong candidateCount) {
        QSettings settings;
        const QString gameKey = autoResolverGameKey(processName());
        settings.beginGroup(QString("autoResolver/process/%1").arg(gameKey));
        settings.setValue("starts", settings.value("starts", 0).toInt() + 1);
        if (workflow.contains("reduce", Qt::CaseInsensitive) || workflow.contains("refinement", Qt::CaseInsensitive)) {
            settings.setValue("reductions", settings.value("reductions", 0).toInt() + 1);
        }
        if (candidateCount == 0) {
            settings.setValue("noCandidateCount", settings.value("noCandidateCount", 0).toInt() + 1);
        }
        if (candidateCount > 0 && candidateCount <= kAutoWriteCandidateLimit) {
            settings.setValue("lowCandidateCheckpoints", settings.value("lowCandidateCheckpoints", 0).toInt() + 1);
        }
        settings.setValue("lastWorkflow", workflow);
        settings.setValue("lastCandidateCount", candidateCount);
        settings.setValue("lastUpdated", QDateTime::currentDateTime().toString(Qt::ISODateWithMs));
        settings.endGroup();
    };

    QVariantList actions;
    actions.append(QVariantMap{{"id", "run_exact"}, {"label", "Scan Auto multi-type"}, {"safe", true}});
    actions.append(QVariantMap{{"id", "try_unknown_increased"}, {"label", "Passer en Unknown"}, {"safe", true}});
    actions.append(QVariantMap{{"id", "open_expert"}, {"label", "Ouvrir Expert"}, {"safe", true}});
    QVariantList executedSafeSteps;
    const int maxSafeSteps = std::clamp(options.value("maxSafeSteps", 2).toInt(), 1, 5);
    auto appendSafeStep = [&executedSafeSteps](const QString& tool, const QString& status, const QString& detail, const QVariantMap& payload = {}) {
        QVariantMap step;
        step["tool"] = tool;
        step["status"] = status;
        step["detail"] = detail;
        step["safe"] = true;
        if (!payload.isEmpty()) {
            step["payload"] = payload;
        }
        executedSafeSteps.append(step);
    };
    result["contextReport"] = getAutoResolveReport(80);

    if (!executeSafe) {
        result["success"] = true;
        result["actionStatus"] = "planned_only";
        result["nextActions"] = actions;
        result["message"] = QString("Plan auto prêt : %1 étapes. Je n'ai rien exécuté parce que le mode exécution sûre est désactivé.")
                                .arg(plan.size());
        appendSmartSearchDebug("auto_resolve_plan", result);
        return result;
    }

    const auto& candidates = scanState().candidates();
    if (m_smartSearchActive && !candidates.isEmpty()) {
        QVariantMap reduction = nextScan("exact", numbers.first());
        appendSafeStep(
            "next_scan",
            reduction.value("success").toBool() ? "success" : "error",
            QString("Réduction exacte avec la nouvelle valeur %1.").arg(numbers.first()),
            reduction);
        result["firstAction"] = reduction;
        result["safeAction"] = "next_scan";
        result["success"] = reduction.value("success").toBool();
        result["actionStatus"] = result.value("success").toBool() ? "safe_reduction_executed" : "safe_reduction_failed";
        const qulonglong remaining = reduction.value("remaining", reduction.value("candidateStoreSize")).toULongLong();
        result["candidateCount"] = remaining;
        result["workflowStatus"] = remaining == 0
            ? "auto_resolve_no_candidate"
            : (remaining <= kAutoWriteCandidateLimit ? "awaiting_write_confirmation" : "needs_more_refinement");
        rememberAutoResolverProgress(result.value("workflowStatus").toString(), remaining);
        result["contextReport"] = getAutoResolveReport(80);

        actions.clear();
        if (remaining == 0) {
            if (executedSafeSteps.size() < maxSafeSteps) {
                QVariantMap encryptedOptions{
                    {"mode", "xor"},
                    {"keySearchBits", 16},
                    {"maxResults", 200},
                    {"writableOnly", true},
                };
                QVariantMap encrypted = scanEncryptedValue(numbers.first(), explicitType.isEmpty() ? QString("Int32") : valueType, encryptedOptions);
                appendSafeStep(
                    "scan_encrypted_value",
                    encrypted.value("success").toBool() ? "success" : "error",
                    QString("Fallback scan chiffré XOR borné après réduction vide."),
                    encrypted);
                result["fallbackAction"] = encrypted;
                result["encryptedMatches"] = encrypted.value("matches");
                const int encryptedCount = encrypted.value("matchesFound", encrypted.value("matchesReturned")).toInt();
                if (encrypted.value("success").toBool() && encryptedCount > 0) {
                    result["workflowStatus"] = "awaiting_encrypted_review";
                    result["candidateCount"] = encryptedCount;
                    actions.append(QVariantMap{{"id", "trace_ui_string"}, {"label", "Comparer avec Trace UI string"}, {"safe", true}});
                    actions.append(QVariantMap{{"id", "review_encrypted_hits"}, {"label", "Inspecter hits chiffrés"}, {"safe", true}});
                    result["message"] = QString("La réduction a vidé les candidats, donc j'ai enchaîné un scan chiffré XOR borné : %1 hit(s). On inspecte ces pistes avant tout write.")
                                            .arg(encryptedCount);
                } else {
                    int uiStringCount = 0;
                    if (executedSafeSteps.size() < maxSafeSteps) {
                        QVariantMap traceOptions{
                            {"ascii", true},
                            {"utf16", true},
                            {"numericBoundary", true},
                            {"writableOnly", true},
                            {"maxResults", 200},
                        };
                        QVariantMap trace = scanUiStrings(numbers.first(), traceOptions);
                        appendSafeStep(
                            "scan_ui_strings",
                            trace.value("success").toBool() ? "success" : "error",
                            "Fallback Trace UI string borné après scan chiffré vide.",
                            trace);
                        result["fallbackTraceUiAction"] = trace;
                        result["uiStringMatches"] = trace.value("matches");
                        uiStringCount = trace.value("matchesFound", trace.value("matchesReturned")).toInt();
                    }
                    if (uiStringCount > 0) {
                        result["workflowStatus"] = "awaiting_trace_ui_review";
                        result["candidateCount"] = uiStringCount;
                        actions.append(QVariantMap{{"id", "trace_ui_sources"}, {"label", "Analyser sources UI"}, {"safe", true}});
                        actions.append(QVariantMap{{"id", "try_unknown_changed"}, {"label", "Basculer en Unknown"}, {"safe", true}});
                        result["message"] = QString("La réduction et le scan chiffré sont vides, mais Trace UI string a trouvé %1 string(s). Prochaine étape : analyser les sources UI.")
                                                .arg(uiStringCount);
                    } else {
                        QVariantMap unknownOptions{
                            {"unknownSnapshotMaxMb", 128},
                            {"writableOnly", true},
                            {"executableOnly", false},
                            {"copyOnWriteOnly", false},
                        };
                        QVariantMap unknown = captureUnknownSnapshotWithOptions(unknownOptions);
                        appendSafeStep(
                            "unknown_capture",
                            unknown.value("success").toBool() ? "success" : "error",
                            "Capture Unknown bornée après fallbacks vides; attente d'une variation utilisateur.",
                            unknown);
                        result["unknownCaptureAction"] = unknown;
                        result["workflowStatus"] = unknown.value("success").toBool() ? "awaiting_unknown_observation" : "auto_resolve_no_candidate";
                        actions.append(QVariantMap{{"id", "continue_unknown_observation"}, {"label", "Continuer après variation"}, {"safe", true}});
                        actions.append(QVariantMap{{"id", "trace_ui_string"}, {"label", "Relancer Trace UI string"}, {"safe", true}});
                        result["message"] = unknown.value("success").toBool()
                            ? QString("La réduction, le scan chiffré et Trace UI string sont vides. J'ai capturé un snapshot Unknown borné : fais varier la valeur, puis donne-moi la nouvelle observation.")
                            : QString("Les fallbacks safe sont vides et la capture Unknown a échoué : %1").arg(unknown.value("error").toString());
                    }
                }
            } else {
                actions.append(QVariantMap{{"id", "try_unknown_changed"}, {"label", "Basculer en Unknown"}, {"safe", true}});
                actions.append(QVariantMap{{"id", "try_encrypted_scan"}, {"label", "Essayer scan chiffré"}, {"safe", true}});
                result["message"] = "J'ai réduit avec la nouvelle valeur, mais il ne reste aucun candidat. Je propose de passer en Unknown ou en scan chiffré borné.";
            }
        } else if (remaining <= kAutoWriteCandidateLimit) {
            const QString writeValue = !m_smartSearchTargetValue.isEmpty()
                ? m_smartSearchTargetValue
                : (numbers.size() > 1 ? numbers.at(1) : numbers.first());
            const QVariantList suggestions = suggestedWritesForCandidates(candidates, writeValue, kAutoWriteCandidateLimit);
            actions.append(QVariantMap{{"id", "confirm_test_write"}, {"label", "Tester l'écriture sur les candidats"}, {"safe", false}, {"requiresConfirmation", true}});
            actions.append(QVariantMap{{"id", "confirm_breakpoint_freeze"}, {"label", "Préparer freeze BP confirmé"}, {"safe", false}, {"requiresConfirmation", true}});
            result["requiresConfirmation"] = true;
            result["confirmationReason"] = "Écriture/freeze sur mémoire de processus : je prépare, tu confirmes avant action.";
            result["suggestedWrites"] = suggestions;
            result["suggestedWrite"] = suggestions.isEmpty() ? QVariantMap{} : suggestions.first().toMap();
            result["message"] = QString("J'ai réduit à %1 candidat(s). On est dans la zone intéressante : prochaine étape, test d'écriture confirmé ou freeze BP confirmé.")
                                    .arg(remaining);
        } else {
            actions.append(QVariantMap{{"id", "reduce_again"}, {"label", "Réduire encore avec une nouvelle valeur"}, {"safe", true}});
            actions.append(QVariantMap{{"id", "trace_ui_string"}, {"label", "Chercher via Trace UI string"}, {"safe", true}});
            result["message"] = QString("J'ai réduit à %1 candidat(s). Fais encore varier la valeur et donne-moi la nouvelle valeur pour continuer automatiquement.")
                                    .arg(remaining);
        }

        result["nextActions"] = actions;
        result["executedSafeSteps"] = executedSafeSteps;
        appendSmartSearchDebug("auto_resolve_reduce", result);
        return result;
    }

    QVariantMap firstScan = startExactScanMultiType(numbers.first(), "Auto");
    appendSafeStep(
        "exact_scan_multi_type",
        firstScan.value("success").toBool() ? "success" : "error",
        QString("Scan initial multi-type pour %1.").arg(numbers.first()),
        firstScan);
    result["firstAction"] = firstScan;
    result["success"] = firstScan.value("success").toBool();
    result["actionStatus"] = result.value("success").toBool() ? "safe_step_executed" : "safe_step_failed";
    const qulonglong candidateCount = firstScan.value("candidateStoreSize", firstScan.value("matchesFound")).toULongLong();
    result["candidateCount"] = candidateCount;

    if (result.value("success").toBool()) {
        m_smartSearchActive = candidateCount > 0;
        m_smartSearchInitialValue = numbers.first();
        m_smartSearchTargetValue = numbers.size() > 1 ? numbers.at(1) : QString();
        m_smartSearchValueType = "Auto";
        result["workflowStatus"] = candidateCount > 0 ? "awaiting_value_change" : "auto_resolve_no_candidate";
        if (candidateCount == 0) {
            if (executedSafeSteps.size() < maxSafeSteps) {
                QVariantMap encryptedOptions{
                    {"mode", "xor"},
                    {"keySearchBits", 16},
                    {"maxResults", 200},
                    {"writableOnly", true},
                };
                QVariantMap encrypted = scanEncryptedValue(numbers.first(), explicitType.isEmpty() ? QString("Int32") : valueType, encryptedOptions);
                appendSafeStep(
                    "scan_encrypted_value",
                    encrypted.value("success").toBool() ? "success" : "error",
                    "Fallback scan chiffré XOR borné après scan exact vide.",
                    encrypted);
                result["fallbackAction"] = encrypted;
                result["encryptedMatches"] = encrypted.value("matches");
                const int encryptedCount = encrypted.value("matchesFound", encrypted.value("matchesReturned")).toInt();
                if (encrypted.value("success").toBool() && encryptedCount > 0) {
                    result["workflowStatus"] = "awaiting_encrypted_review";
                    result["candidateCount"] = encryptedCount;
                    actions.prepend(QVariantMap{{"id", "review_encrypted_hits"}, {"label", "Inspecter hits chiffrés"}, {"safe", true}});
                    actions.prepend(QVariantMap{{"id", "trace_ui_string"}, {"label", "Comparer Trace UI string"}, {"safe", true}});
                    result["message"] = QString("Le scan exact n'a rien trouvé. J'ai enchaîné automatiquement un scan chiffré XOR borné : %1 hit(s). On valide ces pistes avant toute action risquée.")
                                            .arg(encryptedCount);
                } else {
                    int uiStringCount = 0;
                    if (executedSafeSteps.size() < maxSafeSteps) {
                        QVariantMap traceOptions{
                            {"ascii", true},
                            {"utf16", true},
                            {"numericBoundary", true},
                            {"writableOnly", true},
                            {"maxResults", 200},
                        };
                        QVariantMap trace = scanUiStrings(numbers.first(), traceOptions);
                        appendSafeStep(
                            "scan_ui_strings",
                            trace.value("success").toBool() ? "success" : "error",
                            "Fallback Trace UI string borné après scan chiffré vide.",
                            trace);
                        result["fallbackTraceUiAction"] = trace;
                        result["uiStringMatches"] = trace.value("matches");
                        uiStringCount = trace.value("matchesFound", trace.value("matchesReturned")).toInt();
                    }
                    if (uiStringCount > 0) {
                        result["workflowStatus"] = "awaiting_trace_ui_review";
                        result["candidateCount"] = uiStringCount;
                        actions.prepend(QVariantMap{{"id", "trace_ui_sources"}, {"label", "Analyser sources UI"}, {"safe", true}});
                        result["message"] = QString("Le scan exact et le scan chiffré sont vides, mais Trace UI string a trouvé %1 string(s). Prochaine étape : analyser les sources UI.")
                                            .arg(uiStringCount);
                    } else {
                        QVariantMap unknownOptions{
                            {"unknownSnapshotMaxMb", 128},
                            {"writableOnly", true},
                            {"executableOnly", false},
                            {"copyOnWriteOnly", false},
                        };
                        QVariantMap unknown = captureUnknownSnapshotWithOptions(unknownOptions);
                        appendSafeStep(
                            "unknown_capture",
                            unknown.value("success").toBool() ? "success" : "error",
                            "Capture Unknown bornée après fallbacks vides; attente d'une variation utilisateur.",
                            unknown);
                        result["unknownCaptureAction"] = unknown;
                        result["workflowStatus"] = unknown.value("success").toBool() ? "awaiting_unknown_observation" : "auto_resolve_no_candidate";
                        actions.prepend(QVariantMap{{"id", "continue_unknown_observation"}, {"label", "Continuer après variation"}, {"safe", true}});
                        result["message"] = unknown.value("success").toBool()
                            ? QString("Le scan exact, le scan chiffré et Trace UI string sont vides. J'ai capturé un snapshot Unknown borné : fais varier la valeur, puis donne-moi la nouvelle observation.")
                            : QString("Les fallbacks safe sont vides et la capture Unknown a échoué : %1").arg(unknown.value("error").toString());
                    }
                }
            } else {
                actions.prepend(QVariantMap{{"id", "try_encrypted_scan"}, {"label", "Essayer scan chiffré"}, {"safe", true}});
                result["message"] = "J'ai lancé le scan initial, mais il n'a rien trouvé. Prochaine piste : Unknown ou scan chiffré.";
            }
        } else {
            const QString writeValue = numbers.size() > 1 ? numbers.at(1) : numbers.first();
            const QVariantList suggestions = suggestedWritesForCandidates(candidates, writeValue, kAutoWriteCandidateLimit);
            if (candidateCount <= kAutoWriteCandidateLimit) {
                result["requiresConfirmation"] = true;
                result["confirmationReason"] = "Petit nombre de candidats : confirme avant tout test d'écriture ou freeze.";
                result["suggestedWrites"] = suggestions;
                result["suggestedWrite"] = suggestions.isEmpty() ? QVariantMap{} : suggestions.first().toMap();
                actions.prepend(QVariantMap{{"id", "confirm_test_write"}, {"label", "Tester l'écriture sur les candidats"}, {"safe", false}, {"requiresConfirmation", true}});
            }
            result["message"] = QString("J'ai lancé le plan auto et trouvé %1 candidat(s). Fais varier la valeur dans le jeu, puis donne-moi la nouvelle valeur pour réduire.")
                                    .arg(candidateCount);
        }
    } else {
        result["workflowStatus"] = "action_failed";
        result["error"] = firstScan.value("error").toString();
        result["message"] = QString("Le plan est prêt, mais le premier scan a échoué : %1").arg(result.value("error").toString());
    }

    result["nextActions"] = actions;
    result["executedSafeSteps"] = executedSafeSteps;
    rememberAutoResolverProgress(result.value("workflowStatus").toString(), candidateCount);
    result["contextReport"] = getAutoResolveReport(80);
    appendSmartSearchDebug("auto_resolve_start", result);
    return result;
}

// Un nouveau lot d'adresses (ecriture auto reussie, nouveau scan, contexte
// efface, ou confirmation explicite de l'utilisateur) rend obsolete
// l'echelle de secours du lot precedent : remet a zero le palier ET toute
// relance en attente, pour que le prochain message de l'utilisateur ne soit
// pas mal interprete comme la reponse a une question qui ne concerne plus ce
// lot. Factorise ici car deuplique a plus de 10 sites avant extraction —
// chacun devait se souvenir des deux lignes independamment.
void ApplicationController::resetFailureEscalationState() {
    m_failureEscalationLevel = 0;
    m_pendingRecoveryAction.clear();
}

// Echelle de secours quand l'utilisateur signale qu'un lot d'adresses ecrit
// automatiquement n'a pas fonctionne. Plutot que de reboucler indefiniment
// sur "fais varier la valeur, redonne-la moi" (le meme scan numerique qui a
// deja echoue), chaque nouveau signalement sur le meme lot fait avancer d'un
// palier vers une methode differente. Le message "valeur probablement
// protegee/calculee" n'arrive qu'en tout dernier, une fois l'arsenal epuise.
QVariantMap ApplicationController::buildFailureEscalationRecovery(const QString& query, const QStringList& numbers) {
    QVariantMap recovery;
    recovery["success"] = true;
    recovery["query"] = query;
    recovery["aiReady"] = m_ai.isReady();
    recovery["status"] = "bad_targets_reported";
    recovery["actionStatus"] = "needs_recovery_choice";
    recovery["workflowStatus"] = "auto_write_problem";
    recovery["targetValue"] = m_smartSearchTargetValue;
    recovery["activeTargetCount"] = m_lastAutoWriteTargets.size();
    recovery["candidateStoreSize"] = static_cast<qulonglong>(scanState().candidates().size());
    recovery["failureEscalationLevel"] = m_failureEscalationLevel;

    // Le message de signalement d'echec contient parfois la valeur
    // actuellement affichee (ex: "ca n'a pas marche, j'ai maintenant 180xp") :
    // c'est cette valeur-la qu'il faut tracer en priorite. A defaut, on
    // retombe sur la derniere valeur RAPPORTEE comme affichee
    // (m_smartSearchLastObservedValue) — jamais sur m_smartSearchTargetValue,
    // qui est le but jamais atteint et n'a par definition aucune chance
    // d'exister litteralement en memoire/texte a tracer.
    const QString lastValue = !numbers.isEmpty()
        ? numbers.first()
        : (!m_smartSearchLastObservedValue.isEmpty() ? m_smartSearchLastObservedValue : m_smartSearchInitialValue);

    QVariantList invalidatedAddresses;
    for (const auto& target : m_lastAutoWriteTargets) {
        invalidatedAddresses.append(QString::number(target.address, 16));
    }
    recovery["invalidatedAddresses"] = invalidatedAddresses;

    QVariantList actions;
    switch (m_failureEscalationLevel) {
    case 1:
        m_pendingRecoveryAction = "trace_ui_string";
        recovery["message"] = QString(
            "D'accord, ces adresses ne sont pas les bonnes. On change de méthode : au lieu de continuer à deviner par "
            "essais numériques, je vais tracer le texte affiché à l'écran (\"%1\") pour remonter à la vraie source — "
            "l'adresse trouvée était peut-être une simple copie d'affichage. Donne-moi la valeur actuellement affichée "
            "dans le jeu (tu peux juste me répondre par la valeur, pas besoin de cliquer le bouton).")
            .arg(lastValue);
        actions.append(QVariantMap{
            {"id", "trace_ui_string"}, {"label", "Tracer le texte affiché"},
            {"value", lastValue},
            {"reason", "Étape 1/4 : chercher la vraie source derrière la valeur affichée."}});
        break;
    case 2: {
        m_pendingRecoveryAction.clear();
        QString address;
        QString type = "Int32";
        if (!m_lastAutoWriteTargets.isEmpty()) {
            address = QString::number(m_lastAutoWriteTargets.first().address, 16);
            type = killcore::valueTypeToString(m_lastAutoWriteTargets.first().type);
        }
        recovery["message"] = "Toujours pas la bonne piste. Étape suivante : je capture directement l'instruction qui "
                               "écrit sur la dernière adresse pendant que tu fais varier la valeur dans le jeu — ça dit "
                               "si cette adresse est vraiment utilisée par le jeu ou non.";
        actions.append(QVariantMap{
            {"id", "find_what_writes_targets"}, {"label", "Capturer qui écrit dessus"},
            {"address", address}, {"type", type},
            {"reason", "Étape 2/4 : pose un point d'arrêt matériel et capture les prochaines écritures."}});
        break;
    }
    case 3:
        m_pendingRecoveryAction.clear();
        // Contrairement a trace_ui_string, ce pending n'est pas interprete par
        // le classifieur C++ : c'est un signal pour le frontend (doSearch),
        // qui route directement vers runAutoEncryptedScan si la reponse
        // suivante est en texte libre plutot qu'un clic de bouton.
        recovery["pendingRecoveryAction"] = "encrypted_scan";
        recovery["message"] = "On passe aux pistes avancées. Je commence par un scan chiffré (XOR/Add/Sub/NOT) : "
                               "donne-moi la valeur actuellement affichée dans le jeu (pas besoin de cliquer le bouton). "
                               "Si ça ne donne rien non plus, il restera la piste du pointeur stable, pour le cas où "
                               "l'adresse bouge d'une partie à l'autre.";
        actions.append(QVariantMap{
            {"id", "try_encrypted_scan"}, {"label", "Scan chiffré (XOR)"}, {"value", lastValue},
            {"reason", "Étape 3/4 : la valeur est peut-être stockée sous une forme chiffrée simple."}});
        actions.append(QVariantMap{
            {"id", "open_pointer_scan"}, {"label", "Chercher un pointeur stable"},
            {"reason", "Étape 3/4 : l'adresse change peut-être à chaque partie, un pointeur la retrouve automatiquement."}});
        break;
    default: {
        m_pendingRecoveryAction.clear();
        const bool hasRemoteConnection = m_handle.isValid() && processHasActiveRemoteConnections(m_pid);
        recovery["hasActiveRemoteConnection"] = hasRemoteConnection;
        recovery["message"] = hasRemoteConnection
            ? QString(
                  "On a maintenant essayé la recherche directe, le traçage du texte affiché, la capture des écritures "
                  "et les pistes avancées (pointeur/chiffré). Il est probable que cette valeur soit protégée, calculée "
                  "par le jeu à la volée, ou synchronisée avec un serveur — d'ailleurs %1 a actuellement une connexion "
                  "réseau active vers un serveur distant, ce qui renforce cette hypothèse (indice, pas une preuve). Si "
                  "c'est bien ça, la modifier localement ne suffira probablement pas. Tu peux repartir sur une autre "
                  "valeur, ou continuer manuellement dans l'onglet Expert.")
                  .arg(processName())
            : "On a maintenant essayé la recherche directe, le traçage du texte affiché, la capture des écritures et "
              "les pistes avancées (pointeur/chiffré). Il est probable que cette valeur soit protégée, calculée par le "
              "jeu à la volée, ou synchronisée avec un serveur — ce qui la rend difficile à modifier directement avec "
              "KillEngine. Tu peux repartir sur une autre valeur, ou continuer manuellement dans l'onglet Expert.";
        actions.append(QVariantMap{{"id", "open_expert"}, {"label", "Continuer dans Expert"}});
        break;
    }
    }

    actions.append(QVariantMap{{"id", "rollback_batch"}, {"label", "Rollback dernier lot"}});
    actions.append(QVariantMap{{"id", "clear_targets"}, {"label", "Oublier ces adresses"}});
    actions.append(QVariantMap{{"id", "new_search"}, {"label", "Nouvelle recherche"}});
    if (!scanState().candidates().isEmpty()) {
        actions.append(QVariantMap{{"id", "continue_candidates"}, {"label", "Continuer avec les autres candidats"}});
    }
    recovery["recoveryActions"] = actions;
    return recovery;
}

QVariantMap ApplicationController::startSmartSearch(const QString& query) {
    KE_LOG_INFO() << "startSmartSearch(\"" << query.toStdString() << "\")";
    if (m_smartSearchBusy) {
        // Un appel precedent est encore dans une section bloquante qui pompe
        // processEvents() (appel IA ou analyse de sources Trace UI string,
        // cf. commentaire de m_smartSearchBusy dans le header). Sans ce
        // garde-fou, ce second appel s'executerait sur la meme pile et
        // muterait m_candidates / m_lastAutoWriteTargets pendant que le
        // premier appel les lit encore. On rejette proprement plutot que de
        // risquer un etat incoherent.
        QVariantMap busy;
        busy["success"] = false;
        busy["error"] = "Une requête est déjà en cours, réessaie dans un instant.";
        busy["status"] = "ai_busy";
        return busy;
    }
    const QStringList numbers = numbersFromText(query);
    const QStringList chatAddresses = hexAddressesFromText(query);
    const QString explicitValueType = explicitValueTypeFromText(query);
    const QString defaultValueType = explicitValueType.isEmpty() ? QString("Int32") : explicitValueType;
    const bool smartSearchTrainerQuery = killai::wantsTrainerQuery(query);
    // PHASE 130 : meme piege que smartSearchTrainerQuery (PHASE 129) -- une
    // question contenant une adresse 0x... ("est-ce que 0x1234 est un champ
    // affiche...") etait interceptee trop tot par les pre-intents memoire
    // (ActivateMemoryTargets) avant d'atteindre le fast-path analyze_field_stability
    // (ai/ai_engine.cpp::matchFieldStabilityTool, meme liste de mots-cles).
    const bool smartSearchFieldStabilityQuery = killai::wantsFieldStabilityQuery(query);
    // PHASE 140 : meme piege, pour les 5 outils restants d'analyze_field_stability
    // qui prennent une adresse (get_auto_report/analyze_ui_sources n'en ont pas
    // besoin en pratique, pas concernes). Verifications volontairement plus
    // grossieres que leurs matchXxxTool respectifs (ai/ai_engine.cpp) -- servent
    // seulement a eviter que le pre-intent memoire les intercepte avant que
    // processQuery() ait une chance de les router correctement.
    const bool smartSearchAobOrPatchWorkflowQuery = killai::wantsAobOrPatchWorkflowQuery(query);
    const bool smartSearchFindWhatWritesOrTestFieldsQuery = killai::wantsFindWhatWritesOrTestFieldsQuery(query);
    // PHASE 140 : consolide en un seul flag plutot que de continuer a "&&" une
    // liste croissante sur les 8 points de bypass ci-dessous -- prochain outil
    // a router : ajouter sa condition ici, pas un neuvieme "&& !smartSearchXQuery"
    // sur chaque ligne.
    const bool smartSearchBypassesMemoryPreIntent = smartSearchTrainerQuery
        || smartSearchFieldStabilityQuery
        || smartSearchAobOrPatchWorkflowQuery
        || smartSearchFindWhatWritesOrTestFieldsQuery;
    // PHASE 148 : meme liste de mots-cles que matchUiSourcesTool
    // (ai/ai_engine.cpp), duplication grossiere volontaire -- meme convention
    // que les flags smartSearchXxxQuery ci-dessus. Sert a un guard DIFFERENT
    // (pas smartSearchBypassesMemoryPreIntent) : contourne specifiquement le
    // bloc AnswerTraceUiFilterPrompt de startSmartSearch, pas le pre-intent
    // ActivateMemoryTargets -- ces deux outils n'ont pas d'adresse a router
    // via le meme mecanisme que les 5 outils ci-dessus.
    const bool smartSearchExplicitUiSourcesQuery = killai::wantsUiSourcesQuery(query);
    auto& candidates = scanState().candidates();
    const SmartSearchIntent intent = classifySmartSearchIntent(
        query,
        numbers,
        chatAddresses,
        !m_chatMemoryTargets.isEmpty(),
        !m_lastAutoWriteTargets.isEmpty(),
        !candidates.isEmpty(),
        m_smartSearchActive,
        m_pendingRecoveryAction == "trace_ui_string",
        m_pendingRecoveryAction == "trace_ui_filter",
        m_pendingRecoveryAction == "write_target_value");
    if (intent.kind != SmartSearchIntentKind::AnswerTraceUiStringPrompt
        && intent.kind != SmartSearchIntentKind::AnswerTraceUiFilterPrompt
        && intent.kind != SmartSearchIntentKind::AnswerWriteTargetPrompt) {
        // Ce message ne repond pas a la relance en attente (l'utilisateur a
        // peut-etre clique le bouton correspondant a la place, ou envoye tout
        // autre chose) : on ne laisse pas l'etat "en attente" fausser un futur
        // message sans rapport.
        m_pendingRecoveryAction.clear();
    }
    appendSmartSearchDebug("smart_search_query", {
        {"query", query},
        {"numbers", numbers},
        {"addresses", chatAddresses},
        {"intent", smartSearchIntentKindToString(intent.kind)},
        {"intentRationale", intent.rationale},
        {"smartSearchActive", m_smartSearchActive},
        {"candidateCount", static_cast<qulonglong>(candidates.size())},
        {"initialValue", m_smartSearchInitialValue},
        {"targetValue", m_smartSearchTargetValue},
        {"valueType", m_smartSearchValueType},
        {"explicitValueType", explicitValueType},
    });

    auto stampIntent = [&](QVariantMap* payload) {
        if (!payload) return;
        (*payload)["intent"] = smartSearchIntentKindToString(intent.kind);
        (*payload)["intentRationale"] = intent.rationale;
    };

    if (killai::looksLikePureSocialQuery(query, numbers, chatAddresses)) {
        QVariantMap social;
        social["success"] = true;
        social["query"] = query;
        social["aiReady"] = m_ai.isReady();
        social["status"] = "needs_clarification";
        social["actionStatus"] = "not_executed";
        social["workflowStatus"] = "idle";
        social["message"] =
            "Salut ! Dis-moi ce que tu veux chercher ou comprendre : une valeur affichée, une adresse, "
            "un freeze, un trainer, un script Lua, ou une investigation plus guidée.";
        social["debugFile"] = smartSearchDebugFilePath();
        stampIntent(&social);
        appendSmartSearchDebug("smart_search_social_guard", social);
        return social;
    }

    const bool shouldClearSearchContext = intent.resetContext
        && (intent.kind == SmartSearchIntentKind::ResetContext
            || intent.kind == SmartSearchIntentKind::ExactScan
            || intent.kind == SmartSearchIntentKind::GuidedScan);
    if (shouldClearSearchContext) {
        const bool hadCandidates = !candidates.isEmpty();
        const int chatCount = m_chatMemoryTargets.size();
        const int profileCount = m_activeProfileTargets.size();
        m_smartSearchActive = false;
        m_smartSearchInitialValue.clear();
        m_smartSearchTargetValue.clear();
        scanState().clearCandidates();
        clearCandidateUndo();
        clearCandidateValueHistory();
        m_lastAutoWriteTargets.clear();
        m_autoWriteValueHistory.clear();
        m_chatMemoryTargets.clear();
        resetFailureEscalationState();
        m_pendingUiStringCandidates.clear();
        m_smartSearchLastObservedValue.clear();
        m_activeProfileTargets.clear();
        appendSmartSearchDebug("smart_search_reset", {
            {"query", query},
            {"reason", "new search request"},
            {"hadCandidates", hadCandidates},
            {"chatTargetsCleared", chatCount},
            {"profileTargetsCleared", profileCount},
        });
    }

    if (intent.kind == SmartSearchIntentKind::ClearActiveTargets) {
        const int chatCount = m_chatMemoryTargets.size();
        const int profileCount = m_activeProfileTargets.size();
        const int lastCount = m_lastAutoWriteTargets.size();
        m_chatMemoryTargets.clear();
        m_activeProfileTargets.clear();
        m_lastAutoWriteTargets.clear();
        m_autoWriteValueHistory.clear();
        m_lastBatchStartIndex = -1;
        m_lastBatchEndIndex = -1;
        resetFailureEscalationState();
        m_pendingUiStringCandidates.clear();
        m_smartSearchLastObservedValue.clear();

        QVariantMap cleared;
        cleared["success"] = true;
        cleared["query"] = query;
        cleared["aiReady"] = m_ai.isReady();
        cleared["status"] = "active_targets_cleared";
        cleared["actionStatus"] = "executed";
        cleared["workflowStatus"] = "idle";
        cleared["chatTargetsCleared"] = chatCount;
        cleared["profileTargetsCleared"] = profileCount;
        cleared["lastAutoWriteTargetsCleared"] = lastCount;
        cleared["message"] = QString("C'est fait, j'ai oublié les adresses et profils actifs de la conversation.");
        stampIntent(&cleared);
        appendSmartSearchDebug("smart_search_clear_active_targets", cleared);
        return cleared;
    }

    if (intent.kind == SmartSearchIntentKind::ResetContext) {
        QVariantMap reset;
        reset["success"] = true;
        reset["query"] = query;
        reset["aiReady"] = m_ai.isReady();
        reset["status"] = "context_reset";
        reset["actionStatus"] = "executed";
        reset["workflowStatus"] = "idle";
        reset["message"] = "D'accord, je repars sur une recherche propre. Donne-moi la nouvelle valeur à chercher.";
        stampIntent(&reset);
        appendSmartSearchDebug("smart_search_context_reset", reset);
        return reset;
    }

    if (intent.kind == SmartSearchIntentKind::ReportBadTargets) {
        ++m_failureEscalationLevel;
        // Les adresses invalidees ne doivent plus jamais etre la cible par
        // defaut d'un futur nombre isole (ex: l'utilisateur tape juste "240"
        // pour repondre a une toute autre relance) : sans ce clear, la regle
        // generique "des adresses sont actives + un nombre => on ecrit dessus"
        // rattrape silencieusement n'importe quel nombre ulterieur et reecrit
        // sur des adresses deja signalees comme mauvaises. m_lastAutoWriteTargets
        // (utilise pour Find What Writes et l'historique) n'est PAS efface ici.
        m_chatMemoryTargets.clear();
        if (!numbers.isEmpty()) {
            m_smartSearchLastObservedValue = numbers.first();
        }
        QVariantMap recovery = buildFailureEscalationRecovery(query, numbers);
        stampIntent(&recovery);
        appendSmartSearchDebug("smart_search_bad_targets_reported", recovery);
        return recovery;
    }

    if (intent.kind == SmartSearchIntentKind::ReportGoodTargets) {
        // Symetrique de ReportBadTargets : l'utilisateur confirme que le
        // dernier lot ecrit fonctionne vraiment. On sauvegarde chaque adresse
        // comme cible de Profil (locator module+offset si possible, pour
        // survivre a un redemarrage du jeu) et on la marque "confirmee" dans
        // l'historique anti-bruit pour qu'elle ne soit plus jamais retrogradee,
        // meme si elle revient plus tard avec une autre cible (farming normal).
        // Si m_chatMemoryTargets est vide PARCE QUE ce lot vient d'etre
        // signale mauvais (ReportBadTargets vide m_chatMemoryTargets mais
        // garde volontairement m_lastAutoWriteTargets pour Find What Writes,
        // cf. commentaire plus haut), ne PAS retomber dessus ici : une
        // confirmation qui suit immediatement un signalement d'echec sur le
        // meme lot est presque toujours sans rapport (ou contradictoire),
        // et la sauvegarde Profil + l'immunisation anti-bruit sont quasi
        // irreversibles pour se tromper.
        const bool batchJustReportedBad = m_chatMemoryTargets.isEmpty() && m_failureEscalationLevel > 0;
        resetFailureEscalationState();

        const QList<AutoWriteTarget> emptyTargets;
        const auto& confirmedTargets = !m_chatMemoryTargets.isEmpty()
            ? m_chatMemoryTargets
            : (batchJustReportedBad ? emptyTargets : m_lastAutoWriteTargets);
        QVariantMap recovery;
        recovery["success"] = true;
        recovery["query"] = query;
        recovery["aiReady"] = m_ai.isReady();
        recovery["targetValue"] = m_smartSearchTargetValue;

        if (confirmedTargets.isEmpty()) {
            recovery["workflowStatus"] = "idle";
            recovery["message"] = "Content que ça marche ! Je n'ai pas d'adresse active à sauvegarder pour le moment.";
            stampIntent(&recovery);
            appendSmartSearchDebug("smart_search_good_targets_reported", recovery);
            return recovery;
        }

        const QString gameKey = autoResolverGameKey(processName());
        const QString baseName = (!m_smartSearchInitialValue.isEmpty() && !m_smartSearchTargetValue.isEmpty())
            ? QString("Cible confirmée %1→%2").arg(m_smartSearchInitialValue, m_smartSearchTargetValue)
            : QString("Cible confirmée %1").arg(QDateTime::currentDateTime().toString("dd/MM HH:mm"));
        const QString description = QString(
            "Confirmée par l'utilisateur le %1 (recherche %2 → %3).")
            .arg(QDateTime::currentDateTime().toString("dd/MM/yyyy HH:mm"))
            .arg(m_smartSearchInitialValue.isEmpty() ? QString("?") : m_smartSearchInitialValue)
            .arg(m_smartSearchTargetValue.isEmpty() ? QString("?") : m_smartSearchTargetValue);

        QVariantList savedTargets;
        QVariantList historyEntries;
        bool anyModuleOffset = false;
        for (int i = 0; i < confirmedTargets.size(); ++i) {
            const auto& target = confirmedTargets.at(i);
            const QString addressHex = QString::number(target.address, 16);
            const QString typeStr = killcore::valueTypeToString(target.type);
            const QString targetName = confirmedTargets.size() > 1
                ? QString("%1 #%2").arg(baseName).arg(i + 1)
                : baseName;
            const QVariantMap saveResult = saveProfileTarget(gameKey, targetName, addressHex, typeStr, description);
            if (saveResult.value("success").toBool()) {
                savedTargets.append(QVariantMap{
                    {"targetName", targetName},
                    {"address", addressHex},
                    {"type", typeStr},
                    {"locatorKind", saveResult.value("locatorKind")},
                });
                anyModuleOffset = anyModuleOffset || saveResult.value("locatorKind").toString() == "module_offset";
            }
            historyEntries.append(QVariantMap{
                {"address", addressHex},
                {"type", typeStr},
                {"initialValue", m_smartSearchInitialValue},
                {"targetValue", m_smartSearchTargetValue},
                {"confirmed", true},
                {"timestamp", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
            });
        }
        appendCandidateHistory(gameKey, historyEntries);

        recovery["workflowStatus"] = "idle";
        recovery["savedProfileTargets"] = savedTargets;
        recovery["profileName"] = gameKey;
        if (savedTargets.isEmpty()) {
            recovery["message"] = "Content que ça marche ! La sauvegarde en profil a échoué, mais l'adresse reste active pour cette session.";
        } else {
            recovery["message"] = anyModuleOffset
                ? QString(
                      "Nickel ! J'ai sauvegardé %1 dans le profil « %2 » (onglet Profils) — elle survivra à un "
                      "redémarrage du jeu, tu pourras la réactiver direct la prochaine fois sans tout rescanner.")
                      .arg(savedTargets.size() == 1 ? "cette adresse" : QString("ces %1 adresses").arg(savedTargets.size()))
                      .arg(gameKey)
                : QString(
                      "Nickel ! J'ai sauvegardé %1 dans le profil « %2 » (onglet Profils). Attention : elle est en "
                      "mémoire non associée à un module, donc l'adresse ne survivra probablement pas à un "
                      "redémarrage du jeu — il faudra la reconfirmer la prochaine fois.")
                      .arg(savedTargets.size() == 1 ? "cette adresse" : QString("ces %1 adresses").arg(savedTargets.size()))
                      .arg(gameKey);
        }
        stampIntent(&recovery);
        appendSmartSearchDebug("smart_search_good_targets_reported", recovery);
        return recovery;
    }

    if (intent.kind == SmartSearchIntentKind::AnswerTraceUiStringPrompt) {
        const QString traceValue = !numbers.isEmpty() ? numbers.first() : query.trimmed();
        m_smartSearchLastObservedValue = traceValue;

        QVariantMap traceOptions;
        traceOptions["ascii"] = true;
        traceOptions["utf16"] = true;
        traceOptions["writableOnly"] = true;
        const QVariantMap scanResult = scanUiStrings(traceValue, traceOptions);

        QVariantMap recovery;
        recovery["success"] = true;
        recovery["query"] = query;
        recovery["aiReady"] = m_ai.isReady();
        recovery["targetValue"] = m_smartSearchTargetValue;
        recovery["actionStatus"] = scanResult.value("success").toBool() ? "executed" : "failed";
        if (scanResult.value("success").toBool()) {
            const auto stringsFound = scanResult.value("matchesFound").toULongLong();
            if (stringsFound > 0) {
                // On enchaine sur l'etape 2 (Filtrer + Analyser sources) au
                // prochain message : garder les candidats trouves et faire
                // suivre la relance en attente plutot que renvoyer
                // l'utilisateur cliquer manuellement dans Expert.
                m_pendingRecoveryAction = "trace_ui_filter";
                m_pendingUiStringCandidates = scanResult.value("candidates").toList();
                recovery["workflowStatus"] = "trace_ui_string_found";
                recovery["message"] = QString(
                    "Trace UI string : %1 occurrence(s) du texte \"%2\" trouvées en mémoire. Fais varier la valeur dans "
                    "le jeu, puis donne-moi la nouvelle valeur affichée — je filtre les bonnes pistes et je cherche la "
                    "source numérique derrière, automatiquement.")
                    .arg(stringsFound)
                    .arg(traceValue);
            } else {
                m_pendingRecoveryAction.clear();
                recovery["workflowStatus"] = "no_candidate";
                recovery["message"] = QString(
                    "Trace UI string : le texte \"%1\" n'a pas été trouvé en mémoire. Vérifie la valeur affichée "
                    "exacte, ou passe en Unknown.")
                    .arg(traceValue);
                QVariantList recoveryActions;
                recoveryActions.append(QVariantMap{{"id", "try_encrypted_scan"}, {"label", "Scan chiffré (XOR)"}, {"value", traceValue}});
                recoveryActions.append(QVariantMap{{"id", "try_unknown_changed"}, {"label", "Unknown (valeur inconnue)"}});
                recoveryActions.append(QVariantMap{{"id", "new_search"}, {"label", "Nouvelle recherche"}});
                recovery["recoveryActions"] = recoveryActions;
            }
            if (!scanResult.value("candidates").isNull()) {
                recovery["uiStringCandidates"] = scanResult.value("candidates");
            }
        } else {
            m_pendingRecoveryAction.clear();
            recovery["workflowStatus"] = "action_failed";
            recovery["message"] = QString("Le traçage du texte affiché a échoué : %1").arg(scanResult.value("error").toString());
        }
        stampIntent(&recovery);
        appendSmartSearchDebug("smart_search_answer_trace_ui_string_prompt", recovery);
        return recovery;
    }

    // PHASE 148 : une demande EXPLICITE d'analyse de sources ("analyse les
    // sources numeriques...") ne doit pas etre traitee comme la reponse a la
    // relance trace_ui_filter en attente, meme si m_pendingRecoveryAction ==
    // "trace_ui_filter" est actif -- sinon "analyse les sources, c'est
    // toujours 100" est avale par ce bloc (qui lance deja sa propre analyse
    // de sources, mais via le pipeline filter->survivors, pas via l'outil
    // analyze_ui_sources demande). Volontairement NE PAS toucher
    // intent.kind ni m_pendingRecoveryAction ici : laisser tomber jusqu'au
    // repli generique m_ai.processQuery() plus bas, qui route vers
    // matchUiSourcesTool (ai/ai_engine.cpp) -- m_pendingUiStringCandidates
    // reste peuple (ce bloc ne s'execute pas), donc analyze_ui_sources peut
    // toujours s'en servir. Le flow existant "reponse simple = nouvelle
    // valeur" n'est pas touche : ce guard ne matche que sur des mots-cles
    // explicites, jamais sur une simple valeur numerique.
    if (intent.kind == SmartSearchIntentKind::AnswerTraceUiFilterPrompt && !smartSearchExplicitUiSourcesQuery) {
        m_pendingRecoveryAction.clear();
        const QString filterValue = !numbers.isEmpty() ? numbers.first() : query.trimmed();
        m_smartSearchLastObservedValue = filterValue;

        QVariantMap recovery;
        recovery["success"] = true;
        recovery["query"] = query;
        recovery["aiReady"] = m_ai.isReady();
        recovery["targetValue"] = m_smartSearchTargetValue;

        const QVariantMap trackResult = trackUiStringCandidates(m_pendingUiStringCandidates, filterValue);
        const QVariantList survivors = trackResult.value("survivors").toList();
        m_pendingUiStringCandidates.clear();

        if (!trackResult.value("success").toBool() || survivors.isEmpty()) {
            recovery["workflowStatus"] = "no_candidate";
            recovery["actionStatus"] = "failed";
            recovery["message"] = QString(
                "Plus aucune string ne suit la valeur \"%1\" — on a perdu la piste du texte affiché.").arg(filterValue);
            QVariantList recoveryActions;
            recoveryActions.append(QVariantMap{{"id", "try_encrypted_scan"}, {"label", "Scan chiffré (XOR)"}, {"value", filterValue}});
            recoveryActions.append(QVariantMap{{"id", "new_search"}, {"label", "Nouvelle recherche"}});
            recovery["recoveryActions"] = recoveryActions;
            stampIntent(&recovery);
            appendSmartSearchDebug("smart_search_answer_trace_ui_filter_prompt", recovery);
            return recovery;
        }

        // Analyse des sources numeriques autour de chaque string survivante,
        // rayon croissant (mime le pipeline "Auto origine" d'Expert) : on
        // s'arrete au premier rayon qui donne des resultats. Jusqu'a 3 rayons
        // x 20 candidats de ReadProcessMemory (jusqu'a 16 Mo par lecture) :
        // meme classe de risque AppHang que l'attente IA (cf. m_smartSearchBusy
        // dans le header), donc meme remede : pomper processEvents() entre
        // chaque lecture, protege par le meme garde-fou de reentrance.
        const QList<int> radii = {1 * 1024 * 1024, 4 * 1024 * 1024, 16 * 1024 * 1024};
        const int survivorsToAnalyze = std::min<int>(static_cast<int>(survivors.size()), 20);
        QMap<QString, QVariantMap> mergedSources;
        m_smartSearchBusy = true;
        for (int radius : radii) {
            mergedSources.clear();
            for (int i = 0; i < survivorsToAnalyze; ++i) {
                QVariantMap sourceOptions;
                sourceOptions["radiusBytes"] = radius;
                sourceOptions["maxResults"] = 300;
                sourceOptions["alignment"] = 1;
                const QVariantMap sourceResult = analyzeUiStringSources(survivors.at(i).toMap(), filterValue, sourceOptions);
                QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
                const QVariantList candidates = sourceResult.value("candidates").toList();
                for (const auto& item : candidates) {
                    const QVariantMap candidate = item.toMap();
                    const QString key = candidate.value("address").toString() + "|" + candidate.value("type").toString();
                    const auto existing = mergedSources.constFind(key);
                    if (existing == mergedSources.constEnd()
                        || candidate.value("confidence").toDouble() > existing->value("confidence").toDouble()) {
                        mergedSources[key] = candidate;
                    }
                }
            }
            if (!mergedSources.isEmpty()) break;
        }
        m_smartSearchBusy = false;

        if (mergedSources.isEmpty()) {
            recovery["workflowStatus"] = "no_candidate";
            recovery["actionStatus"] = "failed";
            recovery["message"] = QString(
                "%1 string(s) suivent toujours \"%2\", mais aucune source numérique plausible autour, même en "
                "élargissant la recherche jusqu'à 16 Mo. La valeur est peut-être calculée par le jeu plutôt que "
                "stockée telle quelle.")
                .arg(survivors.size())
                .arg(filterValue);
            QVariantList recoveryActions;
            recoveryActions.append(QVariantMap{{"id", "open_expert"}, {"label", "Continuer dans Expert"}});
            recoveryActions.append(QVariantMap{{"id", "new_search"}, {"label", "Nouvelle recherche"}});
            recovery["recoveryActions"] = recoveryActions;
            stampIntent(&recovery);
            appendSmartSearchDebug("smart_search_answer_trace_ui_filter_prompt", recovery);
            return recovery;
        }

        QVariantList sourceList;
        for (const auto& source : mergedSources) {
            sourceList.append(source);
        }
        std::sort(sourceList.begin(), sourceList.end(), [](const QVariant& a, const QVariant& b) {
            return a.toMap().value("confidence").toDouble() > b.toMap().value("confidence").toDouble();
        });
        if (static_cast<size_t>(sourceList.size()) > kAutoWriteCandidateLimit) {
            sourceList = sourceList.mid(0, static_cast<int>(kAutoWriteCandidateLimit));
        }

        const QString writeValue = m_smartSearchTargetValue.isEmpty() ? filterValue : m_smartSearchTargetValue;
        m_lastBatchStartIndex = m_writeHistory.size();
        m_lastAutoWriteTargets.clear();
        QVariantList writeResults;
        bool allWritesOk = true;
        for (const auto& item : sourceList) {
            const QVariantMap candidate = item.toMap();
            const QString address = candidate.value("address").toString();
            const QString type = candidate.value("type").toString();
            auto writeResult = writeMemoryValueConfirmed(address, type, writeValue);
            writeResult.insert("address", address);
            writeResult.insert("value", writeValue);
            writeResult.insert("type", type);
            allWritesOk = allWritesOk && writeResult.value("success").toBool();
            writeResults.append(writeResult);
            if (writeResult.value("success").toBool()) {
                uint64_t address64 = 0;
                killcore::ValueType valueType;
                if (parseHexAddress(address, &address64) && killcore::parseValueType(type, &valueType)) {
                    m_lastAutoWriteTargets.append({address64, valueType});
                }
            }
        }
        m_lastBatchEndIndex = m_writeHistory.size();
        if (m_lastBatchEndIndex == m_lastBatchStartIndex) {
            m_lastBatchStartIndex = -1;
            m_lastBatchEndIndex = -1;
            m_lastAutoWriteTargets.clear();
        }
        if (allWritesOk && !m_lastAutoWriteTargets.isEmpty()) {
            m_smartSearchActive = false;
            m_chatMemoryTargets = m_lastAutoWriteTargets;
            resetFailureEscalationState();
            m_autoWriteValueHistory.clear();
            appendDistinctText(&m_autoWriteValueHistory, filterValue, 12);
            appendDistinctText(&m_autoWriteValueHistory, writeValue, 12);
        }

        recovery["workflowStatus"] = allWritesOk ? "auto_write_done" : "auto_write_partial_or_failed";
        recovery["actionStatus"] = allWritesOk ? "executed" : "failed";
        recovery["autoWriteResults"] = writeResults;
        recovery["autoWriteResult"] = writeResults.isEmpty() ? QVariantMap{} : writeResults.last().toMap();
        recovery["autoWriteCount"] = writeResults.size();
        recovery["activeTargetCount"] = m_chatMemoryTargets.size();
        recovery["previousTargetValue"] = filterValue;
        recovery["writeHistory"] = writeHistoryToVariantList(m_autoWriteValueHistory);
        recovery["rollbackNote"] = "Tu peux annuler toutes les écritures via le bouton rollback batch dans l'assistant.";
        recovery["message"] = allWritesOk
            ? QString(
                  "Traçage terminé : %1 source(s) numérique(s) trouvée(s) derrière le texte affiché, écriture de %2 "
                  "appliquée. Fais varier la valeur pour confirmer que ça tient, ou dis-moi si ça n'a pas marché.")
                  .arg(sourceList.size())
                  .arg(writeValue)
            : QString("Sources numériques trouvées, mais l'écriture a partiellement échoué sur certaines adresses.");
        stampIntent(&recovery);
        appendSmartSearchDebug("smart_search_answer_trace_ui_filter_prompt", recovery);
        return recovery;
    }

    // RiskGate chat (29/08/2026) : ecrire/figer depuis une adresse tapee dans
    // le chat executait reellement la memoire sans jamais passer par
    // confirmRiskAction (constate en direct pendant PHASE 120-D). Corrige en
    // renvoyant desormais une confirmation + recoveryActions (meme patron que
    // write_value/kernel_write plus haut) au lieu d'appeler
    // writeChatMemoryTargetsFromQuery/freezeChatMemoryTargetsFromQuery
    // directement -- ces deux fonctions n'ont pas change, seul ce point
    // d'entree est desormais gate. Un seul recoveryAction regardless du
    // nombre d'adresses : elles restent server-side dans m_chatMemoryTargets,
    // aucune serialisation necessaire.
    const auto makeChatMemoryConfirmation = [&](const QString& actionId, const QString& value,
                                                 const QString& verbInfinitive, const QString& actionRequestedLabel,
                                                 const QString& confirmationReason) {
        QVariantMap confirmResult;
        confirmResult["query"] = query;
        confirmResult["aiReady"] = m_ai.isReady();
        confirmResult["actionStatus"] = "requires_confirmation";
        confirmResult["requiresConfirmation"] = true;
        confirmResult["confirmationReason"] = confirmationReason;
        confirmResult["message"] = QString("%1 : %2 sur %3 adresse(s). Confirme pour appliquer.")
            .arg(actionRequestedLabel, value)
            .arg(m_chatMemoryTargets.size());
        QVariantList recoveryActions;
        recoveryActions.append(QVariantMap{
            {"id", actionId},
            {"label", QString("%1 %2").arg(verbInfinitive, value)},
            {"value", value},
            {"requiresConfirmation", true},
        });
        confirmResult["recoveryActions"] = recoveryActions;
        stampIntent(&confirmResult);
        return confirmResult;
    };

    if (!smartSearchBypassesMemoryPreIntent
        && (intent.kind == SmartSearchIntentKind::ActivateMemoryTargets
        || (intent.kind == SmartSearchIntentKind::WriteMemoryTargets && !chatAddresses.isEmpty())
        || (intent.kind == SmartSearchIntentKind::FreezeMemoryTargets && !chatAddresses.isEmpty()))) {
        auto activation = activateChatMemoryTargetsFromQuery(query);
        if (intent.kind == SmartSearchIntentKind::WriteMemoryTargets
            && activation.value("success").toBool()
            && numbers.size() == 1) {
            return makeChatMemoryConfirmation("chat_memory_write_confirm", numbers.first(), "écrire", "Écriture demandée",
                "Cette action modifie la mémoire de la cible attachée.");
        }
        if (intent.kind == SmartSearchIntentKind::FreezeMemoryTargets
            && activation.value("success").toBool()
            && numbers.size() == 1) {
            return makeChatMemoryConfirmation("chat_memory_freeze_confirm", numbers.first(), "figer", "Freeze demandé",
                "Fige cette/ces adresse(s) en mémoire (écriture répétée). Reste actif jusqu'à désactivation explicite.");
        }
        stampIntent(&activation);
        return activation;
    }

    if (!smartSearchBypassesMemoryPreIntent && intent.kind == SmartSearchIntentKind::WriteMemoryTargets && numbers.size() == 1) {
        return makeChatMemoryConfirmation("chat_memory_write_confirm", numbers.first(), "écrire", "Écriture demandée",
            "Cette action modifie la mémoire de la cible attachée.");
    }

    if (!smartSearchBypassesMemoryPreIntent && intent.kind == SmartSearchIntentKind::FreezeMemoryTargets && numbers.size() == 1) {
        return makeChatMemoryConfirmation("chat_memory_freeze_confirm", numbers.first(), "figer", "Freeze demandé",
            "Fige cette/ces adresse(s) en mémoire (écriture répétée). Reste actif jusqu'à désactivation explicite.");
    }

    if (!smartSearchBypassesMemoryPreIntent && intent.kind == SmartSearchIntentKind::RewriteLastTargets && numbers.size() == 1) {
        // Gate uniquement si TOUTES les cibles viennent du chat -- sinon
        // (mode Auto/UI-string-trace/profil, chatOrigin=false) comportement
        // inchange : jamais de regression sur l'UX de confiance deja
        // etablie du mode Auto (voir AutoWriteTarget::chatOrigin,
        // application_controller.h).
        const bool allChatOrigin = !m_lastAutoWriteTargets.isEmpty()
            && std::all_of(m_lastAutoWriteTargets.begin(), m_lastAutoWriteTargets.end(),
                           [](const AutoWriteTarget& t) { return t.chatOrigin; });
        if (allChatOrigin) {
            return makeChatMemoryConfirmation("rewrite_last_auto_write_confirm", numbers.first(), "remettre", "Réécriture demandée",
                "Réécrit la dernière valeur sur les adresses actives (issues du chat).");
        }
        auto rewriteTargets = rewriteLastAutoWriteTargets(numbers.first(), query);
        stampIntent(&rewriteTargets);
        return rewriteTargets;
    }

    if (!smartSearchBypassesMemoryPreIntent && intent.kind == SmartSearchIntentKind::WriteProfileTargets && numbers.size() == 1) {
        auto profileWrite = writeProfileTargetsFromQuery(query, numbers.first());
        if (profileWrite.value("tool").toString() == "profile_write") {
            stampIntent(&profileWrite);
            return profileWrite;
        }
    }

    QVariantMap result;
    if (intent.kind == SmartSearchIntentKind::ResetContext) {
        result["status"] = "reset_only";
        result["message"] = "D'accord, j'ai oublié le contexte actif. Donne-moi la nouvelle valeur à chercher.";
        result["workflowStatus"] = "idle";
        result["error"] = "";
    } else if (!smartSearchBypassesMemoryPreIntent && intent.kind == SmartSearchIntentKind::ExactScan && numbers.size() == 1) {
        // FirstScanRunning = nouveau lot de candidats, sans rapport avec un
        // eventuel echec signale sur le lot precedent. Sans ce reset, un
        // ExactScan lance sans le mot-cle "nouvelle recherche" (donc sans
        // passer par shouldClearSearchContext plus haut) heriterait du
        // palier d'escalade de secours du lot abandonne et sauterait des
        // etapes de l'echelle ("Tracer le texte affiché") des le premier
        // echec sur cette cible pourtant inedite.
        resetFailureEscalationState();
        QVariantMap args;
        args["value"] = numbers.first();
        args["valueType"] = explicitValueType.isEmpty() ? QString("SmartAuto") : defaultValueType;
        result["status"] = "tool_call";
        result["tool"] = explicitValueType.isEmpty() ? QString("exact_scan_multi_type") : QString("exact_scan");
        result["args"] = args;
        result["rationale"] = intent.rationale;
        result["state"] = "FirstScanRunning";
        result["error"] = "";
    } else if (!smartSearchBypassesMemoryPreIntent && intent.kind == SmartSearchIntentKind::RefineScan && numbers.size() == 1) {
        m_smartSearchLastObservedValue = numbers.first();
        QVariantMap args;
        args["mode"] = "exact";
        args["value"] = numbers.first();
        result["status"] = "tool_call";
        result["tool"] = "next_scan";
        result["args"] = args;
        result["rationale"] = intent.rationale;
        result["state"] = "Refining";
        result["error"] = "";
    } else if (intent.kind == SmartSearchIntentKind::AnswerWriteTargetPrompt) {
        m_pendingRecoveryAction.clear();
        m_smartSearchTargetValue = !numbers.isEmpty() ? numbers.first() : query.trimmed();
        // Rejoue le dernier "next_scan" avec la meme valeur observee (les
        // candidats n'ont pas change) pour retomber dans le tool=="next_scan"
        // standard plus bas, qui gere deja tout : anti-bruit, double
        // confirmation, ecriture. On evite ainsi de dupliquer cette logique.
        QVariantMap args;
        args["mode"] = "exact";
        args["value"] = m_smartSearchLastObservedValue;
        result["status"] = "tool_call";
        result["tool"] = "next_scan";
        result["args"] = args;
        result["rationale"] = intent.rationale;
        result["state"] = "Refining";
        result["error"] = "";
    } else if (!smartSearchBypassesMemoryPreIntent && intent.kind == SmartSearchIntentKind::GuidedScan && numbers.size() >= 2) {
        // Meme raisonnement que pour ExactScan ci-dessus : nouveau lot,
        // l'echelle de secours du lot precedent ne s'applique plus.
        resetFailureEscalationState();
        QVariantMap args;
        args["value"] = numbers.at(0);
        args["valueType"] = explicitValueType.isEmpty() ? QString("SmartAuto") : defaultValueType;
        result["status"] = "tool_call";
        result["tool"] = explicitValueType.isEmpty() ? QString("exact_scan_multi_type") : QString("exact_scan");
        result["args"] = args;
        result["rationale"] = intent.rationale;
        result["state"] = "FirstScanRunning";
        result["error"] = "";
        m_smartSearchTargetValue = numbers.at(1);
        m_smartSearchActive = true;
    } else {
        QVariantMap aiContext;
        aiContext["processAttached"] = m_handle.isValid();
        aiContext["processName"] = processName();
        aiContext["scanActive"] = m_smartSearchActive;
        aiContext["candidateCount"] = static_cast<qulonglong>(candidates.size());
        aiContext["initialValue"] = m_smartSearchInitialValue;
        aiContext["targetValue"] = m_smartSearchTargetValue;
        aiContext["activeTargetCount"] = static_cast<qulonglong>(m_chatMemoryTargets.size());
        aiContext["unknownSnapshotActive"] = !scanState().snapshot().isEmpty();
        aiContext["freezeCount"] = static_cast<qulonglong>(m_freezeHotkeyOverlayManager->freezeManager().entries().size());
        aiContext["valueType"] = m_smartSearchValueType;
        m_smartSearchBusy = true;
        result = m_ai.processQuery(query, aiContext);
        m_smartSearchBusy = false;
    }

    result["query"] = query;
    result["aiReady"] = m_ai.isReady();
    result["debugFile"] = smartSearchDebugFilePath();

    if (result.value("status").toString() != "tool_call") {
        result["actionStatus"] = "not_executed";
        if (result.value("message").toString().trimmed().isEmpty()
            && result.value("error").toString().trimmed().isEmpty()) {
            result["message"] = "D'accord. Donne-moi la valeur à chercher, ou précise que tu veux écrire sur une adresse active.";
        }
        stampIntent(&result);
        return result;
    }

    const QString tool = result.value("tool").toString();
    const QVariantMap args = result.value("args").toMap();
    QVariantMap actionResult;

    // Le modele connait l'issue des actions precedentes: apres un echec il
    // proposera une alternative (multi_type, encrypted, trace UI, unknown).
    const auto noteAiOutcome = [this, &query, &tool](bool success, const QString& detail) {
        m_ai.noteOutcome(query, tool, success ? QStringLiteral("success") : QStringLiteral("failed"));
        Q_UNUSED(detail);
    };

    if (tool == "exact_scan") {
        actionResult = startExactScan(args.value("value").toString(), args.value("valueType").toString());
    } else if (tool == "exact_scan_multi_type") {
        actionResult = startExactScanMultiType(args.value("value").toString(), args.value("valueType").toString());
    } else if (tool == "next_scan") {
        actionResult = nextScan(args.value("mode").toString(), args.value("value").toString());
    } else if (tool == "unknown_capture") {
        actionResult = captureUnknownSnapshot();
    } else if (tool == "unknown_compare") {
        actionResult = unknownNextScan(args.value("mode").toString(), args.value("valueType").toString());
    } else if (tool == "auto_resolve") {
        QVariantMap autoOptions;
        autoOptions["executeSafe"] = true;
        if (!explicitValueType.isEmpty()) autoOptions["valueType"] = defaultValueType;
        actionResult = startAutoResolve(args.value("query").toString(), autoOptions);
        if (!actionResult.value("message").toString().isEmpty()) {
            result["message"] = actionResult.value("message");
        }
        if (!actionResult.value("executedSafeSteps").isNull()) {
            result["executedSafeSteps"] = actionResult.value("executedSafeSteps");
        }
    } else if (tool == "encrypted_scan") {
        QVariantMap encryptedOptions;
        encryptedOptions["mode"] = args.value("mode", "xor");
        encryptedOptions["key"] = args.value("key", "0");
        encryptedOptions["keySearchBits"] = args.value("keySearchBits", 16);
        actionResult = scanEncryptedValue(
            args.value("value").toString(),
            args.value("valueType", explicitValueType.isEmpty() ? QString("Int32") : defaultValueType).toString(),
            encryptedOptions);
        if (actionResult.value("success").toBool()) {
            const auto encryptedMatches = actionResult.value("matchesFound").toULongLong();
            result["workflowStatus"] = encryptedMatches > 0 ? "awaiting_value_change" : "no_candidate";
            result["message"] = encryptedMatches > 0
                ? QString("Scan chiffré : %1 adresse(s) correspondent à %2 sous une forme chiffrée (XOR/Add/Sub). Fais varier la valeur puis redonne-la moi pour affiner.")
                      .arg(encryptedMatches)
                      .arg(args.value("value").toString())
                : QString("Scan chiffré : aucune adresse ne correspond à %1 sous forme chiffrée. On peut tenter la Trace UI string ou Unknown.")
                      .arg(args.value("value").toString());
            if (encryptedMatches == 0) {
                QVariantList recoveryActions;
                recoveryActions.append(QVariantMap{{"id", "trace_ui_string"}, {"label", "Chercher le texte affiché"}, {"value", args.value("value")}});
                recoveryActions.append(QVariantMap{{"id", "try_unknown_changed"}, {"label", "Unknown (valeur inconnue)"}});
                recoveryActions.append(QVariantMap{{"id", "new_search"}, {"label", "Nouvelle recherche"}});
                result["recoveryActions"] = recoveryActions;
            }
        }
    } else if (tool == "prepare_write_checkpoint") {
        // Checkpoint safe: prepare les suggestions d'ecriture sans ecrire.
        const QString checkpointValue = args.value("value", m_smartSearchTargetValue).toString();
        auto suggestions = suggestedWritesForCandidates(candidates, checkpointValue, kAutoWriteCandidateLimit);
        enrichSuggestedWritesWithHistory(&suggestions);
        actionResult["success"] = !suggestions.isEmpty();
        actionResult["suggestedWrites"] = suggestions;
        result["suggestedWrites"] = suggestions;
        result["suggestedWrite"] = suggestions.isEmpty() ? QVariantMap{} : suggestions.first().toMap();
        result["workflowStatus"] = suggestions.isEmpty() ? "no_candidate" : "awaiting_write_confirmation";
        result["message"] = suggestions.isEmpty()
            ? QString("Aucun candidat fiable a preparer. Continue a reduire la liste avec de nouvelles valeurs observees.")
            : QString("Checkpoint pret: %1 adresse(s) candidate(s) pour ecrire %2. Confirme l'ecriture pour appliquer.")
                  .arg(suggestions.size())
                  .arg(checkpointValue);
    } else if (tool == "read_window_text") {
        QVariantMap windowOptions = args;
        if (!windowOptions.contains("includeAllVisible")) windowOptions["includeAllVisible"] = true;
        actionResult = readAttachedWindowText(windowOptions);
        if (actionResult.value("success").toBool()) {
            result["workflowStatus"] = "window_text_observed";
            result["message"] = QString("Inspection fenêtre : %1 fenêtre(s) lue(s). On peut s'en servir pour synchroniser la prochaine variation avant de comparer la mémoire.")
                                  .arg(actionResult.value("windowCount").toInt());
        }
    } else if (tool == "start_changed_pages_diff") {
        QVariantMap diffOptions = args;
        if (!diffOptions.contains("maxBytesMb")) diffOptions["maxBytesMb"] = 64;
        if (!diffOptions.contains("blockSize")) diffOptions["blockSize"] = 64 * 1024;
        if (!diffOptions.contains("privateOnly")) diffOptions["privateOnly"] = true;
        if (!diffOptions.contains("writableOnly")) diffOptions["writableOnly"] = true;
        actionResult = startChangedPagesDiff(diffOptions);
        if (actionResult.value("success").toBool()) {
            result["workflowStatus"] = "awaiting_observed_variation";
            result["message"] = QString("Mode Inspecteur : snapshot lecture seule capturé (%1 blocs, %2 Mo max). Fais varier la valeur affichée, puis donne-moi l'ancienne et la nouvelle valeur pour comparer.")
                                  .arg(actionResult.value("blocksCaptured").toInt())
                                  .arg(diffOptions.value("maxBytesMb").toInt());
        }
    } else if (tool == "finish_changed_pages_diff") {
        QVariantMap diffOptions = args;
        diffOptions.remove("previousValue");
        diffOptions.remove("currentValue");
        actionResult = finishChangedPagesDiff(
            args.value("previousValue").toString(),
            args.value("currentValue").toString(),
            diffOptions);
        if (actionResult.value("success").toBool()) {
            const int hitCount = actionResult.value("hitCount", actionResult.value("hits").toList().size()).toInt();
            result["workflowStatus"] = hitCount > 0 ? "diff_hits_found" : "no_candidate";
            result["message"] = hitCount > 0
                ? QString("Mode Inspecteur : %1 piste(s) trouvée(s) dans les pages réellement modifiées. À valider en watch ou par nouvelle variation avant toute écriture.")
                      .arg(hitCount)
                : QString("Mode Inspecteur : aucune piste numérique directe dans les pages modifiées. On garde l'hypothèse copie UI/buffer et on évite l'écriture directe.");
        }
    } else if (tool == "trace_ui_string") {
        QVariantMap traceOptions;
        traceOptions["ascii"] = true;
        traceOptions["utf16"] = true;
        traceOptions["writableOnly"] = true;
        actionResult = scanUiStrings(args.value("value").toString(), traceOptions);
        if (actionResult.value("success").toBool()) {
            const auto stringsFound = actionResult.value("matchesFound").toULongLong();
            if (stringsFound > 0) {
                // Meme suite conversationnelle que AnswerTraceUiStringPrompt :
                // ce chemin est emprunte quand l'IA choisit directement l'outil
                // trace_ui_string (demande en langage libre, ex: "j'ai un texte
                // a 240 ou je veux trouver la cible") plutot que via l'echelle
                // d'escalade ReportBadTargets. Sans ce meme pending, la reponse
                // suivante de l'utilisateur (la nouvelle valeur affichee) ne
                // continue pas le traçage : elle retombe sur ExactScan et
                // abandonne silencieusement toute la piste deja trouvee.
                m_pendingRecoveryAction = "trace_ui_filter";
                m_pendingUiStringCandidates = actionResult.value("candidates").toList();
                result["workflowStatus"] = "trace_ui_string_found";
            } else {
                m_pendingRecoveryAction.clear();
                result["workflowStatus"] = "no_candidate";
            }
            result["message"] = stringsFound > 0
                ? QString("Trace UI string : %1 occurrence(s) du texte \"%2\" trouvées en mémoire. Fais varier la "
                          "valeur dans le jeu, puis donne-moi la nouvelle valeur affichée — je filtre les bonnes "
                          "pistes et je cherche la source numérique derrière, automatiquement.")
                      .arg(stringsFound)
                      .arg(args.value("value").toString())
                : QString("Trace UI string : le texte \"%1\" n'a pas été trouvé en mémoire. Vérifie la valeur affichée exacte, ou passe en Unknown.")
                      .arg(args.value("value").toString());
            if (!actionResult.value("candidates").isNull()) {
                result["uiStringCandidates"] = actionResult.value("candidates");
            }
            if (stringsFound == 0) {
                QVariantList recoveryActions;
                recoveryActions.append(QVariantMap{{"id", "try_encrypted_scan"}, {"label", "Scan chiffré (XOR)"}, {"value", args.value("value")}});
                recoveryActions.append(QVariantMap{{"id", "try_unknown_changed"}, {"label", "Unknown (valeur inconnue)"}});
                recoveryActions.append(QVariantMap{{"id", "new_search"}, {"label", "Nouvelle recherche"}});
                result["recoveryActions"] = recoveryActions;
            }
        }
    } else if (tool == "trainer_list_features") {
        // PHASE 169 : callVueStoreAction() ici serait un appel a
        // runJavaScript() REENTRANT depuis l'interieur meme du Q_INVOKABLE
        // (startSmartSearch) que le JS de cette page est en train d'attendre
        // -- confirme en direct (PHASE 168/169, docs/PHASE_TRACKER.md) : le
        // callback JS n'arrive jamais dans les 5s, l'outil echoue a 100% par
        // timeout. getTrainerFeaturesSnapshot() est une lecture pure, deja
        // disponible directement dans le meme contexte JS que l'appelant --
        // on delegue donc la lecture + le message a ui/src/stores/app.ts
        // (sendMessage, juste apres le retour de startSmartSearch()) au lieu
        // de faire un aller-retour C++ inutile.
        result["needsLocalStoreAction"] = "trainer_list_features";
        result["actionStatus"] = "pending_local_action";
    } else if (tool == "trainer_create_write") {
        const QString trainerAddress = args.value("address").toString().trimmed();
        const QString trainerValue = args.value("value").toString().trimmed();
        const QString trainerValueType = args.value("valueType", "Int32").toString().trimmed();
        if (trainerAddress.isEmpty() || trainerValue.isEmpty()) {
            result["actionStatus"] = "needs_clarification";
            result["message"] = "Pour créer une feature Trainer depuis le chat, donne une adresse 0x... et la valeur à écrire.";
            stampIntent(&result);
            return result;
        }

        QVariantMap feature;
        feature["name"] = args.value("name", "Assistant Trainer write").toString();
        feature["action"] = "write";
        feature["address"] = trainerAddress;
        feature["valueType"] = trainerValueType.isEmpty() ? QString("Int32") : trainerValueType;
        feature["value"] = trainerValue;

        // PHASE 163 : chercher un locator resilient plutot que de figer
        // aveuglement en 'absolute' -- une adresse absolue promue en Trainer
        // sans locator resilient ne survit generalement pas a un relaunch/
        // changement de scene du process cible (retour d'experience Vampire
        // Survivors, voir docs/PHASE_TRACKER.md PHASE 160/162/163 et memoire
        // feedback_freeze_proposal_uses_trainer). On essaie dans l'ordre :
        // 1) AOB (adresse dans l'image statique du module -- code ou donnee
        //    statique, PAS un objet alloue dynamiquement) ;
        // 2) pointer chain (adresse dans un objet alloue dynamiquement,
        //    atteignable depuis une base statique -- cas heap le plus courant,
        //    reutilise scanPointerChains deja existant) ;
        // 3) sinon 'absolute', mais avec un avertissement honnete plutot que
        //    de laisser croire a une persistance qui n'existe pas.
        QString locatorKind = "absolute";
        QString locatorSummary;
        {
            const auto signature = generateAobSignature(trainerAddress, QVariantMap{{"beforeBytes", 0}, {"length", 20}});
            if (signature.value("success").toBool() && !signature.value("codeReadProtected").toBool()) {
                const QString pattern = signature.value("pattern").toString();
                const auto quality = signature.value("signatureQuality").toMap();
                const int fixedBytes = quality.value("fixedBytes").toInt();
                const int score = quality.value("score").toInt();
                if (!pattern.isEmpty() && fixedBytes >= 3 && score >= 35) {
                    // executableOnly=false : une donnee (.data/.bss) n'est jamais
                    // executable -- meme correctif que PHASE 160 (voir plus haut
                    // dans ce fichier, resolveTrainerFeatureAddress cote frontend).
                    const auto scan = scanAobPattern(pattern, QVariantMap{{"executableOnly", false}, {"imageOnly", true}, {"maxResults", 2}});
                    if (scan.value("success").toBool() && scan.value("matchesFound").toInt() == 1) {
                        locatorKind = "aob";
                        feature["aobPattern"] = pattern;
                        locatorSummary = "verrouillée sur une signature AOB stable (résiste à un relaunch tant que le code/la donnée statique ne change pas de version)";
                    }
                }
            }
        }
        if (locatorKind == "absolute") {
            const auto pointerScan = scanPointerChains(trainerAddress, QVariantMap{{"maxDepth", 3}, {"maxResults", 5}, {"onlyModuleBase", true}});
            const QVariantList chains = pointerScan.value("chains").toList();
            if (pointerScan.value("success").toBool() && !chains.isEmpty()) {
                locatorKind = "pointer_chain";
                feature["pointerChain"] = chains.first();
                locatorSummary = "ancrée via une chaîne de pointeurs (résiste à une réallocation de l'objet en mémoire, ex. nouvelle partie)";
            }
        }
        feature["locatorKind"] = locatorKind;

        // PHASE 169 : la resolution de locator ci-dessus (generateAobSignature/
        // scanAobPattern/scanPointerChains) reste un appel C++ direct, aucun
        // probleme de reentrance -- seule la creation effective de la feature
        // (createTrainerFeature) passait par callVueStoreAction() et heurtait
        // le meme timeout systematique que trainer_list_features ci-dessus.
        // Meme delegation : le JS cree la feature localement et construit le
        // message de succes avec les memes donnees (locatorKind/locatorSummary).
        result["needsLocalStoreAction"] = "trainer_create_write";
        result["actionStatus"] = "pending_local_action";
        result["pendingFeature"] = feature;
        result["locatorSummary"] = locatorSummary;
        result["pendingAddress"] = trainerAddress;
        result["pendingValueType"] = feature.value("valueType");
        result["pendingValue"] = trainerValue;
    } else if (tool == "trainer_delete_feature") {
        const int trainerId = args.value("id").toInt();
        if (trainerId <= 0) {
            result["actionStatus"] = "needs_clarification";
            result["message"] = "Quelle feature Trainer veux-tu supprimer ? Donne son id, ou demande d'abord la liste du Trainer.";
            stampIntent(&result);
            return result;
        }
        // PHASE 169 : meme raison que trainer_list_features ci-dessus.
        result["needsLocalStoreAction"] = "trainer_delete_feature";
        result["actionStatus"] = "pending_local_action";
        result["pendingTrainerId"] = trainerId;
    } else if (tool == "trainer_apply_request" || tool == "trainer_restore_request") {
        // PHASE 120-D (29/08/2026) : la raison d'origine (PHASE 121/129,
        // "eviter timeout ou attente modale invisible") est perimee -- elle
        // date d'avant le patron recoveryActions de PHASE 148, qui resout deja
        // ce risque par construction : le modal confirmRiskAction() ne s'ouvre
        // JAMAIS "en autonome", seulement sur un clic explicite de l'utilisateur
        // sur le bouton recoveryAction. Meme patron que kernel_write/write_value
        // ci-dessus, reutilise les fonctions store deja confirmees par RiskGate
        // (applyTrainerFeature/restoreTrainerFeature, ordre de dependances
        // deja gere en interne).
        const bool restore = tool == "trainer_restore_request";
        const bool all = args.value("all").toBool();
        const QString trainerId = args.value("id").toString().trimmed();
        if (!all && (trainerId.isEmpty() || trainerId.toInt() <= 0)) {
            result["actionStatus"] = "needs_clarification";
            result["message"] = "Demande d'abord la liste du Trainer si tu ne connais pas l'id de la feature à "
                + QString(restore ? "restaurer" : "activer") + ".";
            stampIntent(&result);
            return result;
        }
        result["actionStatus"] = "requires_confirmation";
        result["requiresConfirmation"] = true;
        result["confirmationReason"] = restore
            ? "Restaure la feature Trainer à sa valeur d'origine (désactivation)."
            : "Active la feature Trainer : écrit/fige sa valeur configurée en mémoire.";
        result["message"] = all
            ? (restore ? "Restauration de tout le Trainer demandée. Confirme pour restaurer toutes les features actives."
                       : "Activation de tout le Trainer demandée. Confirme pour appliquer toutes les features.")
            : (restore ? QString("Restauration de la feature Trainer #%1 demandée. Confirme pour restaurer.").arg(trainerId)
                       : QString("Activation de la feature Trainer #%1 demandée. Confirme pour appliquer.").arg(trainerId));
        QVariantList recoveryActions;
        recoveryActions.append(QVariantMap{
            {"id", restore ? "trainer_restore_confirm" : "trainer_apply_confirm"},
            {"label", all
                ? QString(restore ? "Tout restaurer" : "Tout activer")
                : QString(restore ? "Restaurer #%1" : "Activer #%1").arg(trainerId)},
            {"id_target", trainerId},
            {"all", all},
            {"requiresConfirmation", true},
        });
        result["recoveryActions"] = recoveryActions;
        stampIntent(&result);
        return result;
    } else if (tool == "analyze_field_stability") {
        // PHASE 130 : jamais d'ecriture, s'execute directement (pas de
        // requiresConfirmation) comme discover_save_files/inspect_local_settings --
        // meme si ca attache brievement un debugger (comme find_what_writes,
        // deja utilise sans RiskGate modal dans l'UI derriere une simple case
        // a cocher "Debugger autorise").
        const QString stabilityAddress = args.value("address").toString().trimmed();
        if (stabilityAddress.isEmpty()) {
            result["actionStatus"] = "needs_clarification";
            result["message"] = "Il me faut une adresse (0x...) pour analyser si c'est un champ affiché ou une source.";
            stampIntent(&result);
            return result;
        }
        actionResult = analyzeFieldStability(stabilityAddress, {});
        if (actionResult.value("success").toBool()) {
            // Le rationale (core/scanner/display_source_classifier.cpp) est
            // deja une phrase complete et actionnable -- pas besoin d'ajouter
            // de conclusion redondante ici.
            result["workflowStatus"] = "field_stability_analyzed";
            result["message"] = QString("Analyse de %1 : %2")
                .arg(stabilityAddress, actionResult.value("rationale").toString());
        } else {
            result["message"] = actionResult.value("error").toString();
        }
        stampIntent(&result);
        return result;
    } else if (tool == "get_auto_report") {
        // PHASE 140 : lecture seule, cout quasi nul (agrege des donnees deja
        // collectees) -- execute directement, meme categorie que
        // analyze_field_stability ci-dessus.
        const int maxEvents = std::clamp(args.value("maxEvents", 50).toInt(), 5, 200);
        actionResult = getAutoResolveReport(maxEvents);
        if (actionResult.value("success").toBool()) {
            result["workflowStatus"] = "auto_report_ready";
            result["message"] = actionResult.value("summary").toString().isEmpty()
                ? "Rapport d'auto-résolution généré."
                : actionResult.value("summary").toString();
        } else {
            result["message"] = actionResult.value("error").toString();
        }
        stampIntent(&result);
        return result;
    } else if (tool == "analyze_ui_sources") {
        // PHASE 140 : lecture seule (killcore::MemoryReader uniquement), suite
        // naturelle de trace_ui_string. Reutilise m_pendingUiStringCandidates
        // (meme etat que le pending trace_ui_filter) si aucune adresse
        // explicite n'est fournie -- une phrase NL fournit rarement une
        // adresse ET une byteLength en une fois.
        const QString uiSourceValue = args.value("value").toString().trimmed();
        if (uiSourceValue.isEmpty()) {
            result["actionStatus"] = "needs_clarification";
            result["message"] = "Il me faut la valeur actuellement affichée pour analyser les sources numériques.";
            stampIntent(&result);
            return result;
        }

        QVariantMap uiStringCandidate;
        const QString explicitUiSourceAddress = args.value("address").toString().trimmed();
        if (!explicitUiSourceAddress.isEmpty()) {
            uiStringCandidate["address"] = explicitUiSourceAddress;
            uiStringCandidate["byteLength"] = args.value("byteLength", 0);
        } else if (!m_pendingUiStringCandidates.isEmpty()) {
            uiStringCandidate = m_pendingUiStringCandidates.first().toMap();
        } else {
            result["actionStatus"] = "needs_clarification";
            result["message"] = "Je n'ai pas de string UI récente à analyser — lance d'abord trace_ui_string, ou donne-moi directement une adresse.";
            stampIntent(&result);
            return result;
        }

        actionResult = analyzeUiStringSources(uiStringCandidate, uiSourceValue, {});
        if (actionResult.value("success").toBool()) {
            const int matches = actionResult.value("matchesReturned").toInt();
            result["workflowStatus"] = "ui_sources_analyzed";
            result["message"] = matches > 0
                ? QString("Analyse des sources : %1 candidat(s) numérique(s) trouvé(s) près de la string.").arg(matches)
                : "Analyse des sources : aucun candidat numérique trouvé près de cette string.";
            if (!actionResult.value("candidates").isNull()) {
                result["uiSourceCandidates"] = actionResult.value("candidates");
            }
        } else {
            result["message"] = actionResult.value("error").toString();
        }
        stampIntent(&result);
        return result;
    } else if (tool == "generate_aob") {
        // PHASE 140 : lecture seule (voir tool_registry.cpp) -- execute
        // directement. Ne patche jamais rien, se contente de generer/qualifier
        // une signature.
        const QString aobAddress = args.value("address").toString().trimmed();
        if (aobAddress.isEmpty()) {
            result["actionStatus"] = "needs_clarification";
            result["message"] = "Il me faut une adresse (0x...) pour générer une signature AOB.";
            stampIntent(&result);
            return result;
        }
        actionResult = generateAobSignature(aobAddress, {});
        if (actionResult.value("success").toBool()) {
            result["workflowStatus"] = "aob_signature_generated";
            result["message"] = QString("Signature AOB générée pour %1 (qualité : %2) : %3")
                .arg(aobAddress, actionResult.value("signatureRisk").toString(), actionResult.value("pattern").toString());
        } else {
            result["message"] = actionResult.value("error").toString();
        }
        stampIntent(&result);
        return result;
    } else if (tool == "suggest_patch") {
        // PHASE 140 : lecture seule -- suggere des patchs sans jamais les
        // appliquer (applyCodePatch reste un geste UI/pipe distinct, pas
        // expose comme tool Assistant).
        const QString suggestAddress = args.value("address").toString().trimmed();
        if (suggestAddress.isEmpty()) {
            result["actionStatus"] = "needs_clarification";
            result["message"] = "Il me faut une adresse (0x...) pour suggérer des patchs de code (aucune application automatique).";
            stampIntent(&result);
            return result;
        }
        actionResult = suggestCodePatches(suggestAddress, {});
        if (actionResult.value("success").toBool()) {
            const int suggestionCount = actionResult.value("suggestions").toList().size();
            result["workflowStatus"] = "code_patches_suggested";
            result["message"] = suggestionCount > 0
                ? QString("%1 suggestion(s) de patch pour %2 — aucune n'est appliquée, vérifie dans l'onglet AOB/Patch avant d'agir.")
                      .arg(suggestionCount)
                      .arg(suggestAddress)
                : QString("Aucune suggestion de patch trouvée pour %1.").arg(suggestAddress);
        } else {
            result["message"] = actionResult.value("error").toString();
        }
        stampIntent(&result);
        return result;
    } else if (tool == "disassemble_backward") {
        // PHASE 140 : lecture seule (Q_INVOKABLE ... const cote header).
        const QString backwardAddress = args.value("address").toString().trimmed();
        if (backwardAddress.isEmpty()) {
            result["actionStatus"] = "needs_clarification";
            result["message"] = "Il me faut une adresse (0x...) pour désassembler en arrière (lecture seule).";
            stampIntent(&result);
            return result;
        }
        actionResult = disassembleBackward(backwardAddress, {});
        if (actionResult.value("success").toBool()) {
            const int candidateCount = actionResult.value("candidateFields").toList().size();
            result["workflowStatus"] = "disassembled_backward";
            result["message"] = QString("Désassemblage en arrière de %1 : %2 champ(s) candidat(s) trouvé(s).")
                .arg(backwardAddress)
                .arg(candidateCount);
        } else {
            result["message"] = actionResult.value("error").toString();
        }
        stampIntent(&result);
        return result;
    } else if (tool == "find_what_writes") {
        // PHASE 140 : jamais d'execution directe depuis le chat. La variante
        // synchrone findWhatWrites() bloque le thread appelant jusqu'a
        // timeoutMs en attendant une ecriture reelle, et suppose que
        // l'utilisateur fait varier la valeur EN DIRECT dans le jeu pendant la
        // fenetre (voir AGENTS.md "Find What Writes"). Un declenchement
        // autonome depuis le chat attacherait un debugger a l'aveugle sans
        // que personne ne varie la valeur -- capture vide au mieux, risque de
        // crash sur une adresse "chaude" au pire (deja reproduit, PHASE 127).
        // Redirige vers l'UI plutot que d'executer, meme patron que
        // trainer_apply_request/trainer_restore_request.
        const QString fwwAddress = args.value("address").toString().trimmed();
        result["actionStatus"] = "requires_confirmation";
        result["requiresConfirmation"] = true;
        result["confirmationReason"] = "Attache un debugger Win32 et nécessite de faire varier la valeur en direct dans le jeu pendant la capture — pas adapté à une exécution autonome depuis le chat.";
        result["message"] = fwwAddress.isEmpty()
            ? "Pour capturer ce qui écrit une adresse, ouvre l'onglet Expert (section Trace UI string), coche « Debugger autorisé », clique « Écrit par » sur la source concernée, puis fais varier la valeur dans le jeu pendant la fenêtre de capture."
            : QString("Pour capturer ce qui écrit %1, ouvre l'onglet Expert (section Trace UI string), coche « Debugger autorisé », clique « Écrit par », puis fais varier la valeur dans le jeu pendant la fenêtre de capture.").arg(fwwAddress);
        stampIntent(&result);
        return result;
    } else if (tool == "test_candidate_fields") {
        // PHASE 140 : jamais d'execution directe depuis le chat. Ecrit une
        // vraie valeur test sur jusqu'a 5 adresses candidates puis tente de
        // restaurer (restauration non garantie en cas d'echec, voir
        // testCandidateFieldsAsync ci-dessous), et tourne ~60s en tache de
        // fond avec resultat livre par signal Qt (candidateFieldTestFinished)
        // -- ne correspond pas au patron requete/reponse en un seul tour de
        // startSmartSearch. Redirige vers l'UI, meme patron que find_what_writes.
        result["actionStatus"] = "requires_confirmation";
        result["requiresConfirmation"] = true;
        result["confirmationReason"] = "Écrit une vraie valeur test sur la cible (restauration non garantie) et tourne environ une minute en tâche de fond — pas adapté à une exécution autonome depuis le chat.";
        result["message"] = "Pour tester automatiquement quels champs candidats tiennent réellement, ouvre l'onglet Expert, section champ affiché/source, après un désassemblage en arrière — le bouton « Tester automatiquement » lance ce test avec un suivi visuel de la progression.";
        stampIntent(&result);
        return result;
    } else if (tool == "write_value" || tool == "freeze_value") {
        // PHASE 120-D (29/08/2026) : meme patron que kernel_write/speedhack_set/
        // block_process_network juste en dessous -- aucune raison technique ne
        // justifiait que ces deux tools, les plus basiques (simple ecriture/
        // freeze), restent un cul-de-sac texte sans recoveryActions alors que
        // des actions plus sensibles (ecriture kernel, injection speedhack)
        // avaient deja ce patron depuis PHASE 148. Ecart d'implementation
        // corrige, pas une nouvelle capacite : le clic reste obligatoire, le
        // vrai modal confirmRiskAction() cote frontend est inchange.
        const bool isFreeze = (tool == "freeze_value");
        const QString writeAddress = args.value("address").toString().trimmed();
        const QString writeValue = args.value("value").toString().trimmed();
        const QString writeValueType = args.value("valueType", "Int32").toString();
        if (writeAddress.isEmpty() || writeValue.isEmpty()) {
            result["actionStatus"] = "needs_clarification";
            result["message"] = isFreeze
                ? "Il me faut l'adresse (0x...) et la valeur à figer pour préparer le freeze."
                : "Il me faut l'adresse (0x...) et la valeur pour préparer l'écriture.";
            stampIntent(&result);
            return result;
        }
        result["actionStatus"] = "requires_confirmation";
        result["requiresConfirmation"] = true;
        result["confirmationReason"] = isFreeze
            ? "Fige cette adresse en mémoire (écriture répétée). Reste actif jusqu'à désactivation explicite."
            : "Cette action modifie la mémoire de la cible attachée.";
        result["message"] = isFreeze
            ? QString("Freeze demandé : %1 (%2) à 0x%3. Confirme pour figer la valeur.").arg(writeValue, writeValueType, writeAddress)
            : QString("Écriture demandée : %1 (%2) à 0x%3. Confirme pour appliquer.").arg(writeValue, writeValueType, writeAddress);
        QVariantList recoveryActions;
        recoveryActions.append(QVariantMap{
            {"id", isFreeze ? "freeze_value_confirm" : "write_value_confirm"},
            {"label", isFreeze ? QString("Figer %1").arg(writeValue) : QString("Écrire %1").arg(writeValue)},
            {"address", writeAddress},
            {"value", writeValue},
            {"valueType", writeValueType},
            {"requiresConfirmation", true},
        });
        result["recoveryActions"] = recoveryActions;
        stampIntent(&result);
        return result;
    } else if (tool == "kernel_write") {
        // Demande explicite ("écris via le kernel") : contrairement à
        // write_value/freeze_value ci-dessus, on propose une action cliquable
        // (recoveryActions) plutôt que de renvoyer vers l'onglet Mémoire —
        // l'écriture kernel n'a pas d'équivalent dans ce panneau usermode, et
        // c'est justement l'action que l'utilisateur vient de demander.
        // Reste toujours un clic de confirmation explicite, jamais automatique.
        const QString kernelAddress = args.value("address").toString().trimmed();
        const QString kernelValue = args.value("value").toString().trimmed();
        const QString kernelValueType = args.value("valueType", "Int32").toString();
        if (kernelAddress.isEmpty() || kernelValue.isEmpty()) {
            result["actionStatus"] = "needs_clarification";
            result["message"] = "Il me faut l'adresse (0x...) et la valeur pour écrire via le driver kernel.";
            stampIntent(&result);
            return result;
        }
        result["actionStatus"] = "requires_confirmation";
        result["requiresConfirmation"] = true;
        result["confirmationReason"] = "Écriture kernel : contourne les protections mémoire usermode (VirtualProtect/PAGE_GUARD). Action irréversible sans lecture préalable de la valeur d'origine.";
        result["message"] = QString("Écriture kernel demandée : %1 (%2) à 0x%3. Confirme pour appliquer via le driver noyau.")
                                 .arg(kernelValue, kernelValueType, kernelAddress);
        QVariantList recoveryActions;
        recoveryActions.append(QVariantMap{
            {"id", "kernel_write_targets"},
            {"label", QString("Écrire %1 via kernel").arg(kernelValue)},
            {"address", kernelAddress},
            {"value", kernelValue},
            {"valueType", kernelValueType},
            {"requiresConfirmation", true},
        });
        result["recoveryActions"] = recoveryActions;
        stampIntent(&result);
        return result;
    } else if (tool == "speedhack_set") {
        // Demande explicite en langage naturel ("ralentis le jeu", "accélère
        // le temps", "remets la vitesse normale") : même patron que
        // kernel_write ci-dessus, une action cliquable plutôt qu'un renvoi
        // vers un onglet — l'Assistant lui-même décide start/setFactor/stop
        // côté frontend selon l'état actuel (executeCheckpointSpeedhack).
        const QString modeArg = args.value("mode", "set").toString().trimmed().toLower();
        const bool isOff = (modeArg == "off" || modeArg == "stop");
        const double factor = args.value("factor", 1.0).toDouble();

        result["actionStatus"] = "requires_confirmation";
        result["requiresConfirmation"] = true;
        result["confirmationReason"] = isOff
            ? "Désactive le speedhack et remet la vitesse perçue à la normale."
            : "Injecte un composant dans le processus cible pour modifier la vitesse perçue du temps.";
        result["message"] = isOff
            ? "Désactivation du speedhack demandée. Confirme pour remettre la vitesse normale."
            : QString("Speedhack demandé : facteur %1x. Confirme pour appliquer.").arg(factor);
        QVariantList recoveryActions;
        recoveryActions.append(QVariantMap{
            {"id", "speedhack_apply"},
            {"label", isOff ? "Désactiver le speedhack" : QString("Appliquer %1x").arg(factor)},
            {"factor", factor},
            {"mode", isOff ? "off" : "set"},
            {"requiresConfirmation", true},
        });
        result["recoveryActions"] = recoveryActions;
        stampIntent(&result);
        return result;
    } else if (tool == "block_process_network") {
        // Meme patron que speedhack_set ci-dessus : action systeme (regle
        // pare-feu + invite UAC), le frontend est seul a detenir
        // confirmRiskAction -- on renvoie une action cliquable plutot que
        // d'appeler blockProcessNetwork()/unblockProcessNetwork() ici.
        const QString modeArg = args.value("mode", "on").toString().trimmed().toLower();
        const bool isOff = (modeArg == "off" || modeArg == "stop" || modeArg == "unblock");

        result["actionStatus"] = "requires_confirmation";
        result["requiresConfirmation"] = true;
        result["confirmationReason"] = isOff
            ? "Retire la règle pare-feu KillEngine posée pour ce processus."
            : "Ajoute une règle pare-feu Windows bloquant tout le trafic entrant/sortant du processus attaché (invite UAC requise).";
        result["message"] = isOff
            ? "Rétablissement du réseau demandé. Confirme pour retirer la règle pare-feu."
            : "Coupure réseau demandée, pour isoler une éventuelle synchro serveur en arrière-plan. Confirme pour appliquer.";
        QVariantList recoveryActions;
        recoveryActions.append(QVariantMap{
            {"id", "network_block_apply"},
            {"label", isOff ? "Rétablir le réseau" : "Couper le réseau"},
            {"mode", isOff ? "off" : "on"},
            {"requiresConfirmation", true},
        });
        result["recoveryActions"] = recoveryActions;
        stampIntent(&result);
        return result;
    } else if (tool == "discover_save_files") {
        const int maxResults = args.value("maxResults", 50).toInt();
        actionResult = discoverProcessSaveFiles(maxResults);
        if (actionResult.value("success").toBool()) {
            result["workflowStatus"] = "save_files_discovered";
            result["message"] = QString("Fichiers de sauvegarde : %1 trouvé(s) sous le package '%2'. On peut en lire un avec read_save_file_text ou inspecter LocalSettings avec inspect_local_settings.")
                                  .arg(actionResult.value("count").toInt())
                                  .arg(actionResult.value("familyName").toString());
        }
    } else if (tool == "inspect_local_settings") {
        const int maxValues = args.value("maxValues", 200).toInt();
        actionResult = inspectProcessLocalSettings(maxValues);
        if (actionResult.value("success").toBool()) {
            result["workflowStatus"] = "local_settings_inspected";
            result["message"] = QString("LocalSettings inspecté : %1 valeur(s) lue(s) dans settings.dat. Cherche un nom ou une valeur qui correspond à l'affichage du jeu.")
                                  .arg(actionResult.value("count").toInt());
        }
    } else if (tool == "read_save_file_text") {
        const QString path = args.value("path").toString();
        const int maxBytes = args.value("maxBytes", 65536).toInt();
        actionResult = readProcessSaveFileText(path, maxBytes);
        if (actionResult.value("success").toBool()) {
            result["workflowStatus"] = "save_file_text_read";
            result["message"] = actionResult.value("truncated").toBool()
                ? "Fichier lu (tronqué à la taille maximale). Cherche le champ correspondant à la valeur affichée dans le texte."
                : "Fichier lu en entier. Cherche le champ correspondant à la valeur affichée dans le texte.";
        }
    } else if (tool == "patch_file_bytes") {
        // PHASE 148 : bug trouve en auditant la ligne schema -- ce cas
        // appelait patchProcessSaveFileBytes() directement malgre
        // requiresConfirmation=true dans le registre (ai/tool_registry.cpp),
        // un vrai contournement RiskGate pour une ecriture disque reelle.
        // Corrige avec le meme patron que find_what_writes/test_candidate_fields
        // (PHASE 146) : jamais d'execution directe depuis le chat, message
        // de redirection uniquement -- pas de recoveryActions cliquable ici
        // (contrairement a kernel_write/speedhack_set/block_process_network
        // ci-dessus) car cette action n'a pas d'equivalent UI existant vers
        // lequel pointer, et inventer un nouvel id de recoveryAction
        // demanderait du cablage frontend hors perimetre de ce chantier.
        const QString patchPath = args.value("path").toString().trimmed();
        const QString patchFindHex = args.value("findHex").toString().trimmed();
        const QString patchReplaceHex = args.value("replaceHex").toString().trimmed();
        result["actionStatus"] = "requires_confirmation";
        result["requiresConfirmation"] = true;
        result["confirmationReason"] = "Édite en place un fichier de sauvegarde réel sur disque — action non réversible automatiquement, pas d'exécution autonome depuis le chat.";
        result["message"] = (patchPath.isEmpty() || patchFindHex.isEmpty() || patchReplaceHex.isEmpty())
            ? "Édition de fichier de sauvegarde demandée, mais il manque le chemin exact et/ou les séquences hex find/replace. Utilise le pipe d'automatisation ou un script Lua avec patchFileBytes une fois la séquence exacte confirmée (read_save_file_text pour vérifier le contexte avant)."
            : QString("Édition de fichier demandée sur %1 (remplace %2 par %3). Pas d'exécution autonome depuis le chat : utilise le pipe d'automatisation ou un script Lua pour l'appliquer une fois sûr de la séquence exacte.")
                  .arg(patchPath, patchFindHex, patchReplaceHex);
        stampIntent(&result);
        return result;
    } else if (tool == "watch_save_file") {
        const QString path = args.value("path").toString();
        QVariantMap watchOptions;
        watchOptions["timeoutMs"] = args.value("timeoutMs", 5000);
        actionResult = watchSaveFileForChanges(path, watchOptions);
        if (actionResult.value("success").toBool()) {
            result["workflowStatus"] = "save_file_watch_finished";
            result["message"] = actionResult.value("changed").toBool()
                ? QString("Le fichier a changé (%1) pendant la fenêtre d'observation.").arg(actionResult.value("changeType").toString())
                : "Aucun changement détecté pendant la fenêtre d'observation.";
        }
    } else {
        result["actionStatus"] = "unsupported_tool";
        result["actionError"] = QString("Outil Smart Search non supporté: %1").arg(tool);
        stampIntent(&result);
        return result;
    }

    result["actionStatus"] = actionResult.value("success").toBool() ? "executed" : "failed";
    result["actionResult"] = actionResult;
    noteAiOutcome(actionResult.value("success").toBool(), QString());

    if (!actionResult.value("success").toBool()) {
        const QString actionError = actionResult.value("error").toString().trimmed();
        result["workflowStatus"] = "action_failed";
        result["error"] = actionError;
        result["message"] = actionError.isEmpty()
            ? QString("L'action %1 a échoué sans détail. Vérifie le processus attaché et le type de valeur.")
                  .arg(tool)
            : QString("Je voulais agir, mais l'action a échoué : %1").arg(actionError);
    } else if (tool == "exact_scan" || tool == "exact_scan_multi_type") {
        m_smartSearchInitialValue = args.value("value").toString();
        m_smartSearchValueType = args.value("valueType", "Int32").toString();
        if (numbers.size() >= 2) {
            m_smartSearchTargetValue = numbers.at(1);
        }

        const auto count = actionResult.value("candidateStoreSize").toULongLong();
        m_smartSearchActive = count > 0;
        result["targetValue"] = m_smartSearchTargetValue;
        const QString prefix = intent.resetContext
            ? QString("Je repars sur une nouvelle recherche. ")
            : QString();
        const QString typeNote = tool == "exact_scan_multi_type"
            ? QString(" en Auto rapide")
            : QString();
        if (count > 0) {
            result["workflowStatus"] = "awaiting_value_change";
            result["message"] = prefix + QString("J'ai trouvé %1 candidats pour %2%3. Fais bouger la valeur dans le jeu, puis donne-moi la nouvelle valeur pour réduire la liste.")
                                    .arg(count)
                                    .arg(m_smartSearchInitialValue)
                                    .arg(typeNote);
        } else {
            // Scan direct a vide : proposer tout de suite les strategies de
            // repli plutot que redemander une variation sur une recherche
            // qui n'a encore trouve aucun candidat.
            result["workflowStatus"] = "no_candidate";
            result["message"] = prefix + QString("Aucun candidat pour %1%2 en scan direct. Ce n'est pas forcement une impasse : ça peut être une valeur chiffrée/obfusquée, une valeur affichée en texte plutôt qu'en mémoire brute, ou une valeur qui varie déjà.")
                                    .arg(m_smartSearchInitialValue)
                                    .arg(typeNote);
            QVariantList recoveryActions;
            recoveryActions.append(QVariantMap{{"id", "try_encrypted_scan"}, {"label", "Scan chiffré (XOR)"}, {"value", m_smartSearchInitialValue}});
            recoveryActions.append(QVariantMap{{"id", "trace_ui_string"}, {"label", "Chercher le texte affiché"}, {"value", m_smartSearchInitialValue}});
            recoveryActions.append(QVariantMap{{"id", "try_unknown_changed"}, {"label", "Unknown (valeur inconnue)"}});
            recoveryActions.append(QVariantMap{{"id", "new_search"}, {"label", "Nouvelle recherche"}});
            result["recoveryActions"] = recoveryActions;
        }
    } else if (tool == "next_scan" || tool == "unknown_compare") {
        const auto remaining = tool == "next_scan"
                                   ? actionResult.value("remaining").toULongLong()
                                   : actionResult.value("stored").toULongLong();
        result["targetValue"] = m_smartSearchTargetValue;

        const bool withinAutoWriteRange = remaining >= 1 && remaining <= kAutoWriteCandidateLimit;

        if (withinAutoWriteRange && m_smartSearchTargetValue.isEmpty()) {
            // Chemins comme Trace UI string ou un ExactScan a un seul nombre
            // ne definissent jamais m_smartSearchTargetValue (seul GuidedScan,
            // "X que je veux a Y", le fait). Sans ce cas, un utilisateur qui
            // reduit correctement a 1-4 candidats se retrouvait bloque en
            // boucle infinie sur "trop de candidats, raffine encore" (faux :
            // 3 candidats n'est PAS trop, il manque juste la valeur a ecrire).
            m_pendingRecoveryAction = "write_target_value";
            result["workflowStatus"] = "awaiting_new_value";
            result["message"] = QString(
                "Il reste %1 candidat(s), c'est peu — mais je ne sais pas encore quelle valeur écrire. Donne-moi la "
                "valeur que tu veux mettre (juste le nombre, ex: 3000).")
                .arg(remaining);
        } else if (withinAutoWriteRange && !m_smartSearchTargetValue.isEmpty()) {
            auto suggestions = suggestedWritesForCandidates(
                candidates,
                m_smartSearchTargetValue,
                kAutoWriteCandidateLimit);
            enrichSuggestedWritesWithHistory(&suggestions);

            const QString gameKey = autoResolverGameKey(processName());
            const int cleanCandidateCount = flagNoisyCandidates(
                &suggestions, gameKey, m_smartSearchInitialValue, m_smartSearchTargetValue);
            // Si au moins une adresse n'est jamais apparue ailleurs, on ecarte
            // celles deja vues sur une recherche sans rapport plutot que
            // d'ecrire dessus a l'aveugle. Si TOUTES sont suspectes, on ecrit
            // quand meme (rien de mieux a proposer) mais le message et
            // confidenceReason portent deja l'avertissement.
            const bool allSuggestionsNoisy = cleanCandidateCount == 0 && !suggestions.isEmpty();
            if (cleanCandidateCount > 0 && cleanCandidateCount < suggestions.size()) {
                QVariantList cleanOnly;
                for (const auto& item : suggestions) {
                    if (!item.toMap().contains("noisyHistoryHits")) {
                        cleanOnly.append(item);
                    }
                }
                suggestions = cleanOnly;
            }

            QVariantList rejectedSuggestions;
            const auto writeSuggestions = filterAutoWriteSuggestionsByRegion(suggestions, &rejectedSuggestions);
            QVariantList writeResults;
            bool allWritesOk = true;
            m_lastBatchStartIndex = m_writeHistory.size();
            m_lastAutoWriteTargets.clear();

            for (const auto& item : writeSuggestions) {
                const auto suggestion = item.toMap();
                auto writeResult = writeMemoryValueConfirmed(
                    suggestion.value("address").toString(),
                    suggestion.value("type").toString(),
                    suggestion.value("value").toString());
                // Enrichit le résultat avec l'adresse/valeur pour l'affichage UI.
                writeResult.insert("address", suggestion.value("address"));
                writeResult.insert("value", suggestion.value("value"));
                writeResult.insert("type", suggestion.value("type"));
                if (suggestion.contains("valueHistory")) {
                    writeResult.insert("valueHistory", suggestion.value("valueHistory"));
                }
                allWritesOk = allWritesOk && writeResult.value("success").toBool();
                writeResults.append(writeResult);
                if (writeResult.value("success").toBool()) {
                    uint64_t address = 0;
                    killcore::ValueType type;
                    if (parseHexAddress(suggestion.value("address").toString(), &address)
                        && killcore::parseValueType(suggestion.value("type").toString(), &type)) {
                        m_lastAutoWriteTargets.append({address, type});
                    }
                }
            }

            // Memorise ce lot dans l'historique inter-sessions, que l'ecriture
            // ait reussi ou non : meme une tentative sur une mauvaise adresse
            // sert a la reperer comme suspecte la prochaine fois.
            QVariantList historyEntries;
            for (const auto& item : writeSuggestions) {
                const auto suggestion = item.toMap();
                historyEntries.append(QVariantMap{
                    {"address", suggestion.value("address")},
                    {"type", suggestion.value("type")},
                    {"initialValue", m_smartSearchInitialValue},
                    {"targetValue", m_smartSearchTargetValue},
                    {"timestamp", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
                });
            }
            appendCandidateHistory(gameKey, historyEntries);

            m_lastBatchEndIndex = m_writeHistory.size();
            if (m_lastBatchEndIndex == m_lastBatchStartIndex) {
                m_lastBatchStartIndex = -1;
                m_lastBatchEndIndex = -1;
                m_lastAutoWriteTargets.clear();
            }
            if (allWritesOk && !m_lastAutoWriteTargets.isEmpty()) {
                m_smartSearchActive = false;
                m_chatMemoryTargets = m_lastAutoWriteTargets;
                resetFailureEscalationState();
                m_autoWriteValueHistory.clear();
                appendDistinctText(&m_autoWriteValueHistory, m_smartSearchInitialValue, 12);
                appendDistinctText(&m_autoWriteValueHistory, m_smartSearchTargetValue, 12);
            }

            result["workflowStatus"] = allWritesOk ? "auto_write_done" : "auto_write_partial_or_failed";
            result["suggestedWrites"] = suggestions;
            result["suggestedWrite"] = suggestions.isEmpty() ? QVariantMap{} : suggestions.first().toMap();
            result["filteredWriteCandidates"] = rejectedSuggestions;
            result["autoWriteResults"] = writeResults;
            result["autoWriteResult"] = writeResults.isEmpty() ? QVariantMap{} : writeResults.last().toMap();
            result["autoWriteCount"] = writeResults.size();
            result["activeTargetCount"] = m_chatMemoryTargets.size();
            result["previousTargetValue"] = m_smartSearchInitialValue;
            result["writeHistory"] = writeHistoryToVariantList(m_autoWriteValueHistory);
            result["rollbackNote"] = "Tu peux annuler toutes les écritures via le bouton rollback batch dans l'assistant.";
            if (writeSuggestions.isEmpty()) {
                result["workflowStatus"] = "auto_write_partial_or_failed";
                result["message"] = QString("Il reste %1 candidat(s), mais le filtre anti-bruit n'a gardé aucune adresse assez fiable pour une écriture automatique.")
                                        .arg(remaining);
            } else if (allSuggestionsNoisy) {
                result["message"] = QString(
                    "Il reste %1 candidat(s), mais %2 sont déjà apparues comme fiables sur une recherche différente et "
                    "sans rapport avant — probablement du bruit (un compteur interne, pas la vraie donnée). J'ai quand "
                    "même écrit %3 faute de meilleure piste : vérifie particulièrement bien si ça a marché.")
                    .arg(remaining)
                    .arg(writeSuggestions.size())
                    .arg(m_smartSearchTargetValue);
            } else {
                result["message"] = allWritesOk
                                    ? QString("Il reste %1 candidat(s). J'ai écrit automatiquement %2 sur les adresses finales fiables. Je garde ces adresses actives pour les prochaines modifications.")
                                          .arg(remaining)
                                          .arg(m_smartSearchTargetValue)
                                    : QString("Il reste %1 candidat(s), mais au moins une écriture automatique a échoué.")
                                          .arg(remaining);
            }
        } else if (remaining > 1) {
            result["workflowStatus"] = "needs_more_refinement";
            result["message"] = QString("Il reste %1 candidats. Refais varier le score, puis indique-moi la nouvelle valeur.")
                                    .arg(remaining);
        } else {
            result["workflowStatus"] = "no_candidate";
            const QString diagnostic = actionResult.value("diagnostic").toString();
            const QString observedValue = args.value("value").toString().trimmed();
            const QString retryValue = observedValue.isEmpty() ? m_smartSearchInitialValue : observedValue;
            QVariantList recoveryActions;
            recoveryActions.append(QVariantMap{{"id", "undo_reduction"}, {"label", "Restaurer les candidats"}});
            recoveryActions.append(QVariantMap{{"id", "try_changed"}, {"label", "Essayer changed"}});
            recoveryActions.append(QVariantMap{{"id", "try_increased"}, {"label", "Essayer increased"}});
            recoveryActions.append(QVariantMap{{"id", "retry_float32"}, {"label", "Rechercher en Float32"}, {"value", retryValue}, {"target", m_smartSearchTargetValue}});
            recoveryActions.append(QVariantMap{{"id", "retry_int64"}, {"label", "Rechercher en Int64"}, {"value", retryValue}, {"target", m_smartSearchTargetValue}});
            recoveryActions.append(QVariantMap{{"id", "retry_int32_x100"}, {"label", "Rechercher valeur x100"}, {"value", retryValue}, {"target", m_smartSearchTargetValue}});
            recoveryActions.append(QVariantMap{{"id", "try_unknown_increased"}, {"label", "Unknown + increased"}});
            recoveryActions.append(QVariantMap{{"id", "new_search"}, {"label", "Nouvelle recherche"}});
            result["diagnostic"] = diagnostic;
            result["observedValue"] = retryValue;
            result["recoveryActions"] = recoveryActions;
            result["message"] = diagnostic.isEmpty()
                ? QString("Aucun candidat restant. Restaure les candidats précédents, puis essaie changed/increased ou une autre représentation.")
                : QString("Aucun candidat restant. %1").arg(diagnostic);
        }
    }

    stampIntent(&result);
    if (result.value("message").toString().trimmed().isEmpty()
        && result.value("error").toString().trimmed().isEmpty()
        && result.value("actionStatus").toString().trimmed().isEmpty()) {
        result["actionStatus"] = "needs_clarification";
        result["message"] = "Je garde le contexte actuel. Donne-moi une valeur à chercher, une nouvelle valeur observée, ou une adresse mémoire à utiliser.";
    }

    appendSmartSearchDebug("smart_search_result", result);
    return result;
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

} // namespace killengine
