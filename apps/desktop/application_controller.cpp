#include "application_controller.h"

#ifdef Q_OS_WIN
#include <windows.h>
#include <shellapi.h>
#include <iphlpapi.h>
#include <psapi.h>
#include <ws2tcpip.h>
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "ws2_32.lib")
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
#include "claude_chat_manager.h"
#include "clr_inspector_bridge.h"
#include "code_patch_manager.h"
#include "external_tool_profiler.h"
#include "debug_feature_manager.h"
#include "freeze_hotkey_overlay_manager.h"
#include "investigation_notebook_manager.h"
#include "kernel_driver_manager.h"
#include "lua_repl_manager.h"
#include "lua_runtime_locator.h"
#include "memory_heatmap_manager.h"
#include "memory_timeline_manager.h"
#include "pattern_learning_manager.h"
#include "profile_manager.h"
#include "save_file_investigator.h"
#include "scanning_core_manager.h"
#include "settings_diagnostics_manager.h"
#include "smart_search_manager.h"
#include "smart_watchdog_manager.h"
#include "write_freeze_core_manager.h"
#include "query_text_utils.h"
#include "model_locator.h"
#include "display_string_investigator.h"
#include "candidates/candidate_store.h"
#include "crash_handler.h"
#include "debug/hardware_breakpoint.h"
#include "debug/stealth_profiler.h"
#include "inject/lag_switch.h"
#include "inject/http_proxy.h"
#include "profiles/ghidra_bridge.h"
#include "localization/localization.h"
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
#include "scanner/auto_dissect.h"
#include "scanner/value_variants.h"
#include "snapshot/snapshot_store.h"
#include "webview2/webview2_inspector.h"

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
#include <QMutex>
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

QString defaultWebView2CdpEndpoint(const QVariantMap& options) {
    const QString explicitHttpUrl = options.value(QStringLiteral("httpUrl")).toString().trimmed();
    if (!explicitHttpUrl.isEmpty()) {
        return explicitHttpUrl;
    }
    const int directPort = options.value(QStringLiteral("directPort"), 9333).toInt();
    if (directPort > 0) {
        return QStringLiteral("http://127.0.0.1:%1/json").arg(directPort);
    }
    return QStringLiteral("http://127.0.0.1:50080/msedge");
}

QVariantMap webView2ResultEnvelope(const QString& risk, bool requiresConfirmation) {
    QVariantMap result;
    result[QStringLiteral("success")] = false;
    result[QStringLiteral("risk")] = risk;
    result[QStringLiteral("requiresConfirmation")] = requiresConfirmation;
    result[QStringLiteral("capability")] = QStringLiteral("webview2_cdp");
    return result;
}

QString webView2SetupHint() {
    return QStringLiteral(
        "Aucune target CDP WebView2 disponible. Pour une app desktop/Electron/CEF, verifier le port direct "
        "(ex: --remote-debugging-port=9333 puis /json). Pour une app UWP/Store, installer "
        "Tools.DeveloperMode.Core, activer le Portail d'appareil, installer Remote Tools for Microsoft Edge, "
        "relancer la cible avec msEdgeDevToolsWdpRemoteDebugging, puis utiliser http://127.0.0.1:50080/msedge.");
}

int jsonValueToInt(const QJsonValue& value) {
    if (value.isDouble()) {
        return value.toInt();
    }
    bool ok = false;
    const int parsed = value.toString().toInt(&ok);
    return ok ? parsed : 0;
}

bool webView2TargetMatchesPid(const QJsonObject& target, int browserProcessId) {
    if (browserProcessId <= 0) {
        return true;
    }
    const int wdpBrowserPid = jsonValueToInt(target.value(QStringLiteral("wdpBrowserProcessId")));
    const int browserPid = jsonValueToInt(target.value(QStringLiteral("browserProcessId")));
    const int processId = jsonValueToInt(target.value(QStringLiteral("processId")));
    const int pid = jsonValueToInt(target.value(QStringLiteral("pid")));
    if (wdpBrowserPid == 0 && browserPid == 0 && processId == 0 && pid == 0) {
        return true;
    }
    return wdpBrowserPid == browserProcessId
        || browserPid == browserProcessId
        || processId == browserProcessId
        || pid == browserProcessId;
}

bool webView2TargetMatchesTextFilters(const QJsonObject& target, const QVariantMap& options) {
    const QString targetId = options.value(QStringLiteral("targetId"), options.value(QStringLiteral("id"))).toString().trimmed();
    if (!targetId.isEmpty() && target.value(QStringLiteral("id")).toString() != targetId) {
        return false;
    }

    const QString titleContains = options.value(QStringLiteral("pageTitle"), options.value(QStringLiteral("titleContains"))).toString().trimmed();
    if (!titleContains.isEmpty()
        && !target.value(QStringLiteral("title")).toString().contains(titleContains, Qt::CaseInsensitive)) {
        return false;
    }

    const QString urlContains = options.value(QStringLiteral("pageUrl"), options.value(QStringLiteral("urlContains"))).toString().trimmed();
    if (!urlContains.isEmpty()
        && !target.value(QStringLiteral("url")).toString().contains(urlContains, Qt::CaseInsensitive)) {
        return false;
    }

    return true;
}

QJsonArray filterWebView2Targets(const QJsonArray& pages, int browserProcessId, const QVariantMap& options) {
    QJsonArray filtered;
    const bool pageTargetsOnly = options.value(QStringLiteral("pageTargetsOnly"), true).toBool();
    const bool allowAboutBlank = options.value(QStringLiteral("allowAboutBlank"), false).toBool();
    for (const QJsonValue& value : pages) {
        if (!value.isObject()) {
            continue;
        }
        const QJsonObject target = value.toObject();
        if (pageTargetsOnly && target.value(QStringLiteral("type")).toString() != QStringLiteral("page")) {
            continue;
        }
        if (!allowAboutBlank && target.value(QStringLiteral("url")).toString().startsWith(QStringLiteral("about:"), Qt::CaseInsensitive)) {
            continue;
        }
        if (!webView2TargetMatchesPid(target, browserProcessId)) {
            continue;
        }
        if (!webView2TargetMatchesTextFilters(target, options)) {
            continue;
        }
        if (target.value(QStringLiteral("webSocketDebuggerUrl")).toString().isEmpty()) {
            continue;
        }
        filtered.append(target);
    }
    return filtered;
}

QVariantList webView2TargetsToVariantList(const QJsonArray& targets) {
    QVariantList list;
    list.reserve(targets.size());
    for (const QJsonValue& value : targets) {
        if (value.isObject()) {
            list.append(value.toObject().toVariantMap());
        }
    }
    return list;
}

