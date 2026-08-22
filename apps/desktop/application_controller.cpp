#include "application_controller.h"

#ifdef Q_OS_WIN
#include <windows.h>
#include <shellapi.h>
#endif

#include "auto_resolver.h"
#include "model_locator.h"
#include "candidates/candidate_store.h"
#include "crash_handler.h"
#include "debug/hardware_breakpoint.h"
#include "logging/logger.h"
#include "memory/memory_map.h"
#include "memory/memory_reader.h"
#include "memory/memory_writer.h"
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
#include <QLabel>
#include <QMetaObject>
#include <QPointer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QVBoxLayout>
#include <QWidget>
#include <QSettings>
#include <QSet>
#include <QStandardPaths>
#include <QTemporaryFile>

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

double ratePerSecond(size_t count, qint64 elapsedMs) {
    if (elapsedMs <= 0) {
        return 0.0;
    }
    return static_cast<double>(count) * 1000.0 / static_cast<double>(elapsedMs);
}

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

// H4 (docs/STRATEGY_ROOM.md, session Solitaire du 19/08/2026) : ERROR_PARTIAL_COPY
// (299) est renvoyé par ReadProcessMemory sur des pages de code protégées
// contre la lecture externe — observé concrètement sur un exécutable
// Microsoft Store/UWP signé (Solitaire.exe). Le breakpoint matériel peut
// quand même observer l'écriture (registres/RIP), seule la lecture des
// octets d'instruction est bloquée : generateAobSignature/suggestCodePatches
// échouent alors systématiquement, pas par bug mais par protection de la
// cible. Sans ce message, l'erreur Win32 brute ne dit rien de tout ça.
QString codeReadProtectionHint(uint32_t errorCode) {
    if (errorCode == 299) {
        return QStringLiteral(
            "Le code de ce module semble protégé contre la lecture externe "
            "(fréquent sur les exécutables Microsoft Store/UWP signés). "
            "Génération de signature/patch impossible sur cette instruction — "
            "essaie Freeze ou une écriture groupée sur la donnée plutôt qu'un "
            "patch du code.");
    }
    return QString();
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

struct UiInvestigationWindowState {
    uint64_t base{0};
    QByteArray before;
    QString label;
    QString reason;
};

struct UiInvestigationProbeBlockState {
    uint64_t base{0};
    int size{0};
    uint64_t hash{0};
    QString protection;
    QString memoryType;
};

QList<UiInvestigationWindowState>& uiInvestigationWindows() {
    static QList<UiInvestigationWindowState> windows;
    return windows;
}

QList<UiInvestigationProbeBlockState>& uiInvestigationProbeBlocks() {
    static QList<UiInvestigationProbeBlockState> blocks;
    return blocks;
}

uint64_t uiInvestigationHash(const QByteArray& bytes) {
    uint64_t hash = 1469598103934665603ull;
    for (const unsigned char byte : bytes) {
        hash ^= static_cast<uint64_t>(byte);
        hash *= 1099511628211ull;
    }
    return hash;
}

} // namespace

ApplicationController::ApplicationController(QObject* parent)
    : QObject(parent) {
    const size_t candidateThreshold = candidateFileBackedThresholdFromSettings();
    m_candidates.setFileBackedThreshold(candidateThreshold);
    m_previousCandidates.setFileBackedThreshold(candidateThreshold);
    m_freezeTimer.setInterval(100);
    connect(&m_freezeTimer, &QTimer::timeout, this, &ApplicationController::applyFreezeTick);
    m_writeWatchTimer.setInterval(1500);
    connect(&m_writeWatchTimer, &QTimer::timeout, this, &ApplicationController::applyWriteWatchTick);
    m_hotkeys = std::make_unique<killcore::GlobalHotkeyManager>();
    connect(m_hotkeys.get(), &killcore::GlobalHotkeyManager::hotkeyTriggered, this, [this](int id, const killcore::HotkeyAction& action) {
        QVariantMap event;
        event["id"] = id;
        event["targetId"] = action.targetId;
        event["label"] = action.label;
        event["payload"] = action.payload;
        switch (action.type) {
            case killcore::HotkeyActionType::ToggleFreeze: event["type"] = "toggle_freeze"; break;
            case killcore::HotkeyActionType::TogglePatch: event["type"] = "toggle_patch"; break;
            case killcore::HotkeyActionType::WriteValue: event["type"] = "write_value"; break;
            case killcore::HotkeyActionType::ToggleOverlay: event["type"] = "toggle_overlay"; break;
            case killcore::HotkeyActionType::Custom: event["type"] = "custom"; break;
        }
        appendScanTelemetry("global_hotkey_triggered", event);
        emit globalHotkeyTriggered(event);
    });
    m_ai.init();
    KE_LOG_INFO() << "ApplicationController initialized";
}

ApplicationController::~ApplicationController() {
    if (m_clrInspectorProcess && m_clrInspectorProcess->state() != QProcess::NotRunning) {
        callClrInspectorRpc(QStringLiteral("shutdown"), {}, 1000);
        if (m_clrInspectorProcess->state() != QProcess::NotRunning) {
            m_clrInspectorProcess->terminate();
            if (!m_clrInspectorProcess->waitForFinished(1000)) {
                m_clrInspectorProcess->kill();
                m_clrInspectorProcess->waitForFinished(1000);
            }
        }
    }
    if (m_breakpointFreeze) {
        m_breakpointFreeze->stop();
    }
    KE_LOG_INFO() << "ApplicationController destroyed";
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

void ApplicationController::resetHardwareBreakpointStateForPreviousTarget() {
    if (m_pid <= 0) {
        return; // rien n'etait attache avant
    }
    const uint32_t previousPid = static_cast<uint32_t>(m_pid);

    // Detection best-effort de la mort de la cible precedente -- sert
    // uniquement au log, le nettoyage ci-dessous est inconditionnel dans les
    // deux cas (detachement volontaire d'une cible vivante, ou cible deja
    // morte/redemarree, incident du 19-20/08/2026, voir docs/STRATEGY_ROOM.md).
    const killcore::ProcessHandle probe(previousPid, killcore::ProcessAccess::ReadOnly);
    KE_LOG_INFO() << "resetHardwareBreakpointStateForPreviousTarget: previousPid=" << previousPid
                  << (probe.isValid()
                          ? " (toujours vivante, detachement volontaire)"
                          : " (n'existe plus -- cible morte/redemarree, nettoyage avant rattachement)");

    // Best-effort : stop() a son propre desarmement deterministe borne a 2s
    // (core/debug/inprocess_breakpoint.cpp), ne peut donc pas bloquer
    // longtemps meme si la cible a deja disparu.
    if (m_activeInProcessBreakpointSession) {
        m_activeInProcessBreakpointSession->stop();
    }
    if (m_inProcessBreakpointFreezeSession) {
        m_inProcessBreakpointFreezeSession->stop();
    }
    m_inProcessBreakpointWatchInProgress = false;

    // Retour force a Idle inconditionnel : que le nettoyage ci-dessus ait
    // reussi ou non a confirmer un desarmement reel, l'ancienne cible n'est
    // de toute façon plus celle que KillEngine va manipuler ensuite -- ses
    // registres de debug (s'ils existent encore) ne nous concernent plus.
    killcore::HwBreakpointArbiter::instance().resetForPid(previousPid);
}

bool ApplicationController::attachProcess(int pid) {
    KE_LOG_INFO() << "attachProcess(pid=" << pid << ")";

    if (m_breakpointFreeze) {
        m_breakpointFreeze->stop();
    }
    resetHardwareBreakpointStateForPreviousTarget();

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
    m_candidates.clear();
    clearCandidateUndo();
    clearCandidateValueHistory();
    m_snapshot.clear();
    m_lastAutoWriteTargets.clear();
    m_chatMemoryTargets.clear();
    m_activeCodePatches.clear();
    m_activeFunctionHooks.clear();
    m_lastAutoAsmResult.reset();
    m_activeProfileTargets.clear();
    m_autoWriteValueHistory.clear();

    KE_LOG_INFO() << "Attached to PID " << pid << " (" << m_processName.toStdString() << ")";

    emit attachmentChanged();
    return true;
}

void ApplicationController::detachProcess() {
    KE_LOG_INFO() << "detachProcess()";

    if (m_scanInProgress) {
        if (m_activeScanCancellation) {
            m_activeScanCancellation->cancel();
        }
        KE_LOG_INFO() << "Detach deferred because a scan is still running.";
        return;
    }
    if (m_findWhatWritesInProgress) {
        if (m_activeDebugCancellation) {
            m_activeDebugCancellation->cancel();
        }
        KE_LOG_INFO() << "Detach deferred because Find What Writes is still running.";
        return;
    }
    if (m_pageGuardWatchInProgress) {
        if (m_activePageGuardSession) {
            m_activePageGuardSession->stop();
        }
        KE_LOG_INFO() << "Detach deferred because Page Guard watch is still running.";
        return;
    }

    if (m_breakpointFreeze) {
        m_breakpointFreeze->stop();
    }
    if (m_speedhackSession) {
        m_speedhackSession->stop();
        m_speedhackSession.reset();
    }
    detachClrInspector();
    resetHardwareBreakpointStateForPreviousTarget();

    m_handle.close();
    m_pid = 0;
    m_attached = false;
    m_processName.clear();
    m_candidates.clear();
    clearCandidateUndo();
    clearCandidateValueHistory();
    m_snapshot.clear();
    m_freeze.clear();
    m_freezeTimer.stop();
    m_writeWatchTimer.stop();
    m_writeWatchEntries.clear();
    m_lastWriteAddress = 0;
    m_lastWritePreviousValue.clear();
    m_writeHistory.clear();
    m_lastAutoWriteTargets.clear();
    m_chatMemoryTargets.clear();
    m_activeProfileTargets.clear();
    m_autoWriteValueHistory.clear();
    m_lastBatchStartIndex = -1;
    m_lastBatchEndIndex = -1;

    emit attachmentChanged();
}

bool ApplicationController::rememberCandidatesForUndo(QString* error) {
    if (m_candidates.isEmpty()) {
        clearCandidateUndo();
        return true;
    }

    m_previousCandidates = m_candidates.clone(error);
    m_hasPreviousCandidates = m_previousCandidates.size() > 0;
    return m_hasPreviousCandidates;
}

void ApplicationController::clearCandidateUndo() {
    m_previousCandidates.clear();
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
    QVariantMap result;
    QVariantList matches;
    result["success"] = false;
    result["matches"] = matches;

    const QString needleText = value.trimmed();
    if (needleText.isEmpty()) {
        result["error"] = "Valeur texte vide.";
        appendScanTelemetry("ui_string_scan", {
            {"success", false},
            {"error", result.value("error")},
            {"value", value},
        });
        return result;
    }
    if (!m_handle.isValid()) {
        result["error"] = "Aucun processus attaché.";
        appendScanTelemetry("ui_string_scan", {
            {"success", false},
            {"error", result.value("error")},
            {"value", needleText},
        });
        return result;
    }

    killcore::ScanOptions options = scanOptionsFromSettingsAndExpertOptions(optionsMap);
    options.writableOnly = optionsMap.value("writableOnly", true).toBool();
    options.executableOnly = optionsMap.value("executableOnly", false).toBool();
    options.copyOnWriteOnly = optionsMap.value("copyOnWriteOnly", false).toBool();

    const bool scanAscii = optionsMap.value("ascii", true).toBool();
    const bool scanUtf16 = optionsMap.value("utf16", true).toBool();
    const bool numericBoundary = optionsMap.value("numericBoundary", true).toBool();
    const int maxResults = std::clamp(optionsMap.value("maxResults", 5000).toInt(), 1, 50000);
    constexpr size_t kChunkSize = 1024 * 1024;

    QList<QPair<QString, QByteArray>> patterns;
    if (scanAscii) {
        const QByteArray ascii = killcore::encodeUiStringValue(needleText, "ascii");
        if (!ascii.isEmpty()) {
            patterns.append({"ascii", ascii});
        }
    }
    if (scanUtf16) {
        const QByteArray utf16 = killcore::encodeUiStringValue(needleText, "utf16");
        if (!utf16.isEmpty()) {
            patterns.append({"utf16", utf16});
        }
    }
    if (patterns.isEmpty()) {
        result["error"] = "Aucun encodage texte sélectionné.";
        appendScanTelemetry("ui_string_scan", {
            {"success", false},
            {"error", result.value("error")},
            {"value", needleText},
            {"ascii", scanAscii},
            {"utf16", scanUtf16},
        });
        return result;
    }

    emit const_cast<ApplicationController*>(this)->scanStarted();
    emit const_cast<ApplicationController*>(this)->scanProgress(0);

    QElapsedTimer timer;
    timer.start();
    killcore::MemoryReader reader(m_handle);
    const auto regions = killcore::MemoryMap::snapshot(m_handle);
    uint64_t bytesScanned = 0;
    int regionsScanned = 0;
    bool partial = false;

    for (const auto& region : regions) {
        if (matches.size() >= maxResults) {
            partial = true;
            break;
        }
        if (!regionMatchesScanOptions(region, options)) {
            continue;
        }

        const uint64_t regionStart = region.baseAddress;
        const uint64_t regionEnd = region.baseAddress + region.size;
        const uint64_t effectiveStart = std::max(regionStart, options.startAddress);
        const uint64_t effectiveEnd = options.stopAddress == 0 ? regionEnd : std::min(regionEnd, options.stopAddress);
        if (effectiveEnd <= effectiveStart) {
            continue;
        }

        ++regionsScanned;
        const uint64_t regionSize = effectiveEnd - effectiveStart;
        size_t maxPatternSize = 1;
        for (const auto& pattern : patterns) {
            maxPatternSize = std::max(maxPatternSize, static_cast<size_t>(pattern.second.size()));
        }
        const size_t overlap = std::min<size_t>(maxPatternSize > 0 ? maxPatternSize - 1 : 0, 64);

        uint64_t offset = 0;
        QByteArray previousTail;
        while (offset < regionSize && matches.size() < maxResults) {
            const uint64_t remaining = regionSize - offset;
            const size_t toRead = static_cast<size_t>(std::min<uint64_t>(remaining, kChunkSize));
            const uint64_t readAddress = effectiveStart + offset;
            const auto read = reader.readChunked(readAddress, toRead, kChunkSize);
            if (!read.success && !read.partial) {
                break;
            }

            QByteArray buffer = previousTail + read.data;
            const uint64_t bufferBase = readAddress - static_cast<uint64_t>(previousTail.size());
            bytesScanned += read.bytesRead;

            const auto bufferMatches = killcore::findUiStringMatchesInBuffer(
                buffer,
                needleText,
                scanAscii,
                scanUtf16,
                numericBoundary,
                maxResults - matches.size());
            for (const auto& bufferMatch : bufferMatches) {
                QVariantMap match;
                const uint64_t matchAddress = bufferBase + static_cast<uint64_t>(bufferMatch.offset);
                const QByteArray bytes = killcore::encodeUiStringValue(needleText, bufferMatch.encoding);
                match["address"] = uiStringAddress(matchAddress);
                match["encoding"] = bufferMatch.encoding;
                match["text"] = needleText;
                match["byteLength"] = bufferMatch.byteLength;
                match["bytesHex"] = QString::fromLatin1(bytes.toHex(' ').toUpper());
                match["regionBase"] = uiStringAddress(region.baseAddress);
                match["regionSize"] = static_cast<qulonglong>(region.size);
                match["protection"] = killcore::protectionToString(region.protection);
                match["memoryType"] = killcore::memoryTypeToString(region.type);
                match["writable"] = region.writable;
                matches.append(match);
            }

            if (read.data.size() > static_cast<qsizetype>(overlap)) {
                previousTail = read.data.right(static_cast<qsizetype>(overlap));
            } else {
                previousTail = read.data;
            }
            offset += read.bytesRead;
            if (read.bytesRead == 0 || read.partial) {
                break;
            }
        }

        const int percent = regions.isEmpty()
            ? 100
            : std::clamp((regionsScanned * 100) / std::max(1, static_cast<int>(regions.size())), 0, 99);
        emit const_cast<ApplicationController*>(this)->scanProgress(percent);
    }

    if (matches.size() >= maxResults) {
        partial = true;
    }

    emit const_cast<ApplicationController*>(this)->scanProgress(100);

    result["success"] = true;
    result["partial"] = partial;
    result["matches"] = matches;
    result["matchesFound"] = matches.size();
    result["matchesReturned"] = matches.size();
    result["maxResults"] = maxResults;
    result["regionsScanned"] = regionsScanned;
    result["bytesScanned"] = static_cast<qulonglong>(bytesScanned);
    result["elapsedMs"] = static_cast<int>(timer.elapsed());
    result["writableOnly"] = options.writableOnly;
    result["error"] = "";
    appendScanTelemetry("ui_string_scan", {
        {"success", true},
        {"partial", partial},
        {"value", needleText},
        {"ascii", scanAscii},
        {"utf16", scanUtf16},
        {"numericBoundary", numericBoundary},
        {"writableOnly", options.writableOnly},
        {"copyOnWriteOnly", options.copyOnWriteOnly},
        {"executableOnly", options.executableOnly},
        {"maxResults", maxResults},
        {"matchesFound", matches.size()},
        {"matchesReturned", matches.size()},
        {"regionsScanned", regionsScanned},
        {"bytesScanned", static_cast<qulonglong>(bytesScanned)},
        {"elapsedMs", static_cast<int>(timer.elapsed())},
    });
    return result;
}

QVariantMap ApplicationController::trackUiStringCandidates(const QVariantList& candidates, const QString& value) const {
    QVariantMap result;
    QVariantList survivors;
    result["success"] = false;
    result["survivors"] = survivors;
    QElapsedTimer timer;
    timer.start();

    const QString needleText = value.trimmed();
    if (needleText.isEmpty()) {
        result["error"] = "Nouvelle valeur texte vide.";
        appendScanTelemetry("ui_string_track", {
            {"success", false},
            {"error", result.value("error")},
            {"inputCandidates", candidates.size()},
            {"value", value},
        });
        return result;
    }
    if (!m_handle.isValid()) {
        result["error"] = "Aucun processus attaché.";
        appendScanTelemetry("ui_string_track", {
            {"success", false},
            {"error", result.value("error")},
            {"inputCandidates", candidates.size()},
            {"value", needleText},
        });
        return result;
    }

    killcore::MemoryReader reader(m_handle);
    const auto regions = killcore::MemoryMap::snapshot(m_handle);
    int checked = 0;
    int unreadable = 0;
    int moved = 0;
    QSet<QString> seenSurvivors;
    constexpr uint64_t kRescanRadius = 512;

    for (const auto& item : candidates) {
        const QVariantMap candidate = item.toMap();
        uint64_t address = 0;
        if (!parseHexAddress(candidate.value("address").toString(), &address)) {
            continue;
        }

        const QString encoding = candidate.value("encoding", "ascii").toString();
        const QByteArray expected = killcore::encodeUiStringValue(needleText, encoding);
        const int oldLength = std::max(0, candidate.value("byteLength", expected.size()).toInt());
        const int expectedSize = static_cast<int>(expected.size());
        const size_t readSize = static_cast<size_t>(std::clamp(
            std::max(oldLength, expectedSize),
            1,
            128));

        ++checked;
        const auto read = reader.read(address, readSize);
        if (!read.success && !read.partial) {
            ++unreadable;
            continue;
        }

        if (read.data.startsWith(expected)) {
            QVariantMap survivor = candidate;
            const QString survivorKey = encoding + "|" + uiStringAddress(address);
            if (seenSurvivors.contains(survivorKey)) {
                continue;
            }
            seenSurvivors.insert(survivorKey);
            survivor["text"] = needleText;
            survivor["byteLength"] = expected.size();
            survivor["bytesHex"] = QString::fromLatin1(expected.toHex(' ').toUpper());
            survivors.append(survivor);
            continue;
        }

        const killcore::MemoryRegion* region = findRegionContaining(regions, address);
        if (!region || !region->readable || region->guarded || region->size == 0) {
            continue;
        }

        const uint64_t regionStart = region->baseAddress;
        const uint64_t regionEnd = region->baseAddress + region->size;
        const uint64_t windowStart = address > kRescanRadius
            ? std::max(regionStart, address - kRescanRadius)
            : regionStart;
        const uint64_t windowEnd = std::min(regionEnd, address + static_cast<uint64_t>(readSize) + kRescanRadius);
        if (windowEnd <= windowStart) {
            continue;
        }

        const auto windowRead = reader.readChunked(
            windowStart,
            static_cast<size_t>(windowEnd - windowStart),
            4096);
        if (!windowRead.success && !windowRead.partial) {
            continue;
        }

        const bool scanAscii = encoding.compare("ascii", Qt::CaseInsensitive) == 0;
        const bool scanUtf16 = encoding.compare("utf16", Qt::CaseInsensitive) == 0
            || encoding.compare("utf16le", Qt::CaseInsensitive) == 0;
        const auto relocatedMatches = killcore::findUiStringMatchesInBuffer(
            windowRead.data,
            needleText,
            scanAscii,
            scanUtf16,
            /*numericBoundary=*/true,
            16);
        if (relocatedMatches.isEmpty()) {
            continue;
        }

        auto best = relocatedMatches.first();
        uint64_t bestAddress = windowStart + static_cast<uint64_t>(best.offset);
        uint64_t bestDistance = bestAddress > address ? bestAddress - address : address - bestAddress;
        for (const auto& match : relocatedMatches) {
            const uint64_t relocatedAddress = windowStart + static_cast<uint64_t>(match.offset);
            const uint64_t distance = relocatedAddress > address ? relocatedAddress - address : address - relocatedAddress;
            if (distance < bestDistance) {
                best = match;
                bestAddress = relocatedAddress;
                bestDistance = distance;
            }
        }

        const QString survivorKey = best.encoding + "|" + uiStringAddress(bestAddress);
        if (seenSurvivors.contains(survivorKey)) {
            continue;
        }
        seenSurvivors.insert(survivorKey);

        QVariantMap survivor = candidate;
        survivor["address"] = uiStringAddress(bestAddress);
        survivor["movedFrom"] = uiStringAddress(address);
        survivor["movedDistanceBytes"] = static_cast<qulonglong>(bestDistance);
        survivor["encoding"] = best.encoding;
        survivor["text"] = needleText;
        survivor["byteLength"] = best.byteLength;
        survivor["bytesHex"] = QString::fromLatin1(expected.toHex(' ').toUpper());
        survivors.append(survivor);
        ++moved;
    }

    result["success"] = true;
    result["checked"] = checked;
    result["unreadable"] = unreadable;
    result["moved"] = moved;
    result["remaining"] = survivors.size();
    result["survivors"] = survivors;
    result["error"] = "";
    appendScanTelemetry("ui_string_track", {
        {"success", true},
        {"value", needleText},
        {"inputCandidates", candidates.size()},
        {"checked", checked},
        {"unreadable", unreadable},
        {"moved", moved},
        {"remaining", survivors.size()},
        {"elapsedMs", static_cast<int>(timer.elapsed())},
    });
    return result;
}

QVariantMap ApplicationController::analyzeUiStringSources(
    const QVariantMap& stringCandidate,
    const QString& value,
    const QVariantMap& optionsMap) const {
    QVariantMap result;
    QVariantList candidates;
    result["success"] = false;
    result["candidates"] = candidates;
    QElapsedTimer timer;
    timer.start();

    const QString rawValue = value.trimmed();
    if (rawValue.isEmpty()) {
        result["error"] = "Valeur source vide.";
        appendScanTelemetry("ui_string_sources_analyze", {
            {"success", false},
            {"error", result.value("error")},
            {"value", value},
        });
        return result;
    }
    if (!m_handle.isValid()) {
        result["error"] = "Aucun processus attaché.";
        appendScanTelemetry("ui_string_sources_analyze", {
            {"success", false},
            {"error", result.value("error")},
            {"value", rawValue},
        });
        return result;
    }

    uint64_t stringAddress = 0;
    if (!parseHexAddress(stringCandidate.value("address").toString(), &stringAddress)) {
        result["error"] = "Adresse string invalide.";
        appendScanTelemetry("ui_string_sources_analyze", {
            {"success", false},
            {"error", result.value("error")},
            {"value", rawValue},
            {"stringAddress", stringCandidate.value("address").toString()},
        });
        return result;
    }

    const int stringLength = std::clamp(stringCandidate.value("byteLength", 0).toInt(), 0, 256);
    const int radius = std::clamp(optionsMap.value("radiusBytes", 65536).toInt(), 256, 16 * 1024 * 1024);
    const int maxResults = std::clamp(optionsMap.value("maxResults", 200).toInt(), 1, 5000);
    const int alignment = std::clamp(optionsMap.value("alignment", 1).toInt(), 1, 16);

    const auto regions = killcore::MemoryMap::snapshot(m_handle);
    const killcore::MemoryRegion* region = findRegionContaining(regions, stringAddress);
    if (!region || !region->readable || region->guarded || region->size == 0) {
        result["error"] = "Région de la string illisible.";
        appendScanTelemetry("ui_string_sources_analyze", {
            {"success", false},
            {"error", result.value("error")},
            {"value", rawValue},
            {"stringAddress", uiStringAddress(stringAddress)},
            {"radiusBytes", radius},
        });
        return result;
    }

    const uint64_t regionStart = region->baseAddress;
    const uint64_t regionEnd = region->baseAddress + region->size;
    const uint64_t windowStart = stringAddress > static_cast<uint64_t>(radius)
        ? std::max(regionStart, stringAddress - static_cast<uint64_t>(radius))
        : regionStart;
    const uint64_t requestedEnd = stringAddress + static_cast<uint64_t>(stringLength) + static_cast<uint64_t>(radius);
    const uint64_t windowEnd = std::min(regionEnd, requestedEnd);
    if (windowEnd <= windowStart) {
        result["error"] = "Fenêtre d'analyse vide.";
        appendScanTelemetry("ui_string_sources_analyze", {
            {"success", false},
            {"error", result.value("error")},
            {"value", rawValue},
            {"stringAddress", uiStringAddress(stringAddress)},
            {"radiusBytes", radius},
            {"windowStart", uiStringAddress(windowStart)},
            {"windowEnd", uiStringAddress(windowEnd)},
        });
        return result;
    }

    const size_t readSize = static_cast<size_t>(std::min<uint64_t>(windowEnd - windowStart, 32ull * 1024ull * 1024ull));
    killcore::MemoryReader reader(m_handle);
    const auto read = reader.readChunked(windowStart, readSize, 64 * 1024);
    if (!read.success && !read.partial) {
        result["error"] = read.errorMessage;
        appendScanTelemetry("ui_string_sources_analyze", {
            {"success", false},
            {"error", result.value("error")},
            {"value", rawValue},
            {"stringAddress", uiStringAddress(stringAddress)},
            {"radiusBytes", radius},
            {"windowStart", uiStringAddress(windowStart)},
            {"windowEnd", uiStringAddress(windowEnd)},
            {"requestedBytes", static_cast<qulonglong>(readSize)},
        });
        return result;
    }

    const size_t stringOffset = stringAddress > windowStart
        ? static_cast<size_t>(stringAddress - windowStart)
        : 0;
    const auto hits = killcore::findUiStringSourcesInBuffer(
        read.data,
        stringOffset,
        stringLength,
        rawValue,
        maxResults,
        alignment,
        radius);

    for (const auto& hit : hits) {
        const uint64_t address = windowStart + static_cast<uint64_t>(hit.offset);
        QVariantMap entry;
        entry["address"] = uiStringAddress(address);
        entry["type"] = killcore::valueTypeToString(hit.type);
        entry["confidence"] = hit.confidence;
        entry["variantLabel"] = hit.variantLabel;
        entry["lastValueHex"] = QString::fromLatin1(hit.bytes.toHex(' ').toUpper());
        entry["lastValueNumber"] = hit.valueNumber;
        entry["distanceBytes"] = static_cast<qulonglong>(hit.distanceBytes);
        entry["offsetFromString"] = static_cast<qlonglong>(address) - static_cast<qlonglong>(stringAddress);
        entry["regionBase"] = uiStringAddress(region->baseAddress);
        entry["protection"] = killcore::protectionToString(region->protection);
        entry["memoryType"] = killcore::memoryTypeToString(region->type);
        candidates.append(entry);
    }

    result["success"] = true;
    result["partial"] = false;
    result["candidates"] = candidates;
    result["matchesFound"] = hits.size();
    result["matchesReturned"] = candidates.size();
    result["bytesScanned"] = static_cast<qulonglong>(read.bytesRead);
    result["windowStart"] = uiStringAddress(windowStart);
    result["windowEnd"] = uiStringAddress(windowStart + static_cast<uint64_t>(read.bytesRead));
    result["radiusBytes"] = radius;
    result["error"] = "";
    QVariantList samples;
    for (int i = 0; i < std::min<int>(candidates.size(), 10); ++i) {
        const QVariantMap candidate = candidates.at(i).toMap();
        samples.append(QVariantMap{
            {"address", candidate.value("address")},
            {"type", candidate.value("type")},
            {"variantLabel", candidate.value("variantLabel")},
            {"confidence", candidate.value("confidence")},
            {"distanceBytes", candidate.value("distanceBytes")},
            {"lastValueNumber", candidate.value("lastValueNumber")},
        });
    }
    appendScanTelemetry("ui_string_sources_analyze", {
        {"success", true},
        {"value", rawValue},
        {"stringAddress", uiStringAddress(stringAddress)},
        {"stringLength", stringLength},
        {"radiusBytes", radius},
        {"alignment", alignment},
        {"maxResults", maxResults},
        {"windowStart", uiStringAddress(windowStart)},
        {"windowEnd", result.value("windowEnd")},
        {"bytesScanned", static_cast<qulonglong>(read.bytesRead)},
        {"matchesFound", hits.size()},
        {"matchesReturned", candidates.size()},
        {"sampleCount", samples.size()},
        {"samples", samples},
        {"elapsedMs", static_cast<int>(timer.elapsed())},
    });
    return result;
}

QVariantMap ApplicationController::scanMemoryWindow(
    const QString& addressHex,
    const QString& value,
    const QVariantMap& optionsMap) const {
    QVariantMap result;
    QVariantList candidates;
    result["success"] = false;
    result["candidates"] = candidates;
    QElapsedTimer timer;
    timer.start();

    const QString rawValue = value.trimmed();
    if (rawValue.isEmpty()) {
        result["error"] = "Valeur cible vide.";
        return result;
    }
    if (!m_handle.isValid()) {
        result["error"] = "Aucun processus attaché.";
        return result;
    }

    uint64_t anchorAddress = 0;
    if (!parseHexAddress(addressHex, &anchorAddress)) {
        result["error"] = "Adresse ancre invalide.";
        return result;
    }

    const int radius = std::clamp(optionsMap.value("radiusBytes", 65536).toInt(), 256, 16 * 1024 * 1024);
    const int maxResults = std::clamp(optionsMap.value("maxResults", 200).toInt(), 1, 5000);
    const int alignment = std::clamp(optionsMap.value("alignment", 1).toInt(), 1, 16);
    const int excludeBytes = std::clamp(optionsMap.value("excludeBytes", 8).toInt(), 0, 4096);

    const auto regions = killcore::MemoryMap::snapshot(m_handle);
    const killcore::MemoryRegion* region = findRegionContaining(regions, anchorAddress);
    if (!region || !region->readable || region->guarded || region->size == 0) {
        result["error"] = "Région autour de l'adresse ancre illisible.";
        return result;
    }

    const uint64_t regionStart = region->baseAddress;
    const uint64_t regionEnd = region->baseAddress + region->size;
    const uint64_t windowStart = anchorAddress > static_cast<uint64_t>(radius)
        ? std::max(regionStart, anchorAddress - static_cast<uint64_t>(radius))
        : regionStart;
    const uint64_t requestedEnd = anchorAddress + static_cast<uint64_t>(radius);
    const uint64_t windowEnd = std::min(regionEnd, requestedEnd);
    if (windowEnd <= windowStart) {
        result["error"] = "Fenêtre d'analyse vide.";
        return result;
    }

    const size_t readSize = static_cast<size_t>(std::min<uint64_t>(windowEnd - windowStart, 32ull * 1024ull * 1024ull));
    killcore::MemoryReader reader(m_handle);
    const auto read = reader.readChunked(windowStart, readSize, 64 * 1024);
    if (!read.success && !read.partial) {
        result["error"] = read.errorMessage;
        return result;
    }

    const auto hits = killcore::findValuesInMemoryWindow(
        read.data, windowStart, anchorAddress, excludeBytes, rawValue, maxResults, alignment);

    for (const auto& hit : hits) {
        QVariantMap entry;
        entry["address"] = uiStringAddress(hit.address);
        entry["type"] = killcore::valueTypeToString(hit.type);
        entry["variantLabel"] = hit.variantLabel;
        entry["lastValueHex"] = QString::fromLatin1(hit.bytes.toHex(' ').toUpper());
        entry["lastValueNumber"] = hit.valueNumber;
        entry["distanceBytes"] = static_cast<qulonglong>(hit.distanceBytes);
        entry["offsetFromAnchor"] = static_cast<qlonglong>(hit.offsetFromAnchor);
        entry["regionBase"] = uiStringAddress(region->baseAddress);
        entry["protection"] = killcore::protectionToString(region->protection);
        entry["memoryType"] = killcore::memoryTypeToString(region->type);
        candidates.append(entry);
    }

    result["success"] = true;
    result["candidates"] = candidates;
    result["matchesFound"] = hits.size();
    result["matchesReturned"] = candidates.size();
    result["bytesScanned"] = static_cast<qulonglong>(read.bytesRead);
    result["windowStart"] = uiStringAddress(windowStart);
    result["windowEnd"] = uiStringAddress(windowStart + static_cast<uint64_t>(read.bytesRead));
    result["radiusBytes"] = radius;
    result["error"] = "";
    appendScanTelemetry("memory_window_scan", {
        {"success", true},
        {"value", rawValue},
        {"anchorAddress", uiStringAddress(anchorAddress)},
        {"radiusBytes", radius},
        {"alignment", alignment},
        {"excludeBytes", excludeBytes},
        {"maxResults", maxResults},
        {"bytesScanned", static_cast<qulonglong>(read.bytesRead)},
        {"matchesFound", hits.size()},
        {"matchesReturned", candidates.size()},
        {"elapsedMs", static_cast<int>(timer.elapsed())},
    });
    return result;
}

QVariantMap ApplicationController::trackUiStringSources(const QVariantList& sourceCandidates, const QString& value) const {
    QVariantMap result;
    QVariantList survivors;
    result["success"] = false;
    result["survivors"] = survivors;
    QElapsedTimer timer;
    timer.start();

    const QString rawValue = value.trimmed();
    if (rawValue.isEmpty()) {
        result["error"] = "Nouvelle valeur source vide.";
        appendScanTelemetry("ui_string_sources_track", {
            {"success", false},
            {"error", result.value("error")},
            {"inputCandidates", sourceCandidates.size()},
            {"value", value},
        });
        return result;
    }
    if (!m_handle.isValid()) {
        result["error"] = "Aucun processus attaché.";
        appendScanTelemetry("ui_string_sources_track", {
            {"success", false},
            {"error", result.value("error")},
            {"inputCandidates", sourceCandidates.size()},
            {"value", rawValue},
        });
        return result;
    }

    killcore::MemoryReader reader(m_handle);
    int checked = 0;
    int unreadable = 0;
    int incompatible = 0;

    for (const auto& item : sourceCandidates) {
        QVariantMap candidate = item.toMap();
        uint64_t address = 0;
        if (!parseHexAddress(candidate.value("address").toString(), &address)) {
            ++incompatible;
            continue;
        }

        killcore::ValueType type;
        if (!killcore::parseValueType(candidate.value("type").toString(), &type)) {
            ++incompatible;
            continue;
        }

        QString targetError;
        const QString variantLabel = candidate.value("variantLabel").toString();
        const QByteArray expected = killcore::targetBytesForTypeAndVariant(rawValue, type, variantLabel, &targetError);
        if (expected.isEmpty()) {
            ++incompatible;
            continue;
        }

        ++checked;
        const auto read = reader.read(address, killcore::valueTypeSize(type));
        if (!(read.success || read.partial) || read.bytesRead < killcore::valueTypeSize(type)) {
            ++unreadable;
            continue;
        }

        if (bytesEqual(read.data, expected, type)) {
            const double previousConfidence = candidate.value("confidence", 0.5).toDouble();
            const int previousHits = candidate.value("trackHits", 0).toInt();
            candidate["previousValueNumber"] = candidate.value("lastValueNumber");
            candidate["lastValueNumber"] = bytesToDouble(read.data, type);
            candidate["lastValueHex"] = QString::fromLatin1(read.data.left(static_cast<qsizetype>(expected.size())).toHex(' ').toUpper());
            candidate["expectedHex"] = QString::fromLatin1(expected.toHex(' ').toUpper());
            candidate["trackHits"] = previousHits + 1;
            candidate["confidence"] = std::clamp(previousConfidence + 0.12, 0.0, 1.0);
            survivors.append(candidate);
        }
    }

    result["success"] = true;
    result["checked"] = checked;
    result["unreadable"] = unreadable;
    result["incompatible"] = incompatible;
    result["remaining"] = survivors.size();
    result["survivors"] = survivors;
    result["error"] = "";
    QVariantList samples;
    for (int i = 0; i < std::min<int>(survivors.size(), 10); ++i) {
        const QVariantMap survivor = survivors.at(i).toMap();
        samples.append(QVariantMap{
            {"address", survivor.value("address")},
            {"type", survivor.value("type")},
            {"variantLabel", survivor.value("variantLabel")},
            {"confidence", survivor.value("confidence")},
            {"trackHits", survivor.value("trackHits")},
            {"lastValueNumber", survivor.value("lastValueNumber")},
        });
    }
    appendScanTelemetry("ui_string_sources_track", {
        {"success", true},
        {"value", rawValue},
        {"inputCandidates", sourceCandidates.size()},
        {"checked", checked},
        {"unreadable", unreadable},
        {"incompatible", incompatible},
        {"remaining", survivors.size()},
        {"sampleCount", samples.size()},
        {"samples", samples},
        {"elapsedMs", static_cast<int>(timer.elapsed())},
    });
    return result;
}

QVariantMap ApplicationController::inspectUiStringOrigins(
    const QVariantList& stringCandidates,
    const QVariantMap& optionsMap) const {
    QVariantMap result;
    QVariantList targets;
    QVariantList pointerRefs;
    result["success"] = false;
    result["targets"] = targets;
    result["pointerRefs"] = pointerRefs;
    QElapsedTimer timer;
    timer.start();

    if (!m_handle.isValid()) {
        result["error"] = "Aucun processus attaché.";
        appendScanTelemetry("ui_string_origins_inspect", {
            {"success", false},
            {"error", result.value("error")},
            {"inputCandidates", stringCandidates.size()},
        });
        return result;
    }
    if (stringCandidates.isEmpty()) {
        result["error"] = "Aucune string à inspecter.";
        appendScanTelemetry("ui_string_origins_inspect", {
            {"success", false},
            {"error", result.value("error")},
            {"inputCandidates", stringCandidates.size()},
        });
        return result;
    }

    QList<uint64_t> addresses;
    addresses.reserve(stringCandidates.size());
    for (const auto& item : stringCandidates) {
        const QVariantMap candidate = item.toMap();
        uint64_t address = 0;
        if (parseHexAddress(candidate.value("address").toString(), &address)) {
            addresses.append(address);
        }
    }
    if (addresses.isEmpty()) {
        result["error"] = "Aucune adresse string valide.";
        appendScanTelemetry("ui_string_origins_inspect", {
            {"success", false},
            {"error", result.value("error")},
            {"inputCandidates", stringCandidates.size()},
        });
        return result;
    }
    std::sort(addresses.begin(), addresses.end());

    const uint64_t clusterStart = addresses.first();
    const uint64_t clusterEnd = addresses.last();
    const uint64_t clusterSpan = clusterEnd >= clusterStart ? clusterEnd - clusterStart : 0;
    const int maxRefs = std::clamp(optionsMap.value("maxRefs", 500).toInt(), 1, 5000);
    const uint64_t maxScanBytes = static_cast<uint64_t>(
        std::clamp(optionsMap.value("maxScanMb", 512).toInt(), 16, 4096)) * 1024ull * 1024ull;
    const bool writableOnly = optionsMap.value("writableOnly", true).toBool();
    constexpr size_t kChunkSize = 1024 * 1024;
    constexpr uint64_t kPointerSlack = 32;

    uint64_t commonStride = 0;
    if (addresses.size() >= 2) {
        commonStride = addresses.at(1) - addresses.at(0);
        for (qsizetype i = 2; i < addresses.size(); ++i) {
            const uint64_t delta = addresses.at(i) - addresses.at(i - 1);
            if (delta != commonStride) {
                commonStride = 0;
                break;
            }
        }
    }

    const auto regions = killcore::MemoryMap::snapshot(m_handle);
    const killcore::MemoryRegion* clusterRegion = findRegionContaining(regions, clusterStart);
    for (const uint64_t address : addresses) {
        QVariantMap target;
        target["address"] = uiStringAddress(address);
        target["offsetFromCluster"] = static_cast<qlonglong>(address - clusterStart);
        if (clusterRegion) {
            target["regionBase"] = uiStringAddress(clusterRegion->baseAddress);
            target["protection"] = killcore::protectionToString(clusterRegion->protection);
            target["memoryType"] = killcore::memoryTypeToString(clusterRegion->type);
        }
        targets.append(target);
    }

    killcore::MemoryReader reader(m_handle);
    uint64_t bytesScanned = 0;
    int regionsScanned = 0;
    bool partial = false;
    for (const auto& region : regions) {
        if (pointerRefs.size() >= maxRefs || bytesScanned >= maxScanBytes) {
            partial = true;
            break;
        }
        if (!region.readable || region.guarded || region.size < sizeof(uint64_t)) {
            continue;
        }
        if (writableOnly && !region.writable) {
            continue;
        }

        ++regionsScanned;
        uint64_t offset = 0;
        while (offset < region.size && pointerRefs.size() < maxRefs && bytesScanned < maxScanBytes) {
            const uint64_t remaining = region.size - offset;
            const size_t toRead = static_cast<size_t>(std::min<uint64_t>(
                remaining,
                std::min<uint64_t>(kChunkSize, maxScanBytes - bytesScanned)));
            const uint64_t readAddress = region.baseAddress + offset;
            const auto read = reader.readChunked(readAddress, toRead, kChunkSize);
            if (!read.success && !read.partial) {
                break;
            }
            bytesScanned += read.bytesRead;

            const qsizetype limit = read.data.size() - static_cast<qsizetype>(sizeof(uint64_t));
            for (qsizetype i = 0; i <= limit && pointerRefs.size() < maxRefs; i += 8) {
                uint64_t pointed = 0;
                std::memcpy(&pointed, read.data.constData() + i, sizeof(pointed));
                auto nearestIt = std::lower_bound(addresses.begin(), addresses.end(), pointed);
                uint64_t nearest = addresses.first();
                uint64_t nearestDistance = std::numeric_limits<uint64_t>::max();
                if (nearestIt != addresses.end()) {
                    nearest = *nearestIt;
                    nearestDistance = pointed > nearest ? pointed - nearest : nearest - pointed;
                }
                if (nearestIt != addresses.begin()) {
                    const uint64_t candidateAddress = *(nearestIt - 1);
                    const uint64_t distance = pointed > candidateAddress
                        ? pointed - candidateAddress
                        : candidateAddress - pointed;
                    if (distance < nearestDistance) {
                        nearest = candidateAddress;
                        nearestDistance = distance;
                    }
                }
                if (nearestDistance > kPointerSlack) {
                    continue;
                }

                QVariantMap ref;
                const uint64_t pointerAddress = readAddress + static_cast<uint64_t>(i);
                ref["address"] = uiStringAddress(pointerAddress);
                ref["pointsTo"] = uiStringAddress(pointed);
                ref["nearestString"] = uiStringAddress(nearest);
                ref["distanceToString"] = static_cast<qulonglong>(nearestDistance);
                ref["regionBase"] = uiStringAddress(region.baseAddress);
                ref["protection"] = killcore::protectionToString(region.protection);
                ref["memoryType"] = killcore::memoryTypeToString(region.type);
                ref["writable"] = region.writable;
                pointerRefs.append(ref);
            }

            offset += read.bytesRead;
            if (read.bytesRead == 0 || read.partial) {
                break;
            }
        }
    }

    result["success"] = true;
    result["targetCount"] = addresses.size();
    result["targets"] = targets;
    result["clusterStart"] = uiStringAddress(clusterStart);
    result["clusterEnd"] = uiStringAddress(clusterEnd);
    result["clusterSpanBytes"] = static_cast<qulonglong>(clusterSpan);
    result["commonStrideBytes"] = static_cast<qulonglong>(commonStride);
    result["pointerRefs"] = pointerRefs;
    result["pointerRefsFound"] = pointerRefs.size();
    result["bytesScanned"] = static_cast<qulonglong>(bytesScanned);
    result["regionsScanned"] = regionsScanned;
    result["partial"] = partial || pointerRefs.size() >= maxRefs;
    result["error"] = "";
    QVariantList refSamples;
    for (int i = 0; i < std::min<int>(pointerRefs.size(), 12); ++i) {
        const QVariantMap ref = pointerRefs.at(i).toMap();
        refSamples.append(QVariantMap{
            {"address", ref.value("address")},
            {"pointsTo", ref.value("pointsTo")},
            {"nearestString", ref.value("nearestString")},
            {"distanceToString", ref.value("distanceToString")},
            {"memoryType", ref.value("memoryType")},
            {"protection", ref.value("protection")},
            {"writable", ref.value("writable")},
        });
    }
    appendScanTelemetry("ui_string_origins_inspect", {
        {"success", true},
        {"inputCandidates", stringCandidates.size()},
        {"targetCount", addresses.size()},
        {"clusterStart", result.value("clusterStart")},
        {"clusterEnd", result.value("clusterEnd")},
        {"clusterSpanBytes", static_cast<qulonglong>(clusterSpan)},
        {"commonStrideBytes", static_cast<qulonglong>(commonStride)},
        {"maxRefs", maxRefs},
        {"maxScanBytes", static_cast<qulonglong>(maxScanBytes)},
        {"writableOnly", writableOnly},
        {"pointerSlackBytes", static_cast<qulonglong>(kPointerSlack)},
        {"pointerRefsFound", pointerRefs.size()},
        {"bytesScanned", static_cast<qulonglong>(bytesScanned)},
        {"regionsScanned", regionsScanned},
        {"partial", result.value("partial")},
        {"sampleCount", refSamples.size()},
        {"samples", refSamples},
        {"elapsedMs", static_cast<int>(timer.elapsed())},
    });
    return result;
}

QVariantMap ApplicationController::startUiStringInvestigation(
    const QVariantList& stringCandidates,
    const QVariantList& sourceCandidates,
    const QVariantMap& optionsMap) {
    QVariantMap result;
    result["success"] = false;
    result["windows"] = 0;

    if (!m_handle.isValid()) {
        result["error"] = "Aucun processus attaché.";
        return result;
    }

    const int radius = std::clamp(optionsMap.value("radiusBytes", 4096).toInt(), 256, 1024 * 1024);
    const int maxWindows = std::clamp(optionsMap.value("maxWindows", 64).toInt(), 1, 512);
    const uint64_t maxBytes = static_cast<uint64_t>(
        std::clamp(optionsMap.value("maxBytesMb", 16).toInt(), 1, 256)) * 1024ull * 1024ull;
    const bool globalProbe = optionsMap.value("globalProbe", true).toBool();
    const int probeBlockSize = std::clamp(optionsMap.value("probeBlockSize", 64 * 1024).toInt(), 4096, 1024 * 1024);
    const uint64_t maxProbeBytes = static_cast<uint64_t>(
        std::clamp(optionsMap.value("maxProbeMb", 512).toInt(), 16, 2048)) * 1024ull * 1024ull;

    struct RequestWindow {
        uint64_t start{0};
        uint64_t end{0};
        QString label;
        QString reason;
    };
    QList<RequestWindow> requests;
    auto addRequest = [&](uint64_t address, int byteLength, const QString& label, const QString& reason) {
        const uint64_t extra = static_cast<uint64_t>(radius);
        const uint64_t start = address > extra ? address - extra : 0;
        const uint64_t valueEnd = address + static_cast<uint64_t>(std::max(1, byteLength));
        const uint64_t end = valueEnd > std::numeric_limits<uint64_t>::max() - extra
            ? std::numeric_limits<uint64_t>::max()
            : valueEnd + extra;
        requests.append({start, end, label, reason});
    };

    for (const auto& item : stringCandidates) {
        const QVariantMap candidate = item.toMap();
        uint64_t address = 0;
        if (parseHexAddress(candidate.value("address").toString(), &address)) {
            addRequest(
                address,
                candidate.value("byteLength", 8).toInt(),
                QString("string 0x%1").arg(uiStringAddress(address)),
                candidate.value("encoding", "string").toString());
        }
    }
    for (const auto& item : sourceCandidates) {
        const QVariantMap candidate = item.toMap();
        uint64_t address = 0;
        killcore::ValueType type = killcore::ValueType::Int32;
        if (parseHexAddress(candidate.value("address").toString(), &address)
            && killcore::parseValueType(candidate.value("type", "Int32").toString(), &type)) {
            const QString variant = candidate.value("variantLabel").toString();
            addRequest(
                address,
                static_cast<int>(killcore::valueTypeSize(type)),
                QString("source 0x%1").arg(uiStringAddress(address)),
                variant.isEmpty() ? killcore::valueTypeToString(type) : variant);
        }
    }

    if (requests.isEmpty()) {
        result["error"] = "Aucune string/source à observer.";
        return result;
    }

    std::sort(requests.begin(), requests.end(), [](const RequestWindow& a, const RequestWindow& b) {
        return a.start < b.start;
    });

    QList<RequestWindow> merged;
    for (const auto& req : requests) {
        if (merged.isEmpty() || req.start > merged.last().end + 4096) {
            merged.append(req);
            continue;
        }
        auto& last = merged.last();
        last.end = std::max(last.end, req.end);
        last.label += QString(", %1").arg(req.label);
        last.reason += QString(", %1").arg(req.reason);
    }

    killcore::MemoryReader reader(m_handle);
    auto& investigationWindows = uiInvestigationWindows();
    auto& investigationProbeBlocks = uiInvestigationProbeBlocks();
    investigationWindows.clear();
    investigationProbeBlocks.clear();
    uint64_t bytesCaptured = 0;
    uint64_t probeBytesCaptured = 0;
    int unreadable = 0;
    int probeUnreadable = 0;
    int probeRegions = 0;

    for (const auto& window : merged) {
        if (investigationWindows.size() >= maxWindows || bytesCaptured >= maxBytes) {
            break;
        }
        const uint64_t size64 = std::min<uint64_t>(window.end - window.start, maxBytes - bytesCaptured);
        if (size64 == 0) continue;
        const auto read = reader.readChunked(window.start, static_cast<size_t>(size64), 64 * 1024);
        if (!read.success && !read.partial) {
            ++unreadable;
            continue;
        }
        if (read.bytesRead == 0) {
            ++unreadable;
            continue;
        }
        investigationWindows.append({
            window.start,
            read.data.left(static_cast<qsizetype>(read.bytesRead)),
            window.label.left(240),
            window.reason.left(240),
        });
        bytesCaptured += read.bytesRead;
    }

    if (globalProbe) {
        const auto regions = killcore::MemoryMap::snapshot(m_handle);
        for (const auto& region : regions) {
            if (probeBytesCaptured >= maxProbeBytes) {
                break;
            }
            if (!region.readable || !region.writable || region.guarded || region.size == 0) {
                continue;
            }
            ++probeRegions;
            uint64_t offset = 0;
            while (offset < region.size && probeBytesCaptured < maxProbeBytes) {
                const uint64_t address = region.baseAddress + offset;
                const uint64_t remainingRegion = region.size - offset;
                const size_t toRead = static_cast<size_t>(std::min<uint64_t>({
                    static_cast<uint64_t>(probeBlockSize),
                    remainingRegion,
                    maxProbeBytes - probeBytesCaptured,
                }));
                if (toRead == 0) {
                    break;
                }
                const auto read = reader.read(address, toRead);
                if (!read.success && !read.partial) {
                    ++probeUnreadable;
                    offset += static_cast<uint64_t>(toRead);
                    continue;
                }
                if (read.bytesRead > 0) {
                    const QByteArray data = read.data.left(static_cast<qsizetype>(read.bytesRead));
                    investigationProbeBlocks.append({
                        address,
                        static_cast<int>(read.bytesRead),
                        uiInvestigationHash(data),
                        killcore::protectionToString(region.protection),
                        killcore::memoryTypeToString(region.type),
                    });
                    probeBytesCaptured += read.bytesRead;
                }
                offset += static_cast<uint64_t>(toRead);
            }
        }
    }

    m_uiInvestigationStartedMs = QDateTime::currentMSecsSinceEpoch();
    result["success"] = !investigationWindows.isEmpty();
    result["windows"] = investigationWindows.size();
    result["bytesCaptured"] = static_cast<qulonglong>(bytesCaptured);
    result["probeBlocks"] = investigationProbeBlocks.size();
    result["probeBytesCaptured"] = static_cast<qulonglong>(probeBytesCaptured);
    result["probeRegions"] = probeRegions;
    result["probeUnreadable"] = probeUnreadable;
    result["unreadable"] = unreadable;
    result["radiusBytes"] = radius;
    result["error"] = result.value("success").toBool() ? QString() : QString("Aucune fenêtre lisible capturée.");
    appendScanTelemetry("ui_string_investigation_start", {
        {"success", result.value("success")},
        {"stringCandidates", stringCandidates.size()},
        {"sourceCandidates", sourceCandidates.size()},
        {"requestWindows", requests.size()},
        {"mergedWindows", merged.size()},
        {"windows", result.value("windows")},
        {"bytesCaptured", result.value("bytesCaptured")},
        {"probeBlocks", result.value("probeBlocks")},
        {"probeBytesCaptured", result.value("probeBytesCaptured")},
        {"probeRegions", probeRegions},
        {"probeUnreadable", probeUnreadable},
        {"globalProbe", globalProbe},
        {"unreadable", unreadable},
        {"radiusBytes", radius},
        {"error", result.value("error")},
    });
    return result;
}

QVariantMap ApplicationController::finishUiStringInvestigation(const QVariantMap& optionsMap) {
    QVariantMap result;
    QVariantList changes;
    result["success"] = false;
    result["changes"] = changes;

    if (!m_handle.isValid()) {
        result["error"] = "Aucun processus attaché.";
        return result;
    }
    auto& investigationWindows = uiInvestigationWindows();
    if (investigationWindows.isEmpty()) {
        result["error"] = "Aucune enquête live active.";
        return result;
    }

    const int maxChanges = std::clamp(optionsMap.value("maxChanges", 500).toInt(), 1, 5000);
    const int maxGlobalValueHits = std::clamp(optionsMap.value("maxGlobalValueHits", 250).toInt(), 0, 2000);
    const QString globalValue = optionsMap.value("value").toString().trimmed();
    killcore::MemoryReader reader(m_handle);
    int windowsChecked = 0;
    int unreadable = 0;
    int changedBytes = 0;
    bool partial = false;

    for (const auto& window : investigationWindows) {
        if (changes.size() >= maxChanges) {
            partial = true;
            break;
        }
        ++windowsChecked;
        const auto read = reader.readChunked(window.base, static_cast<size_t>(window.before.size()), 64 * 1024);
        if (!read.success && !read.partial) {
            ++unreadable;
            continue;
        }
        const qsizetype comparable = std::min(window.before.size(), read.data.size());
        qsizetype i = 0;
        while (i < comparable && changes.size() < maxChanges) {
            if (window.before.at(i) == read.data.at(i)) {
                ++i;
                continue;
            }
            const qsizetype start = i;
            while (i < comparable && window.before.at(i) != read.data.at(i)) {
                ++i;
            }
            const qsizetype length = i - start;
            changedBytes += static_cast<int>(length);
            const uint64_t address = window.base + static_cast<uint64_t>(start);
            QVariantMap change;
            change["address"] = uiStringAddress(address);
            change["offset"] = static_cast<qulonglong>(start);
            change["length"] = static_cast<int>(length);
            change["label"] = window.label;
            change["reason"] = window.reason;
            change["beforeHex"] = QString::fromLatin1(window.before.mid(start, std::min<qsizetype>(length, 16)).toHex(' ').toUpper());
            change["afterHex"] = QString::fromLatin1(read.data.mid(start, std::min<qsizetype>(length, 16)).toHex(' ').toUpper());
            if (start + 4 <= read.data.size() && start + 4 <= window.before.size()) {
                change["beforeInt32"] = bytesToDouble(window.before.mid(start, 4), killcore::ValueType::Int32);
                change["afterInt32"] = bytesToDouble(read.data.mid(start, 4), killcore::ValueType::Int32);
                change["beforeFloat32"] = bytesToDouble(window.before.mid(start, 4), killcore::ValueType::Float32);
                change["afterFloat32"] = bytesToDouble(read.data.mid(start, 4), killcore::ValueType::Float32);
            }
            changes.append(change);
        }
    }

    QVariantList globalValueHits;
    int probeBlocksChecked = 0;
    int probeBlocksChanged = 0;
    uint64_t probeBytesChecked = 0;
    uint64_t probeChangedBytes = 0;
    int probeUnreadable = 0;
    QSet<QString> globalSeen;
    const auto variants = globalValue.isEmpty()
        ? QList<killcore::ValueVariant>{}
        : killcore::generateScanVariants(globalValue, killcore::ValueType::Int32, false);
    auto& investigationProbeBlocks = uiInvestigationProbeBlocks();
    for (const auto& block : investigationProbeBlocks) {
        if (globalValueHits.size() >= maxGlobalValueHits) {
            partial = true;
            break;
        }
        ++probeBlocksChecked;
        const auto read = reader.read(block.base, static_cast<size_t>(block.size));
        if (!read.success && !read.partial) {
            ++probeUnreadable;
            continue;
        }
        if (read.bytesRead == 0) {
            ++probeUnreadable;
            continue;
        }
        probeBytesChecked += read.bytesRead;
        const QByteArray data = read.data.left(static_cast<qsizetype>(read.bytesRead));
        const uint64_t currentHash = uiInvestigationHash(data);
        if (currentHash == block.hash) {
            continue;
        }
        ++probeBlocksChanged;
        probeChangedBytes += read.bytesRead;

        for (const auto& variant : variants) {
            if (globalValueHits.size() >= maxGlobalValueHits) {
                partial = true;
                break;
            }
            const QByteArray needle = killcore::scanValueToBytes(variant.value);
            if (needle.isEmpty() || needle.size() > data.size()) {
                continue;
            }
            qsizetype from = 0;
            while (globalValueHits.size() < maxGlobalValueHits) {
                const qsizetype found = data.indexOf(needle, from);
                if (found < 0) {
                    break;
                }
                from = found + 1;
                const uint64_t address = block.base + static_cast<uint64_t>(found);
                const QString key = uiStringAddress(address) + "|" + killcore::valueTypeToString(variant.value.type) + "|" + variant.label;
                if (globalSeen.contains(key)) {
                    continue;
                }
                globalSeen.insert(key);

                QVariantMap hit;
                hit["address"] = uiStringAddress(address);
                hit["type"] = killcore::valueTypeToString(variant.value.type);
                hit["variantLabel"] = variant.label;
                hit["confidence"] = variant.secondary ? 0.62 : 0.74;
                hit["lastValueHex"] = QString::fromLatin1(needle.toHex(' ').toUpper());
                hit["lastValueNumber"] = bytesToDouble(needle, variant.value.type);
                hit["distanceBytes"] = static_cast<qulonglong>(0);
                hit["offsetFromString"] = static_cast<qlonglong>(0);
                hit["regionBase"] = uiStringAddress(block.base);
                hit["protection"] = block.protection;
                hit["memoryType"] = block.memoryType;
                hit["origin"] = "global_diff";
                globalValueHits.append(hit);
            }
        }
    }

    const qint64 elapsedMs = m_uiInvestigationStartedMs > 0
        ? QDateTime::currentMSecsSinceEpoch() - m_uiInvestigationStartedMs
        : 0;
    const int capturedWindows = investigationWindows.size();
    investigationWindows.clear();
    const int capturedProbeBlocks = investigationProbeBlocks.size();
    investigationProbeBlocks.clear();
    m_uiInvestigationStartedMs = 0;

    result["success"] = true;
    result["windowsChecked"] = windowsChecked;
    result["capturedWindows"] = capturedWindows;
    result["unreadable"] = unreadable;
    result["changedBytes"] = changedBytes;
    result["changesFound"] = changes.size();
    result["globalValueHits"] = globalValueHits;
    result["globalValueHitsFound"] = globalValueHits.size();
    result["probeBlocksCaptured"] = capturedProbeBlocks;
    result["probeBlocksChecked"] = probeBlocksChecked;
    result["probeBlocksChanged"] = probeBlocksChanged;
    result["probeBytesChecked"] = static_cast<qulonglong>(probeBytesChecked);
    result["probeChangedBytes"] = static_cast<qulonglong>(probeChangedBytes);
    result["probeUnreadable"] = probeUnreadable;
    result["partial"] = partial || changes.size() >= maxChanges;
    result["elapsedMs"] = static_cast<int>(elapsedMs);
    result["changes"] = changes;
    result["error"] = "";
    QVariantList samples;
    for (int idx = 0; idx < std::min<int>(changes.size(), 20); ++idx) {
        samples.append(changes.at(idx));
    }
    appendScanTelemetry("ui_string_investigation_finish", {
        {"success", true},
        {"capturedWindows", capturedWindows},
        {"windowsChecked", windowsChecked},
        {"unreadable", unreadable},
        {"changedBytes", changedBytes},
        {"changesFound", changes.size()},
        {"globalValue", globalValue},
        {"globalValueHitsFound", globalValueHits.size()},
        {"probeBlocksCaptured", capturedProbeBlocks},
        {"probeBlocksChecked", probeBlocksChecked},
        {"probeBlocksChanged", probeBlocksChanged},
        {"probeBytesChecked", static_cast<qulonglong>(probeBytesChecked)},
        {"probeChangedBytes", static_cast<qulonglong>(probeChangedBytes)},
        {"probeUnreadable", probeUnreadable},
        {"partial", result.value("partial")},
        {"elapsedMs", result.value("elapsedMs")},
        {"sampleCount", samples.size()},
        {"samples", samples},
    });
    return result;
}

QVariantMap ApplicationController::startExactScan(const QString& value, const QString& valueType) {
    QVariantMap result;
    QVariantList matches;
    result["success"] = false;

    if (!m_handle.isValid()) {
        result["error"] = "Aucun processus attaché.";
        result["matches"] = matches;
        return result;
    }

    killcore::ValueType type;
    if (!killcore::parseValueType(valueType, &type)) {
        result["error"] = "Type de valeur invalide.";
        result["matches"] = matches;
        return result;
    }

    killcore::ScanValue scanValue;
    QString parseError;
    if (!killcore::parseScanValue(value, type, &scanValue, &parseError)) {
        result["error"] = parseError;
        result["matches"] = matches;
        return result;
    }

    killcore::ScanOptions options = scanOptionsFromSettings();

    emit scanStarted();
    emit scanProgress(0);
    QElapsedTimer timer;
    timer.start();
    killcore::ScanEngine scanner(m_handle);
    const auto scan = scanner.exactScan(scanValue, options);
    const qint64 elapsedMs = timer.elapsed();
    emit scanProgress(90);
    clearCandidateUndo();
    clearCandidateValueHistory();
    m_candidates.replaceFromScan(scan, killcore::scanValueToBytes(scanValue));

    const qsizetype previewCount = std::min<qsizetype>(scan.matches.size(), 50);
    for (qsizetype i = 0; i < previewCount; ++i) {
        const auto& match = scan.matches.at(i);
        QVariantMap entry;
        entry["address"] = QString::number(match.address, 16);
        entry["type"] = killcore::valueTypeToString(match.type);
        matches.append(entry);
    }

    result["success"] = scan.success;
    result["partial"] = scan.partial;
    result["cancelled"] = scan.cancelled;
    result["regionsScanned"] = static_cast<int>(scan.regionsScanned);
    result["bytesScanned"] = static_cast<qulonglong>(scan.bytesScanned);
    result["matchesFound"] = static_cast<qulonglong>(scan.matchesFound);
    result["matchesReturned"] = matches.size();
    result["error"] = scan.errorMessage;
    result["matches"] = matches;
    result["candidateStoreSize"] = static_cast<qulonglong>(m_candidates.size());
    result["elapsedMs"] = static_cast<qulonglong>(elapsedMs);
    result["bytesPerSecond"] = ratePerSecond(scan.bytesScanned, elapsedMs);
    result["matchesPerSecond"] = ratePerSecond(scan.matchesFound, elapsedMs);
    appendSmartSearchDebug("exact_scan", {
        {"value", value},
        {"valueType", valueType},
        {"success", result.value("success")},
        {"partial", result.value("partial")},
        {"matchesFound", result.value("matchesFound")},
        {"candidateStoreSize", result.value("candidateStoreSize")},
        {"error", result.value("error")},
    });
    appendScanTelemetry("exact_scan", {
        {"value", value},
        {"valueType", valueType},
        {"success", result.value("success")},
        {"partial", result.value("partial")},
        {"cancelled", result.value("cancelled")},
        {"regionsScanned", result.value("regionsScanned")},
        {"bytesScanned", result.value("bytesScanned")},
        {"matchesFound", result.value("matchesFound")},
        {"candidateStoreSize", result.value("candidateStoreSize")},
        {"elapsedMs", result.value("elapsedMs")},
        {"bytesPerSecond", result.value("bytesPerSecond")},
        {"matchesPerSecond", result.value("matchesPerSecond")},
        {"error", result.value("error")},
    });
    emit scanStatsUpdated(static_cast<int>(m_candidates.size()));
    emit scanProgress(100);
    return result;
}

QVariantMap ApplicationController::startExactScanMultiType(const QString& value, const QString& valueType) {
    QVariantMap result;
    QVariantList matches;
    result["success"] = false;

    if (!m_handle.isValid()) {
        result["error"] = "Aucun processus attaché.";
        result["matches"] = matches;
        return result;
    }

    killcore::ValueType explicitType = killcore::ValueType::Int32;
    const QString normalizedType = valueType.trimmed();
    const bool smartAuto = normalizedType.compare("SmartAuto", Qt::CaseInsensitive) == 0;
    const bool explicitTypeGiven = !normalizedType.isEmpty()
        && !smartAuto
        && normalizedType.compare("Auto", Qt::CaseInsensitive) != 0;
    if (explicitTypeGiven && !killcore::parseValueType(normalizedType, &explicitType)) {
        result["error"] = "Type de valeur invalide.";
        result["matches"] = matches;
        return result;
    }

    const auto valueVariants = smartAuto
        ? smartAutoScanVariants(value)
        : killcore::generateScanVariants(value, explicitType, explicitTypeGiven);
    if (valueVariants.isEmpty()) {
        result["error"] = QString("Impossible de parser '%1' comme valeur numérique.").arg(value);
        result["matches"] = matches;
        return result;
    }

    QList<killcore::ScanEngine::MultiTypeMatch> variants;
    variants.reserve(valueVariants.size());
    for (const auto& valueVariant : valueVariants) {
        variants.append({valueVariant.value, valueVariant.label, valueVariant.secondary});
    }

    killcore::ScanOptions options = scanOptionsFromSettings();

    emit scanStarted();
    emit scanProgress(0);
    QElapsedTimer timer;
    timer.start();
    killcore::ScanEngine scanner(m_handle);
    const auto scan = scanner.exactScanMultiType(variants, options);
    const qint64 elapsedMs = timer.elapsed();
    emit scanProgress(90);
    clearCandidateUndo();
    clearCandidateValueHistory();
    const auto variantBytes = variantBytesByKey(valueVariants);
    QList<killcore::Candidate> candidates;
    candidates.reserve(scan.matches.size());
    for (const auto& match : scan.matches) {
        killcore::Candidate candidate;
        candidate.address = match.address;
        candidate.type = match.type;
        candidate.lastValue = variantBytes.value(variantKey(match.type, match.variantLabel));
        if (candidate.lastValue.isEmpty()) {
            killcore::ScanValue fallbackValue;
            if (killcore::parseScanValue(value, match.type, &fallbackValue)) {
                candidate.lastValue = killcore::scanValueToBytes(fallbackValue);
            }
        }
        candidate.confidence = match.confidence;
        candidate.variantLabel = match.variantLabel;
        candidates.append(candidate);
    }
    m_candidates.replaceCandidates(candidates);

    const qsizetype previewCount = std::min<qsizetype>(scan.matches.size(), 50);
    for (qsizetype i = 0; i < previewCount; ++i) {
        const auto& match = scan.matches.at(i);
        QVariantMap entry;
        entry["address"] = QString::number(match.address, 16);
        entry["type"] = killcore::valueTypeToString(match.type);
        entry["confidence"] = match.confidence;
        entry["variantLabel"] = match.variantLabel;
        matches.append(entry);
    }

    result["success"] = scan.success;
    result["partial"] = scan.partial;
    result["cancelled"] = scan.cancelled;
    result["regionsScanned"] = static_cast<int>(scan.regionsScanned);
    result["bytesScanned"] = static_cast<qulonglong>(scan.bytesScanned);
    result["matchesFound"] = static_cast<qulonglong>(scan.matchesFound);
    result["matchesReturned"] = matches.size();
    result["error"] = scan.errorMessage;
    result["matches"] = matches;
    result["candidateStoreSize"] = static_cast<qulonglong>(m_candidates.size());
    result["variantCount"] = variants.size();
    result["elapsedMs"] = static_cast<qulonglong>(elapsedMs);
    result["bytesPerSecond"] = ratePerSecond(scan.bytesScanned, elapsedMs);
    result["matchesPerSecond"] = ratePerSecond(scan.matchesFound, elapsedMs);
    appendSmartSearchDebug("exact_scan_multi_type", {
        {"value", value},
        {"valueType", valueType},
        {"smartAuto", smartAuto},
        {"explicitTypeGiven", explicitTypeGiven},
        {"variantCount", variants.size()},
        {"success", result.value("success")},
        {"partial", result.value("partial")},
        {"matchesFound", result.value("matchesFound")},
        {"candidateStoreSize", result.value("candidateStoreSize")},
        {"error", result.value("error")},
    });
    appendScanTelemetry("exact_scan_multi_type", {
        {"value", value},
        {"valueType", valueType},
        {"smartAuto", smartAuto},
        {"explicitTypeGiven", explicitTypeGiven},
        {"variantCount", variants.size()},
        {"success", result.value("success")},
        {"partial", result.value("partial")},
        {"cancelled", result.value("cancelled")},
        {"regionsScanned", result.value("regionsScanned")},
        {"bytesScanned", result.value("bytesScanned")},
        {"matchesFound", result.value("matchesFound")},
        {"candidateStoreSize", result.value("candidateStoreSize")},
        {"elapsedMs", result.value("elapsedMs")},
        {"bytesPerSecond", result.value("bytesPerSecond")},
        {"matchesPerSecond", result.value("matchesPerSecond")},
        {"error", result.value("error")},
    });
    emit scanStatsUpdated(static_cast<int>(m_candidates.size()));
    emit scanProgress(100);
    return result;
}

QVariantMap ApplicationController::startExactScanExpert(
    const QString& value,
    const QString& valueType,
    const QVariantMap& expertOptions) {
    QVariantMap result;
    QVariantList matches;
    result["success"] = false;

    if (!m_handle.isValid()) {
        result["error"] = "Aucun processus attaché.";
        result["matches"] = matches;
        return result;
    }

    killcore::ValueType type;
    if (!killcore::parseValueType(valueType, &type)) {
        result["error"] = "Type de valeur invalide.";
        result["matches"] = matches;
        return result;
    }

    killcore::ScanValue scanValue;
    QString parseError;
    if (!killcore::parseScanValue(value, type, &scanValue, &parseError)) {
        result["error"] = parseError;
        result["matches"] = matches;
        return result;
    }

    killcore::ScanOptions options = scanOptionsFromSettings();

    // Filtres Mode Expert (tous optionnels)
    if (expertOptions.contains("startAddress")) {
        const QString startText = expertOptions.value("startAddress").toString().trimmed();
        if (!startText.isEmpty()) {
            uint64_t startAddress = 0;
            if (parseHexAddress(startText, &startAddress)) {
                options.startAddress = startAddress;
            } else {
                result["error"] = "Adresse de début invalide.";
                result["matches"] = matches;
                return result;
            }
        }
    }
    if (expertOptions.contains("stopAddress")) {
        const QString stopText = expertOptions.value("stopAddress").toString().trimmed();
        if (!stopText.isEmpty()) {
            uint64_t stopAddress = 0;
            if (parseHexAddress(stopText, &stopAddress)) {
                options.stopAddress = stopAddress;
            } else {
                result["error"] = "Adresse de fin invalide.";
                result["matches"] = matches;
                return result;
            }
        }
    }
    if (options.startAddress != 0
        && options.stopAddress != 0
        && options.stopAddress <= options.startAddress) {
        result["error"] = "La fin de plage doit être supérieure au début.";
        result["matches"] = matches;
        return result;
    }
    if (expertOptions.contains("alignment")) {
        bool alignOk = false;
        const auto align = expertOptions.value("alignment").toULongLong(&alignOk);
        if (alignOk && align > 0) {
            options.alignment = static_cast<size_t>(align);
            options.fastScan = false; // alignement explicite désactive le fast scan auto
        }
    }
    options.writableOnly = expertOptions.value("writableOnly", false).toBool();
    options.executableOnly = expertOptions.value("executableOnly", false).toBool();
    options.copyOnWriteOnly = expertOptions.value("copyOnWriteOnly", false).toBool();

    emit scanStarted();
    emit scanProgress(0);
    QElapsedTimer timer;
    timer.start();
    killcore::ScanEngine scanner(m_handle);
    const auto scan = scanner.exactScan(scanValue, options);
    const qint64 elapsedMs = timer.elapsed();
    emit scanProgress(90);
    clearCandidateUndo();
    clearCandidateValueHistory();
    m_candidates.replaceFromScan(scan, killcore::scanValueToBytes(scanValue));

    const qsizetype previewCount = std::min<qsizetype>(scan.matches.size(), 50);
    for (qsizetype i = 0; i < previewCount; ++i) {
        const auto& match = scan.matches.at(i);
        QVariantMap entry;
        entry["address"] = QString::number(match.address, 16);
        entry["type"] = killcore::valueTypeToString(match.type);
        matches.append(entry);
    }

    result["success"] = scan.success;
    result["partial"] = scan.partial;
    result["cancelled"] = scan.cancelled;
    result["regionsScanned"] = static_cast<int>(scan.regionsScanned);
    result["bytesScanned"] = static_cast<qulonglong>(scan.bytesScanned);
    result["matchesFound"] = static_cast<qulonglong>(scan.matchesFound);
    result["matchesReturned"] = matches.size();
    result["error"] = scan.errorMessage;
    result["matches"] = matches;
    result["candidateStoreSize"] = static_cast<qulonglong>(m_candidates.size());
    result["elapsedMs"] = static_cast<qulonglong>(elapsedMs);
    result["bytesPerSecond"] = ratePerSecond(scan.bytesScanned, elapsedMs);
    result["matchesPerSecond"] = ratePerSecond(scan.matchesFound, elapsedMs);
    appendSmartSearchDebug("exact_scan_expert", {
        {"value", value},
        {"valueType", valueType},
        {"startAddress", expertOptions.value("startAddress")},
        {"stopAddress", expertOptions.value("stopAddress")},
        {"alignment", expertOptions.value("alignment")},
        {"writableOnly", options.writableOnly},
        {"executableOnly", options.executableOnly},
        {"copyOnWriteOnly", options.copyOnWriteOnly},
        {"success", result.value("success")},
        {"matchesFound", result.value("matchesFound")},
        {"candidateStoreSize", result.value("candidateStoreSize")},
        {"error", result.value("error")},
    });
    appendScanTelemetry("exact_scan_expert", {
        {"value", value},
        {"valueType", valueType},
        {"startAddress", expertOptions.value("startAddress")},
        {"stopAddress", expertOptions.value("stopAddress")},
        {"alignment", expertOptions.value("alignment")},
        {"writableOnly", options.writableOnly},
        {"executableOnly", options.executableOnly},
        {"copyOnWriteOnly", options.copyOnWriteOnly},
        {"success", result.value("success")},
        {"partial", result.value("partial")},
        {"cancelled", result.value("cancelled")},
        {"regionsScanned", result.value("regionsScanned")},
        {"bytesScanned", result.value("bytesScanned")},
        {"matchesFound", result.value("matchesFound")},
        {"candidateStoreSize", result.value("candidateStoreSize")},
        {"elapsedMs", result.value("elapsedMs")},
        {"bytesPerSecond", result.value("bytesPerSecond")},
        {"matchesPerSecond", result.value("matchesPerSecond")},
        {"error", result.value("error")},
    });
    emit scanStatsUpdated(static_cast<int>(m_candidates.size()));
    emit scanProgress(100);
    return result;
}

QVariantMap ApplicationController::scanEncryptedValue(const QString& value, const QString& valueType, const QVariantMap& optionsMap) {
    QVariantMap result;
    QVariantList matches;
    result["success"] = false;
    result["matches"] = matches;

    if (!m_handle.isValid()) {
        result["error"] = "Aucun processus attaché.";
        return result;
    }

    killcore::ValueType type;
    if (!killcore::parseValueType(valueType, &type)) {
        result["error"] = "Type invalide pour scan chiffré.";
        return result;
    }

    killcore::ScanValue displayScanValue;
    QString parseError;
    if (!killcore::parseScanValue(value, type, &displayScanValue, &parseError)) {
        result["error"] = parseError;
        return result;
    }

    uint64_t displayValue = 0;
    if (type == killcore::ValueType::Float32 || type == killcore::ValueType::Float64) {
        result["error"] = "Le scan chiffré v1 supporte seulement les types entiers.";
        return result;
    }
    const QByteArray displayBytes = killcore::scanValueToBytes(displayScanValue);
    std::memcpy(&displayValue, displayBytes.constData(), std::min<size_t>(displayBytes.size(), sizeof(displayValue)));

    const QString modeText = optionsMap.value("mode", "xor").toString().toLower();
    killcore::EncryptedScanMode encryptedMode = killcore::EncryptedScanMode::XorKey;
    if (modeText == "add") {
        encryptedMode = killcore::EncryptedScanMode::AddKey;
    } else if (modeText == "sub") {
        encryptedMode = killcore::EncryptedScanMode::SubKey;
    } else if (modeText == "not") {
        encryptedMode = killcore::EncryptedScanMode::NotBits;
    } else if (modeText != "xor") {
        result["error"] = "Mode chiffré invalide. Utilise xor, add, sub ou not.";
        return result;
    }

    bool keyOk = false;
    QString keyText = optionsMap.value("key", "0").toString().trimmed();
    if (keyText.startsWith("0x", Qt::CaseInsensitive)) {
        keyText = keyText.mid(2);
        keyOk = true;
    }
    const uint64_t key = keyOk
        ? keyText.toULongLong(&keyOk, 16)
        : optionsMap.value("key", 0).toULongLong(&keyOk);
    const int keySearchBits = std::clamp(optionsMap.value("keySearchBits", 0).toInt(), 0, 32);
    if (!keyOk && keySearchBits == 0 && encryptedMode != killcore::EncryptedScanMode::NotBits) {
        result["error"] = "Clé invalide.";
        return result;
    }

    killcore::EncryptedScanOptions encryptedOptions;
    encryptedOptions.mode = encryptedMode;
    encryptedOptions.key = key;
    encryptedOptions.keySearchBits = keySearchBits;
    encryptedOptions.valueType = type;

    killcore::ScanOptions scanOptions = scanOptionsFromSettingsAndExpertOptions(optionsMap);
    scanOptions.writableOnly = optionsMap.value("writableOnly", true).toBool();
    scanOptions.executableOnly = optionsMap.value("executableOnly", false).toBool();
    scanOptions.copyOnWriteOnly = optionsMap.value("copyOnWriteOnly", false).toBool();
    const int maxResults = std::clamp(optionsMap.value("maxResults", 1000).toInt(), 1, 10000);
    const size_t typeSize = killcore::valueTypeSize(type);
    const size_t chunkSize = std::max<size_t>(1024 * 1024, typeSize);

    emit scanStarted();
    emit scanProgress(0);

    QElapsedTimer timer;
    timer.start();
    killcore::MemoryReader reader(m_handle);
    const auto regions = killcore::MemoryMap::snapshot(m_handle);
    int regionsScanned = 0;
    uint64_t bytesScanned = 0;
    bool partial = false;
    QString error;

    for (const auto& region : regions) {
        if (matches.size() >= maxResults) {
            partial = true;
            break;
        }
        if (!regionMatchesScanOptions(region, scanOptions) || region.size < static_cast<uint64_t>(typeSize)) {
            continue;
        }

        const uint64_t regionStart = region.baseAddress;
        const uint64_t regionEnd = region.baseAddress + region.size;
        const uint64_t effectiveStart = std::max(regionStart, scanOptions.startAddress);
        const uint64_t effectiveEnd = scanOptions.stopAddress == 0 ? regionEnd : std::min(regionEnd, scanOptions.stopAddress);
        if (effectiveEnd <= effectiveStart || effectiveEnd - effectiveStart < typeSize) {
            continue;
        }

        ++regionsScanned;
        const uint64_t regionSize = effectiveEnd - effectiveStart;
        const size_t overlap = typeSize > 0 ? typeSize - 1 : 0;
        QByteArray previousTail;
        uint64_t offset = 0;
        while (offset < regionSize && matches.size() < maxResults) {
            const uint64_t remaining = regionSize - offset;
            const size_t toRead = static_cast<size_t>(std::min<uint64_t>(remaining, chunkSize));
            const uint64_t readAddress = effectiveStart + offset;
            const auto read = reader.readChunked(readAddress, toRead, chunkSize);
            if (!read.success && !read.partial) {
                break;
            }

            QByteArray buffer = previousTail + read.data;
            const uint64_t bufferBase = readAddress - static_cast<uint64_t>(previousTail.size());
            bytesScanned += read.bytesRead;
            auto scan = killcore::scanEncryptedInBuffer(buffer, bufferBase, displayValue, encryptedOptions);
            if (!scan.success && !scan.error.isEmpty()) {
                error = scan.error;
                partial = true;
                break;
            }
            if (scan.partial) {
                partial = true;
            }
            for (const auto& matchInfo : scan.matches) {
                if (matches.size() >= maxResults) {
                    partial = true;
                    break;
                }
                QVariantMap match;
                match["address"] = QString::number(matchInfo.address, 16).toUpper();
                match["type"] = killcore::valueTypeToString(matchInfo.type);
                match["confidence"] = matchInfo.confidence;
                match["variantLabel"] = matchInfo.variantLabel;
                match["regionBase"] = QString::number(region.baseAddress, 16).toUpper();
                match["protection"] = killcore::protectionToString(region.protection);
                match["memoryType"] = killcore::memoryTypeToString(region.type);
                match["writable"] = region.writable;
                matches.append(match);
            }

            previousTail = read.data.size() > static_cast<qsizetype>(overlap)
                ? read.data.right(static_cast<qsizetype>(overlap))
                : read.data;
            offset += read.bytesRead;
            if (read.bytesRead == 0 || read.partial) {
                break;
            }
        }

        const int percent = regions.isEmpty()
            ? 100
            : std::clamp((regionsScanned * 100) / std::max(1, static_cast<int>(regions.size())), 0, 99);
        emit scanProgress(percent);
        if (!error.isEmpty()) {
            break;
        }
    }

    emit scanProgress(100);

    result["success"] = error.isEmpty();
    result["partial"] = partial || matches.size() >= maxResults;
    result["matches"] = matches;
    result["matchesFound"] = matches.size();
    result["matchesReturned"] = matches.size();
    result["maxResults"] = maxResults;
    result["regionsScanned"] = regionsScanned;
    result["bytesScanned"] = static_cast<qulonglong>(bytesScanned);
    result["elapsedMs"] = static_cast<int>(timer.elapsed());
    result["mode"] = modeText;
    result["key"] = QString::number(key, 16).toUpper();
    result["keySearchBits"] = keySearchBits;
    result["error"] = error;
    return result;
}

QVariantMap ApplicationController::startExactScanAsync(
    const QString& value,
    const QString& valueType,
    const QVariantMap& expertOptions) {
    QVariantMap result;
    QVariantList matches;
    result["success"] = false;
    result["started"] = false;

    if (m_scanInProgress) {
        result["error"] = "Un scan est déjà en cours.";
        return result;
    }

    if (!m_handle.isValid()) {
        result["error"] = "Aucun processus attaché.";
        result["matches"] = matches;
        return result;
    }

    killcore::ValueType type;
    if (!killcore::parseValueType(valueType, &type)) {
        result["error"] = "Type de valeur invalide.";
        result["matches"] = matches;
        return result;
    }

    killcore::ScanValue scanValue;
    QString parseError;
    if (!killcore::parseScanValue(value, type, &scanValue, &parseError)) {
        result["error"] = parseError;
        result["matches"] = matches;
        return result;
    }

    killcore::ScanOptions options = scanOptionsFromSettings();
    if (expertOptions.contains("startAddress")) {
        const QString startText = expertOptions.value("startAddress").toString().trimmed();
        if (!startText.isEmpty()) {
            uint64_t startAddress = 0;
            if (!parseHexAddress(startText, &startAddress)) {
                result["error"] = "Adresse de début invalide.";
                result["matches"] = matches;
                return result;
            }
            options.startAddress = startAddress;
        }
    }
    if (expertOptions.contains("stopAddress")) {
        const QString stopText = expertOptions.value("stopAddress").toString().trimmed();
        if (!stopText.isEmpty()) {
            uint64_t stopAddress = 0;
            if (!parseHexAddress(stopText, &stopAddress)) {
                result["error"] = "Adresse de fin invalide.";
                result["matches"] = matches;
                return result;
            }
            options.stopAddress = stopAddress;
        }
    }
    if (options.startAddress != 0
        && options.stopAddress != 0
        && options.stopAddress <= options.startAddress) {
        result["error"] = "La fin de plage doit être supérieure au début.";
        result["matches"] = matches;
        return result;
    }
    if (expertOptions.contains("alignment")) {
        bool alignOk = false;
        const auto align = expertOptions.value("alignment").toULongLong(&alignOk);
        if (alignOk && align > 0) {
            options.alignment = static_cast<size_t>(align);
            options.fastScan = false;
        }
    }
    options.writableOnly = expertOptions.value("writableOnly", false).toBool();
    options.executableOnly = expertOptions.value("executableOnly", false).toBool();
    options.copyOnWriteOnly = expertOptions.value("copyOnWriteOnly", false).toBool();

    const int requestId = m_nextScanRequestId++;
    const int pid = m_pid;
    const QByteArray scannedBytes = killcore::scanValueToBytes(scanValue);
    const QPointer<ApplicationController> self(this);
    auto cancellation = std::make_shared<killcore::CancellationToken>();

    m_scanInProgress = true;
    m_activeScanCancellation = cancellation;
    emit scanStarted();
    emit scanProgress(0);

    int lastWorkerProgress = 0;
    options.progressCallback = [self, lastWorkerProgress](const killcore::ScanProgress& progress) mutable {
        int percent = 1;
        if (progress.bytesTotal > 0) {
            percent = 1 + static_cast<int>((progress.bytesScanned * 94) / progress.bytesTotal);
        } else if (progress.regionsTotal > 0) {
            percent = 1 + static_cast<int>((progress.regionsScanned * 94) / progress.regionsTotal);
        }
        percent = std::clamp(percent, 1, 95);
        if (percent <= lastWorkerProgress || (percent - lastWorkerProgress < 2 && percent < 95)) {
            return;
        }
        lastWorkerProgress = percent;
        emitQueuedScanProgress(self, percent);
    };

    std::thread([self, requestId, pid, value, valueType, expertOptions, scanValue, options, scannedBytes, cancellation]() {
        QElapsedTimer timer;
        timer.start();
        killcore::ScanResult scan;
        killcore::ProcessHandle workerHandle(static_cast<uint32_t>(pid), killcore::ProcessAccess::ReadOnly);
        if (!workerHandle.isValid()) {
            scan.success = false;
            scan.errorMessage = "Impossible d'ouvrir le processus dans le worker de scan.";
        } else {
            killcore::ScanEngine scanner(workerHandle);
            scan = scanner.exactScan(scanValue, options, cancellation.get());
        }
        const qint64 elapsedMs = timer.elapsed();

        if (!self) {
            return;
        }
        QMetaObject::invokeMethod(self.data(), [self, requestId, value, valueType, expertOptions, scannedBytes, scan, elapsedMs]() {
            if (!self) {
                return;
            }

            QVariantMap finished;
            QVariantList finishedMatches;
            if (!scan.cancelled) {
                self->clearCandidateUndo();
                self->clearCandidateValueHistory();
                self->m_candidates.replaceFromScan(scan, scannedBytes);
            }

            const qsizetype previewCount = std::min<qsizetype>(scan.matches.size(), 50);
            for (qsizetype i = 0; i < previewCount; ++i) {
                const auto& match = scan.matches.at(i);
                QVariantMap entry;
                entry["address"] = QString::number(match.address, 16);
                entry["type"] = killcore::valueTypeToString(match.type);
                finishedMatches.append(entry);
            }

            finished["requestId"] = requestId;
            finished["success"] = scan.success;
            finished["partial"] = scan.partial;
            finished["cancelled"] = scan.cancelled;
            finished["regionsScanned"] = static_cast<int>(scan.regionsScanned);
            finished["bytesScanned"] = static_cast<qulonglong>(scan.bytesScanned);
            finished["matchesFound"] = static_cast<qulonglong>(scan.matchesFound);
            finished["matchesReturned"] = finishedMatches.size();
            finished["error"] = scan.errorMessage;
            finished["matches"] = finishedMatches;
            finished["candidateStoreSize"] = static_cast<qulonglong>(self->m_candidates.size());
            finished["elapsedMs"] = static_cast<qulonglong>(elapsedMs);
            finished["bytesPerSecond"] = ratePerSecond(scan.bytesScanned, elapsedMs);
            finished["matchesPerSecond"] = ratePerSecond(scan.matchesFound, elapsedMs);
            finished["candidateStoreFileBacked"] = self->m_candidates.isFileBacked();
            finished["candidateStoreBytes"] = static_cast<qulonglong>(self->m_candidates.storageBytes());
            finished["candidateStoreMemoryBytes"] = static_cast<qulonglong>(self->m_candidates.estimatedMemoryBytes());
            self->appendSmartSearchDebug("exact_scan_async", {
                {"requestId", requestId},
                {"value", value},
                {"valueType", valueType},
                {"startAddress", expertOptions.value("startAddress")},
                {"stopAddress", expertOptions.value("stopAddress")},
                {"alignment", expertOptions.value("alignment")},
                {"success", finished.value("success")},
                {"matchesFound", finished.value("matchesFound")},
                {"candidateStoreSize", finished.value("candidateStoreSize")},
                {"elapsedMs", finished.value("elapsedMs")},
                {"bytesPerSecond", finished.value("bytesPerSecond")},
                {"matchesPerSecond", finished.value("matchesPerSecond")},
                {"candidateStoreFileBacked", finished.value("candidateStoreFileBacked")},
                {"candidateStoreBytes", finished.value("candidateStoreBytes")},
                {"candidateStoreMemoryBytes", finished.value("candidateStoreMemoryBytes")},
                {"cancelled", scan.cancelled},
                {"error", finished.value("error")},
            });
            self->appendScanTelemetry("exact_scan_async", {
                {"requestId", requestId},
                {"value", value},
                {"valueType", valueType},
                {"startAddress", expertOptions.value("startAddress")},
                {"stopAddress", expertOptions.value("stopAddress")},
                {"alignment", expertOptions.value("alignment")},
                {"writableOnly", expertOptions.value("writableOnly")},
                {"executableOnly", expertOptions.value("executableOnly")},
                {"copyOnWriteOnly", expertOptions.value("copyOnWriteOnly")},
                {"success", finished.value("success")},
                {"partial", finished.value("partial")},
                {"cancelled", finished.value("cancelled")},
                {"regionsScanned", finished.value("regionsScanned")},
                {"bytesScanned", finished.value("bytesScanned")},
                {"matchesFound", finished.value("matchesFound")},
                {"candidateStoreSize", finished.value("candidateStoreSize")},
                {"candidateStoreFileBacked", finished.value("candidateStoreFileBacked")},
                {"candidateStoreBytes", finished.value("candidateStoreBytes")},
                {"candidateStoreMemoryBytes", finished.value("candidateStoreMemoryBytes")},
                {"elapsedMs", finished.value("elapsedMs")},
                {"bytesPerSecond", finished.value("bytesPerSecond")},
                {"matchesPerSecond", finished.value("matchesPerSecond")},
                {"error", finished.value("error")},
            });
            self->m_scanInProgress = false;
            self->m_activeScanCancellation.reset();
            emit self->scanStatsUpdated(static_cast<int>(self->m_candidates.size()));
            emit self->scanProgress(100);
            emit self->scanFinished(finished);
        }, Qt::QueuedConnection);
    }).detach();

    result["success"] = true;
    result["started"] = true;
    result["requestId"] = requestId;
    result["error"] = "";
    return result;
}

QVariantMap ApplicationController::cancelActiveScan() {
    QVariantMap result;
    result["success"] = false;
    if (!m_scanInProgress || !m_activeScanCancellation) {
        result["error"] = "Aucun scan actif à annuler.";
        return result;
    }
    m_activeScanCancellation->cancel();
    result["success"] = true;
    result["error"] = "";
    return result;
}

QVariantMap ApplicationController::nextScanAsync(const QString& mode, const QString& value) {
    QVariantMap result;
    result["success"] = false;
    result["started"] = false;

    if (m_scanInProgress) {
        result["error"] = "Un scan est déjà en cours.";
        return result;
    }
    if (!m_handle.isValid()) {
        result["error"] = "Aucun processus attaché.";
        return result;
    }
    if (m_candidates.isEmpty()) {
        result["error"] = "Aucun candidat à filtrer. Lance d'abord un scan exact.";
        return result;
    }

    killcore::NextScanMode scanMode;
    if (!killcore::parseNextScanMode(mode, &scanMode)) {
        result["error"] = "Mode de next scan invalide.";
        return result;
    }

    killcore::Candidate firstCandidate;
    if (!m_candidates.firstCandidate(&firstCandidate)) {
        result["error"] = "Impossible de lire le premier candidat.";
        return result;
    }

    const auto candidateSnapshot = m_candidates.streamSnapshot();
    const auto firstCandidateType = firstCandidate.type;
    const auto candidateThreshold = m_candidates.fileBackedThreshold();
    double targetNumber = 0.0;
    if (scanMode == killcore::NextScanMode::Exact && value.trimmed().isEmpty()) {
        result["error"] = "Valeur requise pour un next scan exact.";
        return result;
    }
    if (scanMode == killcore::NextScanMode::Delta) {
        bool ok = false;
        targetNumber = value.trimmed().replace(',', '.').toDouble(&ok);
        if (!ok) {
            result["error"] = "Valeur delta invalide.";
            return result;
        }
    }

    const int requestId = m_nextScanRequestId++;
    const int pid = m_pid;
    const QPointer<ApplicationController> self(this);
    auto cancellation = std::make_shared<killcore::CancellationToken>();

    m_scanInProgress = true;
    m_activeScanCancellation = cancellation;
    emit scanStarted();
    emit scanProgress(0);

    std::thread([self, requestId, pid, mode, value, scanMode, firstCandidateType, candidateSnapshot, candidateThreshold, targetNumber, cancellation]() mutable {
        QElapsedTimer timer;
        timer.start();
        QVariantMap finished;
        QVariantList debugSamples;
        QVariantList valueHistoryUpdates;
        killcore::CandidateStore survivors;
        survivors.setFileBackedThreshold(candidateThreshold);
        QList<killcore::Candidate> memorySurvivors;
        if (candidateSnapshot.totalCount <= candidateThreshold) {
            memorySurvivors.reserve(static_cast<qsizetype>(candidateSnapshot.totalCount));
        }

        size_t checked = 0;
        size_t unreadable = 0;
        bool cancelled = false;
        QString error;
        bool streamInput = candidateSnapshot.fileBacked;
        bool streamOutput = candidateSnapshot.totalCount > candidateThreshold;
        int lastWorkerProgress = 0;
        const size_t progressTotal = std::max<size_t>(candidateSnapshot.totalCount, 1);
        auto reportCandidateProgress = [&]() {
            int percent = 1 + static_cast<int>((checked * 94) / progressTotal);
            percent = std::clamp(percent, 1, 95);
            if (percent <= lastWorkerProgress || (percent - lastWorkerProgress < 2 && percent < 95)) {
                return;
            }
            lastWorkerProgress = percent;
            emitQueuedScanProgress(self, percent);
        };

        if (streamOutput && !survivors.beginFileBackedReplacement(&error)) {
            streamOutput = false;
        }

        killcore::ProcessHandle workerHandle(static_cast<uint32_t>(pid), killcore::ProcessAccess::ReadOnly);
        if (!error.isEmpty()) {
            // error already set
        } else if (!workerHandle.isValid()) {
            error = "Impossible d'ouvrir le processus dans le worker de next scan.";
        } else {
            killcore::MemoryReader reader(workerHandle);
            QString streamError;
            const bool completed = killcore::CandidateStore::forEachCandidate(candidateSnapshot, [&](const killcore::Candidate& candidate) {
                if (cancellation->isCancelled()) {
                    cancelled = true;
                    error = "Next scan annulé.";
                    return false;
                }

                const size_t bytesToRead = killcore::valueTypeSize(candidate.type);
                const auto read = reader.read(candidate.address, bytesToRead);
                ++checked;
                if (checked % 4096 == 0 || checked == candidateSnapshot.totalCount) {
                    reportCandidateProgress();
                }

                if (!(read.success || read.partial) || read.bytesRead != bytesToRead) {
                    ++unreadable;
                    if (debugSamples.size() < 20) {
                        QVariantMap sample;
                        sample["address"] = QString::number(candidate.address, 16);
                        sample["type"] = killcore::valueTypeToString(candidate.type);
                        sample["previousHex"] = bytesToHex(candidate.lastValue);
                        sample["readable"] = false;
                        sample["readBytes"] = static_cast<qulonglong>(read.bytesRead);
                        sample["error"] = read.errorMessage;
                        debugSamples.append(sample);
                    }
                    return true;
                }

                const QByteArray current = read.data;
                const double previousNumber = bytesToDouble(candidate.lastValue, candidate.type);
                const double currentNumber = bytesToDouble(current, candidate.type);

                bool keep = false;
                killcore::Candidate updatedCandidate = candidate;
                switch (scanMode) {
                    case killcore::NextScanMode::Exact: {
                        QString targetError;
                        keep = matchCandidateExactVariant(value, candidate, current, &updatedCandidate, &targetError);
                        if (!targetError.isEmpty()) {
                            error = targetError.isEmpty()
                                ? "Impossible de construire la valeur cible pour un candidat."
                                : targetError;
                            return false;
                        }
                        break;
                    }
                    case killcore::NextScanMode::Changed:
                        keep = !bytesEqual(current, candidate.lastValue, candidate.type);
                        break;
                    case killcore::NextScanMode::Unchanged:
                        keep = bytesEqual(current, candidate.lastValue, candidate.type);
                        break;
                    case killcore::NextScanMode::Increased:
                        keep = currentNumber > previousNumber;
                        break;
                    case killcore::NextScanMode::Decreased:
                        keep = currentNumber < previousNumber;
                        break;
                    case killcore::NextScanMode::Delta:
                        keep = std::abs((currentNumber - previousNumber) - targetNumber) < 0.000001;
                        break;
                }

                if (debugSamples.size() < 20) {
                    QVariantMap sample;
                    sample["address"] = QString::number(candidate.address, 16);
                    sample["type"] = killcore::valueTypeToString(candidate.type);
                    sample["previousHex"] = bytesToHex(candidate.lastValue);
                    sample["currentHex"] = bytesToHex(current);
                    sample["previousNumber"] = previousNumber;
                    sample["currentNumber"] = currentNumber;
                    sample["keep"] = keep;
                    debugSamples.append(sample);
                }

                if (keep) {
                    if (valueHistoryUpdates.size() < kCandidateHistoryMaxAddresses) {
                        valueHistoryUpdates.append(candidateObservationToVariantMap(updatedCandidate, current, "next_scan_async", true, true));
                    }
                    auto updated = updatedCandidate;
                    updated.lastValue = current;
                    if (streamOutput) {
                        if (!survivors.appendFileBackedCandidate(updated, &error)) {
                            return false;
                        }
                    } else {
                        memorySurvivors.append(updated);
                    }
                }
                return true;
            }, &streamError);

            if (!completed && error.isEmpty()) {
                error = streamError.isEmpty() ? "Next scan interrompu." : streamError;
            }
            if (error.isEmpty() && streamOutput && !survivors.finishFileBackedReplacement(&error)) {
                // error filled by finishFileBackedReplacement
            }
        }

        if (!self) {
            return;
        }
        const qint64 elapsedMs = timer.elapsed();

        QMetaObject::invokeMethod(self.data(), [self, requestId, mode, value, firstCandidateType, candidateSnapshot, survivors = std::move(survivors), memorySurvivors = std::move(memorySurvivors), checked, unreadable, cancelled, error, debugSamples, valueHistoryUpdates, streamInput, streamOutput, elapsedMs]() mutable {
            if (!self) {
                return;
            }

            QString finishError = error;
            if (!cancelled && finishError.isEmpty()) {
                QString undoError;
                if (!self->rememberCandidatesForUndo(&undoError)) {
                    finishError = undoError.isEmpty() ? "Impossible de préparer l'annulation du next scan." : undoError;
                } else {
                    if (streamOutput) {
                        self->m_candidates = std::move(survivors);
                    } else {
                        self->m_candidates.replaceCandidates(memorySurvivors);
                    }
                    self->recordCandidateObservations(valueHistoryUpdates);
                }
            }
            // H1 : streamOutput (file-backed) implique deja un nombre de
            // survivants bien au-dela du seuil "petit groupe" — seul le cas
            // memorySurvivors est pertinent ici, cf. detectStableCandidateGroup.
            QVariantMap stableGroupInfo;
            killcore::NextScanMode parsedMode;
            if (!cancelled && finishError.isEmpty() && !streamOutput
                && killcore::parseNextScanMode(mode, &parsedMode)) {
                self->detectStableCandidateGroup(parsedMode, memorySurvivors, &stableGroupInfo);
            }

            QVariantMap finished;
            finished["requestId"] = requestId;
            finished["kind"] = "next_scan";
            finished["success"] = finishError.isEmpty();
            finished["cancelled"] = cancelled;
            finished["checked"] = static_cast<qulonglong>(checked);
            finished["unreadable"] = static_cast<qulonglong>(unreadable);
            finished["remaining"] = static_cast<qulonglong>(cancelled ? candidateSnapshot.totalCount : self->m_candidates.size());
            finished["error"] = finishError;
            finished["debugBeforeCount"] = static_cast<qulonglong>(candidateSnapshot.totalCount);
            finished["debugMode"] = mode;
            finished["debugValue"] = value;
            finished["streamInput"] = streamInput;
            finished["streamOutput"] = streamOutput;
            finished["fileBacked"] = self->m_candidates.isFileBacked();
            finished["candidateStorePath"] = self->m_candidates.backingFilePath();
            finished["elapsedMs"] = static_cast<qulonglong>(elapsedMs);
            finished["candidatesPerSecond"] = ratePerSecond(checked, elapsedMs);
            finished["candidateStoreBytes"] = static_cast<qulonglong>(self->m_candidates.storageBytes());
            finished["candidateStoreMemoryBytes"] = static_cast<qulonglong>(self->m_candidates.estimatedMemoryBytes());
            if (finished.value("remaining").toULongLong() == 0 && !cancelled) {
                finished["diagnostic"] = noCandidateDiagnosticMessage(finished, "next_scan");
            }
            for (auto it = stableGroupInfo.constBegin(); it != stableGroupInfo.constEnd(); ++it) {
                finished[it.key()] = it.value();
            }

            self->appendSmartSearchDebug("next_scan_async", {
                {"requestId", requestId},
                {"mode", mode},
                {"value", value},
                {"candidateType", killcore::valueTypeToString(firstCandidateType)},
                {"beforeCount", static_cast<qulonglong>(candidateSnapshot.totalCount)},
                {"checked", finished.value("checked")},
                {"unreadable", finished.value("unreadable")},
                {"remaining", finished.value("remaining")},
                {"cancelled", cancelled},
                {"streamInput", streamInput},
                {"streamOutput", streamOutput},
                {"fileBacked", finished.value("fileBacked")},
                {"candidateStorePath", finished.value("candidateStorePath")},
                {"elapsedMs", finished.value("elapsedMs")},
                {"candidatesPerSecond", finished.value("candidatesPerSecond")},
                {"candidateStoreBytes", finished.value("candidateStoreBytes")},
                {"candidateStoreMemoryBytes", finished.value("candidateStoreMemoryBytes")},
                {"valueHistoryUpdateCount", valueHistoryUpdates.size()},
                {"error", finishError},
                {"samples", debugSamples},
            });
            self->appendScanTelemetry("next_scan_async", {
                {"requestId", requestId},
                {"mode", mode},
                {"value", value},
                {"candidateType", killcore::valueTypeToString(firstCandidateType)},
                {"beforeCount", static_cast<qulonglong>(candidateSnapshot.totalCount)},
                {"checked", finished.value("checked")},
                {"unreadable", finished.value("unreadable")},
                {"remaining", finished.value("remaining")},
                {"cancelled", cancelled},
                {"streamInput", streamInput},
                {"streamOutput", streamOutput},
                {"fileBacked", finished.value("fileBacked")},
                {"candidateStorePath", finished.value("candidateStorePath")},
                {"elapsedMs", finished.value("elapsedMs")},
                {"candidatesPerSecond", finished.value("candidatesPerSecond")},
                {"candidateStoreBytes", finished.value("candidateStoreBytes")},
                {"candidateStoreMemoryBytes", finished.value("candidateStoreMemoryBytes")},
                {"error", finishError},
            });

            self->m_scanInProgress = false;
            self->m_activeScanCancellation.reset();
            emit self->scanStatsUpdated(static_cast<int>(self->m_candidates.size()));
            emit self->scanProgress(100);
            emit self->scanFinished(finished);
        }, Qt::QueuedConnection);
    }).detach();

    result["success"] = true;
    result["started"] = true;
    result["requestId"] = requestId;
    result["error"] = "";
    return result;
}

QVariantMap ApplicationController::nextScan(const QString& mode, const QString& value) {
    QVariantMap result;
    result["success"] = false;

    if (!m_handle.isValid()) {
        result["error"] = "Aucun processus attaché.";
        return result;
    }

    if (m_candidates.isEmpty()) {
        result["error"] = "Aucun candidat à filtrer. Lance d'abord un scan exact.";
        return result;
    }

    killcore::NextScanMode scanMode;
    if (!killcore::parseNextScanMode(mode, &scanMode)) {
        result["error"] = "Mode de next scan invalide.";
        return result;
    }

    double targetNumber = 0.0;
    const auto firstCandidateType = m_candidates.candidates().first().type;

    if (scanMode == killcore::NextScanMode::Exact && value.trimmed().isEmpty()) {
        result["error"] = "Valeur requise pour un next scan exact.";
        return result;
    }
    if (scanMode == killcore::NextScanMode::Delta) {
        bool ok = false;
        targetNumber = value.trimmed().replace(',', '.').toDouble(&ok);
        if (!ok) {
            result["error"] = "Valeur delta invalide.";
            return result;
        }
    }

    double rangeMin = 0.0;
    double rangeMax = 0.0;
    if (scanMode == killcore::NextScanMode::Between) {
        // Separateurs acceptes : ',' et ';'. Pas de '-' (ambigu avec un nombre
        // negatif). ';' en priorite pour permettre ',' comme separateur decimal
        // dans chaque borne (ex. "50,5;100,2").
        const QString rangeText = value.trimmed();
        QStringList parts = rangeText.split(';', Qt::SkipEmptyParts);
        if (parts.size() != 2) {
            parts = rangeText.split(',', Qt::SkipEmptyParts);
        }
        bool okMin = false;
        bool okMax = false;
        if (parts.size() == 2) {
            rangeMin = parts[0].trimmed().replace(',', '.').toDouble(&okMin);
            rangeMax = parts[1].trimmed().replace(',', '.').toDouble(&okMax);
        }
        if (!okMin || !okMax) {
            result["error"] = "Plage invalide. Utilise le format \"min,max\" (ex. 50,100). "
                               "Le séparateur '-' n'est pas supporté (ambigu avec un nombre négatif).";
            return result;
        }
        if (rangeMin > rangeMax) {
            result["error"] = "Plage invalide : min doit être ≤ max.";
            return result;
        }
    }

    emit scanStarted();
    emit scanProgress(0);
    killcore::MemoryReader reader(m_handle);
    QList<killcore::Candidate> survivors;
    survivors.reserve(m_candidates.candidates().size());

    const size_t beforeCount = m_candidates.size();
    size_t checked = 0;
    size_t unreadable = 0;
    QVariantList debugSamples;
    QVariantList valueHistoryUpdates;

    for (const auto& candidate : m_candidates.candidates()) {
        const size_t bytesToRead = killcore::valueTypeSize(candidate.type);
        const auto read = reader.read(candidate.address, bytesToRead);
        ++checked;

        if (!(read.success || read.partial) || read.bytesRead != bytesToRead) {
            ++unreadable;
            if (debugSamples.size() < 20) {
                QVariantMap sample;
                sample["address"] = QString::number(candidate.address, 16);
                sample["type"] = killcore::valueTypeToString(candidate.type);
                sample["previousHex"] = bytesToHex(candidate.lastValue);
                sample["readable"] = false;
                sample["readBytes"] = static_cast<qulonglong>(read.bytesRead);
                sample["error"] = read.errorMessage;
                debugSamples.append(sample);
            }
            continue;
        }

        const QByteArray current = read.data;
        const double previousNumber = bytesToDouble(candidate.lastValue, candidate.type);
        const double currentNumber = bytesToDouble(current, candidate.type);

        bool keep = false;
        killcore::Candidate updatedCandidate = candidate;
        switch (scanMode) {
            case killcore::NextScanMode::Exact: {
                QString targetError;
                keep = matchCandidateExactVariant(value, candidate, current, &updatedCandidate, &targetError);
                if (!targetError.isEmpty()) {
                    result["error"] = targetError.isEmpty()
                        ? "Impossible de construire la valeur cible pour un candidat."
                        : targetError;
                    emit scanProgress(100);
                    return result;
                }
                break;
            }
            case killcore::NextScanMode::Changed:
                keep = !bytesEqual(current, candidate.lastValue, candidate.type);
                break;
            case killcore::NextScanMode::Unchanged:
                keep = bytesEqual(current, candidate.lastValue, candidate.type);
                break;
            case killcore::NextScanMode::Increased:
                keep = currentNumber > previousNumber;
                break;
            case killcore::NextScanMode::Decreased:
                keep = currentNumber < previousNumber;
                break;
            case killcore::NextScanMode::Delta:
                keep = std::abs((currentNumber - previousNumber) - targetNumber) < 0.000001;
                break;
            case killcore::NextScanMode::Between:
                keep = currentNumber >= rangeMin && currentNumber <= rangeMax;
                break;
        }

        if (debugSamples.size() < 20) {
            QVariantMap sample;
            sample["address"] = QString::number(candidate.address, 16);
            sample["type"] = killcore::valueTypeToString(candidate.type);
            sample["previousHex"] = bytesToHex(candidate.lastValue);
            sample["currentHex"] = bytesToHex(current);
            sample["previousNumber"] = previousNumber;
            sample["currentNumber"] = currentNumber;
            sample["keep"] = keep;
            debugSamples.append(sample);
        }

        if (keep) {
            if (valueHistoryUpdates.size() < kCandidateHistoryMaxAddresses) {
                valueHistoryUpdates.append(candidateObservationToVariantMap(updatedCandidate, current, "next_scan", true, true));
            }
            auto updated = updatedCandidate;
            updated.lastValue = current;
            survivors.append(updated);
        }
    }

    QString undoError;
    if (!rememberCandidatesForUndo(&undoError)) {
        result["error"] = undoError.isEmpty() ? "Impossible de préparer l'annulation du next scan." : undoError;
        emit scanProgress(100);
        return result;
    }
    m_candidates.replaceCandidates(survivors);
    recordCandidateObservations(valueHistoryUpdates);
    detectStableCandidateGroup(scanMode, survivors, &result);

    result["success"] = true;
    result["checked"] = static_cast<qulonglong>(checked);
    result["unreadable"] = static_cast<qulonglong>(unreadable);
    result["remaining"] = static_cast<qulonglong>(m_candidates.size());
    result["error"] = "";
    result["debugBeforeCount"] = static_cast<qulonglong>(beforeCount);
    result["debugMode"] = mode;
    result["debugValue"] = value;
    if (m_candidates.size() == 0) {
        result["diagnostic"] = noCandidateDiagnosticMessage(result, "next_scan");
    }
    appendSmartSearchDebug("next_scan", {
        {"mode", mode},
        {"value", value},
        {"candidateType", killcore::valueTypeToString(firstCandidateType)},
        {"beforeCount", static_cast<qulonglong>(beforeCount)},
        {"checked", result.value("checked")},
        {"unreadable", result.value("unreadable")},
        {"remaining", result.value("remaining")},
        {"valueHistoryUpdateCount", valueHistoryUpdates.size()},
        {"samples", debugSamples},
    });
    emit scanStatsUpdated(static_cast<int>(m_candidates.size()));
    emit scanProgress(100);
    return result;
}

QVariantMap ApplicationController::undoCandidateScan() {
    QVariantMap result;
    result["success"] = false;
    result["restored"] = false;
    result["count"] = static_cast<qulonglong>(m_candidates.size());

    if (m_scanInProgress) {
        result["error"] = "Impossible de restaurer pendant un scan actif.";
        return result;
    }

    if (!m_hasPreviousCandidates || m_previousCandidates.isEmpty()) {
        result["error"] = "Aucune réduction précédente à restaurer.";
        return result;
    }

    m_candidates = std::move(m_previousCandidates);
    m_hasPreviousCandidates = false;

    result["success"] = true;
    result["restored"] = true;
    result["count"] = static_cast<qulonglong>(m_candidates.size());
    clearCandidateValueHistory();
    result["fileBacked"] = m_candidates.isFileBacked();
    result["candidateStorePath"] = m_candidates.backingFilePath();
    result["candidateStoreBytes"] = static_cast<qulonglong>(m_candidates.storageBytes());
    result["candidateStoreMemoryBytes"] = static_cast<qulonglong>(m_candidates.estimatedMemoryBytes());
    result["error"] = "";

    appendSmartSearchDebug("undo_candidate_scan", {
        {"restored", true},
        {"count", result.value("count")},
        {"fileBacked", result.value("fileBacked")},
        {"candidateStoreBytes", result.value("candidateStoreBytes")},
        {"candidateStoreMemoryBytes", result.value("candidateStoreMemoryBytes")},
    });
    emit scanStatsUpdated(static_cast<int>(m_candidates.size()));
    emit scanProgress(100);
    return result;
}

QVariantMap ApplicationController::getCandidates(int pageIndex, int pageSize, const QString& addressFilter) const {
    QVariantMap result;
    QVariantList candidates;

    const int boundedPageIndex = std::max(0, pageIndex);
    const int boundedPageSize = std::clamp(pageSize, 1, 500);
    const bool hasAddressFilter = !addressFilter.trimmed().isEmpty();
    const bool displaySuppressed = !hasAddressFilter && m_candidates.size() > kCandidateDisplayLimit;

    if (displaySuppressed) {
        result["pageIndex"] = boundedPageIndex;
        result["pageSize"] = boundedPageSize;
        result["totalCount"] = static_cast<qulonglong>(m_candidates.size());
        result["displaySuppressed"] = true;
        result["displayLimit"] = static_cast<qulonglong>(kCandidateDisplayLimit);
        result["fileBacked"] = m_candidates.isFileBacked();
        result["candidateStorePath"] = m_candidates.backingFilePath();
        result["candidateStoreBytes"] = static_cast<qulonglong>(m_candidates.storageBytes());
        result["candidateStoreMemoryBytes"] = static_cast<qulonglong>(m_candidates.estimatedMemoryBytes());
        result["candidates"] = candidates;
        return result;
    }

    const auto page = m_candidates.page(
        static_cast<size_t>(boundedPageIndex),
        static_cast<size_t>(boundedPageSize),
        addressFilter);

    for (const auto& candidate : page.candidates) {
        QVariantMap candidateMap = candidateToVariantMap(candidate);
        // H6 (docs/STRATEGY_ROOM.md, session Solitaire du 19/08/2026) : un
        // candidat stable sur plusieurs next scan n'est PAS la preuve qu'il
        // pilote réellement l'affichage — observé deux fois en conditions
        // réelles (Score et XP de Solitaire, tous deux stables sur 8 cycles
        // mais nécessitant une vraie écriture confirmée pour le prouver).
        // Dimension distincte de la confiance de scan existante : "confirmé
        // par une écriture réussie" plutôt que blend dans un seul score.
        candidateMap["writeVerified"] = hasAddressBeenWriteVerified(candidate.address);
        candidates.append(candidateMap);
    }

    result["pageIndex"] = static_cast<int>(page.pageIndex);
    result["pageSize"] = static_cast<int>(page.pageSize);
    result["totalCount"] = static_cast<qulonglong>(page.totalCount);
    result["displaySuppressed"] = false;
    result["displayLimit"] = static_cast<qulonglong>(kCandidateDisplayLimit);
    result["fileBacked"] = m_candidates.isFileBacked();
    result["candidateStorePath"] = m_candidates.backingFilePath();
    result["candidateStoreBytes"] = static_cast<qulonglong>(m_candidates.storageBytes());
    result["candidateStoreMemoryBytes"] = static_cast<qulonglong>(m_candidates.estimatedMemoryBytes());
    result["candidates"] = candidates;
    return result;
}

QVariantMap ApplicationController::captureUnknownSnapshot() {
    return captureUnknownSnapshotWithOptions({});
}

QVariantMap ApplicationController::captureUnknownSnapshotWithOptions(const QVariantMap& expertOptions) {
    QVariantMap result;
    result["success"] = false;

    if (!m_handle.isValid()) {
        result["error"] = "Aucun processus attaché.";
        return result;
    }

    m_candidates.clear();
    clearCandidateUndo();
    clearCandidateValueHistory();
    m_smartSearchActive = false;
    m_smartSearchInitialValue.clear();
    m_smartSearchTargetValue.clear();

    emit scanStarted();
    emit scanProgress(0);
    const auto options = scanOptionsFromSettingsAndExpertOptions(expertOptions);
    int suggestedDepthMb = 0;
    uint64_t relevantBytes = 0;
    const size_t maxSnapshotBytes = resolveUnknownSnapshotMaxBytes(
        expertOptions, m_handle, options, &suggestedDepthMb, &relevantBytes);
    const auto snapshot = m_snapshot.capture(m_handle, maxSnapshotBytes, nullptr, options);
    emit scanProgress(100);
    result["success"] = snapshot.success;
    result["partial"] = snapshot.partial;
    result["cancelled"] = snapshot.cancelled;
    result["regionsCaptured"] = static_cast<qulonglong>(snapshot.regionsCaptured);
    result["regionsSkipped"] = static_cast<qulonglong>(snapshot.regionsSkipped);
    result["bytesCaptured"] = static_cast<qulonglong>(snapshot.bytesCaptured);
    result["captureLimitBytes"] = static_cast<qulonglong>(maxSnapshotBytes);
    result["captureLimitReached"] = snapshot.partial || snapshot.bytesCaptured >= maxSnapshotBytes;
    result["compressedBytes"] = static_cast<qulonglong>(snapshot.compressedBytes);
    result["mappedStorage"] = snapshot.mappedStorage;
    result["writableOnly"] = options.writableOnly;
    result["executableOnly"] = options.executableOnly;
    result["copyOnWriteOnly"] = options.copyOnWriteOnly;
    result["suggestedDepthMb"] = suggestedDepthMb;
    result["relevantBytes"] = static_cast<qulonglong>(relevantBytes);
    result["autoDepthApplied"] = requestedUnknownSnapshotMaxMb(expertOptions) == -1;
    result["error"] = snapshot.errorMessage;
    return result;
}

QVariantMap ApplicationController::captureUnknownSnapshotAsync() {
    return captureUnknownSnapshotAsyncWithOptions({});
}

QVariantMap ApplicationController::captureUnknownSnapshotAsyncWithOptions(const QVariantMap& expertOptions) {
    QVariantMap result;
    result["success"] = false;
    result["started"] = false;

    if (m_scanInProgress) {
        result["error"] = "Un scan est déjà en cours.";
        return result;
    }
    if (!m_handle.isValid()) {
        result["error"] = "Aucun processus attaché.";
        return result;
    }

    m_candidates.clear();
    clearCandidateUndo();
    clearCandidateValueHistory();
    m_smartSearchActive = false;
    m_smartSearchInitialValue.clear();
    m_smartSearchTargetValue.clear();

    const int requestId = m_nextScanRequestId++;
    const int pid = m_pid;
    int suggestedDepthMb = 0;
    uint64_t relevantBytes = 0;
    auto options = scanOptionsFromSettingsAndExpertOptions(expertOptions);
    const size_t maxSnapshotBytes = resolveUnknownSnapshotMaxBytes(
        expertOptions, m_handle, options, &suggestedDepthMb, &relevantBytes);
    const QPointer<ApplicationController> self(this);
    auto cancellation = std::make_shared<killcore::CancellationToken>();

    m_scanInProgress = true;
    m_activeScanCancellation = cancellation;
    emit scanStarted();
    emit scanProgress(0);

    // Capture "Unknown initial value" : peut lire des centaines de Mo à
    // plusieurs Go de mémoire du processus cible (le pire cas en durée de
    // toute l'app) — même granularité de rapport que startExactScanAsync
    // (SnapshotStore::capture rapporte maintenant par région).
    int lastSnapshotProgress = 0;
    options.progressCallback = [self, lastSnapshotProgress](const killcore::ScanProgress& progress) mutable {
        int percent = 1;
        if (progress.regionsTotal > 0) {
            percent = 1 + static_cast<int>((progress.regionsScanned * 94) / progress.regionsTotal);
        }
        percent = std::clamp(percent, 1, 95);
        if (percent <= lastSnapshotProgress || (percent - lastSnapshotProgress < 2 && percent < 95)) {
            return;
        }
        lastSnapshotProgress = percent;
        emitQueuedScanProgress(self, percent);
    };

    const bool autoDepthApplied = requestedUnknownSnapshotMaxMb(expertOptions) == -1;
    std::thread([self, requestId, pid, maxSnapshotBytes, suggestedDepthMb, relevantBytes, autoDepthApplied, options, cancellation]() mutable {
        killcore::SnapshotStore snapshotStore;
        killcore::ProcessHandle workerHandle(static_cast<uint32_t>(pid), killcore::ProcessAccess::ReadOnly);
        killcore::SnapshotResult snapshot;
        if (!workerHandle.isValid()) {
            snapshot.success = false;
            snapshot.errorMessage = "Impossible d'ouvrir le processus dans le worker unknown.";
        } else {
            snapshot = snapshotStore.capture(workerHandle, maxSnapshotBytes, cancellation.get(), options);
        }

        if (!self) {
            return;
        }

        QMetaObject::invokeMethod(self.data(), [self, requestId, maxSnapshotBytes, suggestedDepthMb, relevantBytes, autoDepthApplied, snapshot, options, snapshotStore = std::move(snapshotStore)]() mutable {
            if (!self) {
                return;
            }

            if (snapshot.success && !snapshot.cancelled) {
                self->m_snapshot = std::move(snapshotStore);
            }

            QVariantMap finished;
            finished["requestId"] = requestId;
            finished["kind"] = "unknown_capture";
            finished["success"] = snapshot.success;
            finished["partial"] = snapshot.partial;
            finished["cancelled"] = snapshot.cancelled;
            finished["regionsCaptured"] = static_cast<qulonglong>(snapshot.regionsCaptured);
            finished["regionsSkipped"] = static_cast<qulonglong>(snapshot.regionsSkipped);
            finished["bytesCaptured"] = static_cast<qulonglong>(snapshot.bytesCaptured);
            finished["captureLimitBytes"] = static_cast<qulonglong>(maxSnapshotBytes);
            finished["captureLimitReached"] = snapshot.partial || snapshot.bytesCaptured >= maxSnapshotBytes;
            finished["compressedBytes"] = static_cast<qulonglong>(snapshot.compressedBytes);
            finished["mappedStorage"] = snapshot.mappedStorage;
            finished["writableOnly"] = options.writableOnly;
            finished["executableOnly"] = options.executableOnly;
            finished["copyOnWriteOnly"] = options.copyOnWriteOnly;
            finished["suggestedDepthMb"] = suggestedDepthMb;
            finished["relevantBytes"] = static_cast<qulonglong>(relevantBytes);
            finished["autoDepthApplied"] = autoDepthApplied;
            finished["error"] = snapshot.errorMessage;

            self->appendSmartSearchDebug("unknown_capture_async", {
                {"requestId", requestId},
                {"success", finished.value("success")},
                {"partial", finished.value("partial")},
                {"cancelled", finished.value("cancelled")},
                {"regionsCaptured", finished.value("regionsCaptured")},
                {"regionsSkipped", finished.value("regionsSkipped")},
                {"bytesCaptured", finished.value("bytesCaptured")},
                {"compressedBytes", finished.value("compressedBytes")},
                {"mappedStorage", finished.value("mappedStorage")},
                {"writableOnly", finished.value("writableOnly")},
                {"executableOnly", finished.value("executableOnly")},
                {"copyOnWriteOnly", finished.value("copyOnWriteOnly")},
                {"error", finished.value("error")},
            });
            self->appendScanTelemetry("unknown_capture_async", {
                {"requestId", requestId},
                {"success", finished.value("success")},
                {"partial", finished.value("partial")},
                {"cancelled", finished.value("cancelled")},
                {"regionsCaptured", finished.value("regionsCaptured")},
                {"regionsSkipped", finished.value("regionsSkipped")},
                {"bytesCaptured", finished.value("bytesCaptured")},
                {"captureLimitBytes", finished.value("captureLimitBytes")},
                {"captureLimitReached", finished.value("captureLimitReached")},
                {"captureLimitBytes", finished.value("captureLimitBytes")},
                {"captureLimitReached", finished.value("captureLimitReached")},
                {"compressedBytes", finished.value("compressedBytes")},
                {"mappedStorage", finished.value("mappedStorage")},
                {"writableOnly", finished.value("writableOnly")},
                {"executableOnly", finished.value("executableOnly")},
                {"copyOnWriteOnly", finished.value("copyOnWriteOnly")},
                {"error", finished.value("error")},
            });

            self->m_scanInProgress = false;
            self->m_activeScanCancellation.reset();
            emit self->scanProgress(100);
            emit self->scanFinished(finished);
        }, Qt::QueuedConnection);
    }).detach();

    result["success"] = true;
    result["started"] = true;
    result["requestId"] = requestId;
    result["error"] = "";
    return result;
}

QVariantMap ApplicationController::unknownNextScan(const QString& mode, const QString& valueType) {
    QVariantMap result;
    result["success"] = false;

    if (!m_handle.isValid()) {
        result["error"] = "Aucun processus attaché.";
        return result;
    }

    killcore::NextScanMode scanMode;
    if (!killcore::parseNextScanMode(mode, &scanMode)) {
        result["error"] = "Mode invalide.";
        return result;
    }
    if (!m_candidates.isEmpty()) {
        QVariantMap refined = nextScan(mode, "");
        refined["kind"] = "unknown_refine";
        refined["refinedFromCandidates"] = true;
        refined["checkedBytes"] = refined.value("checked");
        refined["matchesFound"] = refined.value("remaining");
        refined["stored"] = refined.value("remaining");
        return refined;
    }

    if (m_snapshot.isEmpty()) {
        result["error"] = "Aucun snapshot unknown capturé.";
        return result;
    }
    emit scanStarted();
    emit scanProgress(0);
    const QString normalizedValueType = valueType.trimmed();
    const bool autoType = normalizedValueType.compare("Auto", Qt::CaseInsensitive) == 0
        || normalizedValueType.compare("SmartAuto", Qt::CaseInsensitive) == 0
        || normalizedValueType.isEmpty();

    QList<killcore::Candidate> unknownCandidates;
    size_t checkedBytes = 0;
    size_t matchesFound = 0;
    bool partial = false;
    bool cancelled = false;
    bool compareSuccess = true;
    QString compareError;
    QVariantList typeSummaries;
    QSet<QString> seenCandidates;

    const auto compareTypes = autoType ? unknownAutoValueTypes() : QList<killcore::ValueType>{};
    killcore::ValueType singleType;
    if (!autoType && !killcore::parseValueType(valueType, &singleType)) {
        result["error"] = "Type invalide.";
        emit scanProgress(100);
        return result;
    }

    const auto typesToRun = autoType ? compareTypes : QList<killcore::ValueType>{singleType};
    for (const auto type : typesToRun) {
        const auto scan = m_snapshot.compare(m_handle, type, scanMode);
        checkedBytes += scan.checkedBytes;
        matchesFound += scan.matchesFound;
        partial = partial || scan.partial;
        cancelled = cancelled || scan.cancelled;
        compareSuccess = compareSuccess && scan.success;
        if (!scan.errorMessage.isEmpty() && compareError.isEmpty()) {
            compareError = scan.errorMessage;
        }

        const auto typeCandidates = candidatesFromUnknownScan(m_handle, scan);
        for (const auto& candidate : typeCandidates) {
            const QString key = QString::number(candidate.address, 16) + "|" + killcore::valueTypeToString(candidate.type);
            if (seenCandidates.contains(key)) {
                continue;
            }
            seenCandidates.insert(key);
            unknownCandidates.append(candidate);
            if (unknownCandidates.size() >= kUnknownAutoMaxReturnedMatches) {
                partial = true;
                compareError = QString("Trop de candidats unknown Auto (%1+). Raffine avec changed/increased/decreased ou reduis la plage.")
                                   .arg(kUnknownAutoMaxReturnedMatches);
                break;
            }
        }

        typeSummaries.append(QVariantMap{
            {"type", killcore::valueTypeToString(type)},
            {"success", scan.success},
            {"partial", scan.partial},
            {"checkedBytes", static_cast<qulonglong>(scan.checkedBytes)},
            {"matchesFound", static_cast<qulonglong>(scan.matchesFound)},
            {"stored", typeCandidates.size()},
            {"error", scan.errorMessage},
        });
        if (cancelled || unknownCandidates.size() >= kUnknownAutoMaxReturnedMatches) {
            break;
        }
    }
    emit scanProgress(90);

    QString undoError;
    if (!m_candidates.isEmpty() && !rememberCandidatesForUndo(&undoError)) {
        result["error"] = undoError.isEmpty() ? "Impossible de préparer l'annulation de la comparaison." : undoError;
        emit scanProgress(100);
        return result;
    }
    m_candidates.replaceCandidates(unknownCandidates);

    result["success"] = compareSuccess;
    result["partial"] = partial;
    result["cancelled"] = cancelled;
    result["checkedBytes"] = static_cast<qulonglong>(checkedBytes);
    result["matchesFound"] = static_cast<qulonglong>(matchesFound);
    result["stored"] = static_cast<qulonglong>(m_candidates.size());
    result["valueType"] = autoType ? QString("Auto") : valueType;
    result["typePasses"] = typeSummaries;
    result["error"] = compareError;
    if (m_candidates.size() == 0 && compareSuccess) {
        result["diagnostic"] = noCandidateDiagnosticMessage(result, "unknown_compare");
    }
    emit scanStatsUpdated(static_cast<int>(m_candidates.size()));
    emit scanProgress(100);
    return result;
}

QVariantMap ApplicationController::unknownNextScanAsync(const QString& mode, const QString& valueType) {
    QVariantMap result;
    result["success"] = false;
    result["started"] = false;

    if (m_scanInProgress) {
        result["error"] = "Un scan est déjà en cours.";
        return result;
    }
    if (!m_handle.isValid()) {
        result["error"] = "Aucun processus attaché.";
        return result;
    }

    killcore::NextScanMode scanMode;
    if (!killcore::parseNextScanMode(mode, &scanMode)) {
        result["error"] = "Mode invalide.";
        return result;
    }
    if (!m_candidates.isEmpty()) {
        const size_t refineCandidateCount = m_candidates.size();
        appendScanTelemetry("unknown_refine_async_start", {
            {"mode", mode},
            {"valueType", valueType},
            {"candidateCount", static_cast<qulonglong>(refineCandidateCount)},
            {"fileBacked", m_candidates.isFileBacked()},
            {"candidateStoreBytes", static_cast<qulonglong>(m_candidates.storageBytes())},
            {"candidateStoreMemoryBytes", static_cast<qulonglong>(m_candidates.estimatedMemoryBytes())},
        });
        if (refineCandidateCount <= 20000) {
            QElapsedTimer timer;
            timer.start();
            QVariantMap refined = nextScan(mode, "");
            refined["requestId"] = m_nextScanRequestId++;
            refined["kind"] = "unknown_refine";
            refined["refinedFromCandidates"] = true;
            refined["checkedBytes"] = refined.value("checked");
            refined["matchesFound"] = refined.value("remaining");
            refined["stored"] = refined.value("remaining");
            refined["elapsedMs"] = static_cast<qulonglong>(timer.elapsed());
            appendScanTelemetry("unknown_refine_direct", {
                {"requestId", refined.value("requestId")},
                {"mode", mode},
                {"valueType", valueType},
                {"beforeCount", static_cast<qulonglong>(refineCandidateCount)},
                {"checked", refined.value("checked")},
                {"unreadable", refined.value("unreadable")},
                {"remaining", refined.value("remaining")},
                {"elapsedMs", refined.value("elapsedMs")},
                {"success", refined.value("success")},
                {"error", refined.value("error")},
            });
            return refined;
        }
        QVariantMap refined = nextScanAsync(mode, "");
        refined["refinedFromCandidates"] = true;
        return refined;
    }

    if (m_snapshot.isEmpty()) {
        result["error"] = "Aucun snapshot unknown capturé.";
        return result;
    }
    const QString normalizedValueType = valueType.trimmed();
    const bool autoType = normalizedValueType.compare("Auto", Qt::CaseInsensitive) == 0
        || normalizedValueType.compare("SmartAuto", Qt::CaseInsensitive) == 0
        || normalizedValueType.isEmpty();
    killcore::ValueType singleType = killcore::ValueType::Int32;
    if (!autoType && !killcore::parseValueType(valueType, &singleType)) {
        result["error"] = "Type invalide.";
        return result;
    }
    const auto typesToRun = autoType ? unknownAutoValueTypes() : QList<killcore::ValueType>{singleType};

    const int requestId = m_nextScanRequestId++;
    const int pid = m_pid;
    const QPointer<ApplicationController> self(this);
    auto cancellation = std::make_shared<killcore::CancellationToken>();

    m_scanInProgress = true;
    m_activeScanCancellation = cancellation;
    emit scanStarted();
    emit scanProgress(0);

    std::thread([self, requestId, pid, mode, valueType, autoType, typesToRun, scanMode, cancellation]() {
        QList<killcore::Candidate> unknownCandidates;
        QVariantList typeSummaries;
        size_t checkedBytes = 0;
        size_t matchesFound = 0;
        bool partial = false;
        bool cancelled = false;
        bool compareSuccess = true;
        QString compareError;
        QSet<QString> seenCandidates;
        killcore::ProcessHandle workerHandle(static_cast<uint32_t>(pid), killcore::ProcessAccess::ReadOnly);
        if (!workerHandle.isValid()) {
            compareSuccess = false;
            compareError = "Impossible d'ouvrir le processus dans le worker unknown.";
        } else if (self) {
            // Compare "Unknown" : relit toute la mémoire capturée par le
            // snapshot, une fois par type testé (jusqu'à 6 en mode Auto) —
            // combine la position dans typesToRun et la progression par
            // région à l'intérieur de chaque type pour une seule barre
            // cohérente plutôt que de sauter par paliers de 1/totalTypes.
            const int totalTypes = std::max<int>(1, static_cast<int>(typesToRun.size()));
            int typeIndex = 0;
            int lastUnknownProgress = 0;
            for (const auto type : typesToRun) {
                killcore::ScanOptions compareOptions;
                compareOptions.progressCallback = [self, typeIndex, totalTypes, &lastUnknownProgress](const killcore::ScanProgress& progress) {
                    int withinTypePercent = 0;
                    if (progress.regionsTotal > 0) {
                        withinTypePercent = static_cast<int>((progress.regionsScanned * 100) / progress.regionsTotal);
                    }
                    int percent = 1 + ((typeIndex * 100 + withinTypePercent) * 94) / (totalTypes * 100);
                    percent = std::clamp(percent, 1, 95);
                    if (percent <= lastUnknownProgress || (percent - lastUnknownProgress < 2 && percent < 95)) {
                        return;
                    }
                    lastUnknownProgress = percent;
                    emitQueuedScanProgress(self, percent);
                };
                const auto scan = self->m_snapshot.compare(workerHandle, type, scanMode, cancellation.get(), compareOptions);
                ++typeIndex;
                checkedBytes += scan.checkedBytes;
                matchesFound += scan.matchesFound;
                partial = partial || scan.partial;
                cancelled = cancelled || scan.cancelled;
                compareSuccess = compareSuccess && scan.success;
                if (!scan.errorMessage.isEmpty() && compareError.isEmpty()) {
                    compareError = scan.errorMessage;
                }

                const auto typeCandidates = scan.success && !scan.cancelled
                    ? candidatesFromUnknownScan(workerHandle, scan)
                    : QList<killcore::Candidate>{};
                for (const auto& candidate : typeCandidates) {
                    const QString key = QString::number(candidate.address, 16) + "|" + killcore::valueTypeToString(candidate.type);
                    if (seenCandidates.contains(key)) {
                        continue;
                    }
                    seenCandidates.insert(key);
                    unknownCandidates.append(candidate);
                    if (unknownCandidates.size() >= kUnknownAutoMaxReturnedMatches) {
                        partial = true;
                        compareError = QString("Trop de candidats unknown Auto (%1+). Raffine avec changed/increased/decreased ou reduis la plage.")
                                           .arg(kUnknownAutoMaxReturnedMatches);
                        break;
                    }
                }

                typeSummaries.append(QVariantMap{
                    {"type", killcore::valueTypeToString(type)},
                    {"success", scan.success},
                    {"partial", scan.partial},
                    {"checkedBytes", static_cast<qulonglong>(scan.checkedBytes)},
                    {"matchesFound", static_cast<qulonglong>(scan.matchesFound)},
                    {"stored", typeCandidates.size()},
                    {"error", scan.errorMessage},
                });
                if (cancelled || unknownCandidates.size() >= kUnknownAutoMaxReturnedMatches) {
                    break;
                }
            }
        } else {
            return;
        }

        if (!self) {
            return;
        }

        QMetaObject::invokeMethod(self.data(), [self, requestId, mode, valueType, autoType, compareSuccess, partial, cancelled, checkedBytes, matchesFound, compareError, typeSummaries, unknownCandidates = std::move(unknownCandidates)]() mutable {
            if (!self) {
                return;
            }

            bool finishSuccess = compareSuccess;
            QString finishError = compareError;
            if (!cancelled) {
                QString undoError;
                if (!self->m_candidates.isEmpty() && !self->rememberCandidatesForUndo(&undoError)) {
                    finishSuccess = false;
                    finishError = undoError.isEmpty()
                        ? "Impossible de préparer l'annulation de la comparaison."
                        : undoError;
                } else {
                    self->m_candidates.replaceCandidates(unknownCandidates);
                }
            }

            QVariantMap finished;
            finished["requestId"] = requestId;
            finished["kind"] = "unknown_next";
            finished["success"] = finishSuccess;
            finished["partial"] = partial;
            finished["cancelled"] = cancelled;
            finished["checkedBytes"] = static_cast<qulonglong>(checkedBytes);
            finished["matchesFound"] = static_cast<qulonglong>(matchesFound);
            finished["stored"] = static_cast<qulonglong>(self->m_candidates.size());
            finished["valueType"] = autoType ? QString("Auto") : valueType;
            finished["typePasses"] = typeSummaries;
            finished["error"] = finishError;
            if (self->m_candidates.size() == 0 && finishSuccess) {
                finished["diagnostic"] = noCandidateDiagnosticMessage(finished, "unknown_compare");
            }

            self->appendSmartSearchDebug("unknown_next_async", {
                {"requestId", requestId},
                {"mode", mode},
                {"valueType", valueType},
                {"parsedType", autoType ? QString("Auto") : valueType},
                {"typePasses", typeSummaries},
                {"success", finished.value("success")},
                {"partial", finished.value("partial")},
                {"cancelled", finished.value("cancelled")},
                {"checkedBytes", finished.value("checkedBytes")},
                {"matchesFound", finished.value("matchesFound")},
                {"stored", finished.value("stored")},
                {"error", finished.value("error")},
            });
            self->appendScanTelemetry("unknown_next_async", {
                {"requestId", requestId},
                {"mode", mode},
                {"valueType", valueType},
                {"parsedType", autoType ? QString("Auto") : valueType},
                {"typePasses", typeSummaries},
                {"success", finished.value("success")},
                {"partial", finished.value("partial")},
                {"cancelled", finished.value("cancelled")},
                {"checkedBytes", finished.value("checkedBytes")},
                {"matchesFound", finished.value("matchesFound")},
                {"stored", finished.value("stored")},
                {"error", finished.value("error")},
            });

            self->m_scanInProgress = false;
            self->m_activeScanCancellation.reset();
            emit self->scanStatsUpdated(static_cast<int>(self->m_candidates.size()));
            emit self->scanProgress(100);
            emit self->scanFinished(finished);
        }, Qt::QueuedConnection);
    }).detach();

    result["success"] = true;
    result["started"] = true;
    result["requestId"] = requestId;
    result["error"] = "";
    return result;
}

QVariantMap ApplicationController::writeMemoryValue(const QString& addressHex, const QString& valueType, const QString& value) {
    QVariantMap result;
    result["success"] = false;

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

    killcore::ProcessHandle writeHandle(static_cast<uint32_t>(m_pid), killcore::ProcessAccess::ReadWrite);
    if (!writeHandle.isValid()) {
        result["error"] = "Impossible d'ouvrir le processus en écriture.";
        return result;
    }

    killcore::MemoryWriter writer(writeHandle);
    const auto write = writer.write(address, killcore::scanValueToBytes(scanValue), true);
    if (write.success) {
        m_lastWriteAddress = address;
        m_lastWritePreviousValue = write.previousValue;
        m_writeHistory.append({address, write.previousValue, killcore::scanValueToBytes(scanValue), type, value});
        // H2 (docs/STRATEGY_ROOM.md, session Solitaire du 19/08/2026) : le
        // mecanisme de detection "l'ecriture est repartie toute seule"
        // (registerWriteWatch/writeDidNotHold) existait deja mais ne
        // couvrait que writeMemoryValueConfirmed — pas ce chemin d'ecriture
        // "simple", pourtant celui utilise par le connecteur d'automatisation
        // et par defaut cote Expert. Meme choke point desormais des deux cotes.
        registerWriteWatch(address, type, killcore::scanValueToBytes(scanValue));
    }

    result["success"] = write.success;
    result["verified"] = write.verified;
    result["protectionChanged"] = write.protectionChanged;
    result["bytesWritten"] = static_cast<int>(write.bytesWritten);
    result["error"] = write.errorMessage;
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
    QVariantMap result;
    result["success"] = false;

    if (!m_attached || m_pid <= 0) {
        result["error"] = "Aucun processus attaché.";
        return result;
    }
    if (targets.isEmpty()) {
        result["error"] = "Aucune cible à écrire.";
        return result;
    }
    constexpr int kMaxAtomicTargets = 32;
    if (targets.size() > kMaxAtomicTargets) {
        result["error"] = QString("Trop de cibles pour une écriture groupée (%1 maximum).").arg(kMaxAtomicTargets);
        return result;
    }

    killcore::ProcessHandle writeHandle(static_cast<uint32_t>(m_pid), killcore::ProcessAccess::ReadWrite);
    if (!writeHandle.isValid()) {
        result["error"] = "Impossible d'ouvrir le processus en écriture.";
        return result;
    }

    // Parse et valide tout AVANT de suspendre les threads : la fenêtre
    // suspendue doit être la plus courte possible (juste les
    // WriteProcessMemory), la validation/le parsing n'a pas besoin d'un
    // process figé et peut échouer sans qu'on ait rien suspendu pour rien.
    struct ParsedTarget {
        uint64_t address{0};
        killcore::ValueType type{killcore::ValueType::Int32};
        QByteArray bytes;
        QString addressHex;
        QString typeName;
        QString valueText;
    };
    QList<ParsedTarget> parsed;
    QVariantList parseErrors;
    for (const auto& item : targets) {
        const QVariantMap target = item.toMap();
        const QString addressHex = target.value("address").toString();
        const QString typeName = target.value("type").toString();
        const QString valueText = target.value("value").toString();

        uint64_t address = 0;
        killcore::ValueType type = killcore::ValueType::Int32;
        killcore::ScanValue scanValue;
        QString parseError;
        if (!parseHexAddress(addressHex, &address)) {
            parseError = "Adresse invalide.";
        } else if (!killcore::parseValueType(typeName, &type)) {
            parseError = "Type invalide.";
        } else if (!killcore::parseScanValue(valueText, type, &scanValue, &parseError) && parseError.isEmpty()) {
            parseError = "Valeur invalide.";
        }
        if (!parseError.isEmpty()) {
            QVariantMap errEntry;
            errEntry["address"] = addressHex;
            errEntry["error"] = parseError;
            parseErrors.append(errEntry);
            continue;
        }

        ParsedTarget pt;
        pt.address = address;
        pt.type = type;
        pt.bytes = killcore::scanValueToBytes(scanValue);
        pt.addressHex = addressHex;
        pt.typeName = typeName;
        pt.valueText = valueText;
        parsed.append(pt);
    }

    if (parsed.isEmpty()) {
        result["error"] = "Aucune cible valide à écrire.";
        result["parseErrors"] = parseErrors;
        return result;
    }

    const bool suspendThreads = options.value("suspendThreads", true).toBool();
    int suspendedThreadCount = 0;
    QVariantList writeResults;
    int written = 0;

    {
        std::optional<killcore::ProcessThreadsSuspendGuard> suspendGuard;
        if (suspendThreads) {
            suspendGuard.emplace(static_cast<uint32_t>(m_pid));
            suspendedThreadCount = suspendGuard->suspendedCount();
        }

        killcore::MemoryWriter writer(writeHandle);
        for (const auto& pt : parsed) {
            const auto write = writer.write(pt.address, pt.bytes, true);
            QVariantMap writeResult;
            writeResult["address"] = pt.addressHex;
            writeResult["type"] = pt.typeName;
            writeResult["success"] = write.success;
            writeResult["verified"] = write.verified;
            writeResult["protectionChanged"] = write.protectionChanged;
            writeResult["bytesWritten"] = static_cast<int>(write.bytesWritten);
            writeResult["error"] = write.errorMessage;
            if (write.success) {
                ++written;
                m_writeHistory.append({pt.address, write.previousValue, pt.bytes, pt.type, pt.valueText});
                registerWriteWatch(pt.address, pt.type, pt.bytes); // H2, voir writeMemoryValue
            }
            writeResults.append(writeResult);
        }
        // suspendGuard sort de portée ici -> reprend toutes les threads suspendues
        // avant de construire le reste du resultat (pas de travail superflu
        // pendant que la cible est figee).
    }

    result["success"] = written > 0 && written == parsed.size();
    result["results"] = writeResults;
    result["written"] = written;
    result["total"] = targets.size();
    result["suspendedThreadCount"] = suspendedThreadCount;
    if (!parseErrors.isEmpty()) {
        result["parseErrors"] = parseErrors;
    }
    appendScanTelemetry("write_atomic_multi", result);
    return result;
}

QVariantMap ApplicationController::writeMemoryValuesWithVariants(const QVariantList& targets, const QString& value) {
    QVariantMap result;
    QVariantList writeResults;
    result["success"] = false;
    result["results"] = writeResults;
    result["written"] = 0;
    result["total"] = targets.size();
    QElapsedTimer timer;
    timer.start();

    const QString rawValue = value.trimmed();
    if (targets.isEmpty()) {
        result["error"] = "Aucune cible à écrire.";
        appendScanTelemetry("ui_string_sources_write", {
            {"success", false},
            {"error", result.value("error")},
            {"displayValue", value},
            {"targetCount", targets.size()},
        });
        return result;
    }
    if (rawValue.isEmpty()) {
        result["error"] = "Valeur vide.";
        appendScanTelemetry("ui_string_sources_write", {
            {"success", false},
            {"error", result.value("error")},
            {"displayValue", value},
            {"targetCount", targets.size()},
        });
        return result;
    }

    killcore::ProcessHandle writeHandle(static_cast<uint32_t>(m_pid), killcore::ProcessAccess::ReadWrite);
    if (!writeHandle.isValid()) {
        result["error"] = "Impossible d'ouvrir le processus en écriture.";
        appendScanTelemetry("ui_string_sources_write", {
            {"success", false},
            {"error", result.value("error")},
            {"displayValue", rawValue},
            {"targetCount", targets.size()},
        });
        return result;
    }

    killcore::MemoryWriter writer(writeHandle);
    bool allWritesOk = true;
    int written = 0;
    m_lastBatchStartIndex = m_writeHistory.size();

    for (const auto& item : targets) {
        const QVariantMap target = item.toMap();
        QVariantMap writeResult;
        writeResult["success"] = false;
        writeResult["verified"] = false;
        writeResult["bytesWritten"] = 0;

        const QString addressHex = target.value("address").toString();
        const QString typeName = target.value("type").toString();
        const QString variantLabel = target.value("variantLabel").toString();
        writeResult["address"] = addressHex;
        writeResult["type"] = typeName;
        writeResult["variantLabel"] = variantLabel;
        writeResult["displayValue"] = rawValue;

        uint64_t address = 0;
        if (!parseHexAddress(addressHex, &address)) {
            writeResult["error"] = "Adresse invalide.";
            allWritesOk = false;
            writeResults.append(writeResult);
            continue;
        }

        killcore::ValueType type;
        if (!killcore::parseValueType(typeName, &type)) {
            writeResult["error"] = "Type invalide.";
            allWritesOk = false;
            writeResults.append(writeResult);
            continue;
        }

        QString parseError;
        const QByteArray targetBytes = killcore::targetBytesForTypeAndVariant(rawValue, type, variantLabel, &parseError);
        if (targetBytes.isEmpty()) {
            writeResult["error"] = parseError.isEmpty() ? QString("Valeur incompatible avec cette variante.") : parseError;
            allWritesOk = false;
            writeResults.append(writeResult);
            continue;
        }

        const auto write = writer.write(address, targetBytes, true);
        writeResult["success"] = write.success;
        writeResult["verified"] = write.verified;
        writeResult["protectionChanged"] = write.protectionChanged;
        writeResult["bytesWritten"] = static_cast<int>(write.bytesWritten);
        writeResult["error"] = write.errorMessage;
        writeResult["encodedHex"] = QString::fromLatin1(targetBytes.toHex(' ').toUpper());

        if (write.success) {
            ++written;
            m_lastWriteAddress = address;
            m_lastWritePreviousValue = write.previousValue;
            m_writeHistory.append({address, write.previousValue, targetBytes, type, rawValue});
        } else {
            allWritesOk = false;
        }
        writeResults.append(writeResult);
    }

    m_lastBatchEndIndex = m_writeHistory.size();
    const int protectionChangedCount = static_cast<int>(std::count_if(writeResults.begin(), writeResults.end(), [](const QVariant& item) {
        return item.toMap().value("protectionChanged").toBool();
    }));
    result["success"] = allWritesOk && written > 0;
    result["verified"] = result.value("success").toBool();
    result["protectionChanged"] = protectionChangedCount > 0;
    result["protectionChangedCount"] = protectionChangedCount;
    result["bytesWritten"] = 0;
    result["written"] = written;
    result["total"] = targets.size();
    result["results"] = writeResults;
    result["error"] = allWritesOk
        ? QString()
        : QString("Écriture partielle: %1/%2 réussie(s).").arg(written).arg(targets.size());
    QVariantList samples;
    for (int i = 0; i < std::min<int>(writeResults.size(), 16); ++i) {
        const QVariantMap write = writeResults.at(i).toMap();
        samples.append(QVariantMap{
            {"address", write.value("address")},
            {"type", write.value("type")},
            {"variantLabel", write.value("variantLabel")},
            {"success", write.value("success")},
            {"verified", write.value("verified")},
            {"protectionChanged", write.value("protectionChanged")},
            {"bytesWritten", write.value("bytesWritten")},
            {"encodedHex", write.value("encodedHex")},
            {"error", write.value("error")},
        });
    }
    appendScanTelemetry("ui_string_sources_write", {
        {"success", result.value("success")},
        {"verified", result.value("verified")},
        {"protectionChangedCount", protectionChangedCount},
        {"displayValue", rawValue},
        {"targetCount", targets.size()},
        {"written", written},
        {"failed", targets.size() - written},
        {"error", result.value("error")},
        {"sampleCount", samples.size()},
        {"samples", samples},
        {"elapsedMs", static_cast<int>(timer.elapsed())},
    });
    return result;
}

QVariantMap ApplicationController::writeMemoryValueConfirmed(
    const QString& addressHex,
    const QString& valueType,
    const QString& value,
    bool persistHistory) {
    QVariantMap result;
    result["success"] = false;
    result["verified"] = false;
    result["confirmationMode"] = true;
    result["temporaryVerified"] = false;
    result["restoredBeforeFinal"] = false;
    result["finalVerified"] = false;

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

    killcore::ProcessHandle writeHandle(static_cast<uint32_t>(m_pid), killcore::ProcessAccess::ReadWrite);
    if (!writeHandle.isValid()) {
        result["error"] = "Impossible d'ouvrir le processus en écriture.";
        return result;
    }

    killcore::MemoryWriter writer(writeHandle);
    const QByteArray targetBytes = killcore::scanValueToBytes(scanValue);
    const auto temporaryWrite = writer.write(address, targetBytes, true);
    result["temporaryVerified"] = temporaryWrite.success && temporaryWrite.verified;
    result["temporaryProtectionChanged"] = temporaryWrite.protectionChanged;
    result["bytesWritten"] = static_cast<int>(temporaryWrite.bytesWritten);

    if (!temporaryWrite.success || !temporaryWrite.verified) {
        result["error"] = temporaryWrite.errorMessage.isEmpty()
            ? "La confirmation temporaire de l'adresse a échoué."
            : temporaryWrite.errorMessage;
        return result;
    }

    const QByteArray previousValue = temporaryWrite.previousValue;
    if (previousValue.size() == targetBytes.size()) {
        const auto restore = writer.write(address, previousValue, true);
        result["restoredBeforeFinal"] = restore.success && restore.verified;
        result["restoreProtectionChanged"] = restore.protectionChanged;
        if (!restore.success || !restore.verified) {
            result["error"] = restore.errorMessage.isEmpty()
                ? "La restauration après confirmation temporaire a échoué."
                : restore.errorMessage;
            return result;
        }
    } else {
        result["error"] = "Impossible de restaurer l'ancienne valeur après confirmation.";
        return result;
    }

    const auto finalWrite = writer.write(address, targetBytes, true);
    result["finalVerified"] = finalWrite.success && finalWrite.verified;
    result["success"] = finalWrite.success && finalWrite.verified;
    result["verified"] = finalWrite.verified;
    result["protectionChanged"] = finalWrite.protectionChanged;
    result["bytesWritten"] = static_cast<int>(finalWrite.bytesWritten);
    result["error"] = finalWrite.errorMessage;

    if (result.value("success").toBool()) {
        m_lastWriteAddress = address;
        m_lastWritePreviousValue = previousValue;
        m_writeHistory.append({address, previousValue, targetBytes, type, value});
        // Seul point d'appel de toutes les ecritures confirmees (manuelles ET
        // tous les auto-write du chat Assistant, qui appellent tous cette
        // meme fonction par cible) : surveiller ici couvre tout, sans avoir a
        // instrumenter chaque appelant separement.
        registerWriteWatch(address, type, targetBytes);
        // Meme choke point pour la persistance replay inter-session (roadmap I,
        // "Historique d'ecritures avec replay") : m_writeHistory ci-dessus ne
        // survit pas a la fermeture de KillEngine, cette entree si.
        // persistHistory=false pendant un replay (voir replayWriteHistorySequence)
        // pour ne pas re-logger a l'infini la sequence qu'on est en train de rejouer.
        if (persistHistory) {
            persistWriteHistorySequenceEntry(address, type, value);
        }
    }

    return result;
}

QVariantMap ApplicationController::rollbackLastWriteBatch() {
    QVariantMap result;
    result["success"] = false;

    if (m_lastBatchStartIndex < 0
        || m_lastBatchEndIndex <= m_lastBatchStartIndex
        || m_lastBatchStartIndex >= m_writeHistory.size()) {
        result["error"] = "Aucun batch d'écritures automatiques à restaurer.";
        return result;
    }

    killcore::ProcessHandle writeHandle(static_cast<uint32_t>(m_pid), killcore::ProcessAccess::ReadWrite);
    if (!writeHandle.isValid()) {
        result["error"] = "Impossible d'ouvrir le processus en écriture.";
        return result;
    }

    killcore::MemoryWriter writer(writeHandle);
    int rolled = 0;
    QVariantList restoredWrites;
    const int batchEnd = std::min(m_lastBatchEndIndex, static_cast<int>(m_writeHistory.size()));
    for (int i = batchEnd - 1; i >= m_lastBatchStartIndex; --i) {
        const auto& rec = m_writeHistory.at(i);
        const auto write = writer.write(rec.address, rec.previousValue, true);
        if (write.success) {
            ++rolled;
        }
        QVariantMap restored;
        restored["address"] = QString::number(rec.address, 16);
        restored["type"] = killcore::valueTypeToString(rec.type);
        restored["from"] = rec.valueText;
        restored["to"] = bytesToDouble(rec.previousValue, rec.type);
        restored["success"] = write.success;
        restored["verified"] = write.verified;
        restored["protectionChanged"] = write.protectionChanged;
        restoredWrites.append(restored);
    }

    const int total = batchEnd - m_lastBatchStartIndex;
    for (int i = batchEnd - 1; i >= m_lastBatchStartIndex; --i) {
        m_writeHistory.removeAt(i);
    }
    m_lastBatchStartIndex = -1;
    m_lastBatchEndIndex = -1;
    m_lastAutoWriteTargets.clear();
    if (m_writeHistory.isEmpty()) {
        m_lastWriteAddress = 0;
        m_lastWritePreviousValue.clear();
    }

    result["success"] = (rolled == total);
    result["rolledBack"] = rolled;
    result["total"] = total;
    result["restoredWrites"] = restoredWrites;
    result["error"] = (rolled == total) ? QString() : QString("Seulement %1/%2 restaurées.").arg(rolled).arg(total);
    return result;
}

QVariantMap ApplicationController::rollbackLastWrite() {
    QVariantMap result;
    result["success"] = false;

    if (m_lastWriteAddress == 0 || m_lastWritePreviousValue.isEmpty()) {
        result["error"] = "Aucune écriture à restaurer.";
        return result;
    }

    killcore::ProcessHandle writeHandle(static_cast<uint32_t>(m_pid), killcore::ProcessAccess::ReadWrite);
    if (!writeHandle.isValid()) {
        result["error"] = "Impossible d'ouvrir le processus en écriture.";
        return result;
    }

    killcore::MemoryWriter writer(writeHandle);
    const auto write = writer.write(m_lastWriteAddress, m_lastWritePreviousValue, true);

    result["success"] = write.success;
    result["verified"] = write.verified;
    result["protectionChanged"] = write.protectionChanged;
    result["bytesWritten"] = static_cast<int>(write.bytesWritten);
    result["error"] = write.errorMessage;
    if (write.success) {
        m_lastWriteAddress = 0;
        m_lastWritePreviousValue.clear();
    }
    return result;
}

QVariantMap ApplicationController::findWhatWrites(const QString& addressHex, const QVariantMap& options) {
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

    const int timeoutMs = std::clamp(options.value("timeoutMs", 5000).toInt(), 250, 15000);
    const int maxHitsInt = std::clamp(options.value("maxHits", 10).toInt(), 1, 100);

    KE_LOG_INFO() << "findWhatWrites(address=0x" << std::hex << address
                  << ", pid=" << std::dec << m_pid
                  << ", size=" << sizeBytes
                  << ", timeoutMs=" << timeoutMs
                  << ", maxHits=" << maxHitsInt << ")";

    const auto hits = killcore::findWhatWrites(
        static_cast<uint32_t>(m_pid),
        address,
        breakpointSize,
        timeoutMs,
        static_cast<size_t>(maxHitsInt));

    QVariantList hitList;
    for (const auto& hit : hits) {
        QVariantMap item;
        item["address"] = QString::number(hit.address, 16).toUpper();
        item["instructionPointer"] = QString::number(hit.instructionPointer, 16).toUpper();
        item["threadId"] = static_cast<qulonglong>(hit.threadId);
        item["valueBefore"] = static_cast<qulonglong>(hit.valueBefore);
        item["valueAfter"] = static_cast<qulonglong>(hit.valueAfter);
        item["module"] = hit.module;
        item["moduleOffset"] = QString::number(hit.moduleOffset, 16).toUpper();
        hitList.append(item);
    }

    result["success"] = true;
    result["hits"] = hitList;
    result["hitCount"] = hitList.size();
    result["size"] = sizeBytes;
    result["timeoutMs"] = timeoutMs;
    result["maxHits"] = maxHitsInt;
    result["warning"] = "Cette fonction attache KillEngine comme debugger au processus cible pendant la capture.";
    result["error"] = hits.isEmpty()
        ? "Aucune écriture capturée pendant la fenêtre d'observation."
        : QString();
    return result;
}

QVariantMap ApplicationController::findWhatAccesses(const QString& addressHex, const QVariantMap& options) {
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

    const int timeoutMs = std::clamp(options.value("timeoutMs", 5000).toInt(), 250, 15000);
    const int maxHitsInt = std::clamp(options.value("maxHits", 10).toInt(), 1, 100);

    KE_LOG_INFO() << "findWhatAccesses(address=0x" << std::hex << address
                  << ", pid=" << std::dec << m_pid
                  << ", size=" << sizeBytes
                  << ", timeoutMs=" << timeoutMs
                  << ", maxHits=" << maxHitsInt << ")";

    const auto hits = killcore::findWhatAccesses(
        static_cast<uint32_t>(m_pid),
        address,
        breakpointSize,
        timeoutMs,
        static_cast<size_t>(maxHitsInt));

    QVariantList hitList;
    for (const auto& hit : hits) {
        QVariantMap item;
        item["address"] = QString::number(hit.address, 16).toUpper();
        item["instructionPointer"] = QString::number(hit.instructionPointer, 16).toUpper();
        item["threadId"] = static_cast<qulonglong>(hit.threadId);
        item["valueBefore"] = static_cast<qulonglong>(hit.valueBefore);
        item["valueAfter"] = static_cast<qulonglong>(hit.valueAfter);
        item["module"] = hit.module;
        item["moduleOffset"] = QString::number(hit.moduleOffset, 16).toUpper();
        hitList.append(item);
    }

    result["success"] = true;
    result["hits"] = hitList;
    result["hitCount"] = hitList.size();
    result["size"] = sizeBytes;
    result["timeoutMs"] = timeoutMs;
    result["maxHits"] = maxHitsInt;
    result["warning"] = "Cette fonction attache KillEngine comme debugger au processus cible pendant la capture. "
                         "Capture lecture ET écriture (contrairement à findWhatWrites) : peut révéler une "
                         "instruction de vérification/comparaison distincte de celle qui écrit.";
    result["error"] = hits.isEmpty()
        ? "Aucun accès capturé pendant la fenêtre d'observation."
        : QString();
    return result;
}

QVariantMap ApplicationController::findWhatWritesAsync(const QString& addressHex, const QVariantMap& options) {
    QVariantMap result;
    result["success"] = false;
    result["started"] = false;
    result["address"] = addressHex;

    if (m_findWhatWritesInProgress) {
        result["error"] = "Une capture Find What Writes est déjà en cours.";
        return result;
    }
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

    const int timeoutMs = std::clamp(options.value("timeoutMs", 5000).toInt(), 250, 15000);
    const int maxHitsInt = std::clamp(options.value("maxHits", 10).toInt(), 1, 100);
    const int requestId = m_nextDebugRequestId++;
    const int pid = m_pid;
    const QString requestedAddress = addressHex;
    const QPointer<ApplicationController> self(this);
    auto cancellation = std::make_shared<killcore::CancellationToken>();

    m_findWhatWritesInProgress = true;
    m_activeDebugCancellation = cancellation;

    KE_LOG_INFO() << "findWhatWritesAsync(address=0x" << std::hex << address
                  << ", pid=" << std::dec << pid
                  << ", size=" << sizeBytes
                  << ", timeoutMs=" << timeoutMs
                  << ", maxHits=" << maxHitsInt
                  << ", requestId=" << requestId << ")";

    std::thread([self, requestId, pid, address, requestedAddress, breakpointSize, sizeBytes, timeoutMs, maxHitsInt, cancellation]() {
        const auto hits = killcore::findWhatWrites(
            static_cast<uint32_t>(pid),
            address,
            breakpointSize,
            timeoutMs,
            static_cast<size_t>(maxHitsInt),
            cancellation.get());
        const bool cancelled = cancellation->isCancelled();

        if (!self) {
            return;
        }

        QMetaObject::invokeMethod(self.data(), [self, requestId, requestedAddress, sizeBytes, timeoutMs, maxHitsInt, hits, cancelled]() {
            if (!self) {
                return;
            }

            QVariantList hitList;
            for (const auto& hit : hits) {
                QVariantMap item;
                item["address"] = QString::number(hit.address, 16).toUpper();
                item["instructionPointer"] = QString::number(hit.instructionPointer, 16).toUpper();
                item["threadId"] = static_cast<qulonglong>(hit.threadId);
                item["valueBefore"] = static_cast<qulonglong>(hit.valueBefore);
                item["valueAfter"] = static_cast<qulonglong>(hit.valueAfter);
                item["module"] = hit.module;
                item["moduleOffset"] = QString::number(hit.moduleOffset, 16).toUpper();
                hitList.append(item);
            }

            QVariantMap finished;
            finished["requestId"] = requestId;
            finished["kind"] = "find_what_writes";
            finished["success"] = true;
            finished["address"] = requestedAddress;
            finished["hits"] = hitList;
            finished["hitCount"] = hitList.size();
            finished["size"] = sizeBytes;
            finished["timeoutMs"] = timeoutMs;
            finished["maxHits"] = maxHitsInt;
            finished["cancelled"] = cancelled;
            finished["warning"] = "Cette fonction attache KillEngine comme debugger au processus cible pendant la capture.";
            finished["error"] = cancelled
                ? "Capture Find What Writes annulée."
                : hits.isEmpty()
                ? "Aucune écriture capturée pendant la fenêtre d'observation."
                : QString();

            self->m_findWhatWritesInProgress = false;
            self->m_activeDebugCancellation.reset();
            emit self->findWhatWritesFinished(finished);
        }, Qt::QueuedConnection);
    }).detach();

    result["success"] = true;
    result["started"] = true;
    result["requestId"] = requestId;
    result["size"] = sizeBytes;
    result["timeoutMs"] = timeoutMs;
    result["maxHits"] = maxHitsInt;
    result["error"] = "";
    return result;
}

QVariantMap ApplicationController::cancelFindWhatWrites() {
    QVariantMap result;
    result["success"] = false;
    if (!m_findWhatWritesInProgress || !m_activeDebugCancellation) {
        result["error"] = "Aucune capture Find What Writes active à annuler.";
        return result;
    }

    m_activeDebugCancellation->cancel();
    result["success"] = true;
    result["error"] = "";
    return result;
}

namespace {
// KillEnginePageGuardHandler.dll est produite dans le meme dossier que
// KillEngine.exe (CMAKE_RUNTIME_OUTPUT_DIRECTORY partage, core/CMakeLists.txt).
// Deux candidats couvrent le lancement depuis le build brut (bin/) et depuis
// un futur layout package — meme esprit que openUserGuide() plus bas.
QString resolvePageGuardHandlerPath() {
    const QDir appDir(QCoreApplication::applicationDirPath());
    const QStringList candidates = {
        appDir.filePath("KillEnginePageGuardHandler.dll"),
        appDir.filePath("../lib/KillEnginePageGuardHandler.dll"),
    };
    for (const auto& candidate : candidates) {
        if (QFileInfo::exists(candidate)) {
            return QFileInfo(candidate).absoluteFilePath();
        }
    }
    return QString();
}

// Meme demarche que resolvePageGuardHandlerPath() pour
// KillEngineInProcessBreakpointHandler.dll (core/CMakeLists.txt).
QString resolveInProcessBreakpointHandlerPath() {
    const QDir appDir(QCoreApplication::applicationDirPath());
    const QStringList candidates = {
        appDir.filePath("KillEngineInProcessBreakpointHandler.dll"),
        appDir.filePath("../lib/KillEngineInProcessBreakpointHandler.dll"),
    };
    for (const auto& candidate : candidates) {
        if (QFileInfo::exists(candidate)) {
            return QFileInfo(candidate).absoluteFilePath();
        }
    }
    return QString();
}

// Meme demarche que resolvePageGuardHandlerPath() pour
// KillEngineSpeedhackHandler.dll (core/CMakeLists.txt).
QString resolveSpeedhackHandlerPath() {
    const QDir appDir(QCoreApplication::applicationDirPath());
    const QStringList candidates = {
        appDir.filePath("KillEngineSpeedhackHandler.dll"),
        appDir.filePath("../lib/KillEngineSpeedhackHandler.dll"),
    };
    for (const auto& candidate : candidates) {
        if (QFileInfo::exists(candidate)) {
            return QFileInfo(candidate).absoluteFilePath();
        }
    }
    return QString();
}
} // namespace

QVariantMap ApplicationController::startPageGuardWatchAsync(const QString& addressHex, const QVariantMap& options) {
    QVariantMap result;
    result["success"] = false;
    result["started"] = false;
    result["address"] = addressHex;

    if (m_pageGuardWatchInProgress) {
        result["error"] = "Une capture Page Guard est déjà en cours.";
        return result;
    }
    if (!m_attached || m_pid <= 0 || !m_handle.isValid()) {
        result["error"] = "Aucun processus attaché.";
        return result;
    }

    uint64_t address = 0;
    if (!parseHexAddress(addressHex, &address)) {
        result["error"] = "Adresse invalide.";
        return result;
    }

    const QString handlerPath = resolvePageGuardHandlerPath();
    if (handlerPath.isEmpty()) {
        result["error"] = "KillEnginePageGuardHandler.dll introuvable à côté de KillEngine.exe.";
        return result;
    }

    killcore::PageGuardConfig config;
    config.address = address;
    config.size = static_cast<size_t>(std::clamp(options.value("size", 4).toInt(), 1, 4096));
    config.captureWrites = options.value("captureWrites", true).toBool();
    config.captureReads = options.value("captureReads", false).toBool();
    config.timeoutMs = std::clamp(options.value("timeoutMs", 5000).toInt(), 250, 15000);
    config.maxHits = static_cast<size_t>(std::clamp(options.value("maxHits", 10).toInt(), 1, 100));
    config.injectedHandlerPath = handlerPath;

    const int requestId = m_nextDebugRequestId++;
    const QString requestedAddress = addressHex;
    const QPointer<ApplicationController> self(this);
    auto session = std::make_shared<killcore::PageGuardSession>();
    const uint32_t pid = m_pid;

    m_pageGuardWatchInProgress = true;
    m_activePageGuardSession = session;

    KE_LOG_INFO() << "startPageGuardWatchAsync(address=0x" << std::hex << address << std::dec
                  << ", size=" << config.size
                  << ", timeoutMs=" << config.timeoutMs
                  << ", maxHits=" << config.maxHits
                  << ", requestId=" << requestId << ")";

    std::thread([self, requestId, requestedAddress, config, session, pid]() {
        // ProcessHandle n'est pas copiable (RAII autour d'un HANDLE) — on en
        // rouvre un propre à ce thread plutôt que de partager celui de
        // ApplicationController::m_handle entre threads. AllAccess est requis
        // ici (contrairement au ReadWrite habituel) : l'injection de la DLL
        // handler passe par VirtualAllocEx/WriteProcessMemory/CreateRemoteThread.
        killcore::ProcessHandle ownedHandle(pid, killcore::ProcessAccess::AllAccess);
        if (!ownedHandle.isValid()) {
            if (!self) return;
            QMetaObject::invokeMethod(self.data(), [self, requestId, requestedAddress, config]() {
                if (!self) return;
                QVariantMap finished;
                finished["requestId"] = requestId;
                finished["kind"] = "page_guard_watch";
                finished["success"] = false;
                finished["address"] = requestedAddress;
                finished["hits"] = QVariantList();
                finished["hitCount"] = 0;
                finished["error"] = "Impossible d'ouvrir le processus avec les droits nécessaires à l'injection (PROCESS_ALL_ACCESS).";
                self->m_pageGuardWatchInProgress = false;
                self->m_activePageGuardSession.reset();
                emit self->pageGuardWatchFinished(finished);
            }, Qt::QueuedConnection);
            return;
        }

        const auto pageResult = session->monitor(ownedHandle, config);

        if (!self) {
            return;
        }

        QMetaObject::invokeMethod(self.data(), [self, requestId, requestedAddress, config, pageResult]() {
            if (!self) {
                return;
            }

            QVariantList hitList;
            for (const auto& hit : pageResult.hits) {
                QVariantMap item;
                item["address"] = QString::number(hit.accessAddress, 16).toUpper();
                item["instructionPointer"] = QString::number(hit.instructionPointer, 16).toUpper();
                item["threadId"] = static_cast<qulonglong>(hit.threadId);
                item["isWrite"] = hit.isWrite;
                item["module"] = hit.module;
                item["moduleOffset"] = QString::number(hit.moduleOffset, 16).toUpper();
                hitList.append(item);
            }

            QVariantMap finished;
            finished["requestId"] = requestId;
            finished["kind"] = "page_guard_watch";
            finished["success"] = pageResult.success;
            finished["address"] = requestedAddress;
            finished["hits"] = hitList;
            finished["hitCount"] = hitList.size();
            finished["size"] = static_cast<int>(config.size);
            finished["timeoutMs"] = config.timeoutMs;
            finished["maxHits"] = static_cast<int>(config.maxHits);
            finished["timedOut"] = pageResult.timedOut;
            finished["warning"] = "Capture par PAGE_GUARD (sans canal de debug Win32) : moins précise qu'un hardware breakpoint (granularité page de 4 Ko, hits rapprochés potentiellement fusionnés).";
            finished["error"] = !pageResult.success
                ? pageResult.error
                : pageResult.hits.isEmpty()
                ? "Aucun accès capturé pendant la fenêtre d'observation."
                : QString();

            self->m_pageGuardWatchInProgress = false;
            self->m_activePageGuardSession.reset();
            emit self->pageGuardWatchFinished(finished);
        }, Qt::QueuedConnection);
    }).detach();

    result["success"] = true;
    result["started"] = true;
    result["requestId"] = requestId;
    result["size"] = static_cast<int>(config.size);
    result["timeoutMs"] = config.timeoutMs;
    result["maxHits"] = static_cast<int>(config.maxHits);
    result["error"] = "";
    return result;
}

QVariantMap ApplicationController::cancelPageGuardWatch() {
    QVariantMap result;
    result["success"] = false;
    if (!m_pageGuardWatchInProgress || !m_activePageGuardSession) {
        result["error"] = "Aucune capture Page Guard active à annuler.";
        return result;
    }

    m_activePageGuardSession->stop();
    result["success"] = true;
    result["error"] = "";
    return result;
}

// Roadmap section F, niveau 2 (docs/POWER_UP_ROADMAP.md, docs/STRATEGY_ROOM.md) :
// breakpoint materiel pose depuis un composant charge DANS la cible, sans
// jamais attacher de debugger externe — meme structure que
// startPageGuardWatchAsync ci-dessus, avec InProcessBreakpointSession.
QVariantMap ApplicationController::startInProcessBreakpointWatchAsync(const QString& addressHex, const QVariantMap& options) {
    QVariantMap result;
    result["success"] = false;
    result["started"] = false;
    result["address"] = addressHex;

    if (m_inProcessBreakpointWatchInProgress) {
        result["error"] = "Une capture breakpoint in-process est déjà en cours.";
        return result;
    }
    if (!m_attached || m_pid <= 0 || !m_handle.isValid()) {
        result["error"] = "Aucun processus attaché.";
        return result;
    }

    uint64_t address = 0;
    if (!parseHexAddress(addressHex, &address)) {
        result["error"] = "Adresse invalide.";
        return result;
    }

    const QString handlerPath = resolveInProcessBreakpointHandlerPath();
    if (handlerPath.isEmpty()) {
        result["error"] = "KillEngineInProcessBreakpointHandler.dll introuvable à côté de KillEngine.exe.";
        return result;
    }

    killcore::InProcessBreakpointConfig config;
    config.address = address;
    const int requestedSize = options.value("size", 4).toInt();
    config.size = (requestedSize == 1 || requestedSize == 2 || requestedSize == 8) ? static_cast<size_t>(requestedSize) : 4;
    config.captureWrites = options.value("captureWrites", true).toBool();
    config.timeoutMs = std::clamp(options.value("timeoutMs", 5000).toInt(), 250, 15000);
    config.maxHits = static_cast<size_t>(std::clamp(options.value("maxHits", 10).toInt(), 1, 100));
    config.injectedHandlerPath = handlerPath;

    const int requestId = m_nextDebugRequestId++;
    const QString requestedAddress = addressHex;
    const QPointer<ApplicationController> self(this);
    auto session = std::make_shared<killcore::InProcessBreakpointSession>();
    const uint32_t pid = m_pid;

    m_inProcessBreakpointWatchInProgress = true;
    m_activeInProcessBreakpointSession = session;

    KE_LOG_INFO() << "startInProcessBreakpointWatchAsync(address=0x" << std::hex << address << std::dec
                  << ", size=" << config.size
                  << ", timeoutMs=" << config.timeoutMs
                  << ", maxHits=" << config.maxHits
                  << ", requestId=" << requestId
                  << ", self=" << static_cast<void*>(this)
                  << ", session=" << session.get() << ")";

    std::thread([self, requestId, requestedAddress, config, session, pid]() {
        killcore::ProcessHandle ownedHandle(pid, killcore::ProcessAccess::AllAccess);
        if (!ownedHandle.isValid()) {
            if (!self) return;
            QMetaObject::invokeMethod(self.data(), [self, requestId, requestedAddress, config]() {
                if (!self) return;
                QVariantMap finished;
                finished["requestId"] = requestId;
                finished["kind"] = "inprocess_breakpoint_watch";
                finished["success"] = false;
                finished["address"] = requestedAddress;
                finished["hits"] = QVariantList();
                finished["hitCount"] = 0;
                finished["error"] = "Impossible d'ouvrir le processus avec les droits nécessaires à l'injection (PROCESS_ALL_ACCESS).";
                self->m_inProcessBreakpointWatchInProgress = false;
                self->m_activeInProcessBreakpointSession.reset();
                emit self->inProcessBreakpointWatchFinished(finished);
            }, Qt::QueuedConnection);
            return;
        }

        const auto captureResult = session->monitor(ownedHandle, config);

        if (!self) {
            return;
        }

        QMetaObject::invokeMethod(self.data(), [self, requestId, requestedAddress, config, captureResult]() {
            if (!self) {
                return;
            }

            QVariantList hitList;
            for (const auto& hit : captureResult.hits) {
                QVariantMap item;
                item["instructionPointer"] = QString::number(hit.instructionPointer, 16).toUpper();
                item["threadId"] = static_cast<qulonglong>(hit.threadId);
                item["module"] = hit.module;
                item["moduleOffset"] = QString::number(hit.moduleOffset, 16).toUpper();
                hitList.append(item);
            }

            QVariantMap finished;
            finished["requestId"] = requestId;
            finished["kind"] = "inprocess_breakpoint_watch";
            finished["success"] = captureResult.success;
            finished["address"] = requestedAddress;
            finished["hits"] = hitList;
            finished["hitCount"] = hitList.size();
            finished["size"] = static_cast<int>(config.size);
            finished["timeoutMs"] = config.timeoutMs;
            finished["maxHits"] = static_cast<int>(config.maxHits);
            finished["timedOut"] = captureResult.timedOut;
            finished["warning"] = "Seules les threads créées après l'injection sont couvertes — "
                                   "une écriture qui vient d'une thread déjà active au moment de "
                                   "l'installation peut ne pas être capturée. Si rien n'apparaît, "
                                   "réessaie ou utilise Find What Writes (débogueur externe).";
            finished["error"] = !captureResult.success
                ? captureResult.error
                : captureResult.hits.isEmpty()
                ? "Aucune écriture capturée pendant la fenêtre d'observation."
                : QString();

            self->m_inProcessBreakpointWatchInProgress = false;
            self->m_activeInProcessBreakpointSession.reset();
            emit self->inProcessBreakpointWatchFinished(finished);
        }, Qt::QueuedConnection);
    }).detach();

    result["success"] = true;
    result["started"] = true;
    result["requestId"] = requestId;
    result["size"] = static_cast<int>(config.size);
    result["timeoutMs"] = config.timeoutMs;
    result["maxHits"] = static_cast<int>(config.maxHits);
    result["error"] = "";
    return result;
}

QVariantMap ApplicationController::cancelInProcessBreakpointWatch() {
    QVariantMap result;
    result["success"] = false;
    if (!m_inProcessBreakpointWatchInProgress || !m_activeInProcessBreakpointSession) {
        result["error"] = "Aucune capture breakpoint in-process active à annuler.";
        return result;
    }

    m_activeInProcessBreakpointSession->stop();
    result["success"] = true;
    result["error"] = "";
    return result;
}

QVariantMap ApplicationController::startInProcessBreakpointFreeze(const QString& addressHex, const QString& valueType, const QString& value, const QVariantMap& options) {
    QVariantMap result;
    result["success"] = false;
    result["enabled"] = false;
    result["mode"] = "inprocess";

    if (!m_attached || m_pid <= 0) {
        result["error"] = "Aucun processus attaché.";
        return result;
    }
    if (m_inProcessBreakpointFreezeSession && m_inProcessBreakpointFreezeSession->isFreezing()) {
        result["error"] = "Un freeze breakpoint in-process est déjà actif — arrête-le avant d'en démarrer un autre.";
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

    const QByteArray frozenBytes = killcore::scanValueToBytes(scanValue);
    if (frozenBytes.size() > 8) {
        result["error"] = "Type trop large pour un freeze breakpoint in-process (8 octets maximum).";
        return result;
    }

    const QString handlerPath = resolveInProcessBreakpointHandlerPath();
    if (handlerPath.isEmpty()) {
        result["error"] = "KillEngineInProcessBreakpointHandler.dll introuvable à côté de KillEngine.exe.";
        return result;
    }

    killcore::ProcessHandle ownedHandle(static_cast<uint32_t>(m_pid), killcore::ProcessAccess::AllAccess);
    if (!ownedHandle.isValid()) {
        result["error"] = "Impossible d'ouvrir le processus avec les droits nécessaires à l'injection (PROCESS_ALL_ACCESS).";
        return result;
    }

    const bool captureWrites = options.value("captureWrites", true).toBool();
    auto session = std::make_shared<killcore::InProcessBreakpointSession>();
    QString startError;
    if (!session->startFreeze(ownedHandle, address, frozenBytes.size(), captureWrites, frozenBytes, handlerPath, &startError)) {
        result["error"] = startError;
        return result;
    }

    m_inProcessBreakpointFreezeSession = session;
    result["success"] = true;
    result["enabled"] = true;
    result["armedThreadCount"] = session->freezeStats().armedThreadCount;
    result["warning"] = "Seules les threads créées après l'injection sont couvertes pour l'instant — "
                         "si l'écriture vient d'une thread déjà active au moment de l'installation, "
                         "le freeze peut ne pas tenir. Si ça ne tient pas, réessaie (une nouvelle "
                         "injection réarme la thread appelante) ou utilise le freeze par breakpoint "
                         "externe classique.";
    KE_LOG_INFO() << "startInProcessBreakpointFreeze(address=0x" << std::hex << address << std::dec
                  << ", type=" << valueType.toStdString() << ", value=" << value.toStdString() << ")";
    return result;
}

QVariantMap ApplicationController::stopInProcessBreakpointFreeze() {
    QVariantMap result;
    result["success"] = true;
    result["enabled"] = false;
    result["mode"] = "inprocess";

    if (m_inProcessBreakpointFreezeSession) {
        const auto stats = m_inProcessBreakpointFreezeSession->freezeStats();
        m_inProcessBreakpointFreezeSession->stop();
        result["hits"] = static_cast<qulonglong>(stats.hitCount);
        result["rewrites"] = static_cast<qulonglong>(stats.hitCount);
        m_inProcessBreakpointFreezeSession.reset();
    }
    return result;
}

QVariantMap ApplicationController::getInProcessBreakpointFreezeStats() const {
    QVariantMap result;
    result["mode"] = "inprocess";
    if (!m_inProcessBreakpointFreezeSession || !m_inProcessBreakpointFreezeSession->isFreezing()) {
        result["active"] = false;
        result["hits"] = 0ULL;
        result["rewrites"] = 0ULL;
        result["armedThreadCount"] = 0;
        return result;
    }

    const auto stats = m_inProcessBreakpointFreezeSession->freezeStats();
    result["active"] = stats.active;
    result["hits"] = static_cast<qulonglong>(stats.hitCount);
    result["rewrites"] = static_cast<qulonglong>(stats.hitCount);
    result["armedThreadCount"] = stats.armedThreadCount;
    result["healthy"] = stats.active && !stats.installError;
    return result;
}

QVariantMap ApplicationController::startSpeedhack(double factor) {
    QVariantMap result;
    result["success"] = false;

    if (!m_attached || m_pid <= 0) {
        result["error"] = "Aucun processus attaché.";
        return result;
    }
    if (m_speedhackSession && m_speedhackSession->isActive()) {
        result["error"] = "Un speedhack est déjà actif — change le facteur au lieu d'en redémarrer un.";
        return result;
    }

    const QString handlerPath = resolveSpeedhackHandlerPath();
    if (handlerPath.isEmpty()) {
        result["error"] = "KillEngineSpeedhackHandler.dll introuvable à côté de KillEngine.exe.";
        return result;
    }

    killcore::ProcessHandle ownedHandle(static_cast<uint32_t>(m_pid), killcore::ProcessAccess::AllAccess);
    if (!ownedHandle.isValid()) {
        result["error"] = "Impossible d'ouvrir le processus avec les droits nécessaires à l'injection (PROCESS_ALL_ACCESS).";
        return result;
    }

    if (!m_speedhackSession) {
        m_speedhackSession = std::make_unique<killcore::SpeedhackSession>();
    }
    QString startError;
    if (!m_speedhackSession->start(ownedHandle, factor, handlerPath, &startError)) {
        result["error"] = startError;
        return result;
    }

    const auto stats = m_speedhackSession->stats();
    result["success"] = true;
    result["active"] = stats.active;
    result["factor"] = stats.factor;
    result["hooksInstalledMask"] = static_cast<int>(stats.hooksInstalledMask);
    KE_LOG_INFO() << "startSpeedhack(pid=" << m_pid << ", factor=" << factor << ")";
    return result;
}

QVariantMap ApplicationController::setSpeedhackFactor(double factor) {
    QVariantMap result;
    result["success"] = false;

    if (!m_speedhackSession || !m_speedhackSession->isActive()) {
        result["error"] = "Aucun speedhack actif.";
        return result;
    }
    if (!m_speedhackSession->setFactor(factor)) {
        result["error"] = "Échec du réglage du facteur.";
        return result;
    }

    const auto stats = m_speedhackSession->stats();
    result["success"] = true;
    result["active"] = stats.active;
    result["installError"] = stats.installError;
    result["factor"] = stats.factor;
    result["hooksInstalledMask"] = static_cast<int>(stats.hooksInstalledMask);
    result["pid"] = m_pid;
    return result;
}

QVariantMap ApplicationController::stopSpeedhack() {
    QVariantMap result;
    result["success"] = true;
    result["active"] = false;

    if (m_speedhackSession) {
        m_speedhackSession->stop();
    }
    return result;
}

QVariantMap ApplicationController::getSpeedhackStatus() const {
    QVariantMap result;
    if (!m_speedhackSession || !m_speedhackSession->isActive()) {
        result["success"] = true;
        result["active"] = false;
        result["factor"] = 1.0;
        return result;
    }

    const auto stats = m_speedhackSession->stats();
    result["success"] = true;
    result["active"] = stats.active;
    result["installError"] = stats.installError;
    result["factor"] = stats.factor;
    result["hooksInstalledMask"] = static_cast<int>(stats.hooksInstalledMask);
    result["pid"] = m_pid;
    return result;
}

QVariantMap ApplicationController::findWhatAccessesAsync(const QString& addressHex, const QVariantMap& options) {
    QVariantMap result;
    result["success"] = false;
    result["started"] = false;
    result["address"] = addressHex;

    if (m_findWhatAccessesInProgress || m_findWhatWritesInProgress) {
        result["error"] = "Une capture debugger est deja en cours.";
        return result;
    }
    if (!m_attached || m_pid <= 0) {
        result["error"] = "Aucun processus attache.";
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

    const int timeoutMs = std::clamp(options.value("timeoutMs", 5000).toInt(), 250, 15000);
    const int maxHitsInt = std::clamp(options.value("maxHits", 10).toInt(), 1, 100);
    const int requestId = m_nextDebugRequestId++;
    const int pid = m_pid;
    const QString requestedAddress = addressHex;
    const QPointer<ApplicationController> self(this);
    auto cancellation = std::make_shared<killcore::CancellationToken>();

    m_findWhatAccessesInProgress = true;
    m_activeDebugCancellation = cancellation;

    KE_LOG_INFO() << "findWhatAccessesAsync(address=0x" << std::hex << address
                  << ", pid=" << std::dec << pid
                  << ", size=" << sizeBytes
                  << ", timeoutMs=" << timeoutMs
                  << ", maxHits=" << maxHitsInt
                  << ", requestId=" << requestId << ")";

    std::thread([self, requestId, pid, address, requestedAddress, breakpointSize, sizeBytes, timeoutMs, maxHitsInt, cancellation]() {
        const auto hits = killcore::findWhatAccesses(
            static_cast<uint32_t>(pid),
            address,
            breakpointSize,
            timeoutMs,
            static_cast<size_t>(maxHitsInt),
            cancellation.get());
        const bool cancelled = cancellation->isCancelled();

        if (!self) {
            return;
        }

        QMetaObject::invokeMethod(self.data(), [self, requestId, requestedAddress, sizeBytes, timeoutMs, maxHitsInt, hits, cancelled]() {
            if (!self) {
                return;
            }

            QVariantList hitList;
            for (const auto& hit : hits) {
                QVariantMap item;
                item["address"] = QString::number(hit.address, 16).toUpper();
                item["instructionPointer"] = QString::number(hit.instructionPointer, 16).toUpper();
                item["threadId"] = static_cast<qulonglong>(hit.threadId);
                item["valueBefore"] = static_cast<qulonglong>(hit.valueBefore);
                item["valueAfter"] = static_cast<qulonglong>(hit.valueAfter);
                item["module"] = hit.module;
                item["moduleOffset"] = QString::number(hit.moduleOffset, 16).toUpper();
                hitList.append(item);
            }

            QVariantMap finished;
            finished["requestId"] = requestId;
            finished["kind"] = "find_what_accesses";
            finished["success"] = true;
            finished["address"] = requestedAddress;
            finished["hits"] = hitList;
            finished["hitCount"] = hitList.size();
            finished["size"] = sizeBytes;
            finished["timeoutMs"] = timeoutMs;
            finished["maxHits"] = maxHitsInt;
            finished["cancelled"] = cancelled;
            finished["warning"] = "Cette fonction attache KillEngine comme debugger au processus cible pendant la capture.";
            finished["error"] = cancelled
                ? "Capture Find What Accesses annulee."
                : hits.isEmpty()
                ? "Aucun acces capture pendant la fenetre d'observation."
                : QString();

            self->m_findWhatAccessesInProgress = false;
            self->m_activeDebugCancellation.reset();
            emit self->findWhatAccessesFinished(finished);
        }, Qt::QueuedConnection);
    }).detach();

    result["success"] = true;
    result["started"] = true;
    result["requestId"] = requestId;
    result["size"] = sizeBytes;
    result["timeoutMs"] = timeoutMs;
    result["maxHits"] = maxHitsInt;
    result["error"] = "";
    return result;
}

QVariantMap ApplicationController::scanGroupScan(const QVariantList& entriesList, const QVariantMap& optionsMap) {
    QVariantMap result;
    QVariantList matches;
    result["success"] = false;
    result["matches"] = matches;

    if (!m_handle.isValid()) {
        result["error"] = "Aucun processus attache.";
        return result;
    }

    killcore::GroupScanOptions groupOptions;
    const int maxResults = std::clamp(optionsMap.value("maxResults", 1000).toInt(), 1, 10000);
    groupOptions.maxResults = static_cast<size_t>(maxResults);
    groupOptions.maxDistance = std::clamp(optionsMap.value("maxDistance", 256).toInt(), 4, 4096);

    if (entriesList.isEmpty()) {
        result["error"] = "Aucune entree pour le scan groupe.";
        return result;
    }

    for (const auto& entryVariant : entriesList) {
        const QVariantMap entryMap = entryVariant.toMap();
        killcore::GroupScanEntry entry;

        const QString typeText = entryMap.value("type", "Int32").toString();
        if (!killcore::parseValueType(typeText, &entry.type)) {
            result["error"] = QString("Type invalide pour une entree du scan groupe : %1").arg(typeText);
            return result;
        }

        bool offsetOk = false;
        entry.offset = entryMap.value("offset").toLongLong(&offsetOk);
        if (!offsetOk) {
            result["error"] = "Offset invalide pour une entree du scan groupe.";
            return result;
        }

        killcore::ScanValue scanValue;
        QString parseError;
        const QString valueText = entryMap.value("value").toString();
        if (!killcore::parseScanValue(valueText, entry.type, &scanValue, &parseError)) {
            result["error"] = parseError;
            return result;
        }
        entry.value = scanValue.value;

        groupOptions.entries.append(entry);
    }

    killcore::ScanOptions scanOptions = scanOptionsFromSettingsAndExpertOptions(optionsMap);
    scanOptions.writableOnly = optionsMap.value("writableOnly", true).toBool();

    size_t maxEntrySize = 1;
    for (const auto& entry : groupOptions.entries) {
        maxEntrySize = std::max(maxEntrySize, killcore::valueTypeSize(entry.type));
    }

    emit scanStarted();
    emit scanProgress(0);

    QElapsedTimer timer;
    timer.start();
    killcore::MemoryReader reader(m_handle);
    const auto regions = killcore::MemoryMap::snapshot(m_handle);
    int regionsScanned = 0;
    uint64_t bytesScanned = 0;
    bool partial = false;
    QString error;

    const size_t chunkSize = 1024 * 1024;

    for (const auto& region : regions) {
        if (matches.size() >= maxResults) {
            partial = true;
            break;
        }
        if (!regionMatchesScanOptions(region, scanOptions)) {
            continue;
        }

        const uint64_t regionStart = region.baseAddress;
        const uint64_t regionEnd = region.baseAddress + region.size;
        const uint64_t effectiveStart = std::max(regionStart, scanOptions.startAddress);
        const uint64_t effectiveEnd = scanOptions.stopAddress == 0 ? regionEnd : std::min(regionEnd, scanOptions.stopAddress);
        if (effectiveEnd <= effectiveStart) {
            continue;
        }

        ++regionsScanned;
        const uint64_t regionSize = effectiveEnd - effectiveStart;
        const size_t overlap = static_cast<size_t>(groupOptions.maxDistance) + maxEntrySize;
        QByteArray previousTail;
        uint64_t offset = 0;
        while (offset < regionSize && matches.size() < maxResults) {
            const uint64_t remaining = regionSize - offset;
            const size_t toRead = static_cast<size_t>(std::min<uint64_t>(remaining, chunkSize));
            const uint64_t readAddress = effectiveStart + offset;
            const auto read = reader.readChunked(readAddress, toRead, chunkSize);
            if (!read.success && !read.partial) {
                break;
            }

            QByteArray buffer = previousTail + read.data;
            const uint64_t bufferBase = readAddress - static_cast<uint64_t>(previousTail.size());
            bytesScanned += read.bytesRead;
            auto scan = killcore::scanGroupInBuffer(buffer, bufferBase, groupOptions);
            if (!scan.success && !scan.error.isEmpty()) {
                error = scan.error;
                partial = true;
                break;
            }
            if (scan.partial) {
                partial = true;
            }
            for (const auto& matchInfo : scan.matches) {
                if (matches.size() >= maxResults) {
                    partial = true;
                    break;
                }
                QVariantMap match;
                match["address"] = QString::number(matchInfo.address, 16).toUpper();
                match["type"] = killcore::valueTypeToString(matchInfo.type);
                match["confidence"] = matchInfo.confidence;
                match["variantLabel"] = matchInfo.variantLabel;
                match["regionBase"] = QString::number(region.baseAddress, 16).toUpper();
                match["protection"] = killcore::protectionToString(region.protection);
                match["memoryType"] = killcore::memoryTypeToString(region.type);
                match["writable"] = region.writable;
                matches.append(match);
            }

            previousTail = read.data.size() > static_cast<qsizetype>(overlap)
                ? read.data.right(static_cast<qsizetype>(overlap))
                : read.data;
            offset += read.bytesRead;
            if (read.bytesRead == 0 || read.partial) {
                break;
            }
        }

        const int percent = regions.isEmpty()
            ? 100
            : std::clamp((regionsScanned * 100) / std::max(1, static_cast<int>(regions.size())), 0, 99);
        emit scanProgress(percent);
        if (!error.isEmpty()) {
            break;
        }
    }

    emit scanProgress(100);

    result["success"] = error.isEmpty();
    result["partial"] = partial || matches.size() >= maxResults;
    result["matches"] = matches;
    result["matchesFound"] = matches.size();
    result["maxResults"] = maxResults;
    result["regionsScanned"] = regionsScanned;
    result["bytesScanned"] = static_cast<qulonglong>(bytesScanned);
    result["elapsedMs"] = static_cast<int>(timer.elapsed());
    result["entriesCount"] = groupOptions.entries.size();
    result["error"] = error;
    return result;
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
    QVariantMap result;
    result["success"] = false;
    result["pattern"] = patternText;

    if (!m_attached || m_pid <= 0) {
        result["error"] = "Aucun processus attaché.";
        return result;
    }

    const auto pattern = killcore::parseAobPattern(patternText);
    if (!pattern.isValid()) {
        result["error"] = pattern.error;
        return result;
    }

    killcore::AobScanOptions options;
    options.executableOnly = optionsMap.value("executableOnly", true).toBool();
    options.writableOnly = optionsMap.value("writableOnly", false).toBool();
    options.imageOnly = optionsMap.value("imageOnly", true).toBool();
    options.maxResults = std::clamp(optionsMap.value("maxResults", 200).toInt(), 1, 10000);

    const QString startText = optionsMap.value("startAddress").toString().trimmed();
    const QString stopText = optionsMap.value("stopAddress").toString().trimmed();
    if (!startText.isEmpty() && !parseHexAddress(startText, &options.startAddress)) {
        result["error"] = "Adresse de début invalide.";
        return result;
    }
    if (!stopText.isEmpty() && !parseHexAddress(stopText, &options.stopAddress)) {
        result["error"] = "Adresse de fin invalide.";
        return result;
    }
    if (options.startAddress > 0 && options.stopAddress > 0 && options.startAddress >= options.stopAddress) {
        result["error"] = "La plage AOB est invalide.";
        return result;
    }

    KE_LOG_INFO() << "scanAobPattern(patternBytes=" << pattern.bytes.size()
                  << ", executableOnly=" << options.executableOnly
                  << ", imageOnly=" << options.imageOnly
                  << ", maxResults=" << options.maxResults << ")";

    const auto scan = killcore::scanAobPattern(m_handle, pattern, options);
    QVariantList matches;
    const auto modules = killcore::ProcessEnumerator::enumerateModules(static_cast<uint32_t>(m_pid));
    for (const auto& match : scan.matches) {
        QVariantMap item;
        item["address"] = QString::number(match.address, 16).toUpper();
        item["regionBase"] = QString::number(match.regionBase, 16).toUpper();
        item["regionSize"] = static_cast<qulonglong>(match.regionSize);
        item["protection"] = killcore::protectionToString(match.protection);
        item["memoryType"] = killcore::memoryTypeToString(match.memoryType);
        for (const auto& module : modules) {
            if (match.address >= module.baseAddress && match.address < module.baseAddress + module.size) {
                item["module"] = module.name;
                item["moduleOffset"] = QString::number(match.address - module.baseAddress, 16).toUpper();
                break;
            }
        }
        matches.append(item);
    }

    result["success"] = scan.success;
    result["partial"] = scan.partial;
    result["error"] = scan.error;
    result["bytesScanned"] = static_cast<qulonglong>(scan.bytesScanned);
    result["regionsScanned"] = scan.regionsScanned;
    result["matchesFound"] = scan.matchesFound;
    result["matches"] = matches;
    result["patternBytes"] = static_cast<int>(pattern.bytes.size());
    result["executableOnly"] = options.executableOnly;
    result["imageOnly"] = options.imageOnly;
    const auto quality = killcore::evaluateAobPatternQuality(pattern);
    result["signatureQuality"] = aobPatternQualityToVariantMap(quality);
    result["signatureRisk"] = quality.level;
    result["signatureWarning"] = quality.warning;
    appendScanTelemetry("aob_scan", result);
    return result;
}

QVariantMap ApplicationController::generateAobSignature(const QString& addressHex, const QVariantMap& options) {
    QVariantMap result;
    result["success"] = false;
    result["address"] = addressHex;

    if (!m_attached || !m_handle.isValid()) {
        result["error"] = "Aucun processus attaché.";
        return result;
    }

    uint64_t address = 0;
    if (!parseHexAddress(addressHex, &address)) {
        result["error"] = "Adresse invalide.";
        return result;
    }

    const int beforeBytes = std::clamp(options.value("beforeBytes", 0).toInt(), 0, 32);
    const int length = std::clamp(options.value("length", 24).toInt(), 4, 64);
    const uint64_t startAddress = address > static_cast<uint64_t>(beforeBytes)
        ? address - static_cast<uint64_t>(beforeBytes)
        : address;

    killcore::MemoryReader reader(m_handle);
    const auto read = reader.readChunked(startAddress, static_cast<size_t>(length), 4096);
    if (!read.success && read.bytesRead == 0) {
        const QString hint = codeReadProtectionHint(read.errorCode);
        if (!hint.isEmpty()) {
            result["error"] = hint;
            result["codeReadProtected"] = true;
        } else {
            result["error"] = read.errorMessage.isEmpty() ? QString("Lecture des octets d'instruction impossible.") : read.errorMessage;
        }
        return result;
    }

    QVariantMap moduleInfo;
    const auto modules = killcore::ProcessEnumerator::enumerateModules(static_cast<uint32_t>(m_pid));
    for (const auto& module : modules) {
        if (startAddress >= module.baseAddress && startAddress < module.baseAddress + module.size) {
            moduleInfo["module"] = module.name;
            moduleInfo["moduleOffset"] = QString::number(startAddress - module.baseAddress, 16).toUpper();
            break;
        }
    }

    result["success"] = true;
    result["partial"] = read.partial;
    result["startAddress"] = QString::number(startAddress, 16).toUpper();
    result["instructionAddress"] = QString::number(address, 16).toUpper();
    result["bytesRead"] = static_cast<int>(read.bytesRead);
    result["requestedBytes"] = static_cast<int>(read.requestedBytes);
    result["hex"] = QString::fromLatin1(read.data.toHex(' ').toUpper());
    result["pattern"] = killcore::bytesToAobPattern(read.data);
    result["patternBytes"] = static_cast<int>(read.data.size());
    const auto rawPattern = killcore::parseAobPattern(result.value("pattern").toString());
    const auto quality = killcore::evaluateAobPatternQuality(rawPattern);
    result["signatureQuality"] = aobPatternQualityToVariantMap(quality);
    result["signatureRisk"] = quality.level;
    result["module"] = moduleInfo.value("module");
    result["moduleOffset"] = moduleInfo.value("moduleOffset");
    result["error"] = read.errorMessage;
    result["warning"] = QString("Signature exacte brute. %1 Elle peut nécessiter des wildcards si l'instruction contient offsets/relocations.").arg(quality.warning);
    appendScanTelemetry("aob_signature", result);
    return result;
}

QVariantMap ApplicationController::applyCodePatch(const QString& addressHex, const QString& bytesText, const QVariantMap& options) {
    QVariantMap result;
    result["success"] = false;
    result["address"] = addressHex;
    result["patchBytes"] = bytesText;

    if (!m_attached || !m_handle.isValid()) {
        result["error"] = "Aucun processus attaché.";
        return result;
    }

    uint64_t address = 0;
    if (!parseHexAddress(addressHex, &address)) {
        result["error"] = "Adresse invalide.";
        return result;
    }
    if (m_activeCodePatches.contains(address)) {
        result["error"] = "Un patch actif existe déjà à cette adresse. Restaure-le avant d'en appliquer un autre.";
        result["active"] = true;
        return result;
    }

    const auto patch = killcore::parsePatchBytes(bytesText);
    if (!patch.isValid()) {
        result["error"] = patch.error;
        return result;
    }
    if (patch.bytes.size() > 64) {
        result["error"] = "Patch trop long pour cette version expérimentale (64 bytes maximum).";
        return result;
    }

    const bool verify = options.value("verify", true).toBool();
    const auto applied = killcore::applyCodePatch(m_handle, address, patch.bytes, verify);
    result["success"] = applied.success;
    result["verified"] = applied.verified;
    result["protectionChanged"] = applied.protectionChanged;
    result["bytesWritten"] = static_cast<int>(applied.bytesWritten);
    result["originalBytes"] = QString::fromLatin1(applied.previousBytes.toHex(' ').toUpper());
    result["writtenBytes"] = QString::fromLatin1(patch.bytes.toHex(' ').toUpper());
    result["error"] = applied.error;

    if (applied.success) {
        ActiveCodePatch active;
        active.address = address;
        active.originalBytes = applied.previousBytes;
        active.patchBytes = patch.bytes;
        m_activeCodePatches.insert(address, active);
        result["active"] = true;
        KE_LOG_WARN() << "Code patch applied at 0x" << std::hex << address
                      << " bytes=" << std::dec << patch.bytes.size()
                      << " protectionChanged=" << applied.protectionChanged;
    }

    return result;
}

QVariantMap ApplicationController::suggestCodePatches(const QString& addressHex, const QVariantMap& options) {
    QVariantMap result;
    result["success"] = false;
    result["address"] = addressHex;

    if (!m_attached || !m_handle.isValid()) {
        result["error"] = "Aucun processus attaché.";
        return result;
    }

    uint64_t address = 0;
    if (!parseHexAddress(addressHex, &address)) {
        result["error"] = "Adresse invalide.";
        return result;
    }

    const int maxBytes = std::clamp(options.value("maxBytes", 16).toInt(), 8, 64);
    killcore::MemoryReader reader(m_handle);
    const auto read = reader.readChunked(address, static_cast<size_t>(maxBytes), 4096);
    if (!read.success && read.bytesRead == 0) {
        const QString hint = codeReadProtectionHint(read.errorCode);
        if (!hint.isEmpty()) {
            result["error"] = hint;
            result["codeReadProtected"] = true;
        } else {
            result["error"] = read.errorMessage.isEmpty() ? QString("Lecture instruction impossible.") : read.errorMessage;
        }
        return result;
    }

    const auto instruction = killcore::decodeX64InstructionLength(read.data);
    result["instructionSuccess"] = instruction.success;
    result["instructionLength"] = instruction.length;
    result["mnemonicHint"] = instruction.mnemonicHint;
    result["disassembly"] = instruction.disassembly;
    result["decoder"] = instruction.decoder;
    result["category"] = instruction.category;
    result["stableAobPattern"] = instruction.stableAobPattern;
    const auto stablePattern = killcore::parseAobPattern(instruction.stableAobPattern);
    const auto stableQuality = killcore::evaluateAobPatternQuality(stablePattern);
    result["signatureQuality"] = aobPatternQualityToVariantMap(stableQuality);
    result["signatureRisk"] = stableQuality.level;
    result["bytesRead"] = static_cast<int>(read.bytesRead);
    result["bytes"] = QString::fromLatin1(read.data.left(instruction.length > 0 ? instruction.length : read.data.size()).toHex(' ').toUpper());
    // Registre+deplacement de l'operande memoire destination (vide si non
    // exploitable) : permet au frontend de proposer "Forcer une valeur (hook)"
    // meme quand l'instruction n'a pas d'immediat a substituer directement
    // (source registre) — voir ApplicationController::forceWriteInstructionValue.
    result["memBaseRegister"] = instruction.memBaseRegister;
    result["memDisplacement"] = static_cast<qlonglong>(instruction.memDisplacement);

    if (!instruction.success) {
        result["error"] = instruction.error;
        return result;
    }

    QVariantList suggestions;
    const auto patchSuggestions = killcore::suggestInstructionPatches(instruction);
    for (const auto& suggestion : patchSuggestions) {
        QVariantMap item;
        item["label"] = suggestion.label;
        item["bytesText"] = suggestion.bytesText;
        item["description"] = suggestion.description;
        item["category"] = suggestion.category;
        item["riskLevel"] = suggestion.riskLevel;
        item["risky"] = suggestion.risky;
        item["needsValueInput"] = suggestion.needsValueInput;
        item["valueOffset"] = suggestion.valueOffset;
        item["valueSize"] = suggestion.valueSize;
        suggestions.append(item);
    }

    result["success"] = true;
    result["suggestions"] = suggestions;
    result["warning"] = "Décodage x64 ciblé et expérimental. Vérifie toujours les bytes avant d'appliquer.";
    appendScanTelemetry("aob_patch_suggest", result);
    return result;
}

QVariantMap ApplicationController::disassembleBackward(const QString& addressHex, const QVariantMap& options) const {
    QVariantMap result;
    result["success"] = false;
    result["address"] = addressHex;

    if (!m_attached || !m_handle.isValid()) {
        result["error"] = "Aucun processus attaché.";
        return result;
    }

    uint64_t address = 0;
    if (!parseHexAddress(addressHex, &address)) {
        result["error"] = "Adresse invalide.";
        return result;
    }

    const int windowBytes = std::clamp(options.value("windowBytes", 64).toInt(), 16, 128);
    const int trailingBytes = 16; // Marge pour décoder entièrement l'instruction cible elle-même.
    const uint64_t start = address >= static_cast<uint64_t>(windowBytes) ? address - static_cast<uint64_t>(windowBytes) : 0;
    const int targetOffsetInWindow = static_cast<int>(address - start);

    killcore::MemoryReader reader(m_handle);
    const auto read = reader.readChunked(start, static_cast<size_t>(targetOffsetInWindow + trailingBytes), 4096);
    if (!read.success && read.bytesRead == 0) {
        const QString hint = codeReadProtectionHint(read.errorCode);
        if (!hint.isEmpty()) {
            result["error"] = hint;
            result["codeReadProtected"] = true;
        } else {
            result["error"] = read.errorMessage.isEmpty() ? QString("Lecture mémoire impossible.") : read.errorMessage;
        }
        return result;
    }
    if (targetOffsetInWindow > read.data.size()) {
        result["error"] = "Lecture mémoire trop courte pour atteindre l'adresse cible.";
        return result;
    }

    const auto backward = killcore::disassembleBackwardWindow(read.data, targetOffsetInWindow);
    result["success"] = backward.success;
    result["error"] = backward.error;
    if (!backward.success) {
        return result;
    }

    QVariantList instructions;
    QVariantList candidateFields;
    uint64_t cursor = start + static_cast<uint64_t>(backward.startOffsetInWindow);
    for (const auto& info : backward.instructions) {
        QVariantMap item;
        const QString instrAddressHex = QString::number(cursor, 16).toUpper();
        item["address"] = instrAddressHex;
        item["bytes"] = info.rawBytesText;
        item["disassembly"] = info.disassembly;
        item["mnemonicHint"] = info.mnemonicHint;
        item["category"] = info.category;
        item["memBaseRegister"] = info.memBaseRegister;
        item["memDisplacement"] = static_cast<qlonglong>(info.memDisplacement);
        const bool isCandidateField = !info.memBaseRegister.isEmpty();
        item["isCandidateField"] = isCandidateField;
        instructions.append(item);
        if (isCandidateField) {
            QVariantMap candidate = item;
            candidateFields.append(candidate);
        }
        cursor += static_cast<uint64_t>(info.length);
    }

    result["instructions"] = instructions;
    result["candidateFields"] = candidateFields;
    result["warning"] = "Désassemblage en arrière expérimental (lecture seule). Vérifie toujours les champs candidats avant d'écrire dessus.";
    appendScanTelemetry("disassemble_backward", result);
    return result;
}

// Cadence et garde-fous du sondage "tient/repart" ci-dessous : mêmes valeurs que
// applyWriteWatchTick()/m_writeWatchTimer (1.5s/tick, ~12s, 2 mismatches consecutifs
// pour confirmer une reversion), mais etat et logique dedies — m_activeDebugCancellation
// et registerWriteWatch() restent reserves a leurs usages existants (attach debugger,
// et surveillance passive des ecritures normales), voir commentaire sur
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
    QVariantMap result;
    result["success"] = false;
    result["address"] = addressHex;

    if (!m_attached || !m_handle.isValid()) {
        result["error"] = "Aucun processus attaché.";
        return result;
    }

    uint64_t address = 0;
    if (!parseHexAddress(addressHex, &address)) {
        result["error"] = "Adresse invalide.";
        return result;
    }

    const auto it = m_activeCodePatches.constFind(address);
    if (it == m_activeCodePatches.constEnd()) {
        result["error"] = "Aucun patch actif connu à cette adresse.";
        result["active"] = false;
        return result;
    }

    const auto restored = killcore::restoreCodePatch(m_handle, address, it->originalBytes, true);
    result["success"] = restored.success;
    result["verified"] = restored.verified;
    result["protectionChanged"] = restored.protectionChanged;
    result["bytesWritten"] = static_cast<int>(restored.bytesWritten);
    result["restoredBytes"] = QString::fromLatin1(it->originalBytes.toHex(' ').toUpper());
    result["error"] = restored.error;

    if (restored.success) {
        m_activeCodePatches.remove(address);
        result["active"] = false;
        KE_LOG_WARN() << "Code patch restored at 0x" << std::hex << address;
    } else {
        result["active"] = true;
    }

    return result;
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
    QVariantMap result;
    result["success"] = false;
    result["targetAddress"] = targetAddressHex;
    result["hookAddress"] = hookAddressHex;

    if (!m_attached || !m_handle.isValid()) {
        result["error"] = "Aucun processus attaché.";
        return result;
    }

    uint64_t targetAddress = 0;
    uint64_t hookAddress = 0;
    if (!parseHexAddress(targetAddressHex, &targetAddress) || !parseHexAddress(hookAddressHex, &hookAddress)) {
        result["error"] = "Adresse invalide.";
        return result;
    }
    if (m_activeFunctionHooks.contains(targetAddress)) {
        result["error"] = "Un hook actif existe déjà sur cette adresse. Retire-le avant d'en installer un autre.";
        result["active"] = true;
        return result;
    }

    const auto installed = killcore::installInlineHook(m_handle, targetAddress, hookAddress);
    result["success"] = installed.success;
    result["error"] = installed.error;
    result["trampolineAddress"] = QString::number(installed.trampolineAddress, 16).toUpper();
    result["originalBytes"] = QString::fromLatin1(installed.originalBytes.toHex(' ').toUpper());

    if (installed.success) {
        ActiveFunctionHook active;
        active.targetAddress = targetAddress;
        active.hookFunctionAddress = hookAddress;
        active.trampolineAddress = installed.trampolineAddress;
        active.originalBytes = installed.originalBytes;
        m_activeFunctionHooks.insert(targetAddress, active);
        result["active"] = true;
        KE_LOG_WARN() << "Function hook installed at 0x" << std::hex << targetAddress
                      << " -> 0x" << hookAddress;
    }

    appendScanTelemetry("function_hook_install", result);
    return result;
}

QVariantMap ApplicationController::removeFunctionHook(const QString& targetAddressHex) {
    QVariantMap result;
    result["success"] = false;
    result["targetAddress"] = targetAddressHex;

    if (!m_attached || !m_handle.isValid()) {
        result["error"] = "Aucun processus attaché.";
        return result;
    }

    uint64_t targetAddress = 0;
    if (!parseHexAddress(targetAddressHex, &targetAddress)) {
        result["error"] = "Adresse invalide.";
        return result;
    }

    const auto it = m_activeFunctionHooks.constFind(targetAddress);
    if (it == m_activeFunctionHooks.constEnd()) {
        result["error"] = "Aucun hook actif connu à cette adresse.";
        result["active"] = false;
        return result;
    }

    const auto removed = killcore::removeInlineHook(m_handle, targetAddress, it->originalBytes);
    result["success"] = removed.success;
    result["error"] = removed.error;
    result["restoredBytes"] = QString::fromLatin1(it->originalBytes.toHex(' ').toUpper());

    if (removed.success) {
        m_activeFunctionHooks.remove(targetAddress);
        result["active"] = false;
        KE_LOG_WARN() << "Function hook removed at 0x" << std::hex << targetAddress;
    } else {
        result["active"] = true;
    }

    appendScanTelemetry("function_hook_remove", result);
    return result;
}

namespace {

QVariantMap autoAsmScriptToPreviewVariant(const killcore::AutoAsmScript& script) {
    QVariantMap preview;
    preview["parseSuccess"] = script.success;
    preview["parseError"] = script.error;
    preview["parseErrorLine"] = script.errorLine;

    QVariantList instructions;
    for (const auto& instruction : script.instructions) {
        instructions.append(QVariantMap{
            {"line", instruction.line},
            {"target", instruction.target},
        });
    }
    preview["instructionCount"] = script.instructions.size();
    preview["instructions"] = instructions;

    QVariantList allocations;
    for (const auto& allocation : script.allocations) {
        allocations.append(QVariantMap{{"name", allocation.name}, {"size", static_cast<qulonglong>(allocation.size)}});
    }
    preview["allocations"] = allocations;

    QVariantList labels;
    for (const auto& label : script.labels) {
        labels.append(label.name);
    }
    preview["labels"] = labels;

    return preview;
}

} // namespace

QVariantMap ApplicationController::parseAutoAssemblerScript(const QString& scriptText) const {
    QVariantMap result;
    const auto script = killcore::parseAutoAsmScript(scriptText);
    result = autoAsmScriptToPreviewVariant(script);
    result["success"] = script.success;

    if (script.success) {
        // Resout les modules references par "module"+offset: si un processus
        // est attache, pour que l'apercu montre les vraies adresses/bytes de
        // ces blocs plutot que de les rejeter faute de contexte. Les labels
        // lies a un alloc() (ex: "newmem:") ne peuvent pas etre resolus ici
        // (l'allocation reelle n'a lieu qu'a l'execution) — ils restent
        // affiches a l'offset 0 par defaut, ce qui reste suffisant pour
        // verifier le contenu compile avant d'executer pour de vrai.
        killcore::AutoAsmCompileContext context;
        if (m_attached && m_pid != 0) {
            const auto modules = killcore::ProcessEnumerator::enumerateModules(static_cast<uint32_t>(m_pid));
            for (const auto& instr : script.instructions) {
                if (instr.type != killcore::AutoAsmInstructionType::ModuleLabel) continue;
                if (context.moduleBaseAddresses.contains(instr.target)) continue;
                for (const auto& module : modules) {
                    if (module.name.compare(instr.target, Qt::CaseInsensitive) == 0) {
                        context.moduleBaseAddresses.insert(instr.target, module.baseAddress);
                        break;
                    }
                }
            }
        }

        const auto compiled = killcore::compileAutoAsmScript(script, 0, context);
        result["compileSuccess"] = compiled.success;
        result["compileError"] = compiled.error;
        result["compileErrorLine"] = compiled.errorLine;

        QStringList regionSummaries;
        for (const auto& region : compiled.regions) {
            regionSummaries.append(QStringLiteral("0x%1: %2")
                .arg(QString::number(region.baseAddress, 16).toUpper(),
                     QString::fromLatin1(region.code.toHex(' ').toUpper())));
        }
        result["compiledBytes"] = regionSummaries.join('\n');
        result["compiledRegionCount"] = compiled.regions.size();
    }
    return result;
}

QVariantMap ApplicationController::executeAutoAssemblerScript(const QString& scriptText) {
    QVariantMap result;
    result["success"] = false;

    if (!m_attached || !m_handle.isValid()) {
        result["error"] = "Aucun processus attaché.";
        return result;
    }

    const auto script = killcore::parseAutoAsmScript(scriptText);
    if (!script.success) {
        result["error"] = script.error;
        result["errorLine"] = script.errorLine;
        return result;
    }
    if (m_lastAutoAsmResult.has_value()) {
        result["error"] = "Un script auto-assembler est déjà actif. Restaure-le avant d'en exécuter un autre.";
        result["active"] = true;
        return result;
    }

    const auto executed = killcore::executeAutoAsmScript(m_handle, script);
    result["success"] = executed.success;
    result["error"] = executed.error;
    result["errorLine"] = executed.errorLine;

    QVariantList patchedRegions;
    for (const auto& region : executed.patchedRegions) {
        patchedRegions.append(QVariantMap{
            {"address", QString::number(region.address, 16).toUpper()},
            {"size", region.size},
            {"wasAllocated", region.wasAllocated},
        });
    }
    result["patchedRegions"] = patchedRegions;
    if (!executed.patchedRegions.isEmpty()) {
        // Alias pratique vers la premiere region, pour un script simple a une
        // seule region (le cas le plus courant) sans obliger l'appelant a
        // depouiller patchedRegions.
        result["patchAddress"] = QString::number(executed.patchedRegions.first().address, 16).toUpper();
        result["patchSize"] = executed.patchedRegions.first().size;
    }

    if (executed.success) {
        m_lastAutoAsmResult = executed;
        result["active"] = true;
        KE_LOG_WARN() << "Auto-assembler script executed, " << executed.patchedRegions.size() << " region(s) written";
    }

    appendScanTelemetry("auto_assembler_execute", result);
    return result;
}

QVariantMap ApplicationController::restoreAutoAssemblerScript() {
    QVariantMap result;
    result["success"] = false;

    if (!m_attached || !m_handle.isValid()) {
        result["error"] = "Aucun processus attaché.";
        return result;
    }
    if (!m_lastAutoAsmResult.has_value()) {
        result["error"] = "Aucun script auto-assembler actif à restaurer.";
        result["active"] = false;
        return result;
    }

    const bool restored = killcore::restoreAutoAsmScript(m_handle, *m_lastAutoAsmResult);
    result["success"] = restored;

    if (restored) {
        QVariantList restoredAddresses;
        for (const auto& region : m_lastAutoAsmResult->patchedRegions) {
            restoredAddresses.append(QString::number(region.address, 16).toUpper());
        }
        result["restoredAddresses"] = restoredAddresses;
        m_lastAutoAsmResult.reset();
        result["active"] = false;
        KE_LOG_WARN() << "Auto-assembler script restored.";
    } else {
        result["error"] = "Échec de la restauration du script auto-assembler.";
        result["active"] = true;
    }

    appendScanTelemetry("auto_assembler_restore", result);
    return result;
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
    QVariantMap result;
    result["success"] = false;

    uint64_t address = 0;
    if (!parseHexAddress(addressHex, &address)) {
        result["error"] = "Adresse invalide.";
        return result;
    }

    if (!enabled) {
        m_freeze.remove(address);
        if (!m_freeze.hasMode(killcore::FreezeMode::Polling)) {
            m_freezeTimer.stop();
        }
        result["success"] = true;
        result["enabled"] = false;
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

    if (!hasAddressBeenWriteVerified(address)) {
        result["warning"] = "Cette adresse n'a jamais été écrite avec succès avant ce freeze — "
                             "si c'est un candidat frais (jamais testé par une écriture simple), "
                             "certaines cibles réagissent mal à une réécriture continue non vérifiée "
                             "(jusqu'au crash observé sur une cible réelle). Teste une écriture simple "
                             "et vérifie visuellement avant de figer, si possible.";
    }

    m_freeze.setEntry(address, type, killcore::scanValueToBytes(scanValue), killcore::FreezeMode::Polling);
    if (!m_freezeTimer.isActive()) {
        m_freezeTimer.start();
    }

    result["success"] = true;
    result["enabled"] = true;
    return result;
}

QVariantMap ApplicationController::freezeWithBreakpoint(const QString& addressHex, const QString& valueType, const QString& value, const QVariantMap& options) {
    QVariantMap result;
    result["success"] = false;
    result["enabled"] = false;
    result["mode"] = "breakpoint";

    if (!m_attached || m_pid <= 0) {
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

    const QByteArray frozenBytes = killcore::scanValueToBytes(scanValue);
    switch (frozenBytes.size()) {
        case 1:
        case 2:
        case 4:
        case 8:
            break;
        default:
            result["error"] = "Taille de valeur incompatible avec un hardware breakpoint.";
            return result;
    }

    killcore::BreakpointFreezeMode mode = killcore::BreakpointFreezeMode::RewriteValue;
    const QString modeText = options.value("mode", "rewrite").toString().toLower();
    if (modeText == "capture") {
        mode = killcore::BreakpointFreezeMode::Capture;
    } else if (modeText == "block" || modeText == "blockwrite") {
        mode = killcore::BreakpointFreezeMode::BlockWrite;
    }

    return activateBreakpointFreezeFor(address, type, frozenBytes, mode, modeText);
}

QVariantMap ApplicationController::activateBreakpointFreezeFor(
    uint64_t address,
    killcore::ValueType type,
    const QByteArray& frozenBytes,
    killcore::BreakpointFreezeMode mode,
    const QString& modeText) {
    QVariantMap result;
    m_freeze.setEntry(address, type, frozenBytes, killcore::FreezeMode::HardwareBreakpoint);
    QString restartError;
    const bool ok = restartBreakpointFreezeFromRegistry(mode, &restartError);
    result["success"] = ok;
    result["enabled"] = ok;
    result["address"] = QString::number(address, 16).toUpper();
    result["type"] = killcore::valueTypeToString(type);
    result["bytesWritten"] = 0;
    result["verified"] = false;
    result["breakpointSize"] = frozenBytes.size();
    result["freezeMode"] = modeText;
    result["mode"] = "breakpoint";
    if (!ok) {
        m_freeze.remove(address);
        restartBreakpointFreezeFromRegistry(mode);
        result["error"] = restartError.isEmpty()
            ? "Impossible d'activer le freeze par hardware breakpoint. Vérifie les privilèges debug et la cible."
            : restartError;
    }
    return result;
}

QVariantMap ApplicationController::escalatePollingFreezeToBreakpoint(const QString& addressHex) {
    QVariantMap result;
    result["success"] = false;
    result["enabled"] = false;
    result["mode"] = "breakpoint";

    if (!m_attached || m_pid <= 0) {
        result["error"] = "Aucun processus attaché.";
        return result;
    }

    uint64_t address = 0;
    if (!parseHexAddress(addressHex, &address)) {
        result["error"] = "Adresse invalide.";
        return result;
    }

    const killcore::FreezeEntry* pollingEntry = nullptr;
    for (const auto& entry : m_freeze.entries()) {
        if (entry.address == address && entry.mode == killcore::FreezeMode::Polling) {
            pollingEntry = &entry;
            break;
        }
    }
    if (!pollingEntry) {
        result["error"] = "Aucun freeze polling actif sur cette adresse (déjà arrêté ou déjà en Freeze BP ?).";
        return result;
    }

    // Reutilise directement les bytes/type deja connus de l'entree polling :
    // aucun decodage/re-encodage depuis une chaine, contrairement a
    // freezeWithBreakpoint() qui part d'une saisie utilisateur.
    return activateBreakpointFreezeFor(
        pollingEntry->address,
        pollingEntry->type,
        pollingEntry->value,
        killcore::BreakpointFreezeMode::RewriteValue,
        "rewrite");
}

QVariantMap ApplicationController::stopBreakpointFreeze() {
    QVariantMap result;
    result["success"] = true;
    result["enabled"] = false;
    result["mode"] = "breakpoint";

    if (m_breakpointFreeze) {
        const auto stats = m_breakpointFreeze->stats();
        m_breakpointFreeze->stop();
        m_freeze.removeByMode(killcore::FreezeMode::HardwareBreakpoint);
        result["hits"] = static_cast<qulonglong>(stats.totalHits);
        result["rewrites"] = static_cast<qulonglong>(stats.rewrites);
        result["blocks"] = static_cast<qulonglong>(stats.blocks);
        result["errors"] = static_cast<qulonglong>(stats.errors);
    }
    return result;
}

QVariantMap ApplicationController::getBreakpointFreezeStats() const {
    QVariantMap result;
    result["active"] = m_breakpointFreeze && m_breakpointFreeze->isActive();
    result["mode"] = "breakpoint";
    if (!m_breakpointFreeze) {
        result["hits"] = 0ULL;
        result["rewrites"] = 0ULL;
        result["blocks"] = 0ULL;
        result["errors"] = 0ULL;
        return result;
    }

    const auto stats = m_breakpointFreeze->stats();
    result["hits"] = static_cast<qulonglong>(stats.totalHits);
    result["rewrites"] = static_cast<qulonglong>(stats.rewrites);
    result["blocks"] = static_cast<qulonglong>(stats.blocks);
    result["errors"] = static_cast<qulonglong>(stats.errors);

    // Signal simple, sans nouveau minuteur dédié : si des erreurs de
    // réécriture s'accumulent, le freeze BP n'est probablement pas en train
    // de tenir correctement malgré des hits. L'UI peut afficher ceci sans
    // attendre l'arrêt du freeze.
    result["healthy"] = stats.errors == 0;
    return result;
}

QVariantMap ApplicationController::setFreezeInterval(int intervalMs) {
    QVariantMap result;

    // Bornes raisonnables : 10 ms (très agressif, pour cibles qui réécrivent vite)
    // à 2000 ms (économique). En dehors de ces bornes, on remet le défaut (100 ms).
    const int clamped = (intervalMs <= 0) ? 100 : std::clamp(intervalMs, 10, 2000);

    m_freezeTimer.setInterval(clamped);

    // Si le timer est déjà actif (freeze en cours), on le redémarre avec le nouvel intervalle.
    const bool wasActive = m_freezeTimer.isActive();
    if (wasActive) {
        m_freezeTimer.stop();
        m_freezeTimer.start();
    }

    result["success"] = true;
    result["intervalMs"] = clamped;
    result["wasActive"] = wasActive;
    return result;
}

QVariantMap ApplicationController::registerGlobalHotkey(const QString& comboText, const QVariantMap& actionMap) {
    QVariantMap result;
    result["success"] = false;
    result["combo"] = comboText;

    if (!m_hotkeys) {
        result["error"] = "Gestionnaire de hotkeys indisponible.";
        return result;
    }

    const killcore::HotkeyCombo combo = killcore::HotkeyCombo::fromString(comboText);
    if (combo.keyCode == 0) {
        result["error"] = "Combinaison invalide. Exemple: Ctrl+Alt+F1.";
        return result;
    }

    const QString typeText = actionMap.value("type", "custom").toString().toLower();
    killcore::HotkeyAction action;
    action.type = killcore::HotkeyActionType::Custom;
    if (typeText == "toggle_freeze") {
        action.type = killcore::HotkeyActionType::ToggleFreeze;
    } else if (typeText == "toggle_patch") {
        action.type = killcore::HotkeyActionType::TogglePatch;
    } else if (typeText == "write_value") {
        action.type = killcore::HotkeyActionType::WriteValue;
    } else if (typeText == "toggle_overlay") {
        action.type = killcore::HotkeyActionType::ToggleOverlay;
    }
    action.targetId = actionMap.value("targetId").toString();
    action.label = actionMap.value("label", combo.toString()).toString();
    action.payload = actionMap.value("payload");

    const int id = m_hotkeys->registerHotkey(combo, action);
    if (id < 0) {
        result["error"] = "RegisterHotKey a échoué. La combinaison est peut-être déjà utilisée.";
        return result;
    }

    result["success"] = true;
    result["id"] = id;
    result["combo"] = combo.toString();
    result["type"] = typeText;
    result["targetId"] = action.targetId;
    appendScanTelemetry("global_hotkey_registered", result);
    return result;
}

QVariantMap ApplicationController::unregisterGlobalHotkey(int id) {
    QVariantMap result;
    result["success"] = m_hotkeys && m_hotkeys->unregisterHotkey(id);
    result["id"] = id;
    if (!result.value("success").toBool()) {
        result["error"] = "Hotkey introuvable.";
    }
    appendScanTelemetry("global_hotkey_unregistered", result);
    return result;
}

QVariantMap ApplicationController::getGlobalHotkeys() const {
    QVariantMap result;
    QVariantList hotkeys;
    if (m_hotkeys) {
        const auto entries = m_hotkeys->registeredHotkeys();
        for (int i = 0; i < entries.size(); ++i) {
            const auto& entry = entries[i];
            QVariantMap item;
            item["index"] = i;
            item["combo"] = entry.first.toString();
            item["targetId"] = entry.second.targetId;
            item["label"] = entry.second.label;
            item["payload"] = entry.second.payload;
            hotkeys.append(item);
        }
    }
    result["success"] = true;
    result["hotkeys"] = hotkeys;
    return result;
}

QVariantMap ApplicationController::clearGlobalHotkeys() {
    QVariantMap result;
    if (m_hotkeys) {
        m_hotkeys->unregisterAll();
    }
    result["success"] = true;
    appendScanTelemetry("global_hotkeys_cleared", result);
    return result;
}

QVariantMap ApplicationController::setTrainerOverlayVisible(bool visible, const QVariantMap& options) {
    QVariantMap result;
    result["success"] = true;
    result["visible"] = visible;

    if (visible) {
        if (!m_trainerOverlay) {
            auto* overlay = new QWidget(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
            overlay->setAttribute(Qt::WA_DeleteOnClose, false);
            overlay->setWindowTitle("KillEngine Trainer Overlay");
            overlay->setStyleSheet(
                "QWidget { background: rgba(22, 23, 31, 222); border: 1px solid rgba(122,162,247,110); border-radius: 8px; }"
                "QLabel { color: #c0caf5; font-family: 'Segoe UI'; font-size: 12px; }");
            auto* layout = new QVBoxLayout(overlay);
            layout->setContentsMargins(12, 10, 12, 10);
            auto* label = new QLabel("KillEngine Trainer\nAucune feature active.", overlay);
            label->setTextFormat(Qt::PlainText);
            label->setWordWrap(true);
            layout->addWidget(label);
            m_trainerOverlay = overlay;
            m_trainerOverlayLabel = label;
        }
        const int x = options.value("x", 24).toInt();
        const int y = options.value("y", 24).toInt();
        const int width = std::clamp(options.value("width", 320).toInt(), 180, 640);
        const int height = std::clamp(options.value("height", 140).toInt(), 80, 480);
        m_trainerOverlay->setGeometry(x, y, width, height);
        m_trainerOverlay->show();
        m_trainerOverlay->raise();
    } else if (m_trainerOverlay) {
        m_trainerOverlay->hide();
    }

    appendScanTelemetry("trainer_overlay_visible", result);
    return result;
}

QVariantMap ApplicationController::updateTrainerOverlay(const QVariantMap& state) {
    QVariantMap result;
    result["success"] = false;
    if (!m_trainerOverlay || !m_trainerOverlayLabel) {
        result["error"] = "Overlay Trainer non initialisé.";
        return result;
    }

    const QString title = state.value("title", "KillEngine Trainer").toString();
    const QVariantList features = state.value("features").toList();
    QStringList lines;
    lines << title;
    for (const QVariant& item : features.mid(0, 10)) {
        const QVariantMap feature = item.toMap();
        const QString enabled = feature.value("enabled").toBool() ? "ON " : "OFF";
        lines << QString("%1  %2  %3")
            .arg(enabled, feature.value("name").toString(), feature.value("status").toString());
    }
    if (features.isEmpty()) {
        lines << "Aucune feature Trainer.";
    }
    m_trainerOverlayLabel->setText(lines.join('\n'));
    result["success"] = true;
    result["lineCount"] = lines.size();
    return result;
}

void ApplicationController::applyFreezeTick() {
    if (m_freeze.isEmpty() || m_pid <= 0) {
        m_freezeTimer.stop();
        return;
    }

    killcore::ProcessHandle writeHandle(static_cast<uint32_t>(m_pid), killcore::ProcessAccess::ReadWrite);
    if (!writeHandle.isValid()) {
        return;
    }

    killcore::MemoryWriter writer(writeHandle);
    killcore::MemoryReader reader(writeHandle);
    for (const auto& entry : m_freeze.entries()) {
        if (!entry.enabled || entry.mode != killcore::FreezeMode::Polling) {
            continue;
        }

        // Fiabilite (Phase 18) : lire AVANT de reecrire detecte, sans que
        // l'utilisateur ait besoin de le signaler, qu'une cible reecrit plus
        // vite que ce freeze ne peut suivre (le classique "freeze qui
        // clignote"). La reecriture a lieu dans tous les cas juste apres.
        const auto read = reader.read(entry.address, static_cast<size_t>(entry.value.size()));
        const bool matched = (read.success || read.partial)
            && read.bytesRead == static_cast<size_t>(entry.value.size())
            && read.data == entry.value;

        const bool crossedThreshold = m_freeze.recordPollTick(entry.address, matched);

        writer.write(entry.address, entry.value, false);

        if (crossedThreshold) {
            const QString addressHex = QString::number(entry.address, 16).toUpper();
            const double holdRatePercent = entry.totalTicks > 0
                ? 100.0 * static_cast<double>(entry.totalTicks - entry.totalDriftTicks) / static_cast<double>(entry.totalTicks)
                : 0.0;

            QVariantMap info;
            info["address"] = addressHex;
            info["type"] = killcore::valueTypeToString(entry.type);
            info["mode"] = "polling";
            info["consecutiveDriftTicks"] = entry.consecutiveDriftTicks;
            info["totalDriftTicks"] = entry.totalDriftTicks;
            info["totalTicks"] = entry.totalTicks;
            info["holdRatePercent"] = holdRatePercent;
            info["message"] = QString(
                "Le freeze sur 0x%1 ne tient pas : la valeur repart avant chaque réécriture depuis %2 ticks d'affilée "
                "(tenue mesurée %3%). La cible réécrit probablement plus vite que l'intervalle de polling actuel.")
                .arg(addressHex)
                .arg(entry.consecutiveDriftTicks)
                .arg(QString::number(holdRatePercent, 'f', 0));
            info["suggestion"] = "Passe en Freeze BP (bloque l'écriture à la source) ou lance Écrit par pour trouver l'instruction qui réécrit.";

            appendScanTelemetry("freeze_poll_instability", info);
            emit freezeInstabilityDetected(info);
        }
    }
}

// Nombre de sondages (1.5s d'intervalle, cf. m_writeWatchTimer.setInterval)
// pendant lesquels une adresse fraichement ecrite est surveillee avant
// d'abandonner faute de reversion detectee — ~12s, assez pour attraper un
// jeu qui recalcule/reecrit au tick suivant sans laisser tourner le sondage
// indefiniment sur une adresse qui a fini par tenir.
constexpr int kWriteWatchTicks = 8;
// Nombre d'adresses surveillees en parallele au maximum : au-dela, les plus
// anciennes sont abandonnees plutot que de laisser la liste grossir sans
// borne si l'utilisateur ecrit en rafale (ex: boucle d'ecriture batch).
constexpr int kWriteWatchMaxEntries = 20;
// Sondages consecutifs en desaccord avant de conclure a une vraie reversion
// (et pas un aleas de lecture isole, ex: lu pile pendant une autre ecriture
// concurrente ailleurs dans le processus) — meme principe que le seuil de
// FreezeManager (kFreezePollDriftThreshold), en plus bas car un faux negatif
// ici coute juste un delai de quelques secondes, pas une detection ratee.
constexpr int kWriteWatchConfirmMismatches = 2;

void ApplicationController::registerWriteWatch(uint64_t address, killcore::ValueType type, const QByteArray& expectedBytes) {
    if (expectedBytes.isEmpty()) {
        return;
    }
    // Une adresse deja surveillee est reecrite : on repart sur une fenetre
    // d'observation fraiche plutot que de laisser cohabiter deux entrees
    // pour la meme adresse (la precedente valeur attendue n'a plus de sens).
    for (int i = m_writeWatchEntries.size() - 1; i >= 0; --i) {
        if (m_writeWatchEntries.at(i).address == address) {
            m_writeWatchEntries.removeAt(i);
        }
    }
    while (m_writeWatchEntries.size() >= kWriteWatchMaxEntries) {
        m_writeWatchEntries.removeFirst();
    }
    WriteWatchEntry entry;
    entry.address = address;
    entry.type = type;
    entry.expectedBytes = expectedBytes;
    entry.ticksRemaining = kWriteWatchTicks;
    m_writeWatchEntries.append(entry);
    if (!m_writeWatchTimer.isActive()) {
        m_writeWatchTimer.start();
    }
}

// Sondage independant du freeze (cf. commentaire de m_writeWatchTimer) :
// declenche automatiquement le chemin "Ecrit par" existant (recoveryAction
// find_what_writes_targets, deja cable cote AssistantView.vue) des qu'une
// valeur ecrite par writeMemoryValueConfirmed repart toute seule, au lieu
// d'attendre que l'utilisateur le remarque et clique le bouton a la main —
// le chainage aval (Ecrit par -> AOB -> patch) existe deja, seul le
// declenchement en amont manquait (docs/POWER_UP_ROADMAP.md section H.1).
void ApplicationController::applyWriteWatchTick() {
    if (m_writeWatchEntries.isEmpty() || m_pid <= 0 || !m_handle.isValid()) {
        m_writeWatchTimer.stop();
        return;
    }

    killcore::MemoryReader reader(m_handle);
    for (int i = m_writeWatchEntries.size() - 1; i >= 0; --i) {
        auto& entry = m_writeWatchEntries[i];
        const auto read = reader.read(entry.address, static_cast<size_t>(entry.expectedBytes.size()));
        const bool matches = (read.success || read.partial)
            && read.bytesRead == static_cast<size_t>(entry.expectedBytes.size())
            && read.data == entry.expectedBytes;

        if (!matches) {
            ++entry.consecutiveMismatches;
        } else {
            entry.consecutiveMismatches = 0;
        }

        if (entry.consecutiveMismatches >= kWriteWatchConfirmMismatches) {
            const QString addressHex = QString::number(entry.address, 16).toUpper();
            QVariantMap info;
            info["address"] = addressHex;
            info["type"] = killcore::valueTypeToString(entry.type);
            info["message"] = QString(
                "La valeur écrite à 0x%1 a déjà changé toute seule, quelques secondes après l'écriture — quelque "
                "chose la recalcule ou la réécrit depuis une source que tu n'as pas encore trouvée. Une simple "
                "écriture directe ne suffira pas ici.")
                .arg(addressHex);
            info["suggestion"] = "Capture l'instruction qui écrit dessus pour trouver la vraie source, ou pose un freeze si tu veux juste bloquer cette valeur.";

            appendScanTelemetry("write_did_not_hold", info);
            emit writeDidNotHold(info);
            m_writeWatchEntries.removeAt(i);
            continue;
        }

        if (--entry.ticksRemaining <= 0) {
            // Soit tenu pendant toute la fenetre d'observation, soit un seul
            // mismatch jamais confirme par un second : dans les deux cas rien
            // d'assez sur a signaler, on arrete de surveiller cette adresse.
            m_writeWatchEntries.removeAt(i);
        }
    }

    if (m_writeWatchEntries.isEmpty()) {
        m_writeWatchTimer.stop();
    }
}

bool ApplicationController::restartBreakpointFreezeFromRegistry(killcore::BreakpointFreezeMode mode, QString* error) {
    if (!m_attached || m_pid <= 0) {
        if (error) *error = "Aucun processus attaché.";
        return false;
    }

    const auto entries = m_freeze.entriesForMode(killcore::FreezeMode::HardwareBreakpoint);
    if (entries.isEmpty()) {
        if (m_breakpointFreeze) {
            m_breakpointFreeze->stop();
        }
        return true;
    }

    if (entries.size() > 4) {
        if (error) *error = "Un hardware breakpoint ne peut surveiller que 4 adresses simultanées (DR0-DR3).";
        return false;
    }

    QList<killcore::BreakpointFreezeConfig> configs;
    for (const auto& entry : entries) {
        killcore::BreakpointFreezeConfig config;
        config.address = entry.address;
        config.frozenValue = entry.value;
        config.mode = mode;
        switch (entry.value.size()) {
            case 1: config.size = killcore::BreakpointSize::Byte; break;
            case 2: config.size = killcore::BreakpointSize::Word; break;
            case 4: config.size = killcore::BreakpointSize::DWord; break;
            case 8: config.size = killcore::BreakpointSize::QWord; break;
            default:
                if (error) *error = "Taille de valeur incompatible avec un hardware breakpoint.";
                return false;
        }
        configs.append(config);
    }

    if (!m_breakpointFreeze) {
        m_breakpointFreeze = std::make_unique<killcore::BreakpointFreezeManager>();
    }

    if (!m_breakpointFreeze->startMulti(static_cast<uint32_t>(m_pid), configs)) {
        if (error) *error = "Impossible d'activer la session hardware breakpoint.";
        return false;
    }

    return true;
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
    m_lastBatchStartIndex = m_writeHistory.size();

    for (const auto& target : m_lastAutoWriteTargets) {
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

    m_lastBatchEndIndex = m_writeHistory.size();
    if (m_lastBatchEndIndex == m_lastBatchStartIndex) {
        m_lastBatchStartIndex = -1;
        m_lastBatchEndIndex = -1;
    }
    if (allWritesOk && !m_lastAutoWriteTargets.isEmpty()) {
        m_smartSearchActive = false;
        m_chatMemoryTargets = m_lastAutoWriteTargets;
        resetFailureEscalationState();
        appendDistinctText(&m_autoWriteValueHistory, value, 12);
    }

    QVariantMap actionResult;
    actionResult["success"] = allWritesOk;
    actionResult["remaining"] = static_cast<qulonglong>(m_lastAutoWriteTargets.size());
    actionResult["error"] = allWritesOk ? QString() : QString("Au moins une réécriture a échoué.");

    result["actionResult"] = actionResult;
    result["suggestedWrites"] = suggestions;
    result["suggestedWrite"] = suggestions.isEmpty() ? QVariantMap{} : suggestions.first().toMap();
    result["autoWriteResults"] = writeResults;
    result["autoWriteResult"] = writeResults.isEmpty() ? QVariantMap{} : writeResults.last().toMap();
    result["autoWriteCount"] = writeResults.size();
    result["activeTargetCount"] = m_chatMemoryTargets.size();
    result["previousTargetValue"] = previousTargetValue;
    result["writeHistory"] = writeHistoryToVariantList(m_autoWriteValueHistory);
    result["rollbackNote"] = "Tu peux annuler cette réécriture via le bouton rollback batch dans l'assistant.";
    result["message"] = allWritesOk
        ? QString("J'ai repris les %1 dernière(s) adresse(s) auto-écrite(s) et j'ai mis %2 dessus. Je garde ces adresses actives pour les prochaines modifications.")
              .arg(m_lastAutoWriteTargets.size())
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

    m_chatMemoryTargets.clear();
    m_lastAutoWriteTargets.clear();
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
        for (const auto& target : m_chatMemoryTargets) {
            if (target.address == address) {
                alreadyAdded = true;
                break;
            }
        }
        if (alreadyAdded) {
            continue;
        }

        const AutoWriteTarget target{address, killcore::ValueType::Int32};
        m_chatMemoryTargets.append(target);
        m_lastAutoWriteTargets.append(target);

        QVariantMap suggestion;
        suggestion["source"] = "chat_address";
        suggestion["address"] = QString::number(address, 16);
        suggestion["type"] = killcore::valueTypeToString(target.type);
        suggestions.append(suggestion);
    }

    result["success"] = !m_chatMemoryTargets.isEmpty();
    result["targetCount"] = m_chatMemoryTargets.size();
    result["suggestedWrites"] = suggestions;
    result["message"] = m_chatMemoryTargets.isEmpty()
        ? QString("Je n'ai pas reconnu d'adresse mémoire valide dans ton message.")
        : QString("J'ai sélectionné %1 adresse(s) mémoire depuis ton message. Donne-moi maintenant la valeur à écrire dessus.")
              .arg(m_chatMemoryTargets.size());
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
    m_lastBatchStartIndex = m_writeHistory.size();
    m_lastAutoWriteTargets.clear();

    for (const auto& target : m_chatMemoryTargets) {
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
            m_lastAutoWriteTargets.append(target);
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
        if (m_autoWriteValueHistory.isEmpty() && !previousTargetValue.isEmpty()) {
            appendDistinctText(&m_autoWriteValueHistory, previousTargetValue, 12);
        }
        appendDistinctText(&m_autoWriteValueHistory, value, 12);
    }

    QVariantMap actionResult;
    actionResult["success"] = allWritesOk;
    actionResult["remaining"] = static_cast<qulonglong>(m_chatMemoryTargets.size());
    actionResult["error"] = allWritesOk ? QString() : QString("Au moins une écriture sur adresse donnée a échoué.");

    result["success"] = allWritesOk;
    result["actionResult"] = actionResult;
    result["suggestedWrites"] = suggestions;
    result["suggestedWrite"] = suggestions.isEmpty() ? QVariantMap{} : suggestions.first().toMap();
    result["autoWriteResults"] = writeResults;
    result["autoWriteResult"] = writeResults.isEmpty() ? QVariantMap{} : writeResults.last().toMap();
    result["autoWriteCount"] = writeResults.size();
    result["activeTargetCount"] = m_chatMemoryTargets.size();
    result["previousTargetValue"] = previousTargetValue;
    result["writeHistory"] = writeHistoryToVariantList(m_autoWriteValueHistory);
    result["rollbackNote"] = "Tu peux annuler cette écriture via le bouton rollback batch dans l'assistant.";
    result["message"] = allWritesOk
        ? QString("J'ai écrit %1 sur %2 adresse(s) mémoire sélectionnée(s) dans la conversation. Je garde ces adresses actives pour les prochaines modifications.")
              .arg(value)
              .arg(m_chatMemoryTargets.size())
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
    const auto candidateCount = static_cast<qulonglong>(m_candidates.size());
    const bool hadUndo = m_hasPreviousCandidates;
    const bool hadSnapshot = !m_snapshot.isEmpty();
    const bool wasSmartSearchActive = m_smartSearchActive;

    m_candidates.clear();
    clearCandidateUndo();
    clearCandidateValueHistory();
    m_snapshot.clear();
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
    const auto orphan = scanKillengineTemporaryFiles();
    const qulonglong candidateBytes = static_cast<qulonglong>(m_candidates.storageBytes());
    const qulonglong undoBytes = m_hasPreviousCandidates
        ? static_cast<qulonglong>(m_previousCandidates.storageBytes())
        : 0;
    const qulonglong snapshotBytes = static_cast<qulonglong>(m_snapshot.compressedBytesCaptured());
    const bool hasCandidateFile = m_candidates.isFileBacked();
    const bool hasUndoFile = m_hasPreviousCandidates && m_previousCandidates.isFileBacked();
    const bool hasSnapshotFile = m_snapshot.usesMappedStorage();
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

QVariantMap ApplicationController::clearTemporaryStorage() {
    QVariantMap result;
    if (m_activeScanCancellation) {
        result["success"] = false;
        result["error"] = "Un scan est actif : annule ou attends la fin avant de nettoyer le temporaire.";
        return result;
    }

    const auto before = getTemporaryStorageStatus();
    const qulonglong clearedCandidates = static_cast<qulonglong>(m_candidates.size());
    const bool hadUndo = m_hasPreviousCandidates;
    const bool hadSnapshot = !m_snapshot.isEmpty();

    m_candidates.clear();
    clearCandidateUndo();
    clearCandidateValueHistory();
    m_snapshot.clear();
    m_smartSearchActive = false;
    m_smartSearchInitialValue.clear();
    m_smartSearchTargetValue.clear();

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
        ? QString("Stockage temporaire nettoyé : %1 fichier(s), %2 octet(s) supprimé(s).")
              .arg(removedFiles.size())
              .arg(removedBytes)
        : QString("Nettoyage partiel : %1 fichier(s) supprimé(s), %2 fichier(s) verrouillé(s).")
              .arg(removedFiles.size())
              .arg(failedFiles.size());
    appendSmartSearchDebug("temporary_storage_cleared", result);
    return result;
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
    result["candidateCount"] = static_cast<qulonglong>(m_candidates.size());
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
    result["candidateCount"] = static_cast<qulonglong>(m_candidates.size());
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

    if (!m_handle.isValid()) {
        recommendations.append(QVariantMap{{"id", "attach_process"}, {"label", "Attacher un processus"}, {"safe", true}, {"reason", "Aucun processus actif."}});
    } else if (!m_chatMemoryTargets.isEmpty() || !m_activeProfileTargets.isEmpty()) {
        recommendations.append(QVariantMap{{"id", "guarded_write"}, {"label", "Proposer une écriture confirmée"}, {"safe", false}, {"reason", "Des cibles mémoire sont déjà actives."}});
        recommendations.append(QVariantMap{{"id", "guarded_freeze"}, {"label", "Proposer un freeze confirmé"}, {"safe", false}, {"reason", "Des cibles mémoire sont déjà actives."}});
    } else if (m_smartSearchActive && !m_candidates.isEmpty()) {
        recommendations.append(QVariantMap{{"id", "reduce_with_new_value"}, {"label", "Réduire avec la nouvelle valeur observée"}, {"safe", true}, {"reason", "Une recherche guidée contient encore des candidats."}});
        if (m_candidates.size() <= kAutoWriteCandidateLimit) {
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
    int reduceScore = (m_smartSearchActive && !m_candidates.isEmpty()) ? 95 : 0;
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
    if (m_candidates.size() > 50000) {
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
    } else if (m_smartSearchActive && !m_candidates.isEmpty()) {
        const int confidence = m_candidates.size() <= kAutoWriteCandidateLimit ? 90 : 82;
        nextBestAction = proactiveAction(
            m_candidates.size() <= kAutoWriteCandidateLimit ? "review_top_candidates" : "reduce_with_new_value",
            m_candidates.size() <= kAutoWriteCandidateLimit ? "Préparer test d'écriture confirmé" : "Réduire avec nouvelle valeur",
            m_candidates.size() <= kAutoWriteCandidateLimit ? "prepare_guarded_write" : "next_scan",
            confidence,
            m_candidates.size() > kAutoWriteCandidateLimit,
            m_candidates.size() <= kAutoWriteCandidateLimit ? "write" : "safe",
            m_candidates.size() <= kAutoWriteCandidateLimit
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
        .arg(m_candidates.size())
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

            uint64_t address = 0;
            if (!killcore::resolveLocatorAddress(m_handle, target.locator, &address)) {
                continue;
            }

            matchedGroupName = groupName;
            bool alreadyResolved = false;
            for (const auto& existing : resolvedTargets) {
                if (existing.profileName == profileName && existing.targetName == target.name) {
                    alreadyResolved = true;
                    break;
                }
            }
            if (!alreadyResolved) {
                resolvedTargets.append({profileName, target.name, groupName, address, target.type});
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

    for (const auto& target : resolvedTargets) {
        QVariantMap suggestion;
        suggestion["profile"] = target.profileName;
        suggestion["target"] = target.targetName;
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
        writeResult.insert("profile", suggestion.value("profile"));
        writeResult.insert("target", suggestion.value("target"));
        writeResult.insert("address", suggestion.value("address"));
        writeResult.insert("value", value);
        writeResult.insert("type", suggestion.value("type"));
        if (suggestion.contains("valueHistory")) {
            writeResult.insert("valueHistory", suggestion.value("valueHistory"));
        }
        allWritesOk = allWritesOk && writeResult.value("success").toBool();
        writeResults.append(writeResult);

        if (writeResult.value("success").toBool()) {
            m_lastAutoWriteTargets.append({target.address, target.type});
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
    if (allWritesOk && !m_lastAutoWriteTargets.isEmpty()) {
        m_smartSearchActive = false;
        m_chatMemoryTargets = m_lastAutoWriteTargets;
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
    result["activeTargetCount"] = m_chatMemoryTargets.size();
    result["previousTargetValue"] = previousTargetValue;
    result["writeHistory"] = writeHistoryToVariantList(m_autoWriteValueHistory);
    result["rollbackNote"] = "Tu peux annuler cette écriture via le bouton rollback batch dans l'assistant.";
    result["message"] = allWritesOk
        ? QString("J'ai utilisé le profil et j'ai mis %1 sur %2 cible(s) \"%3\". Je garde ces adresses actives pour les prochaines modifications.")
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

    if (m_smartSearchActive && !m_candidates.isEmpty()) {
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
            const QVariantList suggestions = suggestedWritesForCandidates(m_candidates, writeValue, kAutoWriteCandidateLimit);
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
            const QVariantList suggestions = suggestedWritesForCandidates(m_candidates, writeValue, kAutoWriteCandidateLimit);
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
    recovery["candidateStoreSize"] = static_cast<qulonglong>(m_candidates.size());
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
    if (!m_candidates.isEmpty()) {
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
    const SmartSearchIntent intent = classifySmartSearchIntent(
        query,
        numbers,
        chatAddresses,
        !m_chatMemoryTargets.isEmpty(),
        !m_lastAutoWriteTargets.isEmpty(),
        !m_candidates.isEmpty(),
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
        {"candidateCount", static_cast<qulonglong>(m_candidates.size())},
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

    const bool shouldClearSearchContext = intent.resetContext
        && (intent.kind == SmartSearchIntentKind::ResetContext
            || intent.kind == SmartSearchIntentKind::ExactScan
            || intent.kind == SmartSearchIntentKind::GuidedScan);
    if (shouldClearSearchContext) {
        const bool hadCandidates = !m_candidates.isEmpty();
        const int chatCount = m_chatMemoryTargets.size();
        const int profileCount = m_activeProfileTargets.size();
        m_smartSearchActive = false;
        m_smartSearchInitialValue.clear();
        m_smartSearchTargetValue.clear();
        m_candidates.clear();
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

    if (intent.kind == SmartSearchIntentKind::AnswerTraceUiFilterPrompt) {
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

    if (intent.kind == SmartSearchIntentKind::ActivateMemoryTargets
        || (intent.kind == SmartSearchIntentKind::WriteMemoryTargets && !chatAddresses.isEmpty())
        || (intent.kind == SmartSearchIntentKind::FreezeMemoryTargets && !chatAddresses.isEmpty())) {
        auto activation = activateChatMemoryTargetsFromQuery(query);
        if (intent.kind == SmartSearchIntentKind::WriteMemoryTargets
            && activation.value("success").toBool()
            && numbers.size() == 1) {
            auto writeTargets = writeChatMemoryTargetsFromQuery(query, numbers.first());
            stampIntent(&writeTargets);
            return writeTargets;
        }
        if (intent.kind == SmartSearchIntentKind::FreezeMemoryTargets
            && activation.value("success").toBool()
            && numbers.size() == 1) {
            auto freezeTargets = freezeChatMemoryTargetsFromQuery(query, numbers.first());
            stampIntent(&freezeTargets);
            return freezeTargets;
        }
        stampIntent(&activation);
        return activation;
    }

    if (intent.kind == SmartSearchIntentKind::WriteMemoryTargets && numbers.size() == 1) {
        auto writeTargets = writeChatMemoryTargetsFromQuery(query, numbers.first());
        stampIntent(&writeTargets);
        return writeTargets;
    }

    if (intent.kind == SmartSearchIntentKind::FreezeMemoryTargets && numbers.size() == 1) {
        auto freezeTargets = freezeChatMemoryTargetsFromQuery(query, numbers.first());
        stampIntent(&freezeTargets);
        return freezeTargets;
    }

    if (intent.kind == SmartSearchIntentKind::RewriteLastTargets && numbers.size() == 1) {
        auto rewriteTargets = rewriteLastAutoWriteTargets(numbers.first(), query);
        stampIntent(&rewriteTargets);
        return rewriteTargets;
    }

    if (intent.kind == SmartSearchIntentKind::WriteProfileTargets && numbers.size() == 1) {
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
    } else if (intent.kind == SmartSearchIntentKind::ExactScan && numbers.size() == 1) {
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
    } else if (intent.kind == SmartSearchIntentKind::RefineScan && numbers.size() == 1) {
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
    } else if (intent.kind == SmartSearchIntentKind::GuidedScan && numbers.size() >= 2) {
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
        aiContext["candidateCount"] = static_cast<qulonglong>(m_candidates.size());
        aiContext["initialValue"] = m_smartSearchInitialValue;
        aiContext["targetValue"] = m_smartSearchTargetValue;
        aiContext["activeTargetCount"] = static_cast<qulonglong>(m_chatMemoryTargets.size());
        aiContext["unknownSnapshotActive"] = !m_snapshot.isEmpty();
        aiContext["freezeCount"] = static_cast<qulonglong>(m_freeze.entries().size());
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
        auto suggestions = suggestedWritesForCandidates(m_candidates, checkpointValue, kAutoWriteCandidateLimit);
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
    } else if (tool == "write_value" || tool == "freeze_value") {
        result["actionStatus"] = "requires_confirmation";
        result["requiresConfirmation"] = true;
        result["confirmationReason"] = "Cette action modifie la mémoire. Utilise l'onglet Mémoire pour confirmer manuellement.";
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
            {"label", QString("Confirmer : écrire %1 via kernel").arg(kernelValue)},
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
            {"label", isOff ? "Confirmer : désactiver le speedhack" : QString("Confirmer : appliquer %1x").arg(factor)},
            {"factor", factor},
            {"mode", isOff ? "off" : "set"},
            {"requiresConfirmation", true},
        });
        result["recoveryActions"] = recoveryActions;
        stampIntent(&result);
        return result;
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
                m_candidates,
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
    const QString autoRiskMode = settings.value("ai/autoRiskMode", "Safe").toString();
    result["autoRiskMode"] =
        (autoRiskMode == "Expert" || autoRiskMode == "Trainer") ? autoRiskMode : QString("Safe");
    result["modelPath"] = settings.value("ai/modelPath", "").toString();
    result["modelEnabled"] = settings.value("ai/modelEnabled", true).toBool();
    result["modelThreads"] = boundedSettingInt(settings, "ai/modelThreads", 4, 1, 32);
    return result;
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

QString ApplicationController::clrInspectorPipeName() const {
    return QStringLiteral("KillEngineClrInspectorPipe_%1").arg(QCoreApplication::applicationPid());
}

QString ApplicationController::findClrInspectorExecutable() const {
    const QDir appDir(QCoreApplication::applicationDirPath());
    const QStringList candidates = {
        appDir.filePath("KillEngineClrInspector.exe"),
        appDir.filePath("tools/clr_inspector/KillEngineClrInspector.exe"),
        appDir.filePath("../../tools/clr_inspector/KillEngineClrInspector/bin/Release/net8.0/KillEngineClrInspector.exe"),
        appDir.filePath("../../tools/clr_inspector/KillEngineClrInspector/bin/Debug/net8.0/KillEngineClrInspector.exe"),
        QDir::current().filePath("tools/clr_inspector/KillEngineClrInspector/bin/Release/net8.0/KillEngineClrInspector.exe"),
        QDir::current().filePath("tools/clr_inspector/KillEngineClrInspector/bin/Debug/net8.0/KillEngineClrInspector.exe"),
    };
    for (const auto& candidate : candidates) {
        const QFileInfo file(candidate);
        if (file.exists() && file.isFile()) {
            return file.absoluteFilePath();
        }
    }
    return {};
}

bool ApplicationController::ensureClrInspectorStarted(QString* error) {
    if (m_clrInspectorProcess && m_clrInspectorProcess->state() != QProcess::NotRunning) {
        return true;
    }

    const QString executable = findClrInspectorExecutable();
    if (executable.isEmpty()) {
        if (error) {
            *error = QStringLiteral(
                "KillEngineClrInspector.exe introuvable. Construis le helper avec "
                "scripts/build-clr-inspector.ps1 -Configuration Release avant d'utiliser l'inspecteur CLR.");
        }
        return false;
    }

    m_clrInspectorProcess = std::make_unique<QProcess>();
    m_clrInspectorProcess->setProgram(executable);
    m_clrInspectorProcess->setWorkingDirectory(QFileInfo(executable).absolutePath());
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("KILLENGINE_CLR_INSPECTOR_PIPE_NAME"), clrInspectorPipeName());
    m_clrInspectorProcess->setProcessEnvironment(env);
    m_clrInspectorProcess->start();
    if (!m_clrInspectorProcess->waitForStarted(3000)) {
        if (error) {
            *error = QStringLiteral("Impossible de demarrer KillEngineClrInspector.exe : %1")
                .arg(m_clrInspectorProcess->errorString());
        }
        m_clrInspectorProcess.reset();
        return false;
    }

    KE_LOG_INFO() << "ClrInspector: started helper " << executable.toStdString()
                  << " pipe=" << clrInspectorPipeName().toStdString();
    return true;
}

QVariantMap ApplicationController::callClrInspectorRpc(const QString& method, const QVariantList& params, int timeoutMs) {
    QVariantMap result;
    result["success"] = false;
    result["method"] = method;

    QString startError;
    if (!ensureClrInspectorStarted(&startError)) {
        result["error"] = startError;
        return result;
    }

    QJsonObject request;
    request["id"] = m_clrInspectorRequestId++;
    request["method"] = method;
    request["params"] = QJsonArray::fromVariantList(params);
    QByteArray requestBytes = QJsonDocument(request).toJson(QJsonDocument::Compact);
    requestBytes.append('\n');

    QByteArray responseLine;

#ifdef Q_OS_WIN
    const QString pipePath = QStringLiteral("\\\\.\\pipe\\%1").arg(clrInspectorPipeName());
    const std::wstring pipePathW = pipePath.toStdWString();
    QElapsedTimer connectTimer;
    connectTimer.start();
    HANDLE pipe = INVALID_HANDLE_VALUE;
    while (connectTimer.elapsed() < timeoutMs) {
        pipe = CreateFileW(
            pipePathW.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            0,
            nullptr,
            OPEN_EXISTING,
            0,
            nullptr);
        if (pipe != INVALID_HANDLE_VALUE) {
            break;
        }
        const DWORD err = GetLastError();
        if (err != ERROR_PIPE_BUSY && err != ERROR_FILE_NOT_FOUND) {
            result["error"] = QStringLiteral("Ouverture du pipe ClrMD echouee (error=%1).").arg(err);
            return result;
        }
        const int remainingMs = timeoutMs - static_cast<int>(connectTimer.elapsed());
        if (remainingMs <= 0) {
            break;
        }
        WaitNamedPipeW(pipePathW.c_str(), static_cast<DWORD>(std::min(250, remainingMs)));
    }

    if (pipe == INVALID_HANDLE_VALUE) {
        result["error"] = QStringLiteral("Pipe ClrMD indisponible : timeout sur %1").arg(pipePath);
        return result;
    }

    DWORD written = 0;
    if (!WriteFile(pipe, requestBytes.constData(), static_cast<DWORD>(requestBytes.size()), &written, nullptr)
        || written != static_cast<DWORD>(requestBytes.size())) {
        const DWORD err = GetLastError();
        CloseHandle(pipe);
        result["error"] = QStringLiteral("Ecriture vers le pipe ClrMD echouee (error=%1).").arg(err);
        return result;
    }

    char buffer[512];
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        DWORD bytesRead = 0;
        if (ReadFile(pipe, buffer, sizeof(buffer), &bytesRead, nullptr)) {
            responseLine.append(buffer, static_cast<qsizetype>(bytesRead));
            const qsizetype newline = responseLine.indexOf('\n');
            if (newline >= 0) {
                responseLine = responseLine.left(newline).trimmed();
                break;
            }
            if (bytesRead == 0) {
                break;
            }
            continue;
        }
        const DWORD err = GetLastError();
        if (err == ERROR_MORE_DATA) {
            responseLine.append(buffer, static_cast<qsizetype>(bytesRead));
            continue;
        }
        if (err == ERROR_BROKEN_PIPE) {
            break;
        }
        CloseHandle(pipe);
        result["error"] = QStringLiteral("Lecture du pipe ClrMD echouee (error=%1).").arg(err);
        return result;
    }
    CloseHandle(pipe);
#else
    result["error"] = QStringLiteral("Inspecteur CLR disponible uniquement sur Windows pour l'instant.");
    return result;
#endif

    if (responseLine.contains('\n')) {
        responseLine = responseLine.left(responseLine.indexOf('\n')).trimmed();
    } else {
        responseLine = responseLine.trimmed();
    }

    if (responseLine.isEmpty()) {
        result["error"] = QStringLiteral("Pas de reponse du helper ClrMD.");
        return result;
    }

    QJsonParseError parseError;
    const QJsonDocument responseDoc = QJsonDocument::fromJson(responseLine, &parseError);
    if (parseError.error != QJsonParseError::NoError || !responseDoc.isObject()) {
        result["error"] = QStringLiteral("Reponse ClrMD JSON invalide : %1").arg(parseError.errorString());
        result["raw"] = QString::fromUtf8(responseLine);
        return result;
    }

    const QJsonObject response = responseDoc.object();
    if (response.contains("error")) {
        result["error"] = response.value("error").toString();
        return result;
    }

    result["success"] = true;
    result["result"] = response.value("result").toVariant();
    return result;
}

QVariantMap ApplicationController::getClrInspectorStatus() const {
    QVariantMap result;
    result["success"] = true;
    result["available"] = !findClrInspectorExecutable().isEmpty();
    result["helperPath"] = findClrInspectorExecutable();
    result["pipeName"] = clrInspectorPipeName();
    result["running"] = m_clrInspectorProcess && m_clrInspectorProcess->state() != QProcess::NotRunning;
    result["attachedProcess"] = m_attached;
    result["pid"] = m_pid;
    result["processName"] = m_processName;
    return result;
}

QVariantMap ApplicationController::attachClrInspector() {
    if (!m_attached || m_pid <= 0) {
        return {{"success", false}, {"error", QStringLiteral("Aucun processus attache.")}};
    }
    QVariantMap response = callClrInspectorRpc(QStringLiteral("attach"), {m_pid}, 10000);
    appendScanTelemetry(QStringLiteral("clr_inspector_attach"), {
        {"success", response.value("success").toBool()},
        {"pid", m_pid},
        {"processName", m_processName},
        {"error", response.value("error").toString()},
    });
    return response;
}

QVariantMap ApplicationController::detachClrInspector() {
    if (!m_clrInspectorProcess || m_clrInspectorProcess->state() == QProcess::NotRunning) {
        return {{"success", true}, {"result", QStringLiteral("not running")}};
    }
    QVariantMap response = callClrInspectorRpc(QStringLiteral("detach"), {}, 3000);
    appendScanTelemetry(QStringLiteral("clr_inspector_detach"), {
        {"success", response.value("success").toBool()},
        {"error", response.value("error").toString()},
    });
    return response;
}

QVariantMap ApplicationController::shutdownClrInspector() {
    if (!m_clrInspectorProcess || m_clrInspectorProcess->state() == QProcess::NotRunning) {
        m_clrInspectorProcess.reset();
        return {{"success", true}, {"result", QStringLiteral("not running")}};
    }
    QVariantMap response = callClrInspectorRpc(QStringLiteral("shutdown"), {}, 2000);
    if (m_clrInspectorProcess && m_clrInspectorProcess->state() != QProcess::NotRunning) {
        m_clrInspectorProcess->waitForFinished(1000);
        if (m_clrInspectorProcess->state() != QProcess::NotRunning) {
            m_clrInspectorProcess->terminate();
            m_clrInspectorProcess->waitForFinished(1000);
        }
    }
    m_clrInspectorProcess.reset();
    return response;
}

QVariantMap ApplicationController::flushClrInspectorCache() {
    return callClrInspectorRpc(QStringLiteral("flushCachedData"), {}, 5000);
}

QVariantMap ApplicationController::findClrObjectsByType(const QString& typeSubstring) {
    const QString filter = typeSubstring.trimmed().isEmpty()
        ? QStringLiteral("KillEngine.ClrTestTarget")
        : typeSubstring.trimmed();
    return callClrInspectorRpc(QStringLiteral("findObjectsByType"), {filter}, 15000);
}

QVariantMap ApplicationController::readClrObject(const QString& addressHex) {
    const QString address = addressHex.trimmed();
    if (address.isEmpty()) {
        return {{"success", false}, {"error", QStringLiteral("Adresse objet CLR manquante.")}};
    }
    return callClrInspectorRpc(QStringLiteral("readObject"), {address}, 10000);
}

QVariantMap ApplicationController::enumerateClrRoots(const QString& typeSubstring) {
    const QString filter = typeSubstring.trimmed();
    QVariantList params;
    if (!filter.isEmpty()) {
        params.append(filter);
    }
    return callClrInspectorRpc(QStringLiteral("enumerateRoots"), params, 15000);
}

QVariantMap ApplicationController::probeKernelDriver() const {
    const killcore::KernelDriverBridge bridge;
    const auto probe = bridge.probe();

    QVariantMap capabilities;
    capabilities["protocolVersion"] = static_cast<int>(probe.capabilities.protocolVersion);
    capabilities["healthProbe"] = probe.capabilities.healthProbe;
    capabilities["processMemoryAccess"] = probe.capabilities.processMemoryAccess;
    capabilities["privilegedInstrumentation"] = probe.capabilities.privilegedInstrumentation;

    QVariantMap result;
    result["success"] = probe.status == killcore::KernelDriverProbeStatus::Connected;
    result["status"] = killcore::KernelDriverBridge::statusToString(probe.status);
    result["devicePath"] = probe.devicePath;
    result["message"] = probe.message;
    result["capabilities"] = capabilities;

    KE_LOG_INFO() << "probeKernelDriver: status=" << result.value("status").toString().toStdString()
                  << " message=" << probe.message.toStdString();
    return result;
}

QVariantMap ApplicationController::readMemoryKernel(const QString& addressHex, int size) const {
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

    const int boundedSize = std::clamp(size, 1, 4096);

#ifdef Q_OS_WIN
    const killcore::KernelDriverBridge bridge;
    const auto targetPid = reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(m_handle.pid()));
    const QByteArray data = bridge.readMemory(targetPid, address, static_cast<size_t>(boundedSize));
    if (data.isEmpty()) {
        result["error"] = "Lecture kernel échouée (driver non chargé/non connecté, adresse invalide côté cible, ou accès refusé).";
        KE_LOG_WARN() << "readMemoryKernel: échec pid=" << m_handle.pid() << " address=0x" << QString::number(address, 16).toStdString();
        return result;
    }
    result["success"] = true;
    result["bytesRead"] = data.size();
    result["hex"] = QString::fromLatin1(data.toHex(' ').toUpper());
    KE_LOG_INFO() << "readMemoryKernel: pid=" << m_handle.pid() << " address=0x" << QString::number(address, 16).toStdString()
                  << " bytesRead=" << data.size();
#else
    result["error"] = "Fonctionnalité Windows uniquement.";
#endif
    return result;
}

QVariantMap ApplicationController::writeMemoryKernel(const QString& addressHex, const QString& hexBytes) {
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

    QString hexOnly = hexBytes;
    hexOnly.remove(' ');
    const QByteArray data = QByteArray::fromHex(hexOnly.toLatin1());
    if (data.isEmpty()) {
        result["error"] = "Octets invalides (format hexadécimal attendu, ex: \"90 90 90\").";
        return result;
    }

#ifdef Q_OS_WIN
    const killcore::KernelDriverBridge bridge;
    const auto targetPid = reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(m_handle.pid()));
    const bool written = bridge.writeMemory(targetPid, address, data);
    result["success"] = written;
    if (written) {
        result["bytesWritten"] = data.size();
        KE_LOG_INFO() << "writeMemoryKernel: pid=" << m_handle.pid() << " address=0x" << QString::number(address, 16).toStdString()
                      << " bytesWritten=" << data.size();
    } else {
        result["error"] = "Écriture kernel échouée (driver non chargé/non connecté, adresse invalide côté cible, ou accès refusé).";
        KE_LOG_WARN() << "writeMemoryKernel: échec pid=" << m_handle.pid() << " address=0x" << QString::number(address, 16).toStdString();
    }
#else
    result["error"] = "Fonctionnalité Windows uniquement.";
#endif
    return result;
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
        registerWriteWatch(address, type, data);
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
    QSettings settings;
    QVariantMap result;
    QVariantList modelCandidates;
    QVariantList executableCandidates;
    QVariantList embeddedAgents;
    QVariantList embeddedModelFolders;

    const QString configuredModel = settings.value("ai/modelPath", "").toString().trimmed();
    const bool modelEnabled = settings.value("ai/modelEnabled", true).toBool();
    const QString envModel = QProcessEnvironment::systemEnvironment().value("KILLENGINE_QWEN_GGUF").trimmed();
    const QString envExe = QProcessEnvironment::systemEnvironment().value("KILLENGINE_LLAMA_CLI").trimmed();
    const auto model = killai::ModelLocator::findQwenGguf();

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
                agent["error"] = QString("Manifest illisible.");
                embeddedAgents.append(agent);
                continue;
            }

            const QJsonDocument doc = QJsonDocument::fromJson(manifestFile.readAll());
            if (!doc.isObject()) {
                agent["error"] = QString("Manifest JSON invalide.");
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
    result["backend"] = modelEnabled && model.found && !executablePath.isEmpty() ? QString("llama.cpp") : QString("deterministic");
    result["ready"] = modelEnabled && model.found && !executablePath.isEmpty();
    result["available"] = model.found && !executablePath.isEmpty();
    result["configuredModelPath"] = configuredModel;
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
    result["message"] = result.value("ready").toBool()
        ? QString("IA embarquée prête.")
        : QString("IA embarquée indisponible: modèle ou runtime manquant.");
    return result;
}

QVariantMap ApplicationController::browseForModelFile() {
    QVariantMap result;
    result["success"] = false;

    const QString path = QFileDialog::getOpenFileName(
        nullptr,
        "Choisir un modèle GGUF",
        QString(),
        "Modèles GGUF (*.gguf);;Tous les fichiers (*.*)");

    if (path.isEmpty()) {
        result["cancelled"] = true;
        return result;
    }

    result["success"] = true;
    result["path"] = path;
    return result;
}

QVariantMap ApplicationController::saveSettings(const QVariantMap& incoming) {
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
    const QString autoRiskMode = incoming.value("autoRiskMode", "Safe").toString();
    settings.setValue(
        "ai/autoRiskMode",
        (autoRiskMode == "Expert" || autoRiskMode == "Trainer") ? autoRiskMode : QString("Safe"));
    settings.setValue("ai/modelPath", incoming.value("modelPath", "").toString().trimmed());
    settings.setValue("ai/modelEnabled", incoming.value("modelEnabled", true).toBool());
    settings.setValue(
        "ai/modelThreads",
        std::clamp(incoming.value("modelThreads", 4).toInt(), 1, 32));
    settings.sync();
    const size_t candidateThreshold = candidateFileBackedThresholdFromSettings();
    m_candidates.setFileBackedThreshold(candidateThreshold);
    m_previousCandidates.setFileBackedThreshold(candidateThreshold);

    QVariantMap result = getSettings();
    result["success"] = true;
    return result;
}

QString ApplicationController::getLogFilePath() const {
    return killcore::Logger::instance().logFilePath();
}

QString ApplicationController::getSmartSearchDebugFilePath() const {
    return smartSearchDebugFilePath();
}

QString ApplicationController::getScanTelemetryFilePath() const {
    return scanTelemetryFilePath();
}

QVariantMap ApplicationController::getSmartSearchDebugEvents(int maxEvents) const {
    QVariantMap result;
    QVariantList events;
    result["success"] = false;
    result["path"] = smartSearchDebugFilePath();
    result["events"] = events;

    QSettings settings;
    if (maxEvents <= 0) {
        maxEvents = boundedSettingInt(settings, "diagnostics/smartSearchDebugMaxEvents", 30, 5, 200);
    }
    maxEvents = std::clamp(maxEvents, 1, 200);

    QFile file(smartSearchDebugFilePath());
    if (!file.exists()) {
        result["success"] = true;
        result["error"] = "";
        return result;
    }

    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        result["error"] = "Impossible de lire le fichier debug Smart Search.";
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

QVariantMap ApplicationController::clearSmartSearchDebugEvents() {
    QVariantMap result;
    result["success"] = false;
    result["path"] = smartSearchDebugFilePath();

    QFile file(smartSearchDebugFilePath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        result["error"] = "Impossible de vider le fichier debug Smart Search.";
        return result;
    }

    result["success"] = true;
    result["error"] = "";
    return result;
}

QVariantMap ApplicationController::getLogTail(int maxLines) const {
    QVariantMap result;
    QVariantList lines;
    result["success"] = false;
    result["path"] = getLogFilePath();
    result["lines"] = lines;

    maxLines = std::clamp(maxLines, 1, 1000);

    QFile file(getLogFilePath());
    if (!file.exists()) {
        result["success"] = true;
        result["error"] = "";
        return result;
    }
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        result["error"] = "Impossible de lire le fichier log.";
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

QVariantMap ApplicationController::exportDiagnostics() {
    QVariantMap result;
    result["success"] = false;

    const QString exportDir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation).isEmpty()
        ? QDir::currentPath()
        : QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    const QString timestamp = QDateTime::currentDateTime().toString("yyyy-MM-dd_HH-mm-ss");
    const QString exportPath = QDir(exportDir).filePath("KillEngine-diagnostics-" + timestamp + ".kezdiag");

    QVariantMap manifest;
    manifest["createdAt"] = QDateTime::currentDateTime().toString(Qt::ISODateWithMs);
    manifest["version"] = getVersion();
    manifest["pid"] = m_pid;
    manifest["processName"] = m_processName;
    manifest["attached"] = m_attached;
    manifest["candidateCount"] = static_cast<qulonglong>(m_candidates.size());
    manifest["candidateStoreFileBacked"] = m_candidates.isFileBacked();
    manifest["candidateStorePath"] = m_candidates.backingFilePath();
    manifest["candidateStoreBytes"] = static_cast<qulonglong>(m_candidates.storageBytes());
    manifest["candidateStoreMemoryBytes"] = static_cast<qulonglong>(m_candidates.estimatedMemoryBytes());
    manifest["logFilePath"] = getLogFilePath();
    manifest["smartSearchDebugFilePath"] = smartSearchDebugFilePath();
    manifest["scanTelemetryFilePath"] = scanTelemetryFilePath();
    manifest["crashDirectory"] = CrashHandler::crashDirectory();
    manifest["settings"] = getSettings();

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

    QFile logFile(getLogFilePath());
    if (logFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        appendSection(QFileInfo(logFile).fileName(), logFile.readAll());
    }

    QFile debugFile(smartSearchDebugFilePath());
    if (debugFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        appendSection(QFileInfo(debugFile).fileName(), debugFile.readAll());
    }

    QFile scanTelemetryFile(scanTelemetryFilePath());
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
        result["error"] = "Impossible de créer le fichier diagnostic.";
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
        result["openFolderError"] = "Le dossier de l'export n'a pas pu être ouvert automatiquement. Chemin : " + folderPath;
    } else {
        result["openFolderError"] = "";
    }

    return result;
}

QString ApplicationController::smartSearchDebugFilePath() const {
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (dir.isEmpty()) {
        dir = QDir::currentPath();
    }
    dir += "/logs";
    QDir().mkpath(dir);
    return dir + "/smart_search_debug.jsonl";
}

QString ApplicationController::scanTelemetryFilePath() const {
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (dir.isEmpty()) {
        dir = QDir::currentPath();
    }
    dir += "/logs";
    QDir().mkpath(dir);
    return dir + "/scan_telemetry.jsonl";
}

void ApplicationController::appendSmartSearchDebug(const QString& event, const QVariantMap& payload) const {
    QSettings settings;
    if (!settings.value("diagnostics/smartSearchDebugEnabled", true).toBool()) {
        return;
    }

    QVariantMap entry = payload;
    entry["event"] = event;
    entry["timestamp"] = QDateTime::currentDateTime().toString(Qt::ISODateWithMs);
    entry["pid"] = m_pid;
    entry["processName"] = m_processName;

    const QString path = smartSearchDebugFilePath();
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

void ApplicationController::appendScanTelemetry(const QString& event, const QVariantMap& payload) const {
    QSettings settings;

    QVariantMap entry = payload;
    entry["event"] = event;
    entry["timestamp"] = QDateTime::currentDateTime().toString(Qt::ISODateWithMs);
    entry["pid"] = m_pid;
    entry["processName"] = m_processName;
    entry["performanceMode"] = settings.value("scan/performanceMode", "Auto").toString();
    entry["scanChunkSizeMb"] = boundedSettingInt(settings, "scan/chunkSizeMb", kDefaultScanChunkSizeMb, 0, 64);
    entry["scanMaxWorkerThreads"] = boundedSettingInt(settings, "scan/maxWorkerThreads", kDefaultScanMaxWorkerThreads, 0, 128);
    entry["scanMaxInFlightMb"] = boundedSettingInt(settings, "scan/maxInFlightMb", kDefaultScanMaxInFlightMb, 0, 32768);
    entry["scanMaxResults"] = boundedSettingInt(settings, "scan/maxResults", kDefaultScanMaxResults, 1000, 10000000);
    entry["fastScan"] = settings.value("scan/fastScan", true).toBool();

    const QString path = scanTelemetryFilePath();
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

// ---------------------------------------------------------------------------
// Phase 11 — Profils
// ---------------------------------------------------------------------------

QVariantMap ApplicationController::saveProfileTarget(
    const QString& profileName,
    const QString& targetName,
    const QString& addressHex,
    const QString& valueType,
    const QString& description) {

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

    // Trouve le module qui contient l'adresse pour créer le locator
    const auto modules = killcore::ProcessEnumerator::enumerateModules(m_pid);
    QString bestModule;
    uint64_t bestBase = 0;
    uint64_t bestSize = 0;

    for (const auto& mod : modules) {
        if (address >= mod.baseAddress && address < mod.baseAddress + mod.size) {
            // Choisit le module le plus petit contenant l'adresse (plus précis)
            if (bestModule.isEmpty() || mod.size < bestSize) {
                bestModule = mod.name;
                bestBase = mod.baseAddress;
                bestSize = mod.size;
            }
        }
    }

    killcore::Locator locator;
    if (!bestModule.isEmpty()) {
        locator.kind = killcore::LocatorKind::ModuleOffset;
        locator.module = bestModule;
        locator.offset = address - bestBase;
        locator.lastAddress = address;
    } else {
        // Fallback : adresse absolue (non stable)
        locator.kind = killcore::LocatorKind::Absolute;
        locator.lastAddress = address;
    }

    // Charge le profil existant ou crée un nouveau
    killcore::Profile profile;
    const QString path = killcore::ProfileStore::profilePath(profileName);
    if (QFile::exists(path)) {
        killcore::ProfileStore::load(path, &profile);
    } else {
        profile.gameName = profileName;
        profile.executableName = m_processName;
    }
    if (profile.executableHash.isEmpty()) {
        profile.executableHash = computeExecutableHash(m_handle.executablePath());
    }

    // Ajoute ou met à jour la cible
    bool found = false;
    for (auto& target : profile.targets) {
        if (target.name == targetName) {
            target.type = type;
            target.locator = locator;
            target.description = description;
            found = true;
            break;
        }
    }
    if (!found) {
        profile.targets.append({targetName, type, locator, description});
    }

    const bool saved = killcore::ProfileStore::save(profile, path);
    result["success"] = saved;
    result["profileName"] = profileName;
    result["targetName"] = targetName;
    result["locator"] = locator.toString();
    result["locatorKind"] = (locator.kind == killcore::LocatorKind::ModuleOffset) ? "module_offset" : "absolute";
    result["module"] = locator.module;
    result["offset"] = QString::number(locator.offset, 16);
    result["targetCount"] = profile.targets.size();
    if (!saved) {
        result["error"] = "Échec de la sauvegarde du profil.";
    }

    return result;
}

QVariantList ApplicationController::listProfiles() {
    QVariantList result;
    const auto names = killcore::ProfileStore::listProfiles();
    for (const auto& name : names) {
        QVariantMap entry;
        entry["name"] = name;

        // Charge les infos de base
        killcore::Profile profile;
        if (killcore::ProfileStore::load(killcore::ProfileStore::profilePath(name), &profile)) {
            entry["gameName"] = profile.gameName;
            entry["executableName"] = profile.executableName;
            entry["targetCount"] = profile.targets.size();
            entry["patchCount"] = profile.patches.size();
        }
        result.append(entry);
    }
    return result;
}

QVariantMap ApplicationController::loadProfile(const QString& profileName) {
    QVariantMap result;
    result["success"] = false;

    killcore::Profile profile;
    const QString path = killcore::ProfileStore::profilePath(profileName);
    if (!killcore::ProfileStore::load(path, &profile)) {
        result["error"] = "Profil introuvable ou illisible.";
        return result;
    }

    result["success"] = true;
    result["profileName"] = profileName;
    result["gameName"] = profile.gameName;
    result["executableName"] = profile.executableName;

    QVariantList targetsList;
    for (const auto& target : profile.targets) {
        QVariantMap targetEntry;
        targetEntry["name"] = target.name;
        targetEntry["type"] = killcore::valueTypeToString(target.type);
        targetEntry["locator"] = target.locator.toString();
        targetEntry["description"] = target.description;
        targetsList.append(targetEntry);
    }
    result["targets"] = targetsList;
    result["targetCount"] = targetsList.size();

    QVariantList patchesList;
    for (const auto& patch : profile.patches) {
        QVariantMap patchEntry;
        patchEntry["name"] = patch.name;
        patchEntry["module"] = patch.module;
        patchEntry["moduleOffset"] = QString::number(patch.moduleOffset, 16).toUpper();
        patchEntry["aobPattern"] = patch.aobPattern;
        patchEntry["patchBytes"] = patch.patchBytes;
        patchEntry["originalBytes"] = patch.originalBytes;
        patchEntry["disassembly"] = patch.disassembly;
        patchEntry["riskLevel"] = patch.riskLevel;
        patchEntry["description"] = patch.description;
        patchEntry["signatureScore"] = patch.signatureScore;
        patchEntry["signatureLevel"] = patch.signatureLevel;
        patchEntry["signatureWarning"] = patch.signatureWarning;
        patchEntry["signatureFixedBytes"] = patch.signatureFixedBytes;
        patchEntry["signatureWildcardBytes"] = patch.signatureWildcardBytes;
        patchEntry["signatureUniqueFixedBytes"] = patch.signatureUniqueFixedBytes;
        patchEntry["signatureFixedRatio"] = patch.signatureFixedRatio;
        patchEntry["trainerSafe"] = patch.trainerSafe;
        patchEntry["signatureMatches"] = patch.signatureMatches;
        patchesList.append(patchEntry);
    }
    result["patches"] = patchesList;
    result["patchCount"] = patchesList.size();

    QVariantList autoAsmScriptsList;
    for (const auto& script : profile.autoAsmScripts) {
        QVariantMap scriptEntry;
        scriptEntry["name"] = script.name;
        scriptEntry["scriptText"] = script.scriptText;
        scriptEntry["description"] = script.description;
        scriptEntry["riskLevel"] = script.riskLevel;
        autoAsmScriptsList.append(scriptEntry);
    }
    result["autoAsmScripts"] = autoAsmScriptsList;
    result["autoAsmScriptCount"] = autoAsmScriptsList.size();

    return result;
}

bool ApplicationController::deleteProfile(const QString& profileName) {
    return killcore::ProfileStore::remove(profileName);
}

QVariantMap ApplicationController::resolveProfileTarget(const QString& profileName, const QString& targetName) {
    QVariantMap result;
    result["success"] = false;

    if (!m_handle.isValid()) {
        result["error"] = "Aucun processus attaché.";
        return result;
    }

    killcore::Profile profile;
    const QString path = killcore::ProfileStore::profilePath(profileName);
    if (!killcore::ProfileStore::load(path, &profile)) {
        result["error"] = "Profil introuvable.";
        return result;
    }

    for (const auto& target : profile.targets) {
        if (target.name == targetName) {
            uint64_t address = 0;
            if (killcore::resolveLocatorAddress(m_handle, target.locator, &address)) {
                result["success"] = true;
                result["address"] = QString::number(address, 16);
                result["type"] = killcore::valueTypeToString(target.type);
                result["locator"] = target.locator.toString();
                return result;
            } else {
                result["error"] = "Impossible de résoudre le locator. Le module est peut-être absent.";
                return result;
            }
        }
    }

    result["error"] = "Cible introuvable dans le profil.";
    return result;
}

QVariantMap ApplicationController::activateProfileTarget(const QString& profileName, const QString& targetName) {
    QVariantMap result;
    result["success"] = false;

    if (!m_handle.isValid()) {
        result["error"] = "Aucun processus attaché.";
        return result;
    }

    killcore::Profile profile;
    const QString path = killcore::ProfileStore::profilePath(profileName);
    if (!killcore::ProfileStore::load(path, &profile)) {
        result["error"] = "Profil introuvable.";
        return result;
    }

    for (const auto& target : profile.targets) {
        if (target.name == targetName) {
            uint64_t address = 0;
            if (!killcore::resolveLocatorAddress(m_handle, target.locator, &address)) {
                result["error"] = "Impossible d'activer cette cible. Le module est peut-être absent.";
                return result;
            }

            const ActiveProfileTarget active{
                profileName,
                target.name,
                profileTargetGroupName(target.name),
                address,
                target.type,
            };

            bool updated = false;
            for (auto& existing : m_activeProfileTargets) {
                if (existing.profileName == active.profileName && existing.targetName == active.targetName) {
                    existing = active;
                    updated = true;
                    break;
                }
            }
            if (!updated) {
                m_activeProfileTargets.append(active);
            }

            bool autoWriteTargetUpdated = false;
            for (auto& existing : m_lastAutoWriteTargets) {
                if (existing.address == active.address) {
                    existing.type = active.type;
                    autoWriteTargetUpdated = true;
                    break;
                }
            }
            if (!autoWriteTargetUpdated) {
                m_lastAutoWriteTargets.append({active.address, active.type});
            }

            result["success"] = true;
            result["profileName"] = profileName;
            result["targetName"] = target.name;
            result["groupName"] = active.groupName;
            result["address"] = QString::number(address, 16);
            result["type"] = killcore::valueTypeToString(target.type);
            result["activeTargetCount"] = m_activeProfileTargets.size();
            result["message"] = QString("\"%1\" activé pour l'Assistant à l'adresse 0x%2.")
                                    .arg(target.name, QString::number(address, 16));
            return result;
        }
    }

    result["error"] = "Cible introuvable dans le profil.";
    return result;
}

QVariantMap ApplicationController::saveProfileCodePatch(
    const QString& profileName,
    const QString& patchName,
    const QString& addressHex,
    const QString& aobPattern,
    const QString& patchBytes,
    const QVariantMap& metadata) {

    QVariantMap result;
    result["success"] = false;

    if (!m_handle.isValid()) {
        result["error"] = "Aucun processus attaché.";
        return result;
    }

    const QString cleanProfileName = profileName.trimmed();
    const QString cleanPatchName = patchName.trimmed();
    if (cleanProfileName.isEmpty() || cleanPatchName.isEmpty()) {
        result["error"] = "Nom de profil ou de patch vide.";
        return result;
    }

    uint64_t address = 0;
    if (!parseHexAddress(addressHex, &address)) {
        result["error"] = "Adresse patch invalide.";
        return result;
    }

    const auto parsedPattern = killcore::parseAobPattern(aobPattern);
    if (!parsedPattern.isValid()) {
        result["error"] = parsedPattern.error;
        return result;
    }
    const auto patternQuality = killcore::evaluateAobPatternQuality(parsedPattern);
    result["signatureQuality"] = aobPatternQualityToVariantMap(patternQuality);
    result["signatureRisk"] = patternQuality.level;

    const bool allowWeakAob = metadata.value("allowWeakAob", false).toBool();
    if (!allowWeakAob && (patternQuality.fixedBytes < 3 || patternQuality.score < 35)) {
        result["error"] = QString("Signature AOB trop faible pour Trainer (%1/100, %2 octet(s) fixe(s)). Allonge la signature ou ajoute des octets fixes.")
                              .arg(patternQuality.score)
                              .arg(patternQuality.fixedBytes);
        result["signatureWarning"] = patternQuality.warning;
        appendScanTelemetry("trainer_patch_save_blocked", result);
        return result;
    }

    killcore::AobScanOptions uniquenessOptions;
    uniquenessOptions.executableOnly = true;
    uniquenessOptions.imageOnly = true;
    uniquenessOptions.maxResults = 2;
    const auto uniquenessScan = killcore::scanAobPattern(m_handle, parsedPattern, uniquenessOptions);
    result["signatureMatches"] = uniquenessScan.matchesFound;
    if (!uniquenessScan.success || uniquenessScan.matchesFound != 1) {
        result["error"] = uniquenessScan.matchesFound == 0
            ? QString("Signature AOB introuvable dans le code image executable.")
            : QString("Signature AOB non unique (%1 matches). Sauvegarde Trainer bloquée.").arg(uniquenessScan.matchesFound);
        appendScanTelemetry("trainer_patch_save_blocked", result);
        return result;
    }

    const auto parsedPatch = killcore::parsePatchBytes(patchBytes);
    if (!parsedPatch.isValid()) {
        result["error"] = parsedPatch.error;
        return result;
    }

    const auto modules = killcore::ProcessEnumerator::enumerateModules(m_pid);
    QString moduleName;
    uint64_t moduleOffset = 0;
    uint64_t bestSize = 0;
    for (const auto& module : modules) {
        if (address >= module.baseAddress && address < module.baseAddress + module.size) {
            if (moduleName.isEmpty() || module.size < bestSize) {
                moduleName = module.name;
                moduleOffset = address - module.baseAddress;
                bestSize = module.size;
            }
        }
    }

    QString originalBytes = metadata.value("originalBytes").toString().trimmed();
    if (originalBytes.isEmpty()) {
        killcore::MemoryReader reader(m_handle);
        const auto read = reader.readChunked(address, static_cast<size_t>(parsedPatch.bytes.size()), 4096);
        if (read.success || read.partial) {
            originalBytes = QString::fromLatin1(read.data.toHex(' ').toUpper());
        }
    }

    killcore::Profile profile;
    const QString path = killcore::ProfileStore::profilePath(cleanProfileName);
    if (!killcore::ProfileStore::load(path, &profile)) {
        profile.gameName = cleanProfileName;
        profile.executableName = m_processName;
    }
    // Hash calculé une seule fois (a la creation du profil, ou a la
    // migration d'un profil existant qui n'en avait pas encore un) plutot
    // qu'a chaque sauvegarde : sert de reference "version pour laquelle ce
    // profil a ete fait", pas de suivre la derniere version utilisee.
    if (profile.executableHash.isEmpty()) {
        profile.executableHash = computeExecutableHash(m_handle.executablePath());
    }

    killcore::ProfileCodePatch patch;
    patch.name = cleanPatchName;
    patch.module = moduleName;
    patch.moduleOffset = moduleOffset;
    patch.aobPattern = aobPattern.trimmed();
    patch.patchBytes = patchBytes.trimmed();
    patch.originalBytes = originalBytes;
    patch.disassembly = metadata.value("disassembly").toString();
    patch.riskLevel = metadata.value("riskLevel").toString();
    patch.description = metadata.value("description").toString();
    patch.signatureScore = patternQuality.score;
    patch.signatureLevel = patternQuality.level;
    patch.signatureWarning = patternQuality.warning;
    patch.signatureFixedBytes = patternQuality.fixedBytes;
    patch.signatureWildcardBytes = patternQuality.wildcardBytes;
    patch.signatureUniqueFixedBytes = patternQuality.uniqueFixedBytes;
    patch.signatureFixedRatio = patternQuality.fixedRatio;
    patch.trainerSafe = patternQuality.trainerSafe;
    patch.signatureMatches = uniquenessScan.matchesFound;

    bool replaced = false;
    for (auto& existing : profile.patches) {
        if (existing.name == patch.name) {
            existing = patch;
            replaced = true;
            break;
        }
    }
    if (!replaced) {
        profile.patches.append(patch);
    }

    if (!killcore::ProfileStore::save(profile, path)) {
        result["error"] = "Impossible de sauvegarder le profil.";
        return result;
    }

    result["success"] = true;
    result["profileName"] = cleanProfileName;
    result["patchName"] = cleanPatchName;
    result["module"] = moduleName;
    result["moduleOffset"] = QString::number(moduleOffset, 16).toUpper();
    result["patchCount"] = profile.patches.size();
    result["replaced"] = replaced;
    result["signatureMatches"] = uniquenessScan.matchesFound;
    result["signatureWarning"] = patternQuality.warning;
    appendScanTelemetry("trainer_patch_saved", result);
    return result;
}

QVariantMap ApplicationController::applyProfileCodePatch(const QString& profileName, const QString& patchName) {
    QVariantMap result;
    result["success"] = false;
    result["profileName"] = profileName;
    result["patchName"] = patchName;

    if (!m_handle.isValid()) {
        result["error"] = "Aucun processus attaché.";
        return result;
    }

    killcore::Profile profile;
    const QString path = killcore::ProfileStore::profilePath(profileName);
    if (!killcore::ProfileStore::load(path, &profile)) {
        result["error"] = "Profil introuvable.";
        return result;
    }

    std::optional<killcore::ProfileCodePatch> patch;
    for (const auto& item : profile.patches) {
        if (item.name == patchName) {
            patch = item;
            break;
        }
    }
    if (!patch) {
        result["error"] = "Patch introuvable dans le profil.";
        return result;
    }

    // Cause la plus probable d'une signature AOB qui ne matche plus rien :
    // le jeu a été mis à jour depuis que ce patch a été sauvegardé. Comparer
    // le hash de l'exécutable attaché à celui enregistré permet de le dire
    // avec certitude plutôt que de laisser un "signature introuvable" nu que
    // l'utilisateur ne peut pas distinguer d'un bug du côté de l'outil.
    const bool hasKnownExecutableHash = !profile.executableHash.isEmpty();
    const QString currentExecutableHash = hasKnownExecutableHash
        ? computeExecutableHash(m_handle.executablePath())
        : QString();
    const bool executableVersionMismatch = hasKnownExecutableHash
        && !currentExecutableHash.isEmpty()
        && currentExecutableHash != profile.executableHash;
    result["executableVersionMismatch"] = executableVersionMismatch;

    const auto pattern = killcore::parseAobPattern(patch->aobPattern);
    if (!pattern.isValid()) {
        result["error"] = pattern.error;
        return result;
    }
    const auto quality = killcore::evaluateAobPatternQuality(pattern);
    result["signatureQuality"] = aobPatternQualityToVariantMap(quality);
    result["signatureRisk"] = quality.level;
    if (quality.fixedBytes < 3 || quality.score < 35) {
        result["error"] = QString("Patch bloqué: signature AOB trop faible (%1/100, %2 octet(s) fixe(s)).")
                              .arg(quality.score)
                              .arg(quality.fixedBytes);
        result["signatureWarning"] = quality.warning;
        appendScanTelemetry("trainer_patch_apply_blocked", result);
        return result;
    }

    killcore::AobScanOptions options;
    options.executableOnly = true;
    options.imageOnly = true;
    options.maxResults = 100;
    const auto scan = killcore::scanAobPattern(m_handle, pattern, options);
    if (!scan.success || scan.matches.isEmpty()) {
        if (executableVersionMismatch) {
            result["error"] = "Signature AOB introuvable — et ce patch a été enregistré pour une version différente "
                               "de l'exécutable (l'empreinte du fichier ne correspond pas à celle attachée "
                               "actuellement). C'est très probablement pourquoi : une mise à jour du jeu a changé "
                               "les octets autour de cette instruction. Recapture-la depuis \"Écrit par\" sur cette version.";
        } else {
            result["error"] = scan.error.isEmpty() ? QString("Signature AOB introuvable.") : scan.error;
        }
        appendScanTelemetry("trainer_patch_apply_blocked", result);
        return result;
    }
    result["signatureMatches"] = scan.matchesFound;
    if (scan.matchesFound != 1) {
        result["error"] = QString("Patch bloqué: signature AOB non unique (%1 matches). Regénère une signature plus spécifique.")
                              .arg(scan.matchesFound);
        appendScanTelemetry("trainer_patch_apply_blocked", result);
        return result;
    }

    const auto modules = killcore::ProcessEnumerator::enumerateModules(m_pid);
    uint64_t selectedAddress = scan.matches.first().address;
    bool selectedModuleMatch = false;
    for (const auto& match : scan.matches) {
        for (const auto& module : modules) {
            if (match.address >= module.baseAddress && match.address < module.baseAddress + module.size
                && module.name.compare(patch->module, Qt::CaseInsensitive) == 0) {
                selectedAddress = match.address;
                selectedModuleMatch = true;
                break;
            }
        }
        if (selectedModuleMatch) {
            break;
        }
    }

    result = applyCodePatch(QString::number(selectedAddress, 16), patch->patchBytes, {{"verify", true}});
    result["profileName"] = profileName;
    result["patchName"] = patchName;
    result["matchedAddress"] = QString::number(selectedAddress, 16).toUpper();
    result["matchCount"] = scan.matches.size();
    result["aobPattern"] = patch->aobPattern;
    result["signatureQuality"] = aobPatternQualityToVariantMap(quality);
    result["signatureRisk"] = quality.level;
    result["signatureMatches"] = scan.matchesFound;
    // La signature a matché quand même : pas bloquant, mais vaut la peine
    // d'être su (ex: une mise à jour mineure du jeu qui n'a pas touché ce
    // code précis — le patch est probablement toujours valide, mais moins
    // certain qu'un hash identique).
    result["executableVersionMismatch"] = executableVersionMismatch;
    if (executableVersionMismatch) {
        result["executableVersionWarning"] = "Exécutable d'une version différente de celle où ce patch a été "
                                              "enregistré — la signature a quand même matché, mais vérifie le "
                                              "résultat avant de t'y fier pleinement.";
    }
    appendScanTelemetry(result.value("success").toBool() ? "trainer_patch_apply" : "trainer_patch_apply_failed", result);
    return result;
}

QVariantMap ApplicationController::restoreProfileCodePatch(const QString& profileName, const QString& patchName) {
    QVariantMap result;
    result["success"] = false;
    result["profileName"] = profileName;
    result["patchName"] = patchName;

    if (!m_handle.isValid()) {
        result["error"] = "Aucun processus attaché.";
        return result;
    }

    killcore::Profile profile;
    const QString path = killcore::ProfileStore::profilePath(profileName);
    if (!killcore::ProfileStore::load(path, &profile)) {
        result["error"] = "Profil introuvable.";
        return result;
    }

    for (const auto& patch : profile.patches) {
        if (patch.name != patchName) {
            continue;
        }
        QList<killcore::AobPattern> restorePatterns;
        const auto originalPattern = killcore::parseAobPattern(patch.aobPattern);
        if (originalPattern.isValid()) {
            restorePatterns.append(originalPattern);
        }
        const auto activePattern = killcore::parseAobPattern(patch.patchBytes);
        if (activePattern.isValid()) {
            restorePatterns.append(activePattern);
        }
        if (restorePatterns.isEmpty()) {
            result["error"] = originalPattern.error.isEmpty() ? activePattern.error : originalPattern.error;
            return result;
        }
        killcore::AobScanOptions options;
        options.executableOnly = true;
        options.imageOnly = true;
        options.maxResults = 100;
        for (const auto& restorePattern : restorePatterns) {
            const auto scan = killcore::scanAobPattern(m_handle, restorePattern, options);
            for (const auto& match : scan.matches) {
                if (m_activeCodePatches.contains(match.address)) {
                    result = restoreCodePatch(QString::number(match.address, 16));
                    result["profileName"] = profileName;
                    result["patchName"] = patchName;
                    result["matchedAddress"] = QString::number(match.address, 16).toUpper();
                    return result;
                }
            }
        }
        result["error"] = "Patch non actif dans la session courante.";
        return result;
    }

    result["error"] = "Patch introuvable dans le profil.";
    return result;
}

QVariantMap ApplicationController::applyAllProfileCodePatches(const QString& profileName) {
    QVariantMap result;
    result["success"] = false;
    result["profileName"] = profileName;

    if (!m_handle.isValid()) {
        result["error"] = "Aucun processus attaché.";
        return result;
    }

    killcore::Profile profile;
    const QString path = killcore::ProfileStore::profilePath(profileName);
    if (!killcore::ProfileStore::load(path, &profile)) {
        result["error"] = "Profil introuvable.";
        return result;
    }

    QVariantList patchResults;
    int applied = 0;
    int alreadyActive = 0;
    for (const auto& patch : profile.patches) {
        QVariantMap patchResult = applyProfileCodePatch(profileName, patch.name);
        if (patchResult.value("success").toBool()) {
            ++applied;
        } else if (patchResult.value("active").toBool()) {
            ++alreadyActive;
            patchResult["alreadyActive"] = true;
        }
        patchResults.append(patchResult);
    }

    result["success"] = applied + alreadyActive == profile.patches.size();
    result["applied"] = applied;
    result["alreadyActive"] = alreadyActive;
    result["total"] = profile.patches.size();
    result["results"] = patchResults;
    if (profile.patches.isEmpty()) {
        result["error"] = "Aucun patch trainer dans ce profil.";
    } else if (!result.value("success").toBool()) {
        result["error"] = QString("Application partielle: %1 appliqué(s), %2 déjà actif(s), %3 total.")
                              .arg(applied)
                              .arg(alreadyActive)
                              .arg(profile.patches.size());
    }
    return result;
}

QVariantMap ApplicationController::restoreAllProfileCodePatches(const QString& profileName) {
    QVariantMap result;
    result["success"] = false;
    result["profileName"] = profileName;

    if (!m_handle.isValid()) {
        result["error"] = "Aucun processus attaché.";
        return result;
    }

    killcore::Profile profile;
    const QString path = killcore::ProfileStore::profilePath(profileName);
    if (!killcore::ProfileStore::load(path, &profile)) {
        result["error"] = "Profil introuvable.";
        return result;
    }

    QVariantList patchResults;
    int restored = 0;
    int alreadyInactive = 0;
    for (const auto& patch : profile.patches) {
        QVariantMap patchResult = restoreProfileCodePatch(profileName, patch.name);
        patchResults.append(patchResult);
        if (patchResult.value("success").toBool()) {
            ++restored;
        } else if (patchResult.value("error").toString().contains("non actif", Qt::CaseInsensitive)) {
            ++alreadyInactive;
        }
    }

    result["success"] = restored + alreadyInactive == profile.patches.size();
    result["restored"] = restored;
    result["alreadyInactive"] = alreadyInactive;
    result["total"] = profile.patches.size();
    result["results"] = patchResults;
    if (profile.patches.isEmpty()) {
        result["error"] = "Aucun patch trainer dans ce profil.";
    } else if (!result.value("success").toBool()) {
        result["error"] = QString("Restauration partielle: %1 restauré(s), %2 déjà inactif(s), %3 total.")
                              .arg(restored)
                              .arg(alreadyInactive)
                              .arg(profile.patches.size());
    }
    return result;
}

QVariantMap ApplicationController::inspectProfileCodePatches(const QString& profileName) {
    QVariantMap result;
    result["success"] = false;
    result["profileName"] = profileName;

    if (!m_handle.isValid()) {
        result["error"] = "Aucun processus attaché.";
        return result;
    }

    killcore::Profile profile;
    const QString path = killcore::ProfileStore::profilePath(profileName);
    if (!killcore::ProfileStore::load(path, &profile)) {
        result["error"] = "Profil introuvable.";
        return result;
    }

    killcore::AobScanOptions options;
    options.executableOnly = true;
    options.imageOnly = true;
    options.maxResults = 100;

    QVariantList states;
    int originalCount = 0;
    int activeCount = 0;
    int ambiguousCount = 0;
    int missingCount = 0;
    int invalidCount = 0;

    for (const auto& patch : profile.patches) {
        QVariantMap state;
        state["profileName"] = profileName;
        state["patchName"] = patch.name;
        state["module"] = patch.module;
        state["moduleOffset"] = QString::number(patch.moduleOffset, 16).toUpper();
        state["signatureScore"] = patch.signatureScore;
        state["signatureLevel"] = patch.signatureLevel;
        state["signatureWarning"] = patch.signatureWarning;
        state["signatureFixedBytes"] = patch.signatureFixedBytes;
        state["signatureWildcardBytes"] = patch.signatureWildcardBytes;
        state["signatureUniqueFixedBytes"] = patch.signatureUniqueFixedBytes;
        state["signatureFixedRatio"] = patch.signatureFixedRatio;
        state["trainerSafe"] = patch.trainerSafe;
        state["savedSignatureMatches"] = patch.signatureMatches;
        state["success"] = true;
        state["active"] = false;

        const auto originalPattern = killcore::parseAobPattern(patch.aobPattern);
        const auto patchedPattern = killcore::parseAobPattern(patch.patchBytes);
        if (!originalPattern.isValid() && !patchedPattern.isValid()) {
            state["success"] = false;
            state["status"] = "invalid";
            state["error"] = originalPattern.error.isEmpty() ? patchedPattern.error : originalPattern.error;
            ++invalidCount;
            states.append(state);
            continue;
        }

        int originalMatches = 0;
        int patchedMatches = 0;
        QString originalAddress;
        QString patchedAddress;

        if (originalPattern.isValid()) {
            const auto scan = killcore::scanAobPattern(m_handle, originalPattern, options);
            originalMatches = scan.matches.size();
            if (!scan.matches.isEmpty()) {
                originalAddress = QString::number(scan.matches.first().address, 16).toUpper();
            }
        }

        if (patchedPattern.isValid()) {
            const auto scan = killcore::scanAobPattern(m_handle, patchedPattern, options);
            patchedMatches = scan.matches.size();
            if (!scan.matches.isEmpty()) {
                patchedAddress = QString::number(scan.matches.first().address, 16).toUpper();
            }
        }

        state["originalMatches"] = originalMatches;
        state["patchedMatches"] = patchedMatches;
        state["matchedAddress"] = patchedAddress.isEmpty() ? originalAddress : patchedAddress;

        const bool sessionActive = !patchedAddress.isEmpty() && m_activeCodePatches.contains(patchedAddress.toULongLong(nullptr, 16));
        const auto memoryState = killcore::classifyProfilePatchMemoryState(
            originalMatches,
            patchedMatches,
            sessionActive,
            originalPattern.isValid() || patchedPattern.isValid());
        state["status"] = memoryState.status;
        state["success"] = memoryState.success;
        state["active"] = memoryState.active;

        if (memoryState.status == "active") {
            ++activeCount;
        } else if (memoryState.status == "original") {
            ++originalCount;
        } else if (memoryState.status == "missing") {
            state["error"] = "Signature originale et patchée introuvables.";
            ++missingCount;
        } else {
            state["warning"] = "Signature non unique ou état mixte.";
            ++ambiguousCount;
        }

        states.append(state);
    }

    result["success"] = invalidCount == 0 && missingCount == 0;
    result["states"] = states;
    result["total"] = profile.patches.size();
    result["original"] = originalCount;
    result["active"] = activeCount;
    result["ambiguous"] = ambiguousCount;
    result["missing"] = missingCount;
    result["invalid"] = invalidCount;
    if (profile.patches.isEmpty()) {
        result["error"] = "Aucun patch trainer dans ce profil.";
    } else if (!result.value("success").toBool()) {
        result["error"] = QString("Inspection: %1 actif(s), %2 original(aux), %3 ambigu(s), %4 introuvable(s), %5 invalide(s).")
                              .arg(activeCount)
                              .arg(originalCount)
                              .arg(ambiguousCount)
                              .arg(missingCount)
                              .arg(invalidCount);
    }
    return result;
}

QVariantMap ApplicationController::saveProfileAutoAsmScript(
    const QString& profileName,
    const QString& scriptName,
    const QString& scriptText,
    const QVariantMap& metadata) {
    QVariantMap result;
    result["success"] = false;

    const QString cleanProfileName = profileName.trimmed();
    const QString cleanScriptName = scriptName.trimmed();
    if (cleanProfileName.isEmpty() || cleanScriptName.isEmpty()) {
        result["error"] = "Nom de profil ou de script vide.";
        return result;
    }
    if (scriptText.trimmed().isEmpty()) {
        result["error"] = "Script vide.";
        return result;
    }

    // Refuse de sauvegarder un script qui ne parse meme pas — evite de
    // stocker un texte casse qu'on ne pourra jamais rejouer plus tard.
    const auto parsed = killcore::parseAutoAsmScript(scriptText);
    if (!parsed.success) {
        result["error"] = parsed.error;
        result["errorLine"] = parsed.errorLine;
        return result;
    }

    killcore::Profile profile;
    const QString path = killcore::ProfileStore::profilePath(cleanProfileName);
    if (!killcore::ProfileStore::load(path, &profile)) {
        profile.gameName = cleanProfileName;
        profile.executableName = m_processName;
    }
    if (profile.executableHash.isEmpty() && m_handle.isValid()) {
        profile.executableHash = computeExecutableHash(m_handle.executablePath());
    }

    killcore::ProfileAutoAsmScript script;
    script.name = cleanScriptName;
    script.scriptText = scriptText;
    script.description = metadata.value("description").toString();
    script.riskLevel = metadata.value("riskLevel").toString();

    bool replaced = false;
    for (auto& existing : profile.autoAsmScripts) {
        if (existing.name == script.name) {
            existing = script;
            replaced = true;
            break;
        }
    }
    if (!replaced) {
        profile.autoAsmScripts.append(script);
    }

    if (!killcore::ProfileStore::save(profile, path)) {
        result["error"] = "Impossible de sauvegarder le profil.";
        return result;
    }

    result["success"] = true;
    result["profileName"] = cleanProfileName;
    result["scriptName"] = cleanScriptName;
    result["scriptCount"] = profile.autoAsmScripts.size();
    result["replaced"] = replaced;
    appendScanTelemetry("auto_asm_script_saved", result);
    return result;
}

QVariantMap ApplicationController::applyProfileAutoAsmScript(const QString& profileName, const QString& scriptName) {
    QVariantMap result;
    result["success"] = false;
    result["profileName"] = profileName;
    result["scriptName"] = scriptName;

    killcore::Profile profile;
    const QString path = killcore::ProfileStore::profilePath(profileName);
    if (!killcore::ProfileStore::load(path, &profile)) {
        result["error"] = "Profil introuvable.";
        return result;
    }

    for (const auto& script : profile.autoAsmScripts) {
        if (script.name == scriptName) {
            // Reutilise tel quel le chemin d'execution manuel — meme garde-fou
            // "un seul script actif a la fois", meme suivi de restauration.
            return executeAutoAssemblerScript(script.scriptText);
        }
    }

    result["error"] = "Script auto-assembler introuvable dans ce profil.";
    return result;
}

QVariantMap ApplicationController::deleteProfileAutoAsmScript(const QString& profileName, const QString& scriptName) {
    QVariantMap result;
    result["success"] = false;
    result["profileName"] = profileName;
    result["scriptName"] = scriptName;

    killcore::Profile profile;
    const QString path = killcore::ProfileStore::profilePath(profileName);
    if (!killcore::ProfileStore::load(path, &profile)) {
        result["error"] = "Profil introuvable.";
        return result;
    }

    const int before = profile.autoAsmScripts.size();
    profile.autoAsmScripts.removeIf([&](const killcore::ProfileAutoAsmScript& script) {
        return script.name == scriptName;
    });
    if (profile.autoAsmScripts.size() == before) {
        result["error"] = "Script auto-assembler introuvable dans ce profil.";
        return result;
    }

    if (!killcore::ProfileStore::save(profile, path)) {
        result["error"] = "Impossible de sauvegarder le profil.";
        return result;
    }

    result["success"] = true;
    result["scriptCount"] = profile.autoAsmScripts.size();
    return result;
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
    result["automationPipeOptIn"] = qEnvironmentVariableIsSet("KILLENGINE_AUTOMATION_PIPE");
    result["message"] = luaPath.isEmpty()
        ? QStringLiteral("Aucun interpréteur Lua trouvé dans runtime/lua, lua, le dossier de l'application ou le PATH.")
        : QStringLiteral("Lua externe prêt. Les appels KillEngine passent par le pipe d'automatisation local.");
    return result;
}

QVariantMap ApplicationController::executeLuaScript(const QString& scriptText, const QVariantMap& options) {
    QVariantMap result;
    result["success"] = false;

    const QString trimmedScript = scriptText.trimmed();
    if (trimmedScript.isEmpty()) {
        result["error"] = "Script Lua vide.";
        return result;
    }

    const QString luaPath = findLuaExecutable(options.value("luaPath").toString());
    if (luaPath.isEmpty()) {
        result["error"] = "Aucun interpréteur Lua trouvé. Place lua.exe dans runtime\\lua à côté de KillEngine.exe, ajoute Lua au PATH, ou renseigne options.luaPath.";
        return result;
    }

    QTemporaryFile scriptFile(QDir::temp().filePath("killengine-lua-XXXXXX.lua"));
    scriptFile.setAutoRemove(true);
    if (!scriptFile.open()) {
        result["error"] = QStringLiteral("Impossible de créer le script temporaire Lua : %1").arg(scriptFile.errorString());
        return result;
    }
    scriptFile.write(scriptText.toUtf8());
    scriptFile.flush();
    const QString scriptPath = scriptFile.fileName();
    scriptFile.close();

    const QString helperPath = findKillEngineLuaHelper();
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
        result["error"] = QStringLiteral("Impossible de démarrer Lua : %1").arg(process.errorString());
        result["luaPath"] = luaPath;
        return result;
    }

    bool timedOut = false;
    if (!process.waitForFinished(timeoutMs)) {
        timedOut = true;
        process.kill();
        process.waitForFinished(2000);
    }

    const QString stdoutText = QString::fromUtf8(process.readAllStandardOutput());
    const QString stderrText = QString::fromUtf8(process.readAllStandardError());
    const int exitCode = process.exitCode();

    result["success"] = !timedOut && process.exitStatus() == QProcess::NormalExit && exitCode == 0;
    result["timedOut"] = timedOut;
    result["exitCode"] = exitCode;
    result["luaPath"] = luaPath;
    result["helperPath"] = helperPath;
    result["stdout"] = stdoutText;
    result["stderr"] = stderrText;
    if (!result.value("success").toBool()) {
        result["error"] = timedOut
            ? QStringLiteral("Script Lua interrompu après timeout (%1 ms).").arg(timeoutMs)
            : QStringLiteral("Script Lua terminé avec le code %1.").arg(exitCode);
    }

    appendScanTelemetry(QStringLiteral("lua_script_execute"), {
        {"success", result.value("success").toBool()},
        {"exitCode", exitCode},
        {"timedOut", timedOut},
        {"stdoutBytes", stdoutText.toUtf8().size()},
        {"stderrBytes", stderrText.toUtf8().size()},
    });
    return result;
}

// ---------------------------------------------------------------------------
// Phase 14 — Pointer Chains (jeux modernes / applications dynamiques)
// ---------------------------------------------------------------------------

namespace {

killcore::PointerChain variantMapToPointerChain(const QVariantMap& chainMap, QString* error = nullptr) {
    killcore::PointerChain chain;
    chain.module = chainMap.value("module").toString();
    if (chain.module.isEmpty()) {
        if (error) *error = "Chaine invalide : module manquant.";
        return chain;
    }

    const QString baseOffsetHex = chainMap.value("baseOffset").toString();
    bool ok = false;
    chain.baseOffset = baseOffsetHex.toULongLong(&ok, 16);
    if (!ok) {
        if (error) *error = "Chaine invalide : baseOffset hex invalide.";
        return chain;
    }

    const QVariantList offsets = chainMap.value("offsets").toList();
    for (const auto& offsetVar : offsets) {
        const uint64_t off = offsetVar.toString().toULongLong(&ok, 16);
        if (!ok) {
            if (error) *error = "Chaine invalide : offset hex invalide.";
            return chain;
        }
        chain.offsets.append(off);
    }

    return chain;
}

QVariantMap pointerChainToVariantMap(const killcore::PointerChain& chain) {
    QVariantMap result;
    result["module"] = chain.module;
    result["baseOffset"] = QString::number(chain.baseOffset, 16);
    QVariantList offsets;
    for (uint64_t offset : chain.offsets) {
        offsets.append(QString::number(offset, 16));
    }
    result["offsets"] = offsets;
    result["depth"] = chain.depth();
    result["label"] = chain.toString();
    return result;
}

} // namespace

QVariantMap ApplicationController::scanPointerChains(
    const QString& addressHex,
    const QVariantMap& scanOptions) {

    QVariantMap result;
    result["success"] = false;

    if (!m_handle.isValid()) {
        result["error"] = "Aucun processus attache.";
        return result;
    }

    uint64_t targetAddress = 0;
    if (!parseHexAddress(addressHex, &targetAddress)) {
        result["error"] = "Adresse cible invalide.";
        return result;
    }

    killcore::PointerScanOptions options;
    options.maxDepth = scanOptions.value("maxDepth", 3).toInt();
    options.maxOffset = scanOptions.value("maxOffset", 0x1000).toULongLong();
    options.maxResults = static_cast<size_t>(scanOptions.value("maxResults", 100).toULongLong());
    options.onlyModuleBase = scanOptions.value("onlyModuleBase", true).toBool();
    options.alignment = static_cast<size_t>(scanOptions.value("alignment", 8).toULongLong());

    const QVariant baseModulesVar = scanOptions.value("baseModules");
    if (baseModulesVar.isValid() && baseModulesVar.canConvert<QVariantList>()) {
        for (const auto& mod : baseModulesVar.toList()) {
            options.baseModules.append(mod.toString());
        }
    }

    QElapsedTimer timer;
    timer.start();

    auto scanResult = killcore::scanForPointerChains(m_handle, targetAddress, options);

    result["success"] = scanResult.success;
    result["partial"] = scanResult.partial;
    result["cancelled"] = scanResult.cancelled;
    result["pointersScanned"] = static_cast<qulonglong>(scanResult.pointersScanned);
    result["bytesScanned"] = static_cast<qulonglong>(scanResult.bytesScanned);
    result["elapsedMs"] = static_cast<qulonglong>(timer.elapsed());
    result["chainCount"] = static_cast<int>(scanResult.chains.size());

    if (!scanResult.errorMessage.isEmpty()) {
        result["error"] = scanResult.errorMessage;
    }

    QVariantList chainsList;
    for (const auto& chain : scanResult.chains) {
        chainsList.append(pointerChainToVariantMap(chain));
    }
    result["chains"] = chainsList;

    KE_LOG_INFO() << "scanPointerChains: target=0x" << QString::number(targetAddress, 16).toStdString()
                  << " chains=" << scanResult.chains.size()
                  << " elapsed=" << timer.elapsed() << "ms";
    appendScanTelemetry("pointer_scan", {
        {"targetAddress", QString::number(targetAddress, 16)},
        {"maxDepth", options.maxDepth},
        {"maxOffset", static_cast<qulonglong>(options.maxOffset)},
        {"maxResults", static_cast<qulonglong>(options.maxResults)},
        {"onlyModuleBase", options.onlyModuleBase},
        {"alignment", static_cast<qulonglong>(options.alignment)},
        {"baseModules", options.baseModules},
        {"success", result.value("success")},
        {"partial", result.value("partial")},
        {"cancelled", result.value("cancelled")},
        {"pointersScanned", result.value("pointersScanned")},
        {"bytesScanned", result.value("bytesScanned")},
        {"chainCount", result.value("chainCount")},
        {"elapsedMs", result.value("elapsedMs")},
        {"pointersPerSecond", ratePerSecond(scanResult.pointersScanned, timer.elapsed())},
        {"bytesPerSecond", ratePerSecond(scanResult.bytesScanned, timer.elapsed())},
        {"error", result.value("error")},
    });

    return result;
}

QVariantMap ApplicationController::resolvePointerChain(const QVariantMap& chainMap) {
    QVariantMap result;
    result["success"] = false;

    if (!m_handle.isValid()) {
        result["error"] = "Aucun processus attache.";
        return result;
    }

    QString parseError;
    const auto chain = variantMapToPointerChain(chainMap, &parseError);
    if (!parseError.isEmpty()) {
        result["error"] = parseError;
        return result;
    }

    const auto resolveResult = killcore::resolvePointerChain(m_handle, chain);
    if (!resolveResult.success) {
        result["error"] = resolveResult.errorMessage.isEmpty()
            ? QString("Resolution de chaine echouee.")
            : resolveResult.errorMessage;
        return result;
    }

    result["success"] = true;
    result["finalAddress"] = QString::number(resolveResult.finalAddress, 16);

    QVariantList steps;
    for (uint64_t addr : resolveResult.intermediateAddresses) {
        steps.append(QString::number(addr, 16));
    }
    result["steps"] = steps;

    return result;
}

QVariantMap ApplicationController::suggestStableLocatorForAddress(
    const QString& addressHex,
    const QVariantMap& options) {

    QVariantMap result;
    result["success"] = false;
    result["chainCount"] = 0;

    if (!m_handle.isValid()) {
        result["error"] = "Aucun processus attache.";
        return result;
    }

    uint64_t targetAddress = 0;
    if (!parseHexAddress(addressHex, &targetAddress)) {
        result["error"] = "Adresse invalide.";
        return result;
    }

    // Bornes volontairement modestes : cette methode est pensee pour un appel
    // explicite juste apres une ecriture confirmee, pas pour tourner en fond.
    QVariantMap scanOptions;
    scanOptions["maxDepth"] = options.value("maxDepth", 3);
    scanOptions["maxOffset"] = options.value("maxOffset", 0x1000);
    scanOptions["maxResults"] = options.value("maxResults", 5);
    scanOptions["onlyModuleBase"] = options.value("onlyModuleBase", true);
    scanOptions["alignment"] = options.value("alignment", 8);
    if (options.contains("baseModules")) {
        scanOptions["baseModules"] = options.value("baseModules");
    }

    const QVariantMap scanResult = scanPointerChains(addressHex, scanOptions);
    result["success"] = scanResult.value("success").toBool();
    result["elapsedMs"] = scanResult.value("elapsedMs");
    if (!scanResult.value("error").toString().isEmpty()) {
        result["error"] = scanResult.value("error");
    }
    if (!result["success"].toBool()) {
        return result;
    }

    const QVariantList chains = scanResult.value("chains").toList();
    result["chainCount"] = chains.size();
    if (chains.isEmpty()) {
        result["message"] = QStringLiteral(
            "Aucune chaine de pointeurs stable trouvee vers 0x%1 dans les bornes explorees. "
            "L'adresse absolue devra etre re-recherchee si le processus redemarre.")
            .arg(QString::number(targetAddress, 16).toUpper());
        return result;
    }

    // Meilleure chaine = la plus courte (moins de niveaux = plus stable, plus
    // rapide a resoudre) ; a profondeur egale, le plus petit offset de base.
    QVariantMap best = chains.first().toMap();
    for (const auto& candidate : chains) {
        const QVariantMap chain = candidate.toMap();
        const int candidateDepth = chain.value("depth").toInt();
        const int bestDepth = best.value("depth").toInt();
        if (candidateDepth < bestDepth) {
            best = chain;
        } else if (candidateDepth == bestDepth) {
            const qulonglong candidateBase = chain.value("baseOffset").toString().toULongLong(nullptr, 16);
            const qulonglong bestBase = best.value("baseOffset").toString().toULongLong(nullptr, 16);
            if (candidateBase < bestBase) {
                best = chain;
            }
        }
    }

    result["bestChain"] = best;
    result["message"] = QStringLiteral(
        "%1 chaine(s) de pointeurs stable(s) trouvee(s) vers 0x%2. "
        "Meilleure option : %3 (profondeur %4). Sauvegarde-la dans un profil pour qu'elle survive a un redemarrage.")
        .arg(chains.size())
        .arg(QString::number(targetAddress, 16).toUpper())
        .arg(best.value("label").toString())
        .arg(best.value("depth").toInt());

    appendScanTelemetry("suggest_stable_locator", {
        {"targetAddress", QString::number(targetAddress, 16)},
        {"success", result.value("success")},
        {"chainCount", result.value("chainCount")},
        {"bestDepth", best.value("depth")},
        {"elapsedMs", result.value("elapsedMs")},
    });

    return result;
}

QVariantMap ApplicationController::savePointerChainProfileTarget(
    const QString& profileName,
    const QString& targetName,
    const QVariantMap& chainMap,
    const QString& valueType,
    const QString& description) {

    QVariantMap result;
    result["success"] = false;

    if (!m_handle.isValid()) {
        result["error"] = "Aucun processus attache.";
        return result;
    }

    QString parseError;
    const auto chain = variantMapToPointerChain(chainMap, &parseError);
    if (!parseError.isEmpty()) {
        result["error"] = parseError;
        return result;
    }

    // Valider la chaîne avant de la sauvegarder.
    const auto resolveCheck = killcore::resolvePointerChain(m_handle, chain);
    if (!resolveCheck.success) {
        result["error"] = "La chaine ne se resout pas : " + resolveCheck.errorMessage;
        return result;
    }

    killcore::Profile profile;
    const QString path = killcore::ProfileStore::profilePath(profileName);
    bool isNewProfile = true;
    if (killcore::ProfileStore::load(path, &profile)) {
        isNewProfile = false;
    } else {
        profile.gameName = profileName;
        profile.executableName = m_processName;
    }

    killcore::ValueType type = killcore::ValueType::Int32;
    killcore::parseValueType(valueType, &type);

    // Cherche une cible existante avec le même nom pour la remplacer.
    bool replaced = false;
    for (auto& existing : profile.targets) {
        if (existing.name == targetName) {
            existing.type = type;
            existing.locator.kind = killcore::LocatorKind::PointerChain;
            existing.locator.pointerChain = chain;
            existing.locator.lastAddress = resolveCheck.finalAddress;
            existing.description = description;
            replaced = true;
            break;
        }
    }
    if (!replaced) {
        killcore::ProfileTarget target;
        target.name = targetName;
        target.type = type;
        target.locator.kind = killcore::LocatorKind::PointerChain;
        target.locator.pointerChain = chain;
        target.locator.lastAddress = resolveCheck.finalAddress;
        target.description = description;
        profile.targets.append(target);
    }

    if (!killcore::ProfileStore::save(profile, path)) {
        result["error"] = "Impossible de sauvegarder le profil.";
        return result;
    }

    result["success"] = true;
    result["profileName"] = profileName;
    result["targetName"] = targetName;
    result["resolvedAddress"] = QString::number(resolveCheck.finalAddress, 16);
    result["isNewProfile"] = isNewProfile;
    result["chainLabel"] = chain.toString();
    result["message"] = QString("Cible \"%1\" sauvegardee avec chaine de pointeurs (resout a 0x%2).")
                            .arg(targetName, QString::number(resolveCheck.finalAddress, 16));

    KE_LOG_INFO() << "savePointerChainProfileTarget: profile=" << profileName.toStdString()
                  << " target=" << targetName.toStdString()
                  << " addr=0x" << QString::number(resolveCheck.finalAddress, 16).toStdString();

    return result;
}

} // namespace killengine
