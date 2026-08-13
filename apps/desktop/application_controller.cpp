#include "application_controller.h"

#include "candidates/candidate_store.h"
#include "crash_handler.h"
#include "logging/logger.h"
#include "memory/memory_map.h"
#include "memory/memory_reader.h"
#include "memory/memory_writer.h"
#include "process/process_enumerator.h"
#include "process/process_handle.h"
#include "pointer/pointer_chain.h"
#include "pointer/pointer_scanner.h"
#include "scanner/scan_engine.h"
#include "scanner/scan_types.h"
#include "scanner/display_value_tracker.h"
#include "scanner/value_variants.h"
#include "snapshot/snapshot_store.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QPointer>
#include <QRegularExpression>
#include <QSettings>
#include <QSet>
#include <QStandardPaths>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <thread>

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
    const QRegularExpression re(R"([-+]?\d+(?:[\.,]\d+)?)");
    auto it = re.globalMatch(textWithoutTypeTokens(textWithoutHexAddresses(text)));
    while (it.hasNext()) {
        values.append(it.next().captured(0).replace(',', '.'));
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
        || q.contains("reset");
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
    bool smartSearchActive) {
    SmartSearchIntent intent;
    intent.numbers = numbers;
    intent.addresses = addresses;
    intent.resetContext = looksLikeNewSearchRequest(query);

    const bool hasOneNumber = numbers.size() == 1;
    const bool wantsMemoryWrite = looksLikeMemoryTargetWriteRequest(query);
    const bool wantsLastRewrite = looksLikeLastAutoWriteRewrite(query);
    const bool wantsClearTargets = looksLikeClearActiveTargetsRequest(query);
    const bool reportsBadTargets = looksLikeBadTargetReport(query);
    const bool wantsFreeze = looksLikeFreezeRequest(query);

    if (wantsClearTargets) {
        intent.kind = SmartSearchIntentKind::ClearActiveTargets;
        intent.rationale = "L'utilisateur demande d'oublier les adresses, profils ou cibles actives.";
    } else if (reportsBadTargets && (hasLastAutoWriteTargets || hasChatMemoryTargets)) {
        intent.kind = SmartSearchIntentKind::ReportBadTargets;
        intent.rationale = "L'utilisateur indique que les dernières adresses écrites ne donnent pas le résultat attendu.";
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

QString confidenceLabel(double confidence) {
    if (confidence >= 0.85) {
        return "fiabilité élevée";
    }
    if (confidence >= 0.65) {
        return "fiabilité moyenne";
    }
    return "fiabilité faible";
}

QVariantList suggestedWritesForCandidates(const killcore::CandidateStore& candidates, const QString& value, size_t limit) {
    QVariantList suggestions;
    if (value.isEmpty() || candidates.isEmpty()) {
        return suggestions;
    }

    const auto& all = candidates.candidates();
    const size_t count = std::min(limit, static_cast<size_t>(all.size()));
    for (size_t i = 0; i < count; ++i) {
        const auto& candidate = all.at(static_cast<qsizetype>(i));
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
    m_ai.init();
    KE_LOG_INFO() << "ApplicationController initialized";
}

ApplicationController::~ApplicationController() {
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

bool ApplicationController::attachProcess(int pid) {
    KE_LOG_INFO() << "attachProcess(pid=" << pid << ")";

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
            if (start + 4 <= read.data.size()) {
                change["afterInt32"] = bytesToDouble(read.data.mid(start, 4), killcore::ValueType::Int32);
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
        candidates.append(candidateToVariantMap(candidate));
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
    const auto options = scanOptionsFromSettingsAndExpertOptions(expertOptions);
    const size_t maxSnapshotBytes = resolveUnknownSnapshotMaxBytes(
        expertOptions, m_handle, options, &suggestedDepthMb, &relevantBytes);
    const QPointer<ApplicationController> self(this);
    auto cancellation = std::make_shared<killcore::CancellationToken>();

    m_scanInProgress = true;
    m_activeScanCancellation = cancellation;
    emit scanStarted();
    emit scanProgress(0);

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
            for (const auto type : typesToRun) {
                const auto scan = self->m_snapshot.compare(workerHandle, type, scanMode, cancellation.get());
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
    }

    result["success"] = write.success;
    result["verified"] = write.verified;
    result["protectionChanged"] = write.protectionChanged;
    result["bytesWritten"] = static_cast<int>(write.bytesWritten);
    result["error"] = write.errorMessage;
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
    const QString& value) {
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
        if (m_freeze.isEmpty()) {
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

    m_freeze.setEntry(address, type, killcore::scanValueToBytes(scanValue));
    if (!m_freezeTimer.isActive()) {
        m_freezeTimer.start();
    }

    result["success"] = true;
    result["enabled"] = true;
    return result;
}

QVariantMap ApplicationController::setFreezeInterval(int intervalMs) {
    QVariantMap result;

    // Bornes raisonnables : 10 ms (très agressif, pour jeux type SC2)
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
    for (const auto& entry : m_freeze.entries()) {
        if (entry.enabled) {
            writer.write(entry.address, entry.value, false);
        }
    }
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

QVariantMap ApplicationController::startSmartSearch(const QString& query) {
    KE_LOG_INFO() << "startSmartSearch(\"" << query.toStdString() << "\")";
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
        m_smartSearchActive);
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
        QVariantMap recovery;
        QVariantList actions;
        actions.append(QVariantMap{{"id", "rollback_batch"}, {"label", "Rollback dernier lot"}});
        actions.append(QVariantMap{{"id", "clear_targets"}, {"label", "Oublier ces adresses"}});
        actions.append(QVariantMap{{"id", "new_search"}, {"label", "Nouvelle recherche"}});
        if (!m_candidates.isEmpty()) {
            actions.append(QVariantMap{{"id", "continue_candidates"}, {"label", "Continuer avec les autres candidats"}});
        }

        recovery["success"] = true;
        recovery["query"] = query;
        recovery["aiReady"] = m_ai.isReady();
        recovery["status"] = "bad_targets_reported";
        recovery["actionStatus"] = "needs_recovery_choice";
        recovery["workflowStatus"] = "auto_write_problem";
        recovery["targetValue"] = m_smartSearchTargetValue;
        recovery["activeTargetCount"] = m_chatMemoryTargets.size();
        recovery["candidateStoreSize"] = static_cast<qulonglong>(m_candidates.size());
        recovery["recoveryActions"] = actions;
        recovery["message"] = "D'accord, on ne valide pas ces adresses. Tu peux annuler le dernier lot, oublier ces adresses, repartir sur une nouvelle recherche, ou continuer avec les candidats restants.";
        stampIntent(&recovery);
        appendSmartSearchDebug("smart_search_bad_targets_reported", recovery);
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
        QVariantMap args;
        args["mode"] = "exact";
        args["value"] = numbers.first();
        result["status"] = "tool_call";
        result["tool"] = "next_scan";
        result["args"] = args;
        result["rationale"] = intent.rationale;
        result["state"] = "Refining";
        result["error"] = "";
    } else if (intent.kind == SmartSearchIntentKind::GuidedScan && numbers.size() >= 2) {
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
        result = m_ai.processQuery(query);
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
    } else if (tool == "write_value" || tool == "freeze_value") {
        result["actionStatus"] = "requires_confirmation";
        result["requiresConfirmation"] = true;
        result["confirmationReason"] = "Cette action modifie la mémoire. Utilise l'onglet Mémoire pour confirmer manuellement.";
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
        result["workflowStatus"] = "awaiting_value_change";
        result["targetValue"] = m_smartSearchTargetValue;
        const QString prefix = intent.resetContext
            ? QString("Je repars sur une nouvelle recherche. ")
            : QString();
        const QString typeNote = tool == "exact_scan_multi_type"
            ? QString(" en Auto rapide")
            : QString();
        result["message"] = prefix + QString("J'ai trouvé %1 candidats pour %2%3. Fais bouger la valeur dans le jeu, puis donne-moi la nouvelle valeur pour réduire la liste.")
                                .arg(count)
                                .arg(m_smartSearchInitialValue)
                                .arg(typeNote);
    } else if (tool == "next_scan" || tool == "unknown_compare") {
        const auto remaining = tool == "next_scan"
                                   ? actionResult.value("remaining").toULongLong()
                                   : actionResult.value("stored").toULongLong();
        result["targetValue"] = m_smartSearchTargetValue;

        if (remaining >= 1 && remaining <= kAutoWriteCandidateLimit && !m_smartSearchTargetValue.isEmpty()) {
            auto suggestions = suggestedWritesForCandidates(
                m_candidates,
                m_smartSearchTargetValue,
                kAutoWriteCandidateLimit);
            enrichSuggestedWritesWithHistory(&suggestions);
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
            m_lastBatchEndIndex = m_writeHistory.size();
            if (m_lastBatchEndIndex == m_lastBatchStartIndex) {
                m_lastBatchStartIndex = -1;
                m_lastBatchEndIndex = -1;
                m_lastAutoWriteTargets.clear();
            }
            if (allWritesOk && !m_lastAutoWriteTargets.isEmpty()) {
                m_smartSearchActive = false;
                m_chatMemoryTargets = m_lastAutoWriteTargets;
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
    result["modelPath"] = settings.value("ai/modelPath", "").toString();
    result["modelThreads"] = boundedSettingInt(settings, "ai/modelThreads", 4, 1, 32);
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
    settings.setValue("ai/modelPath", incoming.value("modelPath", "").toString().trimmed());
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

    QDir crashDir(CrashHandler::crashDirectory());
    const auto crashFiles = crashDir.entryInfoList(
        QStringList{"*.crash.txt"},
        QDir::Files,
        QDir::Time);
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

// ---------------------------------------------------------------------------
// Phase 14 — Pointer Chains (StarCraft 2 / jeux modernes)
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