QVariantMap webView2TargetToVariantMap(const QJsonObject& target) {
    return target.toVariantMap();
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
        outcome.error = KE_TXT("Script Lua vide.", "Empty Lua script.");
        return outcome;
    }

    const QString luaPath = findLuaExecutable(options.value("luaPath").toString());
    outcome.luaPath = luaPath;
    if (luaPath.isEmpty()) {
        outcome.error = KE_TXT("Aucun interpréteur Lua trouvé. Place lua.exe dans runtime\\lua à côté de KillEngine.exe, ajoute Lua au PATH, ou renseigne options.luaPath.",
                                "No Lua interpreter found. Place lua.exe in runtime\\lua next to KillEngine.exe, add Lua to PATH, or set options.luaPath.");
        return outcome;
    }

    QTemporaryFile scriptFile(QDir::temp().filePath("killengine-lua-XXXXXX.lua"));
    scriptFile.setAutoRemove(true);
    if (!scriptFile.open()) {
        outcome.error = KE_TXT("Impossible de créer le script temporaire Lua : %1", "Unable to create the temporary Lua script: %1").arg(scriptFile.errorString());
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
        outcome.error = KE_TXT("Impossible de démarrer Lua : %1", "Unable to start Lua: %1").arg(process.errorString());
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
            ? KE_TXT("Script Lua annulé.", "Lua script cancelled.")
            : outcome.timedOut
            ? KE_TXT("Script Lua interrompu après timeout (%1 ms).", "Lua script interrupted after timeout (%1 ms).").arg(timeoutMs)
            : KE_TXT("Script Lua terminé avec le code %1.", "Lua script finished with code %1.").arg(outcome.exitCode);
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
std::optional<uint64_t> bytesToWatchValue(const QByteArray& bytes);

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

std::optional<uint64_t> bytesToWatchValue(const QByteArray& bytes) {
    if (bytes.isEmpty() || bytes.size() > static_cast<int>(sizeof(uint64_t))) {
        return std::nullopt;
    }

    uint64_t value = 0;
    std::memcpy(&value, bytes.constData(), static_cast<size_t>(bytes.size()));
    return value;
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
    m_smartWatchdogManager = std::make_unique<SmartWatchdogManager>(this);
    m_smartWatchdogManager->setReadValueCallback([this](uint64_t address, uint32_t size) -> std::optional<uint64_t> {
        if (!m_attached || !m_handle.isValid() || size == 0 || size > sizeof(uint64_t)) {
            return std::nullopt;
        }

        killcore::MemoryReader reader(m_handle);
        const auto read = reader.read(address, size);
        if (!read.success || read.data.size() < static_cast<int>(size)) {
            return std::nullopt;
        }
        return bytesToWatchValue(read.data.left(static_cast<int>(size)));
    });
    connect(m_smartWatchdogManager.get(), &SmartWatchdogManager::resyncDetected, this, [this](uint64_t address, const QString& suggestion) {
        appendScanTelemetry("smart_watchdog_resync", {
            {"address", QString::number(address, 16).toUpper()},
            {"suggestion", suggestion},
        });
    });
    connect(m_smartWatchdogManager.get(), &SmartWatchdogManager::writeConfirmedStable, this, [this](uint64_t address) {
        appendScanTelemetry("smart_watchdog_stable", {
            {"address", QString::number(address, 16).toUpper()},
        });
    });
    connect(m_smartWatchdogManager.get(), &SmartWatchdogManager::twinPatternDetected, this, [this](uint64_t displayAddress, uint64_t sourceAddress) {
        appendScanTelemetry("smart_watchdog_twin_pattern", {
            {"displayAddress", QString::number(displayAddress, 16).toUpper()},
            {"sourceAddress", QString::number(sourceAddress, 16).toUpper()},
        });
    });
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
      // MemoryHeatmapManager ne prend pas (processHandle, telemetry) au
      // constructeur -- son API reelle (memory_heatmap_manager.h) recoit le
      // handle par appel via startHeatmapCollection(quint64, options), pas a
      // la construction. Corrige ici pour compiler ; le reste du branchement
      // (appel a startHeatmapCollection depuis un Q_INVOKABLE, telemetry via
      // setUpdateCallback) reste a faire par l'agent proprietaire de ce
      // chantier -- voir docs/SALON.md.
      m_memoryHeatmapManager = std::make_unique<MemoryHeatmapManager>(this);
      m_memoryTimelineManager = std::make_unique<MemoryTimelineManager>(this);
      m_patternLearningManager = std::make_unique<PatternLearningManager>(this);
      connect(m_patternLearningManager.get(), &PatternLearningManager::engineDetected, this, &ApplicationController::patternLearningEngineDetected);
      connect(m_patternLearningManager.get(), &PatternLearningManager::patternClassified, this, &ApplicationController::patternLearningClassified);
      connect(m_patternLearningManager.get(), &PatternLearningManager::suggestionReady, this, &ApplicationController::patternLearningSuggestionReady);
      connect(m_patternLearningManager.get(), &PatternLearningManager::trackingUpdated, this, &ApplicationController::patternLearningTrackingUpdated);
      // Ouvre juste un fichier JSON (pas de process attaché nécessaire) --
      // initialisation auto pour que le reste de l'API soit utilisable
      // immédiatement, sans étape "initialize" explicite côté appelant.
      m_patternLearningManager->initialize();
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
    m_webView2Inspector = std::make_unique<killcore::WebView2Inspector>();
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
    m_externalToolProfiler = std::make_unique<ExternalToolProfiler>(
        m_handle,
        [this](const QString& event, const QVariantMap& payload) {
            appendScanTelemetry(event, payload);
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
    m_claudeChatManager = std::make_unique<ClaudeChatManager>(this);
    m_investigationNotebookManager = std::make_unique<InvestigationNotebookManager>();
    m_kernelDriverManager = std::make_unique<KernelDriverManager>(
        m_handle,
        [this](const QString& event, const QVariantMap& payload) {
            appendScanTelemetry(event, payload);
        });
    m_luaReplManager = std::make_unique<LuaReplManager>(this);
    connect(m_luaReplManager.get(), &LuaReplManager::lineFinished, this, &ApplicationController::luaReplLineFinished);
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
    if (m_webView2Inspector) {
        m_webView2Inspector->disconnect();
    }
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
            (*result)["stableGroupHint"] = KE_TXT(
                "%1 candidat(s) restent identiques depuis %2 cycles de next scan — "
                "probablement des copies redondantes de la même valeur. Une écriture "
                "isolée sur un seul risque d'être annulée silencieusement ; essaie "
                "writeMemoryValuesAtomic() pour les écrire tous en même temps.",
                "%1 candidate(s) have stayed identical for %2 next-scan cycles — "
                "probably redundant copies of the same value. Writing to just one "
                "risks being silently overwritten; try "
                "writeMemoryValuesAtomic() to write them all at once.")
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
        result["error"] = KE_TXT("Aucun processus attaché.", "No process attached.");
        return result;
    }
    if (moduleName.trimmed().isEmpty() || functionName.trimmed().isEmpty()) {
        result["error"] = KE_TXT("Module et fonction requis.", "Module and function required.");
        return result;
    }

    uint64_t address = 0;
    QString error;
    if (!killcore::resolveRemoteExportAddress(m_handle, moduleName, functionName, &address, &error)) {
        result["error"] = error.isEmpty() ? KE_TXT("Résolution de symbole échouée.", "Symbol resolution failed.") : error;
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
        result["error"] = KE_TXT("Aucun processus attaché.", "No process attached.");
        return result;
    }
    if (moduleName.trimmed().isEmpty()) {
        result["error"] = KE_TXT("Module requis.", "Module required.");
        return result;
    }

    QStringList names;
    QString error;
    if (!killcore::listRemoteExportNames(m_handle, moduleName, filterSubstring, maxNames, &names, &error)) {
        result["error"] = error.isEmpty() ? KE_TXT("Listage des exports échoué.", "Failed to list exports.") : error;
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

QVariantMap ApplicationController::compareProcessSaveFileSnapshots(const QVariantList& before, const QVariantList& after) const {
    return m_saveFileInvestigator->compareSaveFileSnapshots(before, after);
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
        emit errorOccurred(KE_TXT("Accès insuffisant au processus.", "Insufficient access to the process."));
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
    m_externalToolProfiler->clearSessionState();
    m_activeProfileTargets.clear();
    m_autoWriteValueHistory.clear();
    // Le compteur de tentatives EDR (rate limiting, voir checkEdrBlocking) est
    // par-cible : sans ce reset, un test réalisé sur un ancien processus attaché
    // rendait le tout premier test sur un NOUVEAU processus faussement "fiable"
    // (m_edrCheckCount >= 4 hérité de la session précédente).
    m_edrCheckCount = 0;
    // Meme raison : l'historique de conversation Claude contient des adresses
    // memoire du processus precedent, invalides pour le nouveau.
    m_claudeChatManager->resetConversation();

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
    m_claudeChatManager->resetConversation();

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
            suggestion["noiseFilterReason"] = KE_TXT("Adresse invalide.", "Invalid address.");
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
            suggestion["noiseFilterReason"] = KE_TXT("Région mémoire introuvable.", "Memory region not found.");
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
            suggestion["noiseFilterReason"] = KE_TXT("Région peu pertinente pour une valeur de jeu.", "Region not relevant for a game value.");
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
        result["error"] = KE_TXT("Aucun processus attaché.", "No process attached.");
        return result;
    }

    bool ok = false;
    QString normalized = addressHex.trimmed();
    if (normalized.startsWith("0x", Qt::CaseInsensitive)) {
        normalized = normalized.mid(2);
    }
    const uint64_t address = normalized.toULongLong(&ok, 16);
    if (!ok || address == 0) {
        result["error"] = KE_TXT("Adresse invalide.", "Invalid address.");
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
        result["error"] = KE_TXT("Aucun processus attaché.", "No process attached.");
        return result;
    }

    bool ok = false;
    QString normalized = addressHex.trimmed();
    if (normalized.startsWith("0x", Qt::CaseInsensitive)) {
        normalized = normalized.mid(2);
    }
    const uint64_t address = normalized.toULongLong(&ok, 16);
    if (!ok || address == 0) {
        result["error"] = KE_TXT("Adresse invalide.", "Invalid address.");
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
        result["error"] = KE_TXT("Aucun processus attaché.", "No process attached.");
        return result;
    }

    bool ok = false;
    QString normalized = addressHex.trimmed();
    if (normalized.startsWith("0x", Qt::CaseInsensitive)) {
        normalized = normalized.mid(2);
    }
    const uint64_t address = normalized.toULongLong(&ok, 16);
    if (!ok || address == 0) {
        result["error"] = KE_TXT("Adresse invalide.", "Invalid address.");
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
        result["error"] = KE_TXT("Adresses invalides.", "Invalid addresses.");
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

QVariantMap ApplicationController::findStructureInstancesAsync(const QVariantMap& templateJson) const {
    QVariantMap started;
    started["success"] = false;

    if (!m_handle.isValid()) {
        started["error"] = KE_TXT("Aucun processus attaché.", "No process attached.");
        return started;
    }

    // Reconstruire le StructureTemplate depuis le JSON
    killcore::StructureTemplate tmpl;
    tmpl.name = templateJson.value("name").toString();
    tmpl.instanceDelta = templateJson.value("instanceDelta", 0).toLongLong();

    const auto fieldsArray = templateJson.value("fields").toList();
    for (const auto& fVal : fieldsArray) {
        const auto fMap = fVal.toMap();
        killcore::StructureField f;
        f.offset = fMap.value("offset", 0).toInt();
        const QString typeStr = fMap.value("type").toString();
        if (typeStr == "Int8") f.type = killcore::FieldType::Int8;
        else if (typeStr == "UInt8") f.type = killcore::FieldType::UInt8;
        else if (typeStr == "Int16") f.type = killcore::FieldType::Int16;
        else if (typeStr == "UInt16") f.type = killcore::FieldType::UInt16;
        else if (typeStr == "Int32") f.type = killcore::FieldType::Int32;
        else if (typeStr == "UInt32") f.type = killcore::FieldType::UInt32;
        else if (typeStr == "Int64") f.type = killcore::FieldType::Int64;
        else if (typeStr == "UInt64") f.type = killcore::FieldType::UInt64;
        else if (typeStr == "Float32") f.type = killcore::FieldType::Float32;
        else if (typeStr == "Float64") f.type = killcore::FieldType::Float64;
        else if (typeStr == "Ptr64") f.type = killcore::FieldType::Pointer64;
        else f.type = killcore::FieldType::Int32; // défaut
        f.label = fMap.value("label").toString();
        tmpl.fields.append(f);
    }

    if (tmpl.fields.isEmpty()) {
        started["error"] = KE_TXT("Template vide — aucun champ à matcher.", "Empty template — no field to match.");
        return started;
    }

    killcore::AutoDissectOptions options;
    options.maxResults = std::clamp(templateJson.value("maxResults", 128).toInt(), 1, 1024);
    options.requirePointerValidity = templateJson.value("requirePointerValidity", true).toBool();
    options.minConfidence = std::clamp(templateJson.value("minConfidence", 0.5).toDouble(), 0.0, 1.0);

    // Le scan lui-même (potentiellement des dizaines de millions de positions
    // testées, voir auto_dissect.cpp) tourne sur un thread séparé pour ne pas
    // geler le thread GUI. Le thread rouvre son propre ProcessHandle en
    // lecture seule (même convention que testCandidateFieldsAsync) plutôt
    // que de partager m_handle, qui pourrait être fermé par un
    // detachProcess() concurrent pendant un scan long.
    const int pid = m_pid;
    const QPointer<ApplicationController> self(const_cast<ApplicationController*>(this));
    std::thread([self, pid, tmpl, options]() {
        killcore::ProcessHandle scanHandle(static_cast<uint32_t>(pid), killcore::ProcessAccess::ReadOnly);
        const auto autoResult = killcore::findStructureInstances(scanHandle, tmpl, options);

        QVariantMap result;
        result["success"] = autoResult.success;
        result["error"] = autoResult.error;
        result["scannedRegions"] = autoResult.scannedRegions;
        result["totalCandidates"] = autoResult.totalCandidates;

        QVariantList instances;
        for (const auto& inst : autoResult.instances) {
            QVariantMap item;
            item["baseAddress"] = QString::number(inst.baseAddress, 16).toUpper();
            item["confidence"] = inst.confidence;
            QVariantList values;
            for (const auto& v : inst.fieldValues) {
                values.append(v);
            }
            item["fieldValues"] = values;
            instances.append(item);
        }
        result["instances"] = instances;
        result["instanceCount"] = instances.size();

        if (!self) return;
        QMetaObject::invokeMethod(self.data(), [self, result]() {
            if (!self) return;
            emit self->findStructureInstancesFinished(result);
        }, Qt::QueuedConnection);
    }).detach();

    started["success"] = true;
    started["started"] = true;
    return started;
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

QVariantMap ApplicationController::unknownNextScan(const QString& mode, const QString& valueType, const QString& deltaValue) {
    return m_scanningCoreManager->unknownNextScan(mode, valueType, deltaValue);
}

QVariantMap ApplicationController::unknownNextScanAsync(const QString& mode, const QString& valueType, const QString& deltaValue) {
    return m_scanningCoreManager->unknownNextScanAsync(mode, valueType, deltaValue);
}

void ApplicationController::watchSmartWriteIfPossible(uint64_t address, const QByteArray& writtenBytes, const QByteArray& originalBytes) {
    if (!m_smartWatchdogManager) {
        return;
    }

    const auto writtenValue = bytesToWatchValue(writtenBytes);
    const auto originalValue = bytesToWatchValue(originalBytes);
    if (!writtenValue || !originalValue || writtenBytes.size() != originalBytes.size()) {
        return;
    }

    m_smartWatchdogManager->setEnabled(true);
    m_smartWatchdogManager->watchWrite(
        address,
        *writtenValue,
        *originalValue,
        static_cast<uint32_t>(writtenBytes.size()));
}

QVariantMap ApplicationController::writeMemoryValue(const QString& addressHex, const QString& valueType, const QString& value) {
    uint64_t address = 0;
    killcore::ValueType type = killcore::ValueType::Int32;
    killcore::ScanValue scanValue;
    QString parseError;
    QByteArray originalBytes;
    QByteArray targetBytes;
    const bool canWatch = parseHexAddress(addressHex, &address)
        && killcore::parseValueType(valueType, &type)
        && killcore::parseScanValue(value, type, &scanValue, &parseError);
    if (canWatch && m_handle.isValid()) {
        targetBytes = killcore::scanValueToBytes(scanValue);
        killcore::MemoryReader reader(m_handle);
        const auto read = reader.read(address, static_cast<size_t>(targetBytes.size()));
        if (read.success && read.data.size() >= targetBytes.size()) {
            originalBytes = read.data.left(targetBytes.size());
        }
    }

    QVariantMap result = m_writeFreezeCoreManager->writeMemoryValue(addressHex, valueType, value);
    if (result.value("success").toBool() && !originalBytes.isEmpty()) {
        watchSmartWriteIfPossible(address, targetBytes, originalBytes);
    }
    return result;
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
        result["error"] = KE_TXT("Aucun processus attaché.", "No process attached.");
        return result;
    }

    uint64_t address = 0;
    if (!parseHexAddress(addressHex, &address)) {
        result["error"] = KE_TXT("Adresse invalide.", "Invalid address.");
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
    result["warning"] = KE_TXT("Cette fonction attache KillEngine comme debugger au processus cible pendant la capture (lecture seule, aucune écriture).",
                                "This function attaches KillEngine as a debugger to the target process during capture (read-only, no writes).");
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
        result["error"] = KE_TXT("Aucun processus attaché.", "No process attached.");
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
    result["error"] = KE_TXT("Lecture de texte de fenêtre disponible seulement sous Windows.", "Window text reading is only available on Windows.");
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
        result["error"] = KE_TXT("Aucun processus attaché.", "No process attached.");
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
        result["error"] = KE_TXT("Aucune fenêtre visible trouvée pour ce processus (essaie hwndHex explicite ou ajuste titleContains).",
                                  "No visible window found for this process (try an explicit hwndHex or adjust titleContains).");
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
        result["error"] = KE_TXT("CoInitializeEx a échoué (0x%1).", "CoInitializeEx failed (0x%1).").arg(static_cast<uint32_t>(initHr), 0, 16);
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
        result["error"] = KE_TXT("CoCreateInstance(CUIAutomation) a échoué (0x%1).", "CoCreateInstance(CUIAutomation) failed (0x%1).").arg(static_cast<uint32_t>(hr), 0, 16);
        return result;
    }

    Microsoft::WRL::ComPtr<IUIAutomationElement> root;
    hr = automation->ElementFromHandle(targetHwnd, &root);
    if (FAILED(hr) || !root) {
        cleanupCom();
        result["error"] = KE_TXT("ElementFromHandle (UIA) a échoué pour cette fenêtre (0x%1).", "ElementFromHandle (UIA) failed for this window (0x%1).").arg(static_cast<uint32_t>(hr), 0, 16);
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
        result["error"] = KE_TXT("FindAll (UIA) a échoué -- la cible ne repond peut-etre pas a l'arbre d'accessibilite.",
                                  "FindAll (UIA) failed -- the target may not be responding to the accessibility tree.");
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
    result["error"] = KE_TXT("Lecture UI Automation disponible seulement sous Windows.", "UI Automation reading is only available on Windows.");
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

QVariantMap ApplicationController::startInProcessBreakpointFreezeAsync(const QString& addressHex, const QString& valueType, const QString& value, const QVariantMap& options) {
    QVariantMap started;
    // DebugFeatureManager::startInProcessBreakpointFreeze() rouvre son propre
    // ProcessHandle en interne (indépendant de m_handle) et poll jusqu'à 2s en
    // attendant la confirmation du handler — tourne sur un thread séparé pour
    // ne pas geler le thread GUI, même raisonnement que
    // startHttpProxyAsync/setLagSwitchAsync. m_debugFeatureManager n'est
    // jamais recréé une fois construit, le pointeur brut reste donc valide.
    DebugFeatureManager* dfm = m_debugFeatureManager.get();
    const QPointer<ApplicationController> self(this);
    std::thread([self, dfm, addressHex, valueType, value, options]() {
        const QVariantMap result = dfm->startInProcessBreakpointFreeze(addressHex, valueType, value, options);
        if (!self) return;
        QMetaObject::invokeMethod(self.data(), [self, result]() {
            if (!self) return;
            emit self->inProcessBreakpointFreezeStartFinished(result);
        }, Qt::QueuedConnection);
    }).detach();

    started["success"] = true;
    started["started"] = true;
    return started;
}

QVariantMap ApplicationController::stopInProcessBreakpointFreezeAsync() {
    QVariantMap started;
    DebugFeatureManager* dfm = m_debugFeatureManager.get();
    const QPointer<ApplicationController> self(this);
    std::thread([self, dfm]() {
        const QVariantMap result = dfm->stopInProcessBreakpointFreeze();
        if (!self) return;
        QMetaObject::invokeMethod(self.data(), [self, result]() {
            if (!self) return;
            emit self->inProcessBreakpointFreezeStopFinished(result);
        }, Qt::QueuedConnection);
    }).detach();

    started["success"] = true;
    started["started"] = true;
    return started;
}

QVariantMap ApplicationController::getInProcessBreakpointFreezeStats() const {
    return m_debugFeatureManager->getInProcessBreakpointFreezeStats();
}

QVariantMap ApplicationController::startSpeedhackAsync(double factor) {
    QVariantMap started;
    // SpeedhackSession::start() poll jusqu'à 2s en attendant la confirmation
    // du handler — même raisonnement que startInProcessBreakpointFreezeAsync.
    DebugFeatureManager* dfm = m_debugFeatureManager.get();
    const QPointer<ApplicationController> self(this);
    std::thread([self, dfm, factor]() {
        const QVariantMap result = dfm->startSpeedhack(factor);
        if (!self) return;
        QMetaObject::invokeMethod(self.data(), [self, result]() {
            if (!self) return;
            emit self->speedhackStartFinished(result);
        }, Qt::QueuedConnection);
    }).detach();

    started["success"] = true;
    started["started"] = true;
    return started;
}

QVariantMap ApplicationController::setSpeedhackFactor(double factor) {
    return m_debugFeatureManager->setSpeedhackFactor(factor);
}

QVariantMap ApplicationController::stopSpeedhack() {
    return m_debugFeatureManager->stopSpeedhack();
}

QVariantMap ApplicationController::startApiHookAsync(const QString& moduleName, const QString& functionName,
                                                int mode, qlonglong forcedReturnValue) {
    QVariantMap started;
    // ApiHookSession::start() poll jusqu'à 6s en attendant la confirmation du
    // handler — même raisonnement que startInProcessBreakpointFreezeAsync.
    DebugFeatureManager* dfm = m_debugFeatureManager.get();
    const QPointer<ApplicationController> self(this);
    std::thread([self, dfm, moduleName, functionName, mode, forcedReturnValue]() {
        const QVariantMap result = dfm->startApiHook(moduleName, functionName, mode, forcedReturnValue);
        if (!self) return;
        QMetaObject::invokeMethod(self.data(), [self, result]() {
            if (!self) return;
            emit self->apiHookStartFinished(result);
        }, Qt::QueuedConnection);
    }).detach();

    started["success"] = true;
    started["started"] = true;
    return started;
}

QVariantMap ApplicationController::stopApiHookAsync() {
    QVariantMap started;
    // ApiHookSession::stop() poll jusqu'à 3s en attendant la confirmation du
    // handler — même raisonnement.
    DebugFeatureManager* dfm = m_debugFeatureManager.get();
    const QPointer<ApplicationController> self(this);
    std::thread([self, dfm]() {
        const QVariantMap result = dfm->stopApiHook();
        if (!self) return;
        QMetaObject::invokeMethod(self.data(), [self, result]() {
            if (!self) return;
            emit self->apiHookStopFinished(result);
        }, Qt::QueuedConnection);
    }).detach();

    started["success"] = true;
    started["started"] = true;
    return started;
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
        result["error"] = KE_TXT("Aucun processus attaché.", "No process attached.");
        return result;
    }

    uint64_t address = 0;
    if (!parseHexAddress(addressHex, &address)) {
        result["error"] = KE_TXT("Adresse invalide.", "Invalid address.");
        return result;
    }

    // Parser la chaine hex : accepte "48 8B 00", "488B00", "48 8b 00"
    QString cleaned = hexString.simplified().remove(' ').remove('\t').remove('\n').remove('\r').remove(',');
    if (cleaned.size() % 2 != 0) {
        result["error"] = KE_TXT("Chaine hexadecimale invalide : nombre impair de caracteres.", "Invalid hexadecimal string: odd number of characters.");
        return result;
    }
    if (cleaned.isEmpty()) {
        result["error"] = KE_TXT("Chaine hexadecimale vide.", "Empty hexadecimal string.");
        return result;
    }
    if (cleaned.size() > 4096) {
        result["error"] = KE_TXT("Chaine hexadecimale trop longue (max 2048 octets).", "Hexadecimal string too long (max 2048 bytes).");
        return result;
    }
    const QByteArray bytes = QByteArray::fromHex(cleaned.toLatin1());
    if (bytes.isEmpty()) {
        result["error"] = KE_TXT("Chaine hexadecimale invalide.", "Invalid hexadecimal string.");
        return result;
    }

    killcore::ProcessHandle writeHandle(static_cast<uint32_t>(m_pid), killcore::ProcessAccess::ReadWrite);
    if (!writeHandle.isValid()) {
        result["error"] = KE_TXT("Impossible d'ouvrir le processus en écriture.", "Unable to open the process for writing.");
        return result;
    }

    killcore::MemoryWriter writer(writeHandle);
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
        watchSmartWriteIfPossible(address, bytes, writeResult.previousValue);
        KE_LOG_INFO() << "writeMemoryHex: " << writeResult.bytesWritten << " octets ecrits a 0x" << std::hex << address;
    }
    return result;
}

QVariantMap ApplicationController::dumpMemoryRegion(const QString& addressHex, int size, const QString& fileName) {
    QVariantMap result;
    result["success"] = false;

    if (!m_handle.isValid()) {
        result["error"] = KE_TXT("Aucun processus attaché.", "No process attached.");
        return result;
    }

    uint64_t address = 0;
    if (!parseHexAddress(addressHex, &address)) {
        result["error"] = KE_TXT("Adresse invalide.", "Invalid address.");
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

QVariantMap ApplicationController::captureProfilerCheckpoint(const QString& label, const QVariantMap& options) {
    return m_externalToolProfiler->captureProfilerCheckpoint(label, options);
}

QVariantMap ApplicationController::getProfilerDiff(const QString& labelA, const QString& labelB, const QVariantMap& options) const {
    return m_externalToolProfiler->getProfilerDiff(labelA, labelB, options);
}

QVariantMap ApplicationController::listProfilerCheckpoints() const {
    return m_externalToolProfiler->listProfilerCheckpoints();
}

QVariantMap ApplicationController::clearProfilerSession() {
    return m_externalToolProfiler->clearProfilerSession();
}

QVariantMap ApplicationController::recordProfilerTimelineStep(const QString& stepName, const QVariantMap& options) {
    return m_externalToolProfiler->recordProfilerTimelineStep(stepName, options);
}

QVariantMap ApplicationController::getProfilerTimelineSummary() const {
    return m_externalToolProfiler->getProfilerTimelineSummary();
}

QVariantMap ApplicationController::clearProfilerTimeline() {
    return m_externalToolProfiler->clearProfilerTimeline();
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
        result["error"] = KE_TXT("Un test de champs candidats est déjà en cours.", "A candidate field test is already running.");
        return result;
    }
    if (!m_attached || !m_handle.isValid() || m_pid <= 0) {
        result["error"] = KE_TXT("Aucun processus attaché.", "No process attached.");
        return result;
    }

    uint64_t writeInstructionAddress = 0;
    if (!parseHexAddress(writeInstructionAddressHex, &writeInstructionAddress)) {
        result["error"] = KE_TXT("Adresse d'instruction invalide.", "Invalid instruction address.");
        return result;
    }
    uint64_t knownWriteTargetAddress = 0;
    if (!parseHexAddress(knownWriteTargetAddressHex, &knownWriteTargetAddress)) {
        result["error"] = KE_TXT("Adresse écrite connue invalide.", "Invalid known written address.");
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
        result["error"] = windowRead.errorMessage.isEmpty() ? KE_TXT("Lecture mémoire impossible.", "Unable to read memory.") : windowRead.errorMessage;
        return result;
    }
    if (targetOffsetInWindow > windowRead.data.size()) {
        result["error"] = KE_TXT("Lecture mémoire trop courte pour atteindre l'adresse cible.", "Memory read too short to reach the target address.");
        return result;
    }

    const auto backward = killcore::disassembleBackwardWindow(windowRead.data, targetOffsetInWindow);
    if (!backward.success) {
        result["error"] = backward.error;
        return result;
    }

    auto resolved = killcore::resolveCandidateFieldAddresses(backward.instructions, knownWriteTargetAddress);
    if (resolved.isEmpty()) {
        result["error"] = KE_TXT("Aucun champ candidat résolvable (registre de base différent de celui de l'instruction "
                                  "d'écriture, ou aucun champ mémoire simple en amont).",
                                  "No resolvable candidate field (base register differs from the write "
                                  "instruction's, or no simple memory field upstream).");
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
                outcome["error"] = KE_TXT("Impossible d'ouvrir le processus en écriture.", "Unable to open the process for writing.");
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
                    outcome["error"] = KE_TXT("Lecture de la valeur d'origine impossible.", "Unable to read the original value.");
                    outcomes.append(outcome);
                    continue;
                }

                const double originalValue = bytesToDouble(originalRead.data, candidate.inferredType);
                const QByteArray testBytes = doubleToBytes(originalValue + kCandidateProbeDelta, candidate.inferredType);

                const auto probeWrite = writer.write(candidate.address, testBytes, true);
                if (!probeWrite.success || !probeWrite.verified) {
                    outcome["verdict"] = "error";
                    outcome["error"] = probeWrite.errorMessage.isEmpty() ? KE_TXT("Écriture test impossible.", "Unable to write test value.") : probeWrite.errorMessage;
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
                    outcome["error"] = KE_TXT("Valeur test écrite mais restauration échouée — vérifie manuellement cette adresse.",
                                               "Test value written but restore failed — check this address manually.");
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
    result["warning"] = KE_TXT(
        "Écrit une valeur test transitoire sur chaque champ candidat (jusqu'à %1s par champ), "
        "puis restaure systématiquement la valeur d'origine.",
        "Writes a transient test value to each candidate field (up to %1s per field), "
        "then always restores the original value.")
        .arg(kCandidateTestTicks * kCandidateTestIntervalMs / 1000);
    return result;
}

QVariantMap ApplicationController::cancelCandidateFieldTest() {
    QVariantMap result;
    result["success"] = false;
    if (!m_candidateFieldTestInProgress || !m_activeCandidateFieldTestCancellation) {
        result["error"] = KE_TXT("Aucun test de champs candidats actif à annuler.", "No active candidate field test to cancel.");
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
        result["error"] = KE_TXT("Aucun processus attaché.", "No process attached.");
        return result;
    }
    if (dllPath.trimmed().isEmpty()) {
        result["error"] = KE_TXT("Chemin DLL vide.", "Empty DLL path.");
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
        result["error"] = KE_TXT("Aucun processus attaché.", "No process attached.");
        return result;
    }
    if (memBaseRegister.trimmed().isEmpty()) {
        result["error"] = KE_TXT("Cette instruction n'a pas de destination mémoire exploitable (adressage indexé ou RIP-relatif, non supporté).",
                                  "This instruction has no usable memory destination (indexed or RIP-relative addressing, not supported).");
        return result;
    }
    if (instructionLength < 5) {
        result["error"] = KE_TXT("Instruction trop courte (%1 octet(s)) pour y poser un saut de redirection (5 minimum).",
                                  "Instruction too short (%1 byte(s)) to place a redirect jump (5 minimum).").arg(instructionLength);
        return result;
    }

    uint64_t rip = 0;
    if (!parseHexAddress(ripHex, &rip)) {
        result["error"] = KE_TXT("Adresse invalide.", "Invalid address.");
        return result;
    }

    killcore::ValueType type;
    if (!killcore::parseValueType(valueType, &type)) {
        result["error"] = KE_TXT("Type invalide.", "Invalid type.");
        return result;
    }
    // encodeMemImmMov (core/scripting/auto_assembler.cpp) n'ecrit qu'un
    // immediat 32 bits (dword) : memes limites que "Forcer une valeur".
    if (type == killcore::ValueType::Int64 || type == killcore::ValueType::UInt64
        || type == killcore::ValueType::Float32 || type == killcore::ValueType::Float64) {
        result["error"] = KE_TXT("Seuls les types entiers jusqu'à 32 bits sont supportés pour forcer une valeur ici.",
                                  "Only integer types up to 32 bits are supported to force a value here.");
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
        result["error"] = KE_TXT("Impossible de déterminer le module contenant cette adresse (mémoire allouée dynamiquement, hors d'un module chargé ?).",
                                  "Unable to determine the module containing this address (dynamically allocated memory, outside a loaded module?).");
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
    actionResult["error"] = allWritesOk ? QString() : KE_TXT("Au moins une réécriture a échoué.", "At least one write failed.");

    result["actionResult"] = actionResult;
    result["suggestedWrites"] = suggestions;
    result["suggestedWrite"] = suggestions.isEmpty() ? QVariantMap{} : suggestions.first().toMap();
    result["autoWriteResults"] = writeResults;
    result["autoWriteResult"] = writeResults.isEmpty() ? QVariantMap{} : writeResults.last().toMap();
    result["autoWriteCount"] = writeResults.size();
    result["activeTargetCount"] = writeState.chatTargetCount();
    result["previousTargetValue"] = previousTargetValue;
    result["writeHistory"] = writeHistoryToVariantList(m_autoWriteValueHistory);
    result["rollbackNote"] = KE_TXT("Tu peux annuler cette réécriture via le bouton rollback batch dans l'assistant.",
                                     "You can undo this rewrite via the batch rollback button in the assistant.");
    result["message"] = allWritesOk
        ? KE_TXT("J'ai repris les %1 dernière(s) adresse(s) auto-écrite(s) et j'ai mis %2 dessus. Je garde ces adresses actives pour les prochaines modifications.",
                 "I reused the last %1 auto-written address(es) and set %2 on them. I'm keeping these addresses active for the next changes.")
              .arg(writeState.lastTargetCount())
              .arg(value)
        : KE_TXT("J'ai repris les dernières adresses auto-écrites, mais au moins une réécriture vers %1 a échoué.",
                 "I reused the last auto-written addresses, but at least one write to %1 failed.")
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

QVariantMap ApplicationController::requestWindowsDefenderExclusionAsync() {
    QVariantMap started;
    started["success"] = false;
    started["cancelled"] = false;

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

    // L'élévation UAC + son attente (jusqu'à 15s) tournent sur un thread
    // séparé pour ne pas geler le thread GUI (Q_INVOKABLE via QWebChannel),
    // même raisonnement que spoofDnsAsync.
    const QPointer<ApplicationController> self(this);
    std::thread([self, psCommand, installDir, exeName]() {
        QVariantMap result;
        result["success"] = false;
        result["cancelled"] = false;

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
                result["error"] = KE_TXT("Invite d'élévation refusée par l'utilisateur.", "The user declined the elevation prompt.");
            } else {
                result["error"] = KE_TXT("Impossible de lancer PowerShell élevé (error=%1).", "Unable to launch elevated PowerShell (error=%1).").arg(err);
            }
            KE_LOG_WARN() << "requestWindowsDefenderExclusionAsync: ShellExecuteExW failed, error=" << err;
        } else if (sei.hProcess) {
            WaitForSingleObject(sei.hProcess, 15000);
            DWORD exitCode = 1;
            GetExitCodeProcess(sei.hProcess, &exitCode);
            CloseHandle(sei.hProcess);
            result["success"] = (exitCode == 0);
            if (exitCode != 0) {
                result["error"] = KE_TXT(
                    "Add-MpPreference a échoué (code %1) — l'exclusion est peut-être gérée de façon centralisée "
                    "par une politique d'entreprise (Tamper Protection) et ne peut pas être modifiée localement.",
                    "Add-MpPreference failed (code %1) — the exclusion may be centrally managed "
                    "by an enterprise policy (Tamper Protection) and cannot be changed locally.")
                    .arg(exitCode);
            }
        } else {
            // Pas de handle de process a attendre -- best-effort, on suppose que
            // l'invite s'est affichee correctement.
            result["success"] = true;
        }

        KE_LOG_INFO() << "requestWindowsDefenderExclusionAsync: success=" << result.value("success").toBool()
                      << " path=" << installDir.toStdString() << " process=" << exeName.toStdString();

        if (!self) return;
        QMetaObject::invokeMethod(self.data(), [self, result]() {
            if (!self) return;
            emit self->windowsDefenderExclusionRequestFinished(result);
        }, Qt::QueuedConnection);
    }).detach();

    started["success"] = true;
    started["started"] = true;
#else
    started["error"] = KE_TXT("Fonctionnalité Windows uniquement.", "Windows-only feature.");
#endif

    return started;
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

QVariantMap ApplicationController::blockProcessNetworkAsync() {
    QVariantMap started;
    started["success"] = false;
    started["cancelled"] = false;

    if (!m_attached || m_pid <= 0) {
        started["error"] = KE_TXT("Aucun processus attaché.", "No process attached.");
        return started;
    }

#ifdef Q_OS_WIN
    const QString exePath = QDir::toNativeSeparators(m_handle.executablePath());
    if (exePath.isEmpty()) {
        started["error"] = KE_TXT("Chemin de l'exécutable introuvable pour le processus attaché.", "Executable path not found for the attached process.");
        return started;
    }
    const QString ruleToken = sanitizeFirewallRuleToken(QFileInfo(exePath).fileName());
    const QString ruleOut = firewallRuleNameOut(ruleToken);
    const QString ruleIn = firewallRuleNameIn(ruleToken);

    // Guillemets simples PowerShell pour le chemin — meme convention que
    // requestWindowsDefenderExclusionAsync() ci-dessus (un chemin contenant
    // une apostrophe casserait cette commande, cas limite non gere ici).
    const QString psCommand = QStringLiteral(
        "New-NetFirewallRule -DisplayName '%1' -Direction Outbound -Program '%2' -Action Block -Profile Any -ErrorAction SilentlyContinue | Out-Null; "
        "New-NetFirewallRule -DisplayName '%3' -Direction Inbound -Program '%2' -Action Block -Profile Any -ErrorAction SilentlyContinue | Out-Null")
        .arg(ruleOut, exePath, ruleIn);

    // Même raisonnement que requestWindowsDefenderExclusionAsync : thread
    // séparé pour ne pas geler le thread GUI pendant l'invite UAC. Les
    // membres m_networkBlockRuleToken/m_networkBlockExePath ne sont écrits
    // que dans le callback marshalé sur le thread GUI ci-dessous, jamais
    // depuis le thread de travail.
    const QPointer<ApplicationController> self(this);
    std::thread([self, psCommand, ruleToken, ruleOut, ruleIn, exePath]() {
        QVariantMap result;
        result["success"] = false;
        result["cancelled"] = false;

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
                result["error"] = KE_TXT("Invite d'élévation refusée par l'utilisateur.", "The user declined the elevation prompt.");
            } else {
                result["error"] = KE_TXT("Impossible de lancer PowerShell élevé (error=%1).", "Unable to launch elevated PowerShell (error=%1).").arg(err);
            }
            KE_LOG_WARN() << "blockProcessNetworkAsync: ShellExecuteExW failed, error=" << err;
        } else if (sei.hProcess) {
            WaitForSingleObject(sei.hProcess, 15000);
            DWORD exitCode = 1;
            GetExitCodeProcess(sei.hProcess, &exitCode);
            CloseHandle(sei.hProcess);
            result["success"] = (exitCode == 0);
            if (exitCode != 0) {
                result["error"] = KE_TXT("New-NetFirewallRule a échoué (code %1).", "New-NetFirewallRule failed (code %1).").arg(exitCode);
            }
        } else {
            // Pas de handle de process a attendre -- best-effort, meme logique
            // que requestWindowsDefenderExclusionAsync().
            result["success"] = true;
        }

        if (result.value("success").toBool()) {
            result["ruleOutbound"] = ruleOut;
            result["ruleInbound"] = ruleIn;
            result["exePath"] = exePath;
        }

        KE_LOG_INFO() << "blockProcessNetworkAsync: success=" << result.value("success").toBool()
                      << " exe=" << exePath.toStdString();

        if (!self) return;
        QMetaObject::invokeMethod(self.data(), [self, result, ruleToken, exePath]() {
            if (!self) return;
            if (result.value("success").toBool()) {
                self->m_networkBlockRuleToken = ruleToken;
                self->m_networkBlockExePath = exePath;
            }
            emit self->processNetworkBlockFinished(result);
        }, Qt::QueuedConnection);
    }).detach();

    started["success"] = true;
    started["started"] = true;
#else
    started["error"] = KE_TXT("Fonctionnalité Windows uniquement.", "Windows-only feature.");
#endif

    return started;
}

QVariantMap ApplicationController::unblockProcessNetworkAsync() {
    QVariantMap started;
    started["success"] = false;
    started["cancelled"] = false;

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
        started["error"] = KE_TXT("Aucune règle de blocage réseau KillEngine connue à retirer.", "No known KillEngine network block rule to remove.");
        return started;
    }

    const QString ruleOut = firewallRuleNameOut(ruleToken);
    const QString ruleIn = firewallRuleNameIn(ruleToken);
    const QString psCommand = QStringLiteral(
        "Remove-NetFirewallRule -DisplayName '%1' -ErrorAction SilentlyContinue; "
        "Remove-NetFirewallRule -DisplayName '%2' -ErrorAction SilentlyContinue")
        .arg(ruleOut, ruleIn);

    // Même raisonnement que blockProcessNetworkAsync : thread séparé, et les
    // membres m_networkBlockRuleToken/m_networkBlockExePath ne sont vidés que
    // dans le callback marshalé sur le thread GUI.
    const QPointer<ApplicationController> self(this);
    std::thread([self, psCommand]() {
        QVariantMap result;
        result["success"] = false;
        result["cancelled"] = false;

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
                result["error"] = KE_TXT("Invite d'élévation refusée par l'utilisateur.", "The user declined the elevation prompt.");
            } else {
                result["error"] = KE_TXT("Impossible de lancer PowerShell élevé (error=%1).", "Unable to launch elevated PowerShell (error=%1).").arg(err);
            }
            KE_LOG_WARN() << "unblockProcessNetworkAsync: ShellExecuteExW failed, error=" << err;
        } else if (sei.hProcess) {
            WaitForSingleObject(sei.hProcess, 15000);
            DWORD exitCode = 1;
            GetExitCodeProcess(sei.hProcess, &exitCode);
            CloseHandle(sei.hProcess);
            result["success"] = (exitCode == 0);
            if (exitCode != 0) {
                result["error"] = KE_TXT("Remove-NetFirewallRule a échoué (code %1).", "Remove-NetFirewallRule failed (code %1).").arg(exitCode);
            }
        } else {
            result["success"] = true;
        }

        KE_LOG_INFO() << "unblockProcessNetworkAsync: success=" << result.value("success").toBool();

        if (!self) return;
        QMetaObject::invokeMethod(self.data(), [self, result]() {
            if (!self) return;
            if (result.value("success").toBool()) {
                self->m_networkBlockRuleToken.clear();
                self->m_networkBlockExePath.clear();
            }
            emit self->processNetworkUnblockFinished(result);
        }, Qt::QueuedConnection);
    }).detach();

    started["success"] = true;
    started["started"] = true;
#else
    started["error"] = KE_TXT("Fonctionnalité Windows uniquement.", "Windows-only feature.");
#endif

    return started;
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
        result["error"] = KE_TXT("Impossible d'interroger le pare-feu (timeout).", "Unable to query the firewall (timeout).");
        check.kill();
    }
#endif

    return result;
}

// ---------------------------------------------------------------------------
// Réseau — Connexions actives + modules DLL (lecture seule)
// ---------------------------------------------------------------------------

namespace {
// Cache DNS LRU simple (max 256 entrées, TTL 60s)
struct DnsCacheEntry {
    QString hostname;
    qint64 timestamp; // ms since epoch
};
using DnsCacheMap = QHash<QString, DnsCacheEntry>;
Q_GLOBAL_STATIC(DnsCacheMap, g_dnsCache)
// getProcessNetworkConnectionsAsync() tourne sur un thread de travail séparé
// (voir plus bas) ; en mode Live (rafraîchi toutes les 2s côté frontend), un
// nouveau cycle peut démarrer avant que le précédent n'ait fini de résoudre
// toutes ses IP si la résolution prend plus longtemps que l'intervalle —
// deux threads pourraient alors toucher g_dnsCache en même temps. QHash
// n'est pas thread-safe pour un accès concurrent, d'où ce mutex.
Q_GLOBAL_STATIC(QMutex, g_dnsCacheMutex)

QString resolveIpToHostname(const QString& ip) {
    if (ip.isEmpty()) return QString();

    // Loopback / privé — pas de résolution
    if (ip.startsWith("127.") || ip == "::1" || ip.startsWith("10.") ||
        ip.startsWith("192.168.") || ip.startsWith("169.254.") ||
        ip.startsWith("fe80:") || ip.startsWith("fc00:") || ip.startsWith("fd00:")) {
        return QString();
    }

    // Check cache
    {
        QMutexLocker locker(g_dnsCacheMutex());
        auto& cache = *g_dnsCache();
        if (cache.contains(ip)) {
            const auto& entry = cache[ip];
            if (QDateTime::currentMSecsSinceEpoch() - entry.timestamp < 60000) {
                return entry.hostname; // "" si déjà résolu en échec
            }
            cache.remove(ip);
        }
    }

    // Résolution asynchrone avec timeout
    auto future = std::async(std::launch::async, [ipStr = ip.toStdString()]() -> QString {
        sockaddr_storage addr{};
        sockaddr_in* sin4 = reinterpret_cast<sockaddr_in*>(&addr);
        sockaddr_in6* sin6 = reinterpret_cast<sockaddr_in6*>(&addr);
        socklen_t addrLen = 0;

        if (inet_pton(AF_INET, ipStr.c_str(), &sin4->sin_addr) == 1) {
            sin4->sin_family = AF_INET;
            addrLen = sizeof(sockaddr_in);
        } else if (inet_pton(AF_INET6, ipStr.c_str(), &sin6->sin6_addr) == 1) {
            sin6->sin6_family = AF_INET6;
            addrLen = sizeof(sockaddr_in6);
        } else {
            return QString(); // IP invalide
        }

        char host[NI_MAXHOST] = {0};
        if (getnameinfo(reinterpret_cast<sockaddr*>(&addr), addrLen, host, sizeof(host),
                        nullptr, 0, NI_NAMEREQD) == 0) {
            return QString::fromUtf8(host);
        }
        return QString(); // Échec
    });

    if (future.wait_for(std::chrono::milliseconds(500)) == std::future_status::ready) {
        QString result = future.get();
        QMutexLocker locker(g_dnsCacheMutex());
        auto& cache = *g_dnsCache();
        cache.insert(ip, {result, QDateTime::currentMSecsSinceEpoch()});
        // Nettoyer si > 256
        if (cache.size() > 256) {
            // Supprimer les plus anciennes
            qint64 oldest = QDateTime::currentMSecsSinceEpoch();
            QString oldestKey;
            for (auto it = cache.begin(); it != cache.end(); ++it) {
                if (it.value().timestamp < oldest) {
                    oldest = it.value().timestamp;
                    oldestKey = it.key();
                }
            }
            if (!oldestKey.isEmpty()) cache.remove(oldestKey);
        }
        return result;
    }
    return QString(); // Timeout
}

QString tcpStateToString(DWORD state) {
    switch (state) {
        case MIB_TCP_STATE_CLOSED:    return "CLOSED";
        case MIB_TCP_STATE_LISTEN:    return "LISTEN";
        case MIB_TCP_STATE_SYN_SENT:  return "SYN_SENT";
        case MIB_TCP_STATE_SYN_RCVD:  return "SYN_RECEIVED";
        case MIB_TCP_STATE_ESTAB:     return "ESTABLISHED";
        case MIB_TCP_STATE_FIN_WAIT1: return "FIN_WAIT_1";
        case MIB_TCP_STATE_FIN_WAIT2: return "FIN_WAIT_2";
        case MIB_TCP_STATE_CLOSE_WAIT: return "CLOSE_WAIT";
        case MIB_TCP_STATE_CLOSING:   return "CLOSING";
        case MIB_TCP_STATE_LAST_ACK:  return "LAST_ACK";
        case MIB_TCP_STATE_TIME_WAIT: return "TIME_WAIT";
        case MIB_TCP_STATE_DELETE_TCB: return "DELETE_TCB";
        default:                      return "UNKNOWN";
    }
}

QString formatIpPort(uint32_t ip, uint16_t port) {
    char buf[64];
    in_addr addr{};
    addr.S_un.S_addr = ip;
    const char* ipStr = inet_ntop(AF_INET, &addr, buf, sizeof(buf));
    if (ipStr) {
        return QStringLiteral("%1:%2").arg(QString::fromUtf8(ipStr)).arg(port);
    }
    return QStringLiteral("?.?:%1").arg(port);
}

// Liste des DLL réseau connues
struct NetworkDllInfo {
    const char* namePattern; // nom exact ou préfixe (se termine par *)
    const char* category;
    const char* description;
    bool isPrefix;
};

const NetworkDllInfo kNetworkDlls[] = {
    {"ws2_32.dll",     "winsock", "Windows Socket 2 API", false},
    {"winhttp.dll",    "http",    "Windows HTTP client", false},
    {"wininet.dll",    "http",    "Windows HTTP/FTP client (legacy)", false},
    {"urlmon.dll",     "http",    "URL Moniker (IE/legacy)", false},
    {"mswsock.dll",    "winsock", "Microsoft Windows Socket Helper", false},
    {"dnsapi.dll",     "dns",     "DNS Resolver API", false},
    {"iphlpapi.dll",   "system",  "IP Helper API", false},
    {"curl.dll",       "http",    "cURL library", false},
    {"libcurl.dll",    "http",    "cURL library (alternate name)", false},
    {"openssl.dll",    "crypto",  "OpenSSL", false},
    {"schannel.dll",   "crypto",  "Windows TLS/SSL", false},
    {"httpapi.dll",    "http",    "HTTP Server API", false},
    {"webio.dll",      "http",    "WebIO (WinHTTP internal)", false},
    {"libssl",         "crypto",  "OpenSSL SSL library", true},
    {"libcrypto",      "crypto",  "OpenSSL Crypto library", true},
};

bool isNetworkDll(const QString& dllName) {
    QString lower = dllName.toLower();
    for (const auto& info : kNetworkDlls) {
        if (info.isPrefix) {
            if (lower.startsWith(QString::fromUtf8(info.namePattern))) return true;
        } else {
            if (lower == QString::fromUtf8(info.namePattern)) return true;
        }
    }
    return false;
}

const NetworkDllInfo* findNetworkDllInfo(const QString& dllName) {
    QString lower = dllName.toLower();
    for (const auto& info : kNetworkDlls) {
        if (info.isPrefix) {
            if (lower.startsWith(QString::fromUtf8(info.namePattern))) return &info;
        } else {
            if (lower == QString::fromUtf8(info.namePattern)) return &info;
        }
    }
    return nullptr;
}
} // namespace

QVariantMap ApplicationController::getProcessNetworkConnectionsAsync() {
    QVariantMap started;
    started["success"] = false;

    if (!m_attached || m_pid <= 0) {
        started["error"] = KE_TXT("Aucun processus attaché.", "No process attached.");
        return started;
    }

#ifdef Q_OS_WIN
    // La résolution DNS inverse par connexion (jusqu'à 500ms chacune, voir
    // resolveIpToHostname) peut totaliser plusieurs secondes pour un process
    // avec de nombreuses IP distantes distinctes non cachées — déportée sur
    // un thread séparé pour ne pas geler le thread GUI (Q_INVOKABLE via
    // QWebChannel), y compris en mode "Live" (rafraîchi toutes les 2s).
    const int pid = m_pid;
    const QPointer<ApplicationController> self(this);
    std::thread([self, pid]() {
        QVariantMap result;
        result["success"] = false;
        result["connections"] = QVariantList();

        QVariantList connections;

        // TCP connections
        {
            ULONG size = 0;
            if (GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0) == ERROR_INSUFFICIENT_BUFFER && size > 0) {
                QByteArray buffer(static_cast<int>(size), 0);
                auto* table = reinterpret_cast<MIB_TCPTABLE_OWNER_PID*>(buffer.data());
                if (GetExtendedTcpTable(table, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0) == NO_ERROR) {
                    for (DWORD i = 0; i < table->dwNumEntries; ++i) {
                        const auto& row = table->table[i];
                        if (static_cast<int>(row.dwOwningPid) != pid) continue;

                        uint16_t localPort = ntohs(static_cast<uint16_t>(row.dwLocalPort));
                        uint16_t remotePort = ntohs(static_cast<uint16_t>(row.dwRemotePort));
                        QString localAddr = formatIpPort(row.dwLocalAddr, localPort);
                        QString remoteAddr = formatIpPort(row.dwRemoteAddr, remotePort);
                        QString state = tcpStateToString(row.dwState);

                        QVariantMap conn;
                        conn["protocol"] = "TCP";
                        conn["localAddr"] = localAddr;
                        conn["remoteAddr"] = remoteAddr;
                        conn["state"] = state;
                        conn["pid"] = static_cast<int>(row.dwOwningPid);

                        // Résolution DNS (best-effort)
                        uint32_t remote = ntohl(row.dwRemoteAddr);
                        if (remote != 0 && ((remote >> 24) != 127)) {
                            QString ipOnly = remoteAddr.split(':').first();
                            QString hostname = resolveIpToHostname(ipOnly);
                            conn["remoteHost"] = hostname.isEmpty() ? QVariant() : hostname;
                        } else {
                            conn["remoteHost"] = QVariant();
                        }

                        connections.append(conn);
                    }
                }
            }
        }

        // UDP connections
        {
            ULONG size = 0;
            if (GetExtendedUdpTable(nullptr, &size, FALSE, AF_INET, UDP_TABLE_OWNER_PID, 0) == ERROR_INSUFFICIENT_BUFFER && size > 0) {
                QByteArray buffer(static_cast<int>(size), 0);
                auto* table = reinterpret_cast<MIB_UDPTABLE_OWNER_PID*>(buffer.data());
                if (GetExtendedUdpTable(table, &size, FALSE, AF_INET, UDP_TABLE_OWNER_PID, 0) == NO_ERROR) {
                    for (DWORD i = 0; i < table->dwNumEntries; ++i) {
                        const auto& row = table->table[i];
                        if (static_cast<int>(row.dwOwningPid) != pid) continue;

                        uint16_t localPort = ntohs(static_cast<uint16_t>(row.dwLocalPort));
                        QString localAddr = formatIpPort(row.dwLocalAddr, localPort);

                        QVariantMap conn;
                        conn["protocol"] = "UDP";
                        conn["localAddr"] = localAddr;
                        conn["remoteAddr"] = QVariant(); // UDP est sans connexion : pas de pair distant dans cette table
                        conn["state"] = QVariant(); // UDP n'a pas d'état
                        conn["pid"] = static_cast<int>(row.dwOwningPid);
                        conn["remoteHost"] = QVariant();

                        connections.append(conn);
                    }
                }
            }
        }

        result["success"] = true;
        result["connections"] = connections;

        if (!self) return;
        QMetaObject::invokeMethod(self.data(), [self, result]() {
            if (!self) return;
            emit self->processNetworkConnectionsFinished(result);
        }, Qt::QueuedConnection);
    }).detach();

    started["success"] = true;
    started["started"] = true;
#else
    started["error"] = KE_TXT("Fonctionnalité Windows uniquement.", "Windows-only feature.");
#endif

    return started;
}

QVariantMap ApplicationController::getProcessNetworkModules() {
    QVariantMap result;
    result["success"] = false;
    result["modules"] = QVariantList();

    if (!m_attached || m_pid <= 0) {
        result["error"] = KE_TXT("Aucun processus attaché.", "No process attached.");
        return result;
    }

#ifdef Q_OS_WIN
    HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, static_cast<DWORD>(m_pid));
    if (!hProcess) {
        result["error"] = KE_TXT("Impossible d'ouvrir le processus.", "Unable to open the process.");
        return result;
    }

    HMODULE modules[1024];
    DWORD cbNeeded = 0;
    if (!EnumProcessModules(hProcess, modules, sizeof(modules), &cbNeeded)) {
        CloseHandle(hProcess);
        result["error"] = KE_TXT("EnumProcessModules échoué.", "EnumProcessModules failed.");
        return result;
    }

    QVariantList dllList;
    DWORD moduleCount = cbNeeded / sizeof(HMODULE);
    for (DWORD i = 0; i < moduleCount; ++i) {
        wchar_t path[MAX_PATH] = {0};
        if (GetModuleFileNameExW(hProcess, modules[i], path, MAX_PATH) > 0) {
            QString dllPath = QString::fromWCharArray(path);
            QString dllName = QFileInfo(dllPath).fileName();
            if (isNetworkDll(dllName)) {
                const auto* info = findNetworkDllInfo(dllName);
                QVariantMap mod;
                mod["name"] = dllName;
                mod["path"] = dllPath;
                mod["category"] = info ? QString::fromUtf8(info->category) : "system";
                mod["description"] = info ? QString::fromUtf8(info->description) : dllName;
                dllList.append(mod);
            }
        }
    }
    CloseHandle(hProcess);

    // Trier par catégorie puis nom
    std::sort(dllList.begin(), dllList.end(), [](const QVariant& a, const QVariant& b) {
        auto catA = a.toMap()["category"].toString();
        auto catB = b.toMap()["category"].toString();
        if (catA != catB) return catA < catB;
        return a.toMap()["name"].toString() < b.toMap()["name"].toString();
    });

    result["success"] = true;
    result["modules"] = dllList;
#else
    result["error"] = KE_TXT("Fonctionnalité Windows uniquement.", "Windows-only feature.");
#endif

    return result;
}

// ---------------------------------------------------------------------------
// Proxy HTTP — injection DLL + MinHook sur HttpSendRequest/WinHttpSendRequest
// ---------------------------------------------------------------------------

QVariantMap ApplicationController::startHttpProxyAsync(int port, bool interceptHttps) {
    QVariantMap started;
    started["success"] = false;
    if (!m_attached || m_pid <= 0) {
        started["error"] = KE_TXT("Aucun processus attaché.", "No process attached.");
        return started;
    }

#ifdef Q_OS_WIN
    if (!m_httpProxySession) {
        m_httpProxySession = std::make_unique<killcore::HttpProxySession>();
    }

    if (m_httpProxySession->isActive()) {
        started["error"] = KE_TXT("Un proxy HTTP est déjà actif.", "An HTTP proxy is already active.");
        return started;
    }

    // Trouver le handler DLL
    const QString handlerPath = QCoreApplication::applicationDirPath()
        + QStringLiteral("/KillEngineHttpProxyHandler.dll");
    if (!QFile::exists(handlerPath)) {
        started["error"] = KE_TXT("Handler proxy HTTP introuvable: ", "HTTP proxy handler not found: ") + handlerPath;
        return started;
    }

    // HttpProxySession::start() pose l'injection puis poll jusqu'à 6s en
    // attendant la confirmation du handler — tourne sur un thread séparé
    // pour ne pas geler le thread GUI (Q_INVOKABLE via QWebChannel). Le
    // thread rouvre son propre ProcessHandle (même convention que
    // testCandidateFieldsAsync) : m_handle appartient au thread GUI et
    // pourrait être fermé par un detachProcess() concurrent, alors qu'un
    // handle indépendant reste valable pour toute la durée de l'opération.
    // m_httpProxySession n'est jamais recréé une fois construit (voir le
    // reste de ce fichier), le pointeur brut reste donc valide.
    killcore::HttpProxySession* session = m_httpProxySession.get();
    const int pid = m_pid;
    const QPointer<ApplicationController> self(this);
    std::thread([self, session, pid, port, interceptHttps, handlerPath]() {
        QVariantMap result;
        result["success"] = false;

        killcore::ProcessHandle freshHandle(static_cast<uint32_t>(pid), killcore::ProcessAccess::ReadWrite);
        QString error;
        if (!session->start(freshHandle, interceptHttps, handlerPath, &error)) {
            result["error"] = error;
        } else {
            result["success"] = true;
            result["port"] = port;
            result["interceptHttps"] = interceptHttps;
            KE_LOG_INFO() << "startHttpProxyAsync: port=" << port << " interceptHttps=" << interceptHttps;
        }

        if (!self) return;
        QMetaObject::invokeMethod(self.data(), [self, result]() {
            if (!self) return;
            emit self->httpProxyStartFinished(result);
        }, Qt::QueuedConnection);
    }).detach();

    started["success"] = true;
    started["started"] = true;
#else
    started["error"] = KE_TXT("Fonctionnalité Windows uniquement.", "Windows-only feature.");
#endif

    return started;
}

QVariantMap ApplicationController::stopHttpProxyAsync() {
    QVariantMap started;
    started["success"] = false;

    if (!m_httpProxySession || !m_httpProxySession->isActive()) {
        started["error"] = KE_TXT("Aucun proxy HTTP actif.", "No active HTTP proxy.");
        return started;
    }

#ifdef Q_OS_WIN
    // HttpProxySession::stop() poll jusqu'à 3s en attendant la confirmation
    // du handler — même raisonnement que startHttpProxyAsync ci-dessus.
    killcore::HttpProxySession* session = m_httpProxySession.get();
    const QPointer<ApplicationController> self(this);
    std::thread([self, session]() {
        session->stop();
        KE_LOG_INFO() << "stopHttpProxyAsync: stopped";

        QVariantMap result;
        result["success"] = true;

        if (!self) return;
        QMetaObject::invokeMethod(self.data(), [self, result]() {
            if (!self) return;
            emit self->httpProxyStopFinished(result);
        }, Qt::QueuedConnection);
    }).detach();

    started["success"] = true;
    started["started"] = true;
#else
    started["error"] = KE_TXT("Fonctionnalité Windows uniquement.", "Windows-only feature.");
#endif

    return started;
}

QVariantMap ApplicationController::getHttpProxyRequests() {
    QVariantMap result;
    result["success"] = false;
    result["requests"] = QVariantList();

    if (!m_httpProxySession || !m_httpProxySession->isActive()) {
        result["error"] = KE_TXT("Aucun proxy HTTP actif.", "No active HTTP proxy.");
        return result;
    }

    auto requests = m_httpProxySession->getRequests();
    QVariantList list;
    for (const auto& req : requests) {
        QVariantMap entry;
        entry["method"] = req.method;
        entry["url"] = req.url;
        entry["requestBody"] = req.requestBody;
        entry["responseBody"] = req.responseBody;
        entry["timestamp"] = req.timestamp;
        entry["modified"] = req.modified;
        list.append(entry);
    }

    result["success"] = true;
    result["requests"] = list;
    return result;
}

QVariantMap ApplicationController::modifyHttpRequest(const QString& requestId, const QString& newRequestBody) {
    QVariantMap result;
    result["success"] = false;

    if (!m_httpProxySession || !m_httpProxySession->isActive()) {
        result["error"] = KE_TXT("Aucun proxy HTTP actif.", "No active HTTP proxy.");
        return result;
    }

    // requestId est l'index de la requête dans le buffer
    bool ok = false;
    int index = requestId.toInt(&ok);
    if (!ok || index < 0) {
        result["error"] = KE_TXT("Index de requête invalide.", "Invalid request index.");
        return result;
    }

    if (m_httpProxySession->modifyRequest(index, newRequestBody)) {
        result["success"] = true;
        KE_LOG_INFO() << "modifyHttpRequest: index=" << index;
    } else {
        result["error"] = KE_TXT("Impossible de modifier cette requête (déjà envoyée ou index invalide).", "Unable to modify this request (already sent or invalid index).");
    }

    return result;
}

// ---------------------------------------------------------------------------
// Spoof DNS — écriture fichier hosts Windows (UAC requis)
// ---------------------------------------------------------------------------

namespace {
QString hostsFilePath() {
    return QStringLiteral("C:\\Windows\\System32\\drivers\\etc\\hosts");
}

QString sanitizeHostsLine(const QString& domain, const QString& targetIp) {
    // Nettoyer les caractères dangereux pour la commande PowerShell
    QString safeDomain = domain;
    safeDomain.replace("'", "''");
    QString safeIp = targetIp;
    safeIp.replace("'", "''");
    return QStringLiteral("{0} {1}").arg(safeIp, safeDomain);
}
} // namespace

QVariantMap ApplicationController::spoofDnsAsync(const QString& domain, const QString& targetIp) {
    QVariantMap started;
    started["success"] = false;
    if (domain.isEmpty() || targetIp.isEmpty()) {
        started["error"] = KE_TXT("Domaine et IP cible requis.", "Domain and target IP required.");
        return started;
    }

#ifdef Q_OS_WIN
    const QString hostsPath = hostsFilePath();
    const QString line = sanitizeHostsLine(domain, targetIp);

    // PowerShell : lire le fichier, vérifier si la ligne existe, ajouter si non
    const QString psCommand = QStringLiteral(
        "$path = '%1'; "
        "$line = '%2'; "
        "$content = Get-Content $path -ErrorAction SilentlyContinue; "
        "if ($content -match ('^\\s*' + [regex]::Escape('%3') + '\\s')) { "
        "  'already_exists'; "
        "} else { "
        "  Add-Content -Path $path -Value $line -Encoding utf8; "
        "  'added'; "
        "}").arg(hostsPath, line, domain);

    // L'élévation UAC + l'exécution PowerShell tournent sur un thread séparé :
    // sinon WaitForSingleObject(15s) gèle tout le thread GUI (Q_INVOKABLE via
    // QWebChannel s'exécute sur le thread propriétaire de l'objet) pendant
    // toute la durée de l'invite UAC.
    const QPointer<ApplicationController> self(this);
    std::thread([self, psCommand, domain, targetIp]() {
        QVariantMap result;
        result["success"] = false;

        const std::wstring parameters =
            L"-NoProfile -ExecutionPolicy Bypass -Command \"" + psCommand.toStdWString() + L"\"";

        SHELLEXECUTEINFOW sei{};
        sei.cbSize = sizeof(sei);
        sei.fMask = SEE_MASK_NOCLOSEPROCESS;
        sei.hwnd = nullptr;
        sei.lpVerb = L"runas"; // UAC requis
        sei.lpFile = L"powershell.exe";
        sei.lpParameters = parameters.c_str();
        sei.nShow = SW_HIDE;

        if (!ShellExecuteExW(&sei)) {
            const DWORD err = GetLastError();
            if (err == ERROR_CANCELLED) {
                result["cancelled"] = true;
                result["error"] = KE_TXT("Invite d'élévation refusée par l'utilisateur.", "The user declined the elevation prompt.");
            } else {
                result["error"] = KE_TXT("Impossible de lancer PowerShell élevé (error=%1).", "Unable to launch elevated PowerShell (error=%1).").arg(err);
            }
        } else if (sei.hProcess) {
            WaitForSingleObject(sei.hProcess, 15000);
            DWORD exitCode = 1;
            GetExitCodeProcess(sei.hProcess, &exitCode);
            CloseHandle(sei.hProcess);

            if (exitCode == 0) {
                result["success"] = true;
                result["domain"] = domain;
                result["targetIp"] = targetIp;
                result["action"] = "added";
            } else {
                result["error"] = KE_TXT("PowerShell a échoué (code %1).", "PowerShell failed (code %1).").arg(exitCode);
            }
        } else {
            result["success"] = true; // Best-effort
            result["domain"] = domain;
            result["targetIp"] = targetIp;
        }

        KE_LOG_INFO() << "spoofDnsAsync: domain=" << domain.toStdString()
                      << " ip=" << targetIp.toStdString()
                      << " success=" << result.value("success").toBool();

        if (!self) return;
        QMetaObject::invokeMethod(self.data(), [self, result]() {
            if (!self) return;
            emit self->dnsSpoofFinished(result);
        }, Qt::QueuedConnection);
    }).detach();

    started["success"] = true;
    started["started"] = true;
#else
    started["error"] = KE_TXT("Fonctionnalité Windows uniquement.", "Windows-only feature.");
#endif

    return started;
}

QVariantMap ApplicationController::restoreDnsAsync(const QString& domain) {
    QVariantMap started;
    started["success"] = false;
    if (domain.isEmpty()) {
        started["error"] = KE_TXT("Domaine requis.", "Domain required.");
        return started;
    }

#ifdef Q_OS_WIN
    const QString hostsPath = hostsFilePath();

    // PowerShell : lire le fichier, retirer les lignes correspondant au domaine
    const QString psCommand = QStringLiteral(
        "$path = '%1'; "
        "$domain = '%2'; "
        "$content = Get-Content $path -ErrorAction SilentlyContinue; "
        "$filtered = $content | Where-Object { $_ -notmatch ('^\\s*' + [regex]::Escape($domain) + '\\s') }; "
        "$filtered | Set-Content -Path $path -Encoding utf8; "
        "'removed'").arg(hostsPath, domain);

    const QPointer<ApplicationController> self(this);
    std::thread([self, psCommand, domain]() {
        QVariantMap result;
        result["success"] = false;

        const std::wstring parameters =
            L"-NoProfile -ExecutionPolicy Bypass -Command \"" + psCommand.toStdWString() + L"\"";

        SHELLEXECUTEINFOW sei{};
        sei.cbSize = sizeof(sei);
        sei.fMask = SEE_MASK_NOCLOSEPROCESS;
        sei.hwnd = nullptr;
        sei.lpVerb = L"runas"; // UAC requis
        sei.lpFile = L"powershell.exe";
        sei.lpParameters = parameters.c_str();
        sei.nShow = SW_HIDE;

        if (!ShellExecuteExW(&sei)) {
            const DWORD err = GetLastError();
            if (err == ERROR_CANCELLED) {
                result["cancelled"] = true;
                result["error"] = KE_TXT("Invite d'élévation refusée par l'utilisateur.", "The user declined the elevation prompt.");
            } else {
                result["error"] = KE_TXT("Impossible de lancer PowerShell élevé (error=%1).", "Unable to launch elevated PowerShell (error=%1).").arg(err);
            }
        } else if (sei.hProcess) {
            WaitForSingleObject(sei.hProcess, 15000);
            DWORD exitCode = 1;
            GetExitCodeProcess(sei.hProcess, &exitCode);
            CloseHandle(sei.hProcess);

            if (exitCode == 0) {
                result["success"] = true;
                result["domain"] = domain;
                result["action"] = "removed";
            } else {
                result["error"] = KE_TXT("PowerShell a échoué (code %1).", "PowerShell failed (code %1).").arg(exitCode);
            }
        } else {
            result["success"] = true; // Best-effort
            result["domain"] = domain;
        }

        KE_LOG_INFO() << "restoreDnsAsync: domain=" << domain.toStdString()
                      << " success=" << result.value("success").toBool();

        if (!self) return;
        QMetaObject::invokeMethod(self.data(), [self, result]() {
            if (!self) return;
            emit self->dnsRestoreFinished(result);
        }, Qt::QueuedConnection);
    }).detach();

    started["success"] = true;
    started["started"] = true;
#else
    started["error"] = KE_TXT("Fonctionnalité Windows uniquement.", "Windows-only feature.");
#endif

    return started;
}

// ---------------------------------------------------------------------------
// Lag switch — injection DLL + MinHook sur recv/WSARecv
// ---------------------------------------------------------------------------

QVariantMap ApplicationController::setLagSwitchAsync(bool enabled, int delayMs) {
    QVariantMap result;
    result["success"] = false;
    if (!m_attached || m_pid <= 0) {
        result["error"] = KE_TXT("Aucun processus attaché.", "No process attached.");
        return result;
    }
    if (delayMs < 0 || delayMs > 10000) {
        result["error"] = KE_TXT("Délai invalide (0-10000 ms).", "Invalid delay (0-10000 ms).");
        return result;
    }

#ifdef Q_OS_WIN
    if (!m_lagSwitchSession) {
        m_lagSwitchSession = std::make_unique<killcore::LagSwitchSession>();
    }

    if (enabled && !m_lagSwitchSession->isActive()) {
        // Démarrage : LagSwitchSession::start() poll jusqu'à 6s en attendant
        // la confirmation du handler — tourne sur un thread séparé pour ne
        // pas geler le thread GUI. Même convention que
        // startHttpProxyAsync/testCandidateFieldsAsync : le thread rouvre
        // son propre ProcessHandle plutôt que de partager m_handle, qui
        // pourrait être fermé par un detachProcess() concurrent.
        const QString handlerPath = QCoreApplication::applicationDirPath()
            + QStringLiteral("/KillEngineLagSwitchHandler.dll");
        if (!QFile::exists(handlerPath)) {
            result["error"] = KE_TXT("Handler lag switch introuvable: ", "Lag switch handler not found: ") + handlerPath;
            return result;
        }

        killcore::LagSwitchSession* session = m_lagSwitchSession.get();
        const int pid = m_pid;
        const QPointer<ApplicationController> self(this);
        std::thread([self, session, pid, delayMs, handlerPath]() {
            killcore::ProcessHandle freshHandle(static_cast<uint32_t>(pid), killcore::ProcessAccess::ReadWrite);
            QString error;
            QVariantMap finished;
            if (!session->start(freshHandle, delayMs, handlerPath, &error)) {
                finished["success"] = false;
                finished["error"] = error;
            } else {
                finished["success"] = true;
                finished["active"] = true;
                finished["delayMs"] = delayMs;
                KE_LOG_INFO() << "setLagSwitchAsync: started delayMs=" << delayMs;
            }

            if (!self) return;
            QMetaObject::invokeMethod(self.data(), [self, finished]() {
                if (!self) return;
                emit self->lagSwitchFinished(finished);
            }, Qt::QueuedConnection);
        }).detach();

        result["success"] = true;
        result["started"] = true;
        return result;
    }

    if (!enabled && m_lagSwitchSession->isActive()) {
        // Arrêt : LagSwitchSession::stop() poll jusqu'à 3s, même raisonnement.
        killcore::LagSwitchSession* session = m_lagSwitchSession.get();
        const QPointer<ApplicationController> self(this);
        std::thread([self, session]() {
            session->stop();
            KE_LOG_INFO() << "setLagSwitchAsync: stopped";

            QVariantMap finished;
            finished["success"] = true;
            finished["active"] = false;

            if (!self) return;
            QMetaObject::invokeMethod(self.data(), [self, finished]() {
                if (!self) return;
                emit self->lagSwitchFinished(finished);
            }, Qt::QueuedConnection);
        }).detach();

        result["success"] = true;
        result["started"] = true;
        return result;
    }

    // Ni démarrage ni arrêt réel : juste une mise à jour du délai sur une
    // session déjà active (instantané, pas besoin de thread séparé), ou une
    // désactivation alors que rien n'est actif.
    if (enabled) {
        m_lagSwitchSession->setDelayMs(delayMs);
        result["success"] = true;
        result["active"] = true;
        result["delayMs"] = delayMs;
    } else {
        result["success"] = true;
        result["active"] = false;
    }

    KE_LOG_INFO() << "setLagSwitchAsync: enabled=" << enabled << " delayMs=" << delayMs
                  << " success=" << result.value("success").toBool();
#else
    result["error"] = KE_TXT("Fonctionnalité Windows uniquement.", "Windows-only feature.");
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

QVariantMap ApplicationController::getWebView2InspectorStatus() const {
    QVariantMap result = webView2ResultEnvelope(QStringLiteral("debug"), false);
    result[QStringLiteral("success")] = true;
    result[QStringLiteral("connected")] = m_webView2Inspector && m_webView2Inspector->isConnected();
    result[QStringLiteral("browserProcessId")] = m_webView2BrowserProcessId;
    result[QStringLiteral("endpoint")] = m_webView2Endpoint;
    result[QStringLiteral("target")] = m_webView2ActiveTarget;
    return result;
}

QVariantMap ApplicationController::listWebView2CdpTargets(int browserProcessId, const QVariantMap& options) const {
    QVariantMap result = webView2ResultEnvelope(QStringLiteral("debug"), false);
    const QString httpUrl = defaultWebView2CdpEndpoint(options);
    const QJsonArray pages = killcore::WebView2Inspector::listAvailablePages(httpUrl);
    const QJsonArray targets = filterWebView2Targets(pages, browserProcessId, options);

    result[QStringLiteral("success")] = true;
    result[QStringLiteral("endpoint")] = httpUrl;
    result[QStringLiteral("browserProcessId")] = browserProcessId;
    result[QStringLiteral("totalDiscovered")] = pages.size();
    result[QStringLiteral("count")] = targets.size();
    result[QStringLiteral("targets")] = webView2TargetsToVariantList(targets);
    result[QStringLiteral("pageTargetsOnly")] = options.value(QStringLiteral("pageTargetsOnly"), true).toBool();
    result[QStringLiteral("allowAboutBlank")] = options.value(QStringLiteral("allowAboutBlank"), false).toBool();
    if (pages.isEmpty()) {
        result[QStringLiteral("warning")] = webView2SetupHint();
    } else if (targets.isEmpty()) {
        result[QStringLiteral("warning")] = QStringLiteral(
            "Des targets CDP existent, mais aucune ne correspond aux filtres demandes. "
            "Essayer browserProcessId=0, allowAboutBlank=true, ou retirer pageTitle/pageUrl/targetId.");
    }
    return result;
}

QVariantMap ApplicationController::connectWebView2Inspector(int browserProcessId, const QVariantMap& options) {
    QVariantMap result = webView2ResultEnvelope(QStringLiteral("debug"), true);
    if (!m_webView2Inspector) {
        result[QStringLiteral("error")] = QStringLiteral("Inspecteur WebView2 non initialise.");
        return result;
    }

    const QString explicitWsUrl = options.value(QStringLiteral("webSocketDebuggerUrl")).toString().trimmed();
    QJsonObject selectedTarget;
    QString wsUrl = explicitWsUrl;
    const QString httpUrl = defaultWebView2CdpEndpoint(options);

    if (wsUrl.isEmpty()) {
        const QJsonArray pages = killcore::WebView2Inspector::listAvailablePages(httpUrl);
        const QJsonArray targets = filterWebView2Targets(pages, browserProcessId, options);
        if (targets.isEmpty()) {
            result[QStringLiteral("endpoint")] = httpUrl;
            result[QStringLiteral("browserProcessId")] = browserProcessId;
            result[QStringLiteral("totalDiscovered")] = pages.size();
            result[QStringLiteral("error")] = pages.isEmpty()
                ? webView2SetupHint()
                : QStringLiteral("Aucune target WebView2 CDP exploitable ne correspond au PID/filtre demande.");
            return result;
        }
        selectedTarget = targets.first().toObject();
        wsUrl = selectedTarget.value(QStringLiteral("webSocketDebuggerUrl")).toString();
    }

    if (wsUrl.isEmpty()) {
        result[QStringLiteral("error")] = QStringLiteral("Target CDP sans webSocketDebuggerUrl.");
        return result;
    }

    if (m_webView2Inspector->isConnected()) {
        m_webView2Inspector->disconnect();
    }
    const bool connected = m_webView2Inspector->connectToWebSocket(wsUrl);
    result[QStringLiteral("success")] = connected;
    result[QStringLiteral("connected")] = connected;
    result[QStringLiteral("endpoint")] = httpUrl;
    result[QStringLiteral("browserProcessId")] = browserProcessId;
    result[QStringLiteral("webSocketDebuggerUrl")] = wsUrl;
    if (!selectedTarget.isEmpty()) {
        result[QStringLiteral("target")] = webView2TargetToVariantMap(selectedTarget);
    }
    if (!connected) {
        result[QStringLiteral("error")] = QStringLiteral(
            "Connexion WebSocket CDP echouee. Verifier que la target WebView2 existe toujours et que Remote Tools expose bien /msedge.");
        return result;
    }

    m_webView2Endpoint = httpUrl;
    m_webView2BrowserProcessId = browserProcessId;
    m_webView2ActiveTarget = selectedTarget.isEmpty()
        ? QVariantMap{{QStringLiteral("webSocketDebuggerUrl"), wsUrl}}
        : webView2TargetToVariantMap(selectedTarget);
    return result;
}

QVariantMap ApplicationController::disconnectWebView2Inspector() {
    QVariantMap result = webView2ResultEnvelope(QStringLiteral("debug"), false);
    if (m_webView2Inspector) {
        m_webView2Inspector->disconnect();
    }
    m_webView2ActiveTarget.clear();
    m_webView2Endpoint.clear();
    m_webView2BrowserProcessId = 0;
    result[QStringLiteral("success")] = true;
    result[QStringLiteral("connected")] = false;
    return result;
}

QVariantMap ApplicationController::evaluateWebView2JavaScript(const QString& expression, const QVariantMap& options) {
    QVariantMap result = webView2ResultEnvelope(QStringLiteral("script"), true);
    if (!m_webView2Inspector || !m_webView2Inspector->isConnected()) {
        result[QStringLiteral("error")] = QStringLiteral("Aucune target WebView2 CDP connectee.");
        return result;
    }
    if (expression.trimmed().isEmpty()) {
        result[QStringLiteral("error")] = QStringLiteral("Expression JavaScript vide.");
        return result;
    }

    const bool returnByValue = options.value(QStringLiteral("returnByValue"), true).toBool();
    const QJsonObject response = m_webView2Inspector->evaluateJavaScript(expression, returnByValue);
    result[QStringLiteral("raw")] = response.toVariantMap();
    result[QStringLiteral("target")] = m_webView2ActiveTarget;
    if (response.isEmpty()) {
        result[QStringLiteral("error")] = QStringLiteral("Evaluation JavaScript sans reponse CDP.");
        return result;
    }
    if (response.contains(QStringLiteral("error"))) {
        result[QStringLiteral("error")] = QStringLiteral("Erreur CDP pendant Runtime.evaluate.");
        result[QStringLiteral("cdpError")] = response.value(QStringLiteral("error")).toVariant();
        return result;
    }

    const QJsonObject responseResult = response.value(QStringLiteral("result")).toObject();
    if (responseResult.contains(QStringLiteral("exceptionDetails"))) {
        result[QStringLiteral("error")] = QStringLiteral("Exception JavaScript pendant Runtime.evaluate.");
        result[QStringLiteral("exceptionDetails")] = responseResult.value(QStringLiteral("exceptionDetails")).toVariant();
        return result;
    }

    const QJsonObject remoteObject = responseResult.value(QStringLiteral("result")).toObject();
    result[QStringLiteral("success")] = true;
    result[QStringLiteral("type")] = remoteObject.value(QStringLiteral("type")).toString();
    result[QStringLiteral("subtype")] = remoteObject.value(QStringLiteral("subtype")).toString();
    result[QStringLiteral("description")] = remoteObject.value(QStringLiteral("description")).toString();
    result[QStringLiteral("value")] = remoteObject.value(QStringLiteral("value")).toVariant();
    return result;
}

QVariantMap ApplicationController::findWebView2DisplayedValues(const QString& value, const QVariantMap& options) {
    QVariantMap result = webView2ResultEnvelope(QStringLiteral("debug"), false);
    if (!m_webView2Inspector || !m_webView2Inspector->isConnected()) {
        result[QStringLiteral("error")] = QStringLiteral("Aucune target WebView2 CDP connectee.");
        return result;
    }

    bool ok = false;
    const int numericValue = value.trimmed().toInt(&ok);
    if (!ok) {
        result[QStringLiteral("error")] = QStringLiteral("Valeur numerique invalide pour findDisplayedValues.");
        return result;
    }

    const QJsonArray matches = m_webView2Inspector->findDisplayedValues(numericValue);
    const int maxResults = std::max(1, options.value(QStringLiteral("maxResults"), 100).toInt());
    QJsonArray limited;
    for (int i = 0; i < matches.size() && i < maxResults; ++i) {
        limited.append(matches.at(i));
    }
    result[QStringLiteral("success")] = true;
    result[QStringLiteral("value")] = numericValue;
    result[QStringLiteral("count")] = limited.size();
    result[QStringLiteral("totalMatches")] = matches.size();
    result[QStringLiteral("matches")] = webView2TargetsToVariantList(limited);
    result[QStringLiteral("target")] = m_webView2ActiveTarget;
    return result;
}

QVariantMap ApplicationController::findWebView2DisplayedText(const QString& text, const QVariantMap& options) {
    QVariantMap result = webView2ResultEnvelope(QStringLiteral("debug"), false);
    if (!m_webView2Inspector || !m_webView2Inspector->isConnected()) {
        result[QStringLiteral("error")] = QStringLiteral("Aucune target WebView2 CDP connectee.");
        return result;
    }
    if (text.isEmpty()) {
        result[QStringLiteral("error")] = QStringLiteral("Texte vide.");
        return result;
    }

    const QJsonArray matches = m_webView2Inspector->findDisplayedText(text);
    const int maxResults = std::max(1, options.value(QStringLiteral("maxResults"), 100).toInt());
    QJsonArray limited;
    for (int i = 0; i < matches.size() && i < maxResults; ++i) {
        limited.append(matches.at(i));
    }
    result[QStringLiteral("success")] = true;
    result[QStringLiteral("text")] = text;
    result[QStringLiteral("count")] = limited.size();
    result[QStringLiteral("totalMatches")] = matches.size();
    result[QStringLiteral("matches")] = webView2TargetsToVariantList(limited);
    result[QStringLiteral("target")] = m_webView2ActiveTarget;
    return result;
}

namespace {
// Meme flag et meme port que WEBVIEW-A/B (voir docs/PHASE_TRACKER.md) : force
// le port de debug CDP sur TOUS les hotes WebView2 du user Windows courant a
// leur prochain lancement, pas seulement une cible visee.
const QString kWebView2DebugFlagEnvName = QStringLiteral("WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS");
const QString kWebView2DebugFlagValue = QStringLiteral("--remote-debugging-port=9333");
const QString kWebView2DevModeCapabilityName = QStringLiteral("Tools.DeveloperMode.Core~~~~0.0.1.0");

// Equivalent natif de `setx` (qui ecrit HKCU\Environment puis broadcast) :
// QSettings seul persiste la valeur mais ne notifie pas les process deja
// lances -- necessaire pour qu'une relance immediate de la cible la voie.
void broadcastEnvironmentChange() {
    DWORD_PTR dwResult = 0;
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0,
        reinterpret_cast<LPARAM>(L"Environment"), SMTO_ABORTIFHUNG, 5000, &dwResult);
}
} // namespace

QVariantMap ApplicationController::enableWebView2CdpDebugFlag() {
    QVariantMap result;
    result[QStringLiteral("success")] = false;
#ifdef Q_OS_WIN
    QSettings env(QStringLiteral("HKEY_CURRENT_USER\\Environment"), QSettings::NativeFormat);
    env.setValue(kWebView2DebugFlagEnvName, kWebView2DebugFlagValue);
    env.sync();
    broadcastEnvironmentChange();
    result[QStringLiteral("success")] = true;
    result[QStringLiteral("value")] = kWebView2DebugFlagValue;
    KE_LOG_INFO() << "enableWebView2CdpDebugFlag: variable posee, relance des cibles WebView2 requise.";
#else
    result[QStringLiteral("error")] = KE_TXT("Fonctionnalité Windows uniquement.", "Windows-only feature.");
#endif
    return result;
}

QVariantMap ApplicationController::disableWebView2CdpDebugFlag() {
    QVariantMap result;
    result[QStringLiteral("success")] = false;
#ifdef Q_OS_WIN
    QSettings env(QStringLiteral("HKEY_CURRENT_USER\\Environment"), QSettings::NativeFormat);
    env.remove(kWebView2DebugFlagEnvName);
    env.sync();
    broadcastEnvironmentChange();
    result[QStringLiteral("success")] = true;
    KE_LOG_INFO() << "disableWebView2CdpDebugFlag: variable retiree.";
#else
    result[QStringLiteral("error")] = KE_TXT("Fonctionnalité Windows uniquement.", "Windows-only feature.");
#endif
    return result;
}

QVariantMap ApplicationController::getWebView2CdpDebugFlagStatus() const {
    QVariantMap result;
#ifdef Q_OS_WIN
    QSettings env(QStringLiteral("HKEY_CURRENT_USER\\Environment"), QSettings::NativeFormat);
    const QString value = env.value(kWebView2DebugFlagEnvName).toString();
    result[QStringLiteral("success")] = true;
    result[QStringLiteral("enabled")] = !value.isEmpty();
    result[QStringLiteral("value")] = value;
#else
    result[QStringLiteral("success")] = false;
    result[QStringLiteral("error")] = KE_TXT("Fonctionnalité Windows uniquement.", "Windows-only feature.");
#endif
    return result;
}

QVariantMap ApplicationController::getWebView2SystemPrepStatus() const {
    QVariantMap result;
    result[QStringLiteral("success")] = true;
#ifdef Q_OS_WIN
    QSettings appModelUnlock(
        QStringLiteral("HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\AppModelUnlock"),
        QSettings::NativeFormat);
    result[QStringLiteral("developerModeEnabled")] =
        appModelUnlock.value(QStringLiteral("AllowDevelopmentWithoutDevLicense"), 0).toInt() != 0;
    result[QStringLiteral("allowAllTrustedApps")] =
        appModelUnlock.value(QStringLiteral("AllowAllTrustedApps"), 0).toInt() != 0;

    QProcess probe;
    probe.setProgram(QStringLiteral("powershell.exe"));
    probe.setArguments({
        QStringLiteral("-NoProfile"),
        QStringLiteral("-Command"),
        QStringLiteral("(Get-WindowsCapability -Online -Name %1).State").arg(kWebView2DevModeCapabilityName),
    });
    probe.start();
    const bool finished = probe.waitForFinished(15000);
    const QString output = QString::fromLocal8Bit(probe.readAllStandardOutput()).trimmed();
    result[QStringLiteral("capabilityQueried")] = finished;
    result[QStringLiteral("capabilityState")] = output.isEmpty() ? QStringLiteral("Inconnu") : output;
    result[QStringLiteral("capabilityInstalled")] =
        output.compare(QStringLiteral("Installed"), Qt::CaseInsensitive) == 0;
#else
    result[QStringLiteral("success")] = false;
    result[QStringLiteral("error")] = KE_TXT("Fonctionnalité Windows uniquement.", "Windows-only feature.");
#endif
    return result;
}

QVariantMap ApplicationController::installWebView2DeveloperModeCapability() {
    QVariantMap result;
    result[QStringLiteral("success")] = false;
#ifdef Q_OS_WIN
    // Meme mecanisme que requestWindowsDefenderExclusion()/blockProcessNetwork()
    // : invite UAC visible via `runas`, jamais silencieux. Contrairement a ces
    // deux-la, on n'attend PAS la fin du process (WaitForSingleObject) : cette
    // installation peut prendre plusieurs minutes et rester silencieuse cote
    // DISM (verifie en session live, cf. docs/PHASE_TRACKER.md) -- bloquer
    // l'appel Q_INVOKABLE ce longtemps gelerait l'UI. L'utilisateur relance un
    // getWebView2SystemPrepStatus() pour re-tester une fois termine.
    const QString psCommand = QStringLiteral(
        "Add-WindowsCapability -Online -Name %1; "
        "Write-Host 'Termine -- vous pouvez fermer cette fenetre.'; Start-Sleep -Seconds 5")
        .arg(kWebView2DevModeCapabilityName);
    const std::wstring parameters =
        L"-NoProfile -ExecutionPolicy Bypass -Command \"" + psCommand.toStdWString() + L"\"";

    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.hwnd = nullptr;
    sei.lpVerb = L"runas";
    sei.lpFile = L"powershell.exe";
    sei.lpParameters = parameters.c_str();
    sei.nShow = SW_SHOWNORMAL;

    if (!ShellExecuteExW(&sei)) {
        const DWORD err = GetLastError();
        if (err == ERROR_CANCELLED) {
            result[QStringLiteral("cancelled")] = true;
            result[QStringLiteral("error")] = KE_TXT("Invite UAC refusée par l'utilisateur.", "The UAC prompt was declined by the user.");
        } else {
            result[QStringLiteral("error")] = KE_TXT("ShellExecuteExW a échoué (code %1).", "ShellExecuteExW failed (code %1).").arg(err);
        }
        return result;
    }
    if (sei.hProcess) {
        CloseHandle(sei.hProcess);
    }
    result[QStringLiteral("success")] = true;
    result[QStringLiteral("message")] = KE_TXT(
        "Installation lancée dans une fenêtre PowerShell élevée. Peut prendre plusieurs minutes et rester "
        "silencieuse : suivre l'état dans Paramètres > Système > Fonctionnalités facultatives > Historique, "
        "ou relancer un diagnostic ici une fois terminé.",
        "Installation started in an elevated PowerShell window. It may take several minutes and stay "
        "silent: check progress in Settings > System > Optional Features > History, "
        "or rerun a diagnostic here once finished.");
    KE_LOG_INFO() << "installWebView2DeveloperModeCapability: installation lancée (async, UAC affiché).";
#else
    result[QStringLiteral("error")] = KE_TXT("Fonctionnalité Windows uniquement.", "Windows-only feature.");
#endif
    return result;
}

namespace {
// Repli statique si aucune target about:blank n'est disponible sur le host
// pour calculer une baseline dynamique -- liste courte des globales window
// les plus communes a un moteur Chromium (pas exhaustive, juste assez pour
// filtrer le bruit le plus flagrant). Prefer toujours la baseline dynamique
// quand possible (voir probeWebView2GlobalScope).
const QSet<QString>& webView2StaticGlobalsExclusion() {
    static const QSet<QString> kExclusion = {
        QStringLiteral("window"), QStringLiteral("self"), QStringLiteral("document"),
        QStringLiteral("name"), QStringLiteral("location"), QStringLiteral("history"),
        QStringLiteral("navigator"), QStringLiteral("screen"), QStringLiteral("customElements"),
        QStringLiteral("navigation"), QStringLiteral("locationbar"), QStringLiteral("menubar"),
        QStringLiteral("personalbar"), QStringLiteral("scrollbars"), QStringLiteral("statusbar"),
        QStringLiteral("toolbar"), QStringLiteral("status"), QStringLiteral("closed"),
        QStringLiteral("frames"), QStringLiteral("length"), QStringLiteral("top"),
        QStringLiteral("opener"), QStringLiteral("parent"), QStringLiteral("frameElement"),
        QStringLiteral("origin"), QStringLiteral("external"), QStringLiteral("innerWidth"),
        QStringLiteral("innerHeight"), QStringLiteral("scrollX"), QStringLiteral("scrollY"),
        QStringLiteral("pageXOffset"), QStringLiteral("pageYOffset"), QStringLiteral("visualViewport"),
        QStringLiteral("screenX"), QStringLiteral("screenY"), QStringLiteral("outerWidth"),
        QStringLiteral("outerHeight"), QStringLiteral("devicePixelRatio"), QStringLiteral("event"),
        QStringLiteral("clientInformation"), QStringLiteral("screenLeft"), QStringLiteral("screenTop"),
        QStringLiteral("styleMedia"), QStringLiteral("crypto"), QStringLiteral("indexedDB"),
        QStringLiteral("fetch"), QStringLiteral("caches"), QStringLiteral("performance"),
        QStringLiteral("localStorage"), QStringLiteral("sessionStorage"), QStringLiteral("chrome"),
        QStringLiteral("console"), QStringLiteral("alert"), QStringLiteral("confirm"),
        QStringLiteral("prompt"), QStringLiteral("open"), QStringLiteral("close"),
        QStringLiteral("focus"), QStringLiteral("blur"), QStringLiteral("print"),
        QStringLiteral("setTimeout"), QStringLiteral("clearTimeout"), QStringLiteral("setInterval"),
        QStringLiteral("clearInterval"), QStringLiteral("requestAnimationFrame"),
        QStringLiteral("cancelAnimationFrame"), QStringLiteral("getComputedStyle"),
        QStringLiteral("matchMedia"), QStringLiteral("getSelection"), QStringLiteral("postMessage"),
        QStringLiteral("addEventListener"), QStringLiteral("removeEventListener"),
        QStringLiteral("dispatchEvent"), QStringLiteral("btoa"), QStringLiteral("atob"),
        QStringLiteral("structuredClone"), QStringLiteral("queueMicrotask"), QStringLiteral("webkitURL"),
        QStringLiteral("onload"), QStringLiteral("onerror"), QStringLiteral("onunload"),
        QStringLiteral("onbeforeunload"), QStringLiteral("onresize"), QStringLiteral("onscroll"),
    };
    return kExclusion;
}
} // namespace

QVariantMap ApplicationController::probeWebView2GlobalScope() {
    QVariantMap result = webView2ResultEnvelope(QStringLiteral("debug"), false);
    if (!m_webView2Inspector || !m_webView2Inspector->isConnected()) {
        result[QStringLiteral("error")] = QStringLiteral("Aucune target WebView2 CDP connectee.");
        return result;
    }

    const QJsonObject pageScope = m_webView2Inspector->probeGlobalScope();
    if (!pageScope.value(QStringLiteral("success")).toBool()) {
        result[QStringLiteral("error")] = pageScope.value(QStringLiteral("error")).toString(
            QStringLiteral("Sondage du scope global echoue."));
        return result;
    }

    // Baseline dynamique : chercher une target about:blank sur le meme host
    // pour connaitre les globales natives de cette version precise de
    // Chromium, via une connexion CDP jetable independante de
    // m_webView2Inspector (qui reste sur la target de l'utilisateur pendant
    // tout le sondage -- decision d'architecture WEBVIEW-F issue de
    // docs/SALON.md, voir docs/PHASE_TRACKER.md).
    QSet<QString> baselineNames;
    bool baselineUsed = false;
    const QString currentWsUrl = m_webView2ActiveTarget.value(QStringLiteral("webSocketDebuggerUrl")).toString();
    const QJsonArray pages = killcore::WebView2Inspector::listAvailablePages(m_webView2Endpoint);
    QVariantMap blankFilterOptions;
    blankFilterOptions[QStringLiteral("pageTargetsOnly")] = true;
    blankFilterOptions[QStringLiteral("allowAboutBlank")] = true;
    const QJsonArray blankCandidates = filterWebView2Targets(pages, m_webView2BrowserProcessId, blankFilterOptions);

    QString baselineWsUrl;
    for (const QJsonValue& candidate : blankCandidates) {
        const QJsonObject candidateObj = candidate.toObject();
        const QString url = candidateObj.value(QStringLiteral("url")).toString();
        const QString wsUrl = candidateObj.value(QStringLiteral("webSocketDebuggerUrl")).toString();
        if (url.startsWith(QStringLiteral("about:"), Qt::CaseInsensitive) && wsUrl != currentWsUrl && !wsUrl.isEmpty()) {
            baselineWsUrl = wsUrl;
            break;
        }
    }

    if (!baselineWsUrl.isEmpty()) {
        auto baselineInspector = std::make_unique<killcore::WebView2Inspector>();
        if (baselineInspector->connectToWebSocket(baselineWsUrl)) {
            const QJsonObject baselineScope = baselineInspector->probeGlobalScope();
            if (baselineScope.value(QStringLiteral("success")).toBool()) {
                for (const QJsonValue& g : baselineScope.value(QStringLiteral("globals")).toArray()) {
                    baselineNames.insert(g.toObject().value(QStringLiteral("name")).toString());
                }
                baselineUsed = true;
            }
            baselineInspector->disconnect();
        }
        // baselineInspector detruit ici (fin de scope) : n'affecte jamais la
        // connexion m_webView2Inspector de l'utilisateur.
    }

    if (!baselineUsed) {
        baselineNames = webView2StaticGlobalsExclusion();
    }

    QJsonArray customGlobals;
    const QJsonArray allGlobals = pageScope.value(QStringLiteral("globals")).toArray();
    for (const QJsonValue& g : allGlobals) {
        const QJsonObject entry = g.toObject();
        if (!baselineNames.contains(entry.value(QStringLiteral("name")).toString())) {
            customGlobals.append(entry);
        }
    }

    result[QStringLiteral("success")] = true;
    result[QStringLiteral("customGlobals")] = webView2TargetsToVariantList(customGlobals);
    result[QStringLiteral("customGlobalsCount")] = customGlobals.size();
    result[QStringLiteral("totalGlobalsSeen")] = allGlobals.size();
    result[QStringLiteral("baselineMode")] = baselineUsed
        ? QStringLiteral("dynamic_about_blank")
        : QStringLiteral("static_fallback");
    result[QStringLiteral("media")] = pageScope.value(QStringLiteral("media")).toObject().toVariantMap();
    result[QStringLiteral("target")] = m_webView2ActiveTarget;
    return result;
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
        result["error"] = KE_TXT("Aucun processus attaché.", "No process attached.");
        return result;
    }

    uint64_t address = 0;
    if (!parseHexAddress(addressHex, &address)) {
        result["error"] = KE_TXT("Adresse invalide.", "Invalid address.");
        return result;
    }

    killcore::ValueType type;
    if (!killcore::parseValueType(valueType, &type)) {
        result["error"] = KE_TXT("Type invalide.", "Invalid type.");
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
        result["error"] = KE_TXT("Écriture kernel échouée (driver non chargé/non connecté, adresse invalide côté cible, ou accès refusé).",
                                  "Kernel write failed (driver not loaded/connected, invalid target address, or access denied).");
        KE_LOG_WARN() << "writeMemoryValueKernel: échec pid=" << m_handle.pid() << " address=0x" << QString::number(address, 16).toStdString();
    }
#else
    result["error"] = KE_TXT("Fonctionnalité Windows uniquement.", "Windows-only feature.");
#endif
    return result;
}

QVariantMap ApplicationController::handleTable(const QString& ownerPid, const QString& handleValue, bool hide) const {
    return m_kernelDriverManager->handleTable(ownerPid, handleValue, hide);
}

QVariantMap ApplicationController::getAiModelStatus() const {
    return m_settingsDiagnosticsManager->getAiModelStatus();
}

void ApplicationController::warmupLocalAiModel() {
    // Delai nul: rend la main a l'appelant JS immediatement, le demarrage du
    // serveur llama.cpp (bloquant jusqu'a ~90s au tout premier chargement
    // modele, cf. LlamaServer::startAndWait) s'execute au tour de boucle
    // d'evenements suivant sans faire attendre le WebChannel.
    QTimer::singleShot(0, this, [this]() {
        m_ai.warmupLocalModel();
    });
}

QVariantMap ApplicationController::browseForModelFile() {
    return m_settingsDiagnosticsManager->browseForModelFile();
}

namespace {
// Résolution des scripts d'installation des modules complémentaires :
// dev (build/bin -> ../../scripts) ET package portable (scripts/ à la racine,
// cf. scripts/package-windows.ps1 qui copie killengine.lua + automation-pipe-call.ps1).
QString findModuleCatalogScript(const QString& scriptName) {
    const QDir appDir(QCoreApplication::applicationDirPath());
    const QStringList candidates = {
        appDir.filePath(QStringLiteral("scripts/%1").arg(scriptName)),
        appDir.filePath(QStringLiteral("../scripts/%1").arg(scriptName)),
        appDir.filePath(QStringLiteral("../../scripts/%1").arg(scriptName)),
        QDir::current().filePath(QStringLiteral("scripts/%1").arg(scriptName)),
    };
    for (const auto& candidate : candidates) {
        const QFileInfo file(candidate);
        if (file.exists() && file.isFile()) {
            return file.absoluteFilePath();
        }
    }
    return {};
}

QString findModuleCatalogModelDir() {
    const QDir appDir(QCoreApplication::applicationDirPath());
    const QStringList candidates = {
        appDir.filePath("model/qwen"),
        appDir.filePath("../../model/qwen"),
        QDir::current().filePath("model/qwen"),
    };
    for (const auto& candidate : candidates) {
        if (QFileInfo(candidate).isDir()) {
            return QDir(candidate).absolutePath();
        }
    }
    return {};
}
} // namespace

QVariantMap ApplicationController::getModuleCatalog() const {
    QVariantMap result;
    result["success"] = true;
    QVariantList modules;

    // 1) Runtime Lua externe (lua.exe + helper killengine.lua).
    {
        const QVariantMap lua = getLuaScriptingStatus();
        const bool available = lua.value("available").toBool();
        QVariantMap item;
        item["id"] = QStringLiteral("lua_runtime");
        item["displayName"] = QStringLiteral("Runtime Lua externe");
        item["description"] = QStringLiteral("Interpréteur lua.exe + helper scripts/killengine.lua — scripting Lua pilotant KillEngine via le pipe d'automatisation.");
        item["installed"] = available;
        item["status"] = available ? QStringLiteral("ok") : QStringLiteral("missing");
        item["detail"] = lua.value("message").toString();
        item["path"] = lua.value("luaPath").toString();
        item["installable"] = true;
        item["installKind"] = QStringLiteral("script");
        modules.append(item);
    }

    // 2) Modèle IA embarqué (GGUF partagé des agents).
    {
        const QVariantMap ai = getAiModelStatus();
        const bool ready = ai.value("ready").toBool();
        QVariantMap item;
        item["id"] = QStringLiteral("ai_model");
        item["displayName"] = QStringLiteral("Modèle IA embarqué (GGUF)");
        item["description"] = QStringLiteral("Qwen3.5-2B Q4_K_M (~1,4 Go) pour l'Assistant et l'Auto Resolver — téléchargé depuis Hugging Face (bartowski/Qwen_Qwen3.5-2B-GGUF, même quantification que le modèle embarqué).");
        item["installed"] = ready;
        item["status"] = ready ? QStringLiteral("ok") : QStringLiteral("missing");
        item["detail"] = ai.value("message").toString();
        item["path"] = ai.value("modelPath").toString();
        item["installable"] = true;
        item["installKind"] = QStringLiteral("download");
        modules.append(item);
    }

    // 3) Inspecteur CLR (helper .NET ClrMD).
    {
        const QVariantMap clr = getClrInspectorStatus();
        const bool available = clr.value("available").toBool();
        QVariantMap item;
        item["id"] = QStringLiteral("clr_inspector");
        item["displayName"] = QStringLiteral("Inspecteur CLR (ClrMD)");
        item["description"] = QStringLiteral("Helper .NET KillEngineClrInspector.exe — lecture/écriture des objets managés des cibles .NET via named pipe.");
        item["installed"] = available;
        item["status"] = available ? QStringLiteral("ok") : QStringLiteral("missing");
        item["detail"] = available ? QStringLiteral("Helper détecté.") : QStringLiteral("Helper introuvable — build local requis (SDK .NET 8+).");
        item["path"] = clr.value("helperPath").toString();
        item["installable"] = true;
        item["installKind"] = QStringLiteral("script");
        modules.append(item);
    }

    // 4) Driver noyau optionnel.
    {
        const QVariantMap drv = probeKernelDriver();
        const bool connected = drv.value("status").toString().compare(QStringLiteral("connected"), Qt::CaseInsensitive) == 0;
        QVariantMap item;
        item["id"] = QStringLiteral("kernel_driver");
        item["displayName"] = QStringLiteral("Driver noyau KillEngineKernel");
        item["description"] = QStringLiteral("Driver kernel optionnel (mémoire privilégiée, table de handles) — service Windows KillEngineKernel.");
        item["installed"] = connected;
        item["status"] = connected ? QStringLiteral("ok") : QStringLiteral("missing");
        item["detail"] = drv.value("message").toString();
        item["path"] = drv.value("devicePath").toString();
        item["installable"] = true;
        item["installKind"] = QStringLiteral("elevated");
        modules.append(item);
    }

    // --- MODULES-V2 : Section 2 — Environnement de test ---

    // 5) EDR blocking check (diagnostic, pas d'installation).
    {
        QVariantMap item;
        item["id"] = QStringLiteral("edr_exclusion");
        item["displayName"] = QStringLiteral("Exclusion EDR / Defender");
        item["description"] = QStringLiteral("Vérifie si l'EDR bloque l'injection de code (VirtualAllocEx/WriteProcessMemory/CreateRemoteThread) et propose d'ajouter une exclusion pour le dossier build/bin.");
        item["installed"] = false; // diagnostic dynamique
        item["status"] = QStringLiteral("unknown");
        item["detail"] = QStringLiteral("Cliquez sur 'Vérifier' pour tester.");
        item["installable"] = true;
        item["installKind"] = QStringLiteral("diagnostic");
        item["section"] = QStringLiteral("test_env");
        modules.append(item);
    }

    // 6) Debug privilege check.
    {
        QVariantMap item;
        item["id"] = QStringLiteral("debug_privilege");
        item["displayName"] = QStringLiteral("Privilège SeDebugName");
        item["description"] = QStringLiteral("Vérifie et active le privilège SeDebugName — requis pour tous les tests de breakpoint matériel et d'injection.");
        item["installed"] = false; // diagnostic dynamique
        item["status"] = QStringLiteral("unknown");
        item["detail"] = QStringLiteral("Cliquez sur 'Vérifier' pour tester.");
        item["installable"] = true;
        item["installKind"] = QStringLiteral("diagnostic");
        item["section"] = QStringLiteral("test_env");
        modules.append(item);
    }

    // --- MODULES-V2 : Section 3 — Sécurité / Stealth ---

    // 7) Stealth SC2 profile.
    {
        QVariantMap item;
        item["id"] = QStringLiteral("stealth_sc2_profile");
        item["displayName"] = QStringLiteral("Profil Stealth SC2");
        item["description"] = QStringLiteral("Applique le profil stealth SC2 (anti-debug PEB + process mask + dll mask) en un clic — pour les jeux AAA/online.");
        item["installed"] = m_stealthActive;
        item["status"] = m_stealthActive ? QStringLiteral("ok") : QStringLiteral("missing");
        item["detail"] = m_stealthActive
            ? QStringLiteral("Stealth actif (profil : %1)").arg(m_stealthProfile)
            : QStringLiteral("Stealth inactif — profils disponibles : sc2, default, minimal.");
        item["installable"] = true;
        item["installKind"] = QStringLiteral("stealth");
        item["section"] = QStringLiteral("stealth");
        modules.append(item);
    }

    // 8) Handle hider (kernel driver).
    {
        const QVariantMap drv = probeKernelDriver();
        const bool connected = drv.value("status").toString().compare(QStringLiteral("connected"), Qt::CaseInsensitive) == 0;
        QVariantMap item;
        item["id"] = QStringLiteral("handle_hider");
        item["displayName"] = QStringLiteral("Masquage de handles (kernel)");
        item["description"] = QStringLiteral("Masque les handles KillEngine dans la table de handles de la cible via le driver kernel (IOCTL 0x804) — invisible à NtQuerySystemInformation/SystemHandleTable.");
        item["installed"] = connected;
        item["status"] = connected ? QStringLiteral("ok") : QStringLiteral("missing");
        item["detail"] = connected
            ? QStringLiteral("Driver kernel actif — prêt à masquer des handles.")
            : QStringLiteral("Driver kernel non connecté — installez d'abord le module 'Driver noyau'.");
        item["installable"] = false; // action ponctuelle, pas d'installation
        item["installKind"] = QStringLiteral("stealth");
        item["section"] = QStringLiteral("stealth");
        modules.append(item);
    }

    result["modules"] = modules;
    return result;
}

QVariantMap ApplicationController::installModule(const QString& moduleId, const QVariantMap& options) {
    QVariantMap result;
    result["success"] = false;
    result["started"] = false;
    result["moduleId"] = moduleId;
    Q_UNUSED(options);

    if (m_moduleInstallInProgress) {
        result["error"] = QStringLiteral("Une installation de module est déjà en cours.");
        return result;
    }
    // MODULES-V2 : modules V2 (diagnostic/stealth) — pas d'installation async,
    // action synchrone directe.
    if (moduleId == QStringLiteral("edr_exclusion")) {
        return addEdrExclusionAsync(QString());
    }
    if (moduleId == QStringLiteral("debug_privilege")) {
        return enableDebugPrivilege();
    }
    if (moduleId == QStringLiteral("stealth_sc2_profile")) {
        return applyStealthProfile(QStringLiteral("sc2"));
    }
    if (moduleId == QStringLiteral("handle_hider")) {
        result["error"] = QStringLiteral("Utilisez la fonctionnalité de masquage de handle depuis l'Expert ou le pipe d'automatisation.");
        return result;
    }

    if (moduleId != QStringLiteral("lua_runtime")
        && moduleId != QStringLiteral("ai_model")
        && moduleId != QStringLiteral("clr_inspector")
        && moduleId != QStringLiteral("kernel_driver")) {
        result["error"] = QStringLiteral("Module inconnu : %1").arg(moduleId);
        return result;
    }

    // kernel_driver : invite UAC visible, async, même mécanisme que
    // installWebView2DeveloperModeCapability — le service Windows est créé et
    // démarré dans la fenêtre PowerShell élevée, pas de thread worker ici.
    if (moduleId == QStringLiteral("kernel_driver")) {
        const QString script = findModuleCatalogScript(QStringLiteral("install-kernel-driver.ps1"));
        if (script.isEmpty()) {
            result["error"] = QStringLiteral("scripts/install-kernel-driver.ps1 introuvable.");
            return result;
        }
        const std::wstring parameters =
            L"-NoProfile -ExecutionPolicy Bypass -File \"" + script.toStdWString() + L"\" -Configuration Release";
        SHELLEXECUTEINFOW sei{};
        sei.cbSize = sizeof(sei);
        sei.fMask = SEE_MASK_NOCLOSEPROCESS;
        sei.hwnd = nullptr;
        sei.lpVerb = L"runas";
        sei.lpFile = L"powershell.exe";
        sei.lpParameters = parameters.c_str();
        sei.nShow = SW_SHOWNORMAL;
        if (!ShellExecuteExW(&sei)) {
            const DWORD err = GetLastError();
            if (err == ERROR_CANCELLED) {
                result["cancelled"] = true;
                result["error"] = QStringLiteral("Invite UAC refusée par l'utilisateur.");
            } else {
                result["error"] = QStringLiteral("ShellExecuteExW a échoué (code %1).").arg(err);
            }
            return result;
        }
        if (sei.hProcess) {
            CloseHandle(sei.hProcess);
        }
        result["success"] = true;
        result["started"] = true;
        result["message"] = QStringLiteral(
            "Installation lancée dans une fenêtre PowerShell élevée (service Windows KillEngineKernel). "
            "Relancer le diagnostic Modules une fois terminé.");
        KE_LOG_INFO() << "installModule(kernel_driver): installation lancée (async, UAC affiché).";
        return result;
    }

    // lua_runtime / clr_inspector : script PowerShell local dans un thread worker.
    // ai_model : téléchargement curl.exe du GGUF officiel dans un thread worker.
    QString script;
    if (moduleId != QStringLiteral("ai_model")) {
        const QString scriptName = moduleId == QStringLiteral("lua_runtime")
            ? QStringLiteral("setup-lua-runtime.ps1")
            : QStringLiteral("build-clr-inspector.ps1");
        script = findModuleCatalogScript(scriptName);
        if (script.isEmpty()) {
            result["error"] = QStringLiteral("%1 introuvable.").arg(scriptName);
            return result;
        }
    }

    const int requestId = ++m_moduleInstallRequestId;
    const QString requestedModule = moduleId;
    const QString requestedScript = script;
    const QPointer<ApplicationController> self(this);
    auto cancellation = std::make_shared<killcore::CancellationToken>();

    m_moduleInstallInProgress = true;
    m_moduleInstallId = moduleId;
    m_activeModuleInstallCancellation = cancellation;

    KE_LOG_INFO() << "installModule(" << moduleId.toStdString() << ", requestId=" << requestId << ")";

    std::thread([self, requestId, requestedModule, requestedScript, cancellation]() {
        QVariantMap finished;
        finished["requestId"] = requestId;
        finished["moduleId"] = requestedModule;
        finished["success"] = false;

        if (requestedModule == QStringLiteral("ai_model")) {
            // Source officielle bartowski (même quantification Q4_K_M que le
            // modèle embarqué) — téléchargement en .partial puis renommage,
            // jamais de fichier final tronqué laissé derrière.
            const QString modelDir = findModuleCatalogModelDir();
            if (modelDir.isEmpty()) {
                finished["error"] = QStringLiteral("Dossier model/qwen introuvable.");
            } else {
                const QString url = QStringLiteral(
                    "https://huggingface.co/bartowski/Qwen_Qwen3.5-2B-GGUF/resolve/main/Qwen_Qwen3.5-2B-Q4_K_M.gguf");
                const QString dest = QDir(modelDir).filePath(QStringLiteral("Qwen_Qwen3.5-2B-Q4_K_M.gguf"));
                const QString partial = dest + QStringLiteral(".partial");
                QFile::remove(partial);

                QProcess proc;
                proc.setProgram(QStringLiteral("curl.exe"));
                proc.setArguments({
                    QStringLiteral("-L"), QStringLiteral("--fail"), QStringLiteral("--retry"), QStringLiteral("3"),
                    QStringLiteral("--connect-timeout"), QStringLiteral("30"),
                    QStringLiteral("-o"), partial, url,
                });
                proc.setProcessChannelMode(QProcess::MergedChannels);
                proc.start();
                if (!proc.waitForStarted(15000)) {
                    finished["error"] = QStringLiteral("curl.exe n'a pas démarré.");
                } else {
                    while (proc.state() == QProcess::Running) {
                        if (cancellation->isCancelled()) {
                            proc.kill();
                            finished["cancelled"] = true;
                            finished["message"] = QStringLiteral("Téléchargement annulé.");
                            QFile::remove(partial);
                            break;
                        }
                        if (proc.waitForReadyRead(500)) {
                            const QString line = QString::fromLocal8Bit(proc.readAllStandardOutput()).trimmed();
                            if (!line.isEmpty()) {
                                QVariantMap progress;
                                progress["requestId"] = requestId;
                                progress["moduleId"] = requestedModule;
                                progress["percent"] = -1;
                                progress["message"] = QStringLiteral("Téléchargement du modèle (~1,4 Go)…");
                                QMetaObject::invokeMethod(self.data(), [self, progress]() {
                                    if (self) {
                                        emit self->moduleInstallProgress(progress);
                                    }
                                }, Qt::QueuedConnection);
                            }
                        }
                    }
                    proc.waitForFinished(10000);
                    if (!finished.contains(QStringLiteral("cancelled"))) {
                        const bool ok = proc.exitStatus() == QProcess::NormalExit && proc.exitCode() == 0;
                        if (ok && QFile::exists(partial)) {
                            QFile::remove(dest);
                            QFile::rename(partial, dest);
                            finished["success"] = true;
                            finished["message"] = QStringLiteral("Modèle téléchargé : %1").arg(dest);
                        } else {
                            finished["error"] = QStringLiteral("Échec du téléchargement (code %1).").arg(proc.exitCode());
                            QFile::remove(partial);
                        }
                    }
                }
            }
        } else {
            QProcess proc;
            proc.setProgram(QStringLiteral("powershell.exe"));
            proc.setArguments({
                QStringLiteral("-NoProfile"),
                QStringLiteral("-ExecutionPolicy"), QStringLiteral("Bypass"),
                QStringLiteral("-File"), requestedScript,
            });
            if (requestedModule == QStringLiteral("lua_runtime")) {
                proc.arguments().append(QStringLiteral("-Force"));
            }
            proc.setProcessChannelMode(QProcess::MergedChannels);
            proc.start();
            if (!proc.waitForStarted(15000)) {
                finished["error"] = QStringLiteral("powershell.exe n'a pas démarré.");
            } else {
                while (proc.state() == QProcess::Running) {
                    if (cancellation->isCancelled()) {
                        proc.kill();
                        finished["cancelled"] = true;
                        finished["message"] = QStringLiteral("Installation annulée.");
                        break;
                    }
                    if (proc.waitForReadyRead(500)) {
                        const QString line = QString::fromLocal8Bit(proc.readAllStandardOutput()).trimmed();
                        if (!line.isEmpty()) {
                            QVariantMap progress;
                            progress["requestId"] = requestId;
                            progress["moduleId"] = requestedModule;
                            progress["percent"] = -1;
                            progress["message"] = line;
                            QMetaObject::invokeMethod(self.data(), [self, progress]() {
                                if (self) {
                                    emit self->moduleInstallProgress(progress);
                                }
                            }, Qt::QueuedConnection);
                        }
                    }
                }
                proc.waitForFinished(10000);
                if (!finished.contains(QStringLiteral("cancelled"))) {
                    const bool ok = proc.exitStatus() == QProcess::NormalExit && proc.exitCode() == 0;
                    finished["success"] = ok;
                    if (ok) {
                        finished["message"] = requestedModule == QStringLiteral("lua_runtime")
                            ? QStringLiteral("Runtime Lua installé dans runtime/lua.")
                            : QStringLiteral("Inspecteur CLR compilé.");
                    } else {
                        finished["error"] = QStringLiteral("Échec du script (code %1).").arg(proc.exitCode());
                    }
                }
            }
        }

        if (!self) {
            return;
        }
        QMetaObject::invokeMethod(self.data(), [self, finished]() {
            if (!self) {
                return;
            }
            self->m_moduleInstallInProgress = false;
            self->m_moduleInstallId.clear();
            self->m_activeModuleInstallCancellation.reset();
            self->appendScanTelemetry(QStringLiteral("module_install"), {
                {"moduleId", finished.value("moduleId")},
                {"success", finished.value("success")},
            });
            emit self->moduleInstallFinished(finished);
        }, Qt::QueuedConnection);
    }).detach();

    result["success"] = true;
    result["started"] = true;
    result["requestId"] = requestId;
    return result;
}

QVariantMap ApplicationController::cancelModuleInstall() {
    QVariantMap result;
    result["success"] = false;
    if (!m_moduleInstallInProgress || !m_activeModuleInstallCancellation) {
        result["error"] = QStringLiteral("Aucune installation de module en cours.");
        return result;
    }
    m_activeModuleInstallCancellation->cancel();
    result["success"] = true;
    return result;
}

// ---------------------------------------------------------------------------
// MODULES-V2 : Environnement de test + Sécurité/Stealth
// ---------------------------------------------------------------------------

namespace {
#ifdef Q_OS_WIN
// Lance une commande via ShellExecuteExW en élévation UAC (verbe "runas"),
// attend sa fin et rapporte success/error selon le code de sortie — même
// mécanisme que addEdrExclusion, factorisé ici pour les actions registre
// Defender (disable/re-enable) qui doivent toutes deux passer par ce chemin.
QVariantMap runElevatedCommand(const QString& program, const QString& args, DWORD timeoutMs = 30000) {
    QVariantMap result;
    result["success"] = false;

    const std::wstring cmd = program.toStdWString();
    const std::wstring argsW = args.toStdWString();

    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas";
    sei.lpFile = cmd.c_str();
    sei.lpParameters = argsW.c_str();
    sei.nShow = SW_HIDE;

    if (!ShellExecuteExW(&sei)) {
        const DWORD err = GetLastError();
        if (err == ERROR_CANCELLED) {
            result["error"] = QStringLiteral("L'utilisateur a refusé l'élévation UAC.");
        } else {
            result["error"] = QStringLiteral("ShellExecuteExW failed (error: %1)").arg(err);
        }
        return result;
    }

    if (sei.hProcess) {
        WaitForSingleObject(sei.hProcess, timeoutMs);
        DWORD exitCode = 0;
        GetExitCodeProcess(sei.hProcess, &exitCode);
        CloseHandle(sei.hProcess);

        if (exitCode == 0) {
            result["success"] = true;
        } else {
            result["error"] = QStringLiteral("La commande a retourné le code %1.").arg(exitCode);
        }
    }
    return result;
}
#endif
} // namespace

QVariantMap ApplicationController::setWindowsDefenderDisabledAsync(bool disabled) {
    QVariantMap started;
    started["success"] = false;
#ifdef Q_OS_WIN
    // Chercher le script .bat : d'abord dans le layout distribué (scripts/ à côté de l'exe),
    // puis dans le layout dev (../../scripts/).
    const QString appDir = QCoreApplication::applicationDirPath();
    const QString batName = QStringLiteral("disable_defender_registry.bat");
    QString batPath = QDir(appDir).filePath("scripts/" + batName);
    if (!QFile::exists(batPath)) {
        batPath = QDir(appDir).filePath("../../scripts/" + batName);
    }
    const QString batPathNative = QDir::toNativeSeparators(batPath);

    if (!QFile::exists(batPath)) {
        started["error"] = QStringLiteral("Script introuvable : %1 — utilise le fichier .bat manuellement.").arg(batPathNative);
        return started;
    }

    // L'élévation UAC + l'exécution du script tournent sur un thread séparé :
    // sinon le WaitForSingleObject(30s) de runElevatedCommand gèle tout le
    // thread GUI (Q_INVOKABLE via QWebChannel s'exécute sur le thread
    // propriétaire de l'objet) pendant toute la durée de l'invite UAC.
    const QPointer<ApplicationController> self(this);
    std::thread([self, batPathNative, disabled]() {
        const QString cmdArgs = QStringLiteral("/c \"%1\"").arg(batPathNative);
        QVariantMap result = runElevatedCommand(QStringLiteral("cmd.exe"), cmdArgs);

        if (result.value("success").toBool()) {
            result["disabled"] = disabled;
            result["message"] = disabled
                ? QStringLiteral("Script de désactivation exécuté. Vérifie la console pour le résultat. Redémarre Windows pour que les modifications prennent effet.")
                : QStringLiteral("Script de réactivation exécuté. Vérifie la console pour le résultat. Redémarre Windows pour que les modifications prennent effet.");
        }

        if (!self) return;
        QMetaObject::invokeMethod(self.data(), [self, result]() {
            if (!self) return;
            emit self->windowsDefenderDisabledFinished(result);
        }, Qt::QueuedConnection);
    }).detach();

    started["success"] = true;
    started["started"] = true;
#else
    started["error"] = QStringLiteral("Windows only");
#endif
    return started;
}

QVariantMap ApplicationController::setDefenderBehaviorMonitoringDisabledAsync(bool disabled) {
    QVariantMap started;
    started["success"] = false;
#ifdef Q_OS_WIN
    // Le script disable_defender_registry.bat gère déjà les deux clés
    // (DisableAntiSpyware + DisableBehaviorMonitoring). On le réutilise ici.
    const QString appDir = QCoreApplication::applicationDirPath();
    const QString batName = QStringLiteral("disable_defender_registry.bat");
    QString batPath = QDir(appDir).filePath("scripts/" + batName);
    if (!QFile::exists(batPath)) {
        batPath = QDir(appDir).filePath("../../scripts/" + batName);
    }
    const QString batPathNative = QDir::toNativeSeparators(batPath);

    if (!QFile::exists(batPath)) {
        started["error"] = QStringLiteral("Script introuvable : %1 — utilise le fichier .bat manuellement.").arg(batPathNative);
        return started;
    }

    const QPointer<ApplicationController> self(this);
    std::thread([self, batPathNative, disabled]() {
        const QString cmdArgs = QStringLiteral("/c \"%1\"").arg(batPathNative);
        QVariantMap result = runElevatedCommand(QStringLiteral("cmd.exe"), cmdArgs);

        if (result.value("success").toBool()) {
            result["disabled"] = disabled;
            result["message"] = disabled
                ? QStringLiteral("Script de désactivation exécuté. Vérifie la console pour le résultat. Redémarre Windows pour que les modifications prennent effet.")
                : QStringLiteral("Script de réactivation exécuté. Vérifie la console pour le résultat. Redémarre Windows pour que les modifications prennent effet.");
        }

        if (!self) return;
        QMetaObject::invokeMethod(self.data(), [self, result]() {
            if (!self) return;
            emit self->defenderBehaviorMonitoringDisabledFinished(result);
        }, Qt::QueuedConnection);
    }).detach();

    started["success"] = true;
    started["started"] = true;
#else
    started["error"] = QStringLiteral("Windows only");
#endif
    return started;
}

QVariantMap ApplicationController::checkEdrBlocking() const {
    QVariantMap result;
    result["success"] = false;
    result["blocked"] = true;
    result["error"] = KE_TXT("Aucun processus attaché.", "No process attached.");

    if (!m_handle.isValid()) {
        return result;
    }

    // Incrémenter le compteur de tentatives (rate limiting EDR detection)
    ++m_edrCheckCount;
    result["attemptCount"] = m_edrCheckCount;

#ifdef Q_OS_WIN
    HANDLE hProcess = OpenProcess(
        PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION,
        FALSE, static_cast<DWORD>(m_pid));
    if (!hProcess) {
        result["error"] = QStringLiteral("OpenProcess failed (error: %1)").arg(GetLastError());
        return result;
    }

    // Test 1 : VirtualAllocEx
    LPVOID mem = VirtualAllocEx(hProcess, nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    const DWORD allocErr = GetLastError();
    if (!mem) {
        CloseHandle(hProcess);
        result["blocked"] = true;
        result["stage"] = QStringLiteral("VirtualAllocEx");
        result["errorCode"] = static_cast<int>(allocErr);
        if (allocErr == ERROR_ACCESS_DENIED) {
            result["error"] = QStringLiteral(
                "VirtualAllocEx bloqué (ACCESS_DENIED) — signature EDR/Defender for Endpoint détectée. "
                "Ajoutez une exclusion pour le dossier build/bin dans Windows Security.");
        } else {
            result["error"] = QStringLiteral("VirtualAllocEx failed (error: %1)").arg(allocErr);
        }
        result["success"] = true;
        return result;
    }

    // Test 2 : WriteProcessMemory (write dummy bytes)
    const unsigned char dummy[16] = {0};
    SIZE_T written = 0;
    const BOOL wpmOk = WriteProcessMemory(hProcess, mem, dummy, sizeof(dummy), &written);
    const DWORD wpmErr = GetLastError();
    if (!wpmOk) {
        VirtualFreeEx(hProcess, mem, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        result["blocked"] = true;
        result["stage"] = QStringLiteral("WriteProcessMemory");
        result["errorCode"] = static_cast<int>(wpmErr);
        result["error"] = QStringLiteral("WriteProcessMemory bloqué (error: %1)").arg(wpmErr);
        result["success"] = true;
        return result;
    }

    // Test 3 : GetProcAddress + CreateRemoteThread (simulation LoadLibrary)
    HMODULE hKernel32 = GetModuleHandleW(L"kernel32.dll");
    FARPROC loadLibraryAddr = nullptr;
    if (hKernel32) {
        loadLibraryAddr = GetProcAddress(hKernel32, "LoadLibraryW");
    }
    if (!loadLibraryAddr) {
        // Pas un blocage EDR, juste un problème local — on considère que c'est OK
        VirtualFreeEx(hProcess, mem, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        result["blocked"] = false;
        result["success"] = true;
        result["message"] = QStringLiteral("GetProcAddress(Local LoadLibraryW) échoué, mais VirtualAllocEx/WriteProcessMemory OK.");
        return result;
    }

    HANDLE hThread = CreateRemoteThread(hProcess, nullptr, 0,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(loadLibraryAddr), mem, 0, nullptr);
    const DWORD crtErr = GetLastError();
    VirtualFreeEx(hProcess, mem, 0, MEM_RELEASE);

    // Vérifier si le process cible a été tué par l'EDR pendant le test
    // (certains EDR tuent le process cible au lieu de bloquer l'allocation)
    DWORD exitCode = STILL_ACTIVE;
    if (GetExitCodeProcess(hProcess, &exitCode) && exitCode != STILL_ACTIVE) {
        CloseHandle(hProcess);
        if (hThread) CloseHandle(hThread);
        result["blocked"] = true;
        result["stage"] = QStringLiteral("ProcessKilled");
        result["errorCode"] = static_cast<int>(exitCode);
        result["processKilled"] = true;
        result["error"] = QStringLiteral(
            "Le processus cible a été tué pendant le test — signature EDR/Defender active. "
            "L'EDR a détecté l'injection simulée et a terminé le process. "
            "Ajoutez une exclusion pour le dossier build/bin dans Windows Security.");
        result["success"] = true;

        // Émettre le signal et détacher proprement
        QVariantMap killInfo;
        killInfo["pid"] = m_pid;
        killInfo["processName"] = m_processName;
        killInfo["exitCode"] = static_cast<int>(exitCode);
        ApplicationController* self = const_cast<ApplicationController*>(this);
        emit self->processKilledByEdr(killInfo);

        // Détacher le process (il est mort, le handle est invalide)
        QMetaObject::invokeMethod(self, "detachProcess", Qt::QueuedConnection);

        return result;
    }

    CloseHandle(hProcess);

    if (!hThread) {
        result["blocked"] = true;
        result["stage"] = QStringLiteral("CreateRemoteThread");
        result["errorCode"] = static_cast<int>(crtErr);
        if (crtErr == ERROR_ACCESS_DENIED) {
            result["error"] = QStringLiteral(
                "CreateRemoteThread bloqué (ACCESS_DENIED) — signature EDR/Defender for Endpoint détectée. "
                "Ajoutez une exclusion pour le dossier build/bin dans Windows Security.");
        } else {
            result["error"] = QStringLiteral("CreateRemoteThread failed (error: %1)").arg(crtErr);
        }
        result["success"] = true;
        return result;
    }

    // Tout a réussi — pas de blocage EDR détecté
    CloseHandle(hThread);
    result["blocked"] = false;
    result["success"] = true;

    // Avertissement rate limiting EDR : les premières tentatives peuvent passer
    // car l'EDR est en mode apprentissage. Le résultat n'est fiable qu'après
    // plusieurs tentatives ou après avoir ajouté l'exclusion.
    if (m_edrCheckCount < 4) {
        result["provisional"] = true;
        result["message"] = QStringLiteral(
            "Aucun blocage détecté à cette tentative (%1), mais l'EDR peut être en mode apprentissage. "
            "Le résultat n'est pas encore fiable — ne clique pas plusieurs fois de suite (rate limiting). "
            "Ajoute l'exclusion PowerShell pour un résultat définitif.")
            .arg(m_edrCheckCount);
    } else {
        result["provisional"] = false;
        result["message"] = QStringLiteral(
            "Aucun blocage EDR détecté après %1 tentatives — injection de code fonctionnelle sur cette machine.")
            .arg(m_edrCheckCount);
    }
#else
    result["error"] = QStringLiteral("EDR check is Windows-only");
#endif

    return result;
}

QVariantMap ApplicationController::addEdrExclusionAsync(const QString& path) {
    QVariantMap started;
    started["success"] = false;

    const QString exclusionPath = path.isEmpty()
        ? QDir(QCoreApplication::applicationDirPath()).filePath("bin")
        : path;

#ifdef Q_OS_WIN
    // Chercher le script .bat : d'abord dans le layout distribué (scripts/ à côté de l'exe),
    // puis dans le layout dev (../../scripts/).
    const QString appDir = QCoreApplication::applicationDirPath();
    const QString batName = QStringLiteral("add_defender_exclusion.bat");
    QString batPath = QDir(appDir).filePath("scripts/" + batName);
    if (!QFile::exists(batPath)) {
        batPath = QDir(appDir).filePath("../../scripts/" + batName);
    }
    const QString batPathNative = QDir::toNativeSeparators(batPath);
    const bool useBat = QFile::exists(batPath);

    // Même raisonnement que setWindowsDefenderDisabledAsync : l'élévation UAC
    // (ShellExecuteExW "runas") et son attente tournent sur un thread séparé
    // pour ne pas geler le thread GUI pendant toute la durée de l'invite UAC
    // + l'exécution de la commande.
    const QPointer<ApplicationController> self(this);
    std::thread([self, exclusionPath, batPathNative, useBat]() {
        QVariantMap result;
        result["success"] = false;

        if (!useBat) {
            // Fallback : exécuter PowerShell directement si le .bat est absent
            const std::wstring cmd = L"powershell.exe";
            const std::wstring args =
                L"-NoProfile -Command \"Add-MpPreference -ExclusionPath '" +
                exclusionPath.toStdWString() + L"'\"";

            SHELLEXECUTEINFOW sei{};
            sei.cbSize = sizeof(sei);
            sei.fMask = SEE_MASK_NOCLOSEPROCESS;
            sei.lpVerb = L"runas";
            sei.lpFile = cmd.c_str();
            sei.lpParameters = args.c_str();
            sei.nShow = SW_HIDE;

            if (!ShellExecuteExW(&sei)) {
                const DWORD err = GetLastError();
                if (err == ERROR_CANCELLED) {
                    result["error"] = QStringLiteral("L'utilisateur a refusé l'élévation UAC.");
                } else {
                    result["error"] = QStringLiteral("ShellExecuteExW failed (error: %1)").arg(err);
                }
            } else if (sei.hProcess) {
                WaitForSingleObject(sei.hProcess, 30000);
                DWORD exitCode = 0;
                GetExitCodeProcess(sei.hProcess, &exitCode);
                CloseHandle(sei.hProcess);

                if (exitCode == 0) {
                    result["success"] = true;
                    result["message"] = QStringLiteral("Exclusion ajoutée : %1").arg(exclusionPath);
                } else {
                    result["error"] = QStringLiteral("PowerShell a retourné le code %1.").arg(exitCode);
                }
            }
        } else {
            const QString cmdArgs = QStringLiteral("/c \"%1\"").arg(batPathNative);
            result = runElevatedCommand(QStringLiteral("cmd.exe"), cmdArgs);

            if (result.value("success").toBool()) {
                result["message"] = QStringLiteral("Script d'exclusion exécuté. Vérifie la console pour le résultat.");
            }
        }

        if (!self) return;
        QMetaObject::invokeMethod(self.data(), [self, result]() {
            if (!self) return;
            emit self->edrExclusionAddedFinished(result);
        }, Qt::QueuedConnection);
    }).detach();

    started["success"] = true;
    started["started"] = true;
#else
    started["error"] = QStringLiteral("EDR exclusion is Windows-only");
#endif

    return started;
}

QVariantMap ApplicationController::checkDebugPrivilege() const {
    QVariantMap result;
    result["success"] = false;
    result["hasDebugPrivilege"] = false;

#ifdef Q_OS_WIN
    HANDLE hToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
        result["error"] = QStringLiteral("OpenProcessToken failed (error: %1)").arg(GetLastError());
        return result;
    }

    TOKEN_PRIVILEGES tp{};
    DWORD size = 0;
    if (GetTokenInformation(hToken, TokenPrivileges, &tp, sizeof(tp), &size) ||
        GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
        // Allouer la bonne taille
        std::vector<BYTE> buffer(size);
        if (GetTokenInformation(hToken, TokenPrivileges, buffer.data(), static_cast<DWORD>(buffer.size()), &size)) {
            const auto* privileges = reinterpret_cast<const TOKEN_PRIVILEGES*>(buffer.data());
            bool found = false;
            for (DWORD i = 0; i < privileges->PrivilegeCount; ++i) {
                wchar_t name[64] = {};
                DWORD nameLen = sizeof(name) / sizeof(wchar_t);
                LUID luid = privileges->Privileges[i].Luid;
                LookupPrivilegeNameW(nullptr, &luid, name, &nameLen);
                if (QString::fromWCharArray(name) == QString::fromWCharArray(SE_DEBUG_NAME)) {
                    found = true;
                    const bool enabled = (privileges->Privileges[i].Attributes & SE_PRIVILEGE_ENABLED) != 0;
                    result["hasDebugPrivilege"] = true;
                    result["enabled"] = enabled;
                    result["success"] = true;
                    result["message"] = enabled
                        ? QStringLiteral("SeDebugName est actif.")
                        : QStringLiteral("SeDebugName est présent mais désactivé.");
                    break;
                }
            }
            if (!found) {
                result["hasDebugPrivilege"] = false;
                result["success"] = true;
                result["message"] = QStringLiteral("SeDebugName n'est pas présent dans les privilèges du token.");
            }
        }
    }
    CloseHandle(hToken);
#else
    result["error"] = QStringLiteral("Debug privilege check is Windows-only");
#endif

    return result;
}

QVariantMap ApplicationController::enableDebugPrivilege() {
    QVariantMap result;
    result["success"] = false;

#ifdef Q_OS_WIN
    HANDLE hToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken)) {
        result["error"] = QStringLiteral("OpenProcessToken failed (error: %1)").arg(GetLastError());
        return result;
    }

    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (!LookupPrivilegeValueW(nullptr, SE_DEBUG_NAME, &tp.Privileges[0].Luid)) {
        result["error"] = QStringLiteral("LookupPrivilegeValueW failed (error: %1)").arg(GetLastError());
        CloseHandle(hToken);
        return result;
    }

    const BOOL ok = AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(tp), nullptr, nullptr);
    const DWORD err = GetLastError();
    CloseHandle(hToken);

    if (!ok || err == ERROR_NOT_ALL_ASSIGNED) {
        result["error"] = QStringLiteral("AdjustTokenPrivileges failed (error: %1). Vérifiez que vous avez les droits admin.").arg(err);
        return result;
    }

    result["success"] = true;
    result["message"] = QStringLiteral("SeDebugName activé avec succès.");
#else
    result["error"] = QStringLiteral("Debug privilege enable is Windows-only");
#endif

    return result;
}

QVariantMap ApplicationController::applyStealthProfile(const QString& profile) {
    // Réutilise applyStealthMode existant — c'est déjà un Q_INVOKABLE
    // mais on veut un retour explicite pour la vue Modules.
    return applyStealthMode(profile);
}

QVariantMap ApplicationController::restoreStealthProfile() {
    // Réutilise restoreStealthMode existant.
    return restoreStealthMode();
}

QVariantMap ApplicationController::hideHandle(uint64_t ownerPid, uint64_t handleValue) {
    QVariantMap result;
    result["success"] = false;

    // Utilise le kernel driver manager existant (signature QString-based).
    const auto drvResult = m_kernelDriverManager->handleTable(
        QString::number(ownerPid),
        QStringLiteral("0x") + QString::number(handleValue, 16),
        true /* hide */);

    if (!drvResult.value("success").toBool()) {
        result["error"] = drvResult.value("error").toString();
        return result;
    }

    result["success"] = true;
    result["message"] = drvResult.value("message").toString();
    return result;
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
        ? KE_TXT("Aucun interpréteur Lua trouvé dans runtime/lua, lua, le dossier de l'application ou le PATH.",
                 "No Lua interpreter found in runtime/lua, lua, the application folder, or PATH.")
        : KE_TXT("Lua externe prêt. Les appels KillEngine passent par le pipe d'automatisation local.",
                 "External Lua ready. KillEngine calls go through the local automation pipe.");
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
        result["error"] = KE_TXT("Un script Lua est déjà en cours d'exécution.", "A Lua script is already running.");
        return result;
    }
    if (scriptText.trimmed().isEmpty()) {
        result["error"] = KE_TXT("Script Lua vide.", "Empty Lua script.");
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
        result["error"] = KE_TXT("Aucun script Lua actif à annuler.", "No active Lua script to cancel.");
        return result;
    }

    m_activeLuaScriptCancellation->cancel();
    result["success"] = true;
    result["error"] = "";
    return result;
}

QVariantMap ApplicationController::startLuaRepl(const QVariantMap& options) {
    return m_luaReplManager->start(options);
}

QVariantMap ApplicationController::sendLuaReplLine(const QString& line) {
    return m_luaReplManager->sendLine(line);
}

QVariantMap ApplicationController::getLuaReplLineResult(int requestId) const {
    return m_luaReplManager->lineResult(requestId);
}

QVariantMap ApplicationController::getLuaReplHistory(int maxEntries) const {
    return m_luaReplManager->history(maxEntries);
}

QVariantMap ApplicationController::getLuaReplCompletions(const QString& prefix) const {
    return m_luaReplManager->completions(prefix);
}

QVariantMap ApplicationController::stopLuaRepl() {
    return m_luaReplManager->stop();
}

QVariantMap ApplicationController::getLuaReplStatus() const {
    return m_luaReplManager->status();
}

QVariantMap ApplicationController::startMemoryHeatmap(const QString& addressHex, const QVariantMap& options) {
    QVariantMap result;
    result["success"] = false;

    if (!m_handle.isValid()) {
        result["error"] = KE_TXT("Aucun processus attaché.", "No process attached.");
        return result;
    }

    QVariantMap mergedOptions = options;
    const QString trimmedAddress = addressHex.trimmed();
    if (!trimmedAddress.isEmpty()) {
        bool ok = false;
        const quint64 minAddress = trimmedAddress.toULongLong(&ok, 16);
        if (!ok) {
            result["error"] = KE_TXT("Adresse de base invalide (attendu hexadécimal, ex. 7ff600000000).", "Invalid base address (expected hexadecimal, e.g. 7ff600000000).");
            return result;
        }
        mergedOptions["minAddress"] = static_cast<qulonglong>(minAddress);
    }

    const quint64 nativeHandle = reinterpret_cast<quint64>(m_handle.rawHandle());
    const bool started = m_memoryHeatmapManager->startHeatmapCollection(nativeHandle, mergedOptions);
    result["success"] = started;
    if (!started) {
        result["error"] = KE_TXT("Impossible de démarrer la collecte heatmap (déjà en cours ?).", "Unable to start heatmap collection (already running?).");
    }
    return result;
}

QVariantMap ApplicationController::stopMemoryHeatmap() {
    QVariantMap result;
    m_memoryHeatmapManager->stopHeatmapCollection();
    result["success"] = true;
    result["stats"] = m_memoryHeatmapManager->getHeatmapStats();
    result["topRegions"] = m_memoryHeatmapManager->getTopHeatmapRegions(100);
    return result;
}

QVariantMap ApplicationController::getMemoryHeatmapStatus() const {
    QVariantMap result;
    result["success"] = true;
    result["collecting"] = m_memoryHeatmapManager->isCollecting();
    result["stats"] = m_memoryHeatmapManager->getHeatmapStats();
    return result;
}

QVariantMap ApplicationController::getMemoryHeatmapData() const {
    QVariantMap result;
    result["success"] = true;
    result["topRegions"] = m_memoryHeatmapManager->getTopHeatmapRegions(100);
    result["stats"] = m_memoryHeatmapManager->getHeatmapStats();
    return result;
}

QVariantMap ApplicationController::addTimelineAddress(const QString& addressHex, int valueSize) {
    QVariantMap result;
    result["success"] = m_memoryTimelineManager->addAddress(addressHex, valueSize);
    if (!result["success"].toBool()) {
        result["error"] = KE_TXT("Adresse invalide (attendu hexadécimal, ex. 7ff600000000).", "Invalid address (expected hexadecimal, e.g. 7ff600000000).");
    }
    return result;
}

QVariantMap ApplicationController::removeTimelineAddress(const QString& addressHex) {
    QVariantMap result;
    result["success"] = m_memoryTimelineManager->removeAddress(addressHex);
    return result;
}

QVariantMap ApplicationController::clearTimelineAddresses() {
    m_memoryTimelineManager->clearAddresses();
    QVariantMap result;
    result["success"] = true;
    return result;
}

QVariantMap ApplicationController::getTimelineWatchedAddresses() const {
    QVariantMap result;
    result["success"] = true;
    result["addresses"] = m_memoryTimelineManager->getWatchedAddresses();
    return result;
}

QVariantMap ApplicationController::setTimelineConfig(const QVariantMap& options) {
    if (options.contains("samplingIntervalMs")) {
        m_memoryTimelineManager->setSamplingInterval(options.value("samplingIntervalMs").toInt());
    }
    if (options.contains("maxDurationMs")) {
        m_memoryTimelineManager->setMaxDuration(options.value("maxDurationMs").toInt());
    }
    if (options.contains("trackOnlyChanges")) {
        m_memoryTimelineManager->setTrackOnlyChanges(options.value("trackOnlyChanges").toBool());
    }
    QVariantMap result;
    result["success"] = true;
    result["config"] = m_memoryTimelineManager->getConfig();
    return result;
}

QVariantMap ApplicationController::getTimelineConfig() const {
    QVariantMap result;
    result["success"] = true;
    result["config"] = m_memoryTimelineManager->getConfig();
    return result;
}

QVariantMap ApplicationController::startTimelineCollection() {
    QVariantMap result;
    if (!m_handle.isValid()) {
        result["success"] = false;
        result["error"] = KE_TXT("Aucun processus attaché.", "No process attached.");
        return result;
    }
    m_memoryTimelineManager->setProcessHandle(m_handle.rawHandle());
    const bool started = m_memoryTimelineManager->startCollection();
    result["success"] = started;
    if (!started) {
        result["error"] = KE_TXT("Impossible de démarrer (aucune adresse surveillée, ou collecte déjà en cours ?).", "Unable to start (no watched address, or collection already running?).");
    }
    return result;
}

QVariantMap ApplicationController::stopTimelineCollection() {
    m_memoryTimelineManager->stopCollection();
    QVariantMap result;
    result["success"] = true;
    return result;
}

QVariantMap ApplicationController::getTimelineStatus() const {
    QVariantMap result;
    result["success"] = true;
    result["collecting"] = m_memoryTimelineManager->isCollecting();
    result["watchedAddressCount"] = m_memoryTimelineManager->watchedAddressCount();
    result["stats"] = m_memoryTimelineManager->currentStats();
    return result;
}

QVariantMap ApplicationController::getTimelineSeriesForAddress(const QString& addressHex) const {
    QVariantMap result;
    result["success"] = true;
    result["series"] = m_memoryTimelineManager->getSeriesForAddress(addressHex);
    return result;
}

QVariantMap ApplicationController::getAllTimelineSeries() const {
    QVariantMap result;
    result["success"] = true;
    result["series"] = m_memoryTimelineManager->getAllSeries();
    return result;
}

QVariantMap ApplicationController::findVolatileTimelineAddresses(double threshold) {
    QVariantMap result;
    result["success"] = true;
    result["addresses"] = m_memoryTimelineManager->findVolatileAddresses(threshold);
    return result;
}

QVariantMap ApplicationController::findStableTimelineAddresses(int minDurationMs) {
    QVariantMap result;
    result["success"] = true;
    result["addresses"] = m_memoryTimelineManager->findStableAddresses(minDurationMs);
    return result;
}

QVariantMap ApplicationController::exportTimelineToJson() {
    QVariantMap result;
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + "/KillEngine/timeline";
    QDir().mkpath(dir);
    const QString filePath = dir + "/timeline_" + QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss") + ".json";
    result["success"] = m_memoryTimelineManager->exportToJson(filePath);
    result["filepath"] = filePath;
    if (!result["success"].toBool()) {
        result["error"] = KE_TXT("Échec de l'export JSON.", "JSON export failed.");
    }
    return result;
}

QVariantMap ApplicationController::exportTimelineToCsv() {
    QVariantMap result;
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + "/KillEngine/timeline";
    QDir().mkpath(dir);
    const QString filePath = dir + "/timeline_" + QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss") + ".csv";
    result["success"] = m_memoryTimelineManager->exportToCsv(filePath);
    result["filepath"] = filePath;
    if (!result["success"].toBool()) {
        result["error"] = KE_TXT("Échec de l'export CSV.", "CSV export failed.");
    }
    return result;
}

QVariantMap ApplicationController::detectTimelinePatterns(const QString& addressHex) {
    return m_memoryTimelineManager->detectPatterns(addressHex);
}

QVariantMap ApplicationController::analyzeTimelineBehavior(const QString& addressHex) {
    return m_memoryTimelineManager->analyzeBehavior(addressHex);
}

QVariantMap ApplicationController::predictTimelineNextValue(const QString& addressHex) {
    return m_memoryTimelineManager->predictNextValue(addressHex);
}

QVariantMap ApplicationController::findTimelineCorrelations() {
    QVariantMap result;
    result["success"] = true;
    result["correlations"] = m_memoryTimelineManager->findCorrelations();
    return result;
}

QVariantMap ApplicationController::generateTimelineReport() {
    return m_memoryTimelineManager->generateReport();
}

bool ApplicationController::isPatternLearningInitialized() const {
    return m_patternLearningManager->isInitialized();
}

QVariantMap ApplicationController::getPatternLearningStatistics() const {
    return m_patternLearningManager->getStatistics();
}

QVariantMap ApplicationController::detectGameEngine(const QVariantList& moduleNames, const QVariantMap& memorySample) {
    return m_patternLearningManager->detectEngine(moduleNames, memorySample);
}

QVariantMap ApplicationController::classifyMemoryPattern(const QString& addressHex, const QVariantList& valueHistory, const QVariantList& timestamps) {
    return m_patternLearningManager->classifyPattern(addressHex, valueHistory, timestamps);
}

QVariantMap ApplicationController::loadGameProfile(const QString& gameName) {
    return m_patternLearningManager->loadProfile(gameName);
}

bool ApplicationController::saveGameProfile(const QVariantMap& profile) {
    return m_patternLearningManager->saveProfile(profile);
}

QVariantList ApplicationController::listKnownGameProfiles() {
    return m_patternLearningManager->listKnownGames();
}

bool ApplicationController::deleteGameProfile(const QString& gameName) {
    return m_patternLearningManager->deleteProfile(gameName);
}

void ApplicationController::recordLearningSession(const QVariantMap& session) {
    m_patternLearningManager->recordSession(session);
}

QVariantList ApplicationController::suggestPatternResolutionPaths(const QString& gameName, int targetType) {
    return m_patternLearningManager->suggestResolutionPaths(gameName, targetType);
}

QVariantList ApplicationController::suggestPatternValueTypes(int engineType, int patternType) {
    return m_patternLearningManager->suggestValueTypes(engineType, patternType);
}

double ApplicationController::getPatternValueTypeSuccessRate(const QString& gameName, const QString& valueType) {
    return m_patternLearningManager->getTypeSuccessRate(gameName, valueType);
}

QVariantList ApplicationController::clusterPatternAddresses(const QVariantList& addresses, const QVariantList& features) {
    return m_patternLearningManager->clusterAddresses(addresses, features);
}

void ApplicationController::startPatternTracking(const QString& addressHex, const QString& valueType) {
    m_patternLearningManager->startPatternTracking(addressHex, valueType);
}

void ApplicationController::stopPatternTracking(const QString& addressHex) {
    m_patternLearningManager->stopPatternTracking(addressHex);
}

void ApplicationController::recordPatternTrackingValue(const QString& addressHex, double value) {
    m_patternLearningManager->recordValue(addressHex, value);
}

QVariantMap ApplicationController::getPatternTrackingAnalysis(const QString& addressHex) {
    return m_patternLearningManager->getPatternAnalysis(addressHex);
}

QVariantMap ApplicationController::analyzePatternCandidates(const QVariantList& candidates) {
    return m_patternLearningManager->analyzeCandidates(candidates);
}

QVariantList ApplicationController::getTopPatternSuggestions(const QString& gameName, int patternType, int count) {
    return m_patternLearningManager->getTopSuggestions(gameName, patternType, count);
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
        result["error"] = KE_TXT("Action non autorisee (liste blanche C++ callVueStoreAction) : ", "Action not authorized (C++ callVueStoreAction whitelist): ") + action;
        return result;
    }
    if (!m_webEnginePage) {
        result["error"] = KE_TXT("QWebEnginePage non initialisee (setWebEnginePage jamais appele).", "QWebEnginePage not initialized (setWebEnginePage never called).");
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
        result["error"] = KE_TXT("Timeout (5s) en attendant la reponse JS -- la page a-t-elle bien fini de charger le store ?",
                                  "Timeout (5s) waiting for the JS response -- has the page finished loading the store?");
        return result;
    }

    const QVariantMap jsMap = state->jsResult.toMap();
    const bool jsSuccess = jsMap.value("success", false).toBool();
    result["success"] = jsSuccess;
    if (jsSuccess) {
        result["result"] = jsMap.value("result");
    } else {
        result["error"] = jsMap.value("error", KE_TXT("Erreur JS inconnue (reponse non reconnue).", "Unknown JS error (unrecognized response).")).toString();
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

QVariantMap ApplicationController::setExternalAiApiKey(const QString& apiKey) {
    return m_claudeChatManager->setApiKey(apiKey);
}

QVariantMap ApplicationController::clearExternalAiApiKey() {
    return m_claudeChatManager->clearApiKey();
}

bool ApplicationController::hasExternalAiApiKey() const {
    return m_claudeChatManager->hasApiKey();
}

QVariantMap ApplicationController::setActiveAiBackend(const QString& backend) {
    return m_claudeChatManager->setActiveBackend(backend);
}

QString ApplicationController::getActiveAiBackend() const {
    return m_claudeChatManager->activeBackend();
}

int ApplicationController::getExternalAiRequestCount() const {
    return m_claudeChatManager->requestCount();
}

void ApplicationController::resolveClaudePendingAction(const QString& pendingId, const QVariantMap& result) {
    m_claudeChatManager->resolvePendingAction(pendingId, result);
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
        result["error"] = KE_TXT("Aucun processus attaché.", "Not attached to a process.");
        return result;
    }
    if (m_stealthActive) {
        result["success"] = false;
        result["error"] = KE_TXT("Mode stealth déjà actif (profil : ", "Stealth mode already active (profile: ") + m_stealthProfile + ")";
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
        result["error"] = KE_TXT("Profil inconnu : ", "Unknown profile: ") + profile + KE_TXT(". Profils supportés : sc2, default, minimal", ". Supported: sc2, default, minimal");
        return result;
    }

    QStringList errors;
    int modulesActivated = 0;

    if (antiDebug) {
        auto antiResult = m_antiDebugSession.start(static_cast<uint32_t>(m_pid));
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
        result["error"] = KE_TXT("Aucun module stealth activé.", "No stealth modules activated.");
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
        result["error"] = KE_TXT("Mode stealth non actif.", "Stealth mode is not active.");
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

QVariantMap ApplicationController::getStealthStatus() const {
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

QVariantMap ApplicationController::analyzeStealthRisk() const {
    QVariantMap result;
    if (!m_attached) {
        result["success"] = false;
        result["error"] = KE_TXT("Aucun processus attaché.", "No process attached.");
        return result;
    }

    const auto modules = killcore::ProcessEnumerator::enumerateModules(static_cast<uint32_t>(m_pid));
    QStringList moduleNames;
    moduleNames.reserve(modules.size());
    for (const auto& module : modules) {
        moduleNames.append(module.name);
    }

    BOOL debuggerPresent = FALSE;
    CheckRemoteDebuggerPresent(m_handle.rawHandle(), &debuggerPresent);

    const auto analysis = killcore::StealthProfiler::analyze(
        moduleNames,
        debuggerPresent != FALSE,
        m_antiDebugSession.isActive(),
        m_stealthProcessMaskActive,
        m_stealthDllMaskActive);

    result = analysis.toVariantMap();
    result["success"] = true;
    result["moduleCount"] = moduleNames.size();
    result["stealthActive"] = m_stealthActive;
    result["stealthProfile"] = m_stealthProfile;
    return result;
}

} // namespace killengine
