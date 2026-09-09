#include "scanning_core_manager.h"

#include "application_controller.h"

#include "localization/localization.h"

#include "candidates/candidate_store.h"
#include "memory/memory_map.h"
#include "memory/memory_reader.h"
#include "process/process_handle.h"
#include "scanner/candidate_confidence.h"
#include "scanner/encrypted_scan.h"
#include "scanner/scan_engine.h"
#include "scanner/value_variants.h"
#include "snapshot/snapshot_store.h"

#include <QElapsedTimer>
#include <QMetaObject>
#include <QPointer>
#include <QRegularExpression>
#include <QSettings>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <thread>

namespace killengine {
namespace {

constexpr size_t kCandidateDisplayLimit = 250000;
constexpr qsizetype kUnknownAutoMaxReturnedMatches = 250000;
constexpr int kCandidateHistoryMaxAddresses = 10000;
constexpr int kDefaultScanMaxResults = 1000000;
constexpr int kDefaultScanChunkSizeMb = 0;
constexpr int kDefaultScanMaxWorkerThreads = 0;
constexpr int kDefaultScanMaxInFlightMb = 0;
constexpr int kDefaultCandidateFileThreshold = 250000;
constexpr int kDefaultUnknownSnapshotMaxMb = 128;

double bytesToDouble(const QByteArray& bytes, killcore::ValueType type);
QString bytesToHex(const QByteArray& bytes);

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
double ratePerSecond(size_t count, qint64 elapsedMs) {
    if (elapsedMs <= 0) {
        return 0.0;
    }
    return static_cast<double>(count) * 1000.0 / static_cast<double>(elapsedMs);
}



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

uint64_t relevantBytesForUnknownSnapshotOptions(
    const QList<killcore::MemoryRegion>& regions,
    const killcore::ScanOptions& options) {
    uint64_t total = 0;
    for (const auto& region : regions) {
        if (!regionMatchesScanOptions(region, options)) continue;

        const uint64_t regionStart = region.baseAddress;
        const uint64_t regionEnd = region.baseAddress + region.size;
        const uint64_t effectiveStart = std::max(regionStart, options.startAddress);
        const uint64_t effectiveEnd = options.stopAddress == 0 ? regionEnd : std::min(regionEnd, options.stopAddress);
        if (effectiveEnd <= effectiveStart) continue;
        total += effectiveEnd - effectiveStart;
    }
    return total;
}



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
    return killcore::generateScanVariants(rawValue, killcore::ValueType::Int32, false);
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



// SC2-UNKNOWN-1 : traduit un delta AFFICHÉ (ex. "+7") en delta BRUT attendu
// pour ce candidat précis, selon la variante d'échelle déjà taguée sur son
// variantLabel (ex. "Int32 x4096" -> 7*4096=28672). Même raisonnement que
// targetBytesForCandidate ci-dessus pour le mode Exact, appliqué au mode
// Delta : sans ça, un narrowing "delta +7" sur des candidats issus d'un scan
// multi-échelle ne matchait jamais les représentations scalées (le brut
// change de 28672, pas de 7).
double targetDeltaForCandidate(double rawDelta, const killcore::Candidate& candidate) {
    if (candidateNeedsAutoVariantMatch(candidate)) {
        return rawDelta;
    }
    const auto variants = killcore::generateDeltaVariants(rawDelta, candidate.type);
    for (const auto& variant : variants) {
        if (variant.label == candidate.variantLabel) {
            return variant.rawDelta;
        }
    }
    return rawDelta; // Label de variante non scalé (ex. cross-type "Float32 from Int32") : pas de scaling connu.
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
                    ? KE_TXT("Impossible de construire la valeur cible pour un candidat.", "Could not build the target value for a candidate.")
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
                ? KE_TXT("Impossible de construire la valeur cible pour un candidat.", "Could not build the target value for a candidate.")
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

QString bytesToHex(const QByteArray& bytes) {
    return QString::fromLatin1(bytes.toHex(' '));
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

// Contrepartie "décodage" de candidateObservationToVariantMap() : reconstruit
// le format attendu par CandidateConfidenceContext::valueHistory à partir du
// QHash<adresse, QVariantList> accumulé par ApplicationController au fil des
// next_scan (m_candidateValueHistory).
QHash<uint64_t, QList<killcore::ConfidenceObservation>> buildConfidenceValueHistory(
    const QHash<uint64_t, QVariantList>& rawHistory) {
    QHash<uint64_t, QList<killcore::ConfidenceObservation>> history;
    history.reserve(rawHistory.size());
    for (auto it = rawHistory.constBegin(); it != rawHistory.constEnd(); ++it) {
        QList<killcore::ConfidenceObservation> observations;
        observations.reserve(it.value().size());
        for (const auto& entryVariant : it.value()) {
            const QVariantMap entry = entryVariant.toMap();
            if (!entry.value("readable", true).toBool()) {
                continue; // Une lecture ratée ne renseigne rien sur la stabilité.
            }
            killcore::ConfidenceObservation obs;
            obs.address = it.key();
            obs.previousValue = entry.value("previousNumber").toDouble();
            obs.currentValue = entry.value("currentNumber").toDouble();
            obs.kept = entry.value("kept", true).toBool();
            observations.append(obs);
        }
        if (!observations.isEmpty()) {
            history.insert(it.key(), observations);
        }
    }
    return history;
}

QString noCandidateDiagnosticMessage(const QVariantMap& actionResult, const QString& tool) {
    const qulonglong checked = tool == "next_scan"
        ? actionResult.value("checked").toULongLong()
        : actionResult.value("checkedBytes").toULongLong();
    const qulonglong unreadable = actionResult.value("unreadable").toULongLong();
    const QVariantList samples = actionResult.value("debugSamples").toList();

    QStringList parts;
    if (tool == "next_scan") {
        parts.append(KE_TXT("J'ai comparé %1 adresse(s).", "I compared %1 address(es).").arg(checked));
        if (unreadable > 0) {
            parts.append(KE_TXT("%1 adresse(s) étaient illisibles.", "%1 address(es) were unreadable.").arg(unreadable));
        }
    } else {
        parts.append(KE_TXT("La comparaison unknown a parcouru %1 octet(s).", "The unknown comparison scanned %1 byte(s).").arg(checked));
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
            sampleTexts.append(KE_TXT("0x%1 : illisible", "0x%1: unreadable").arg(address));
        }
    }
    if (!sampleTexts.isEmpty()) {
        parts.append(KE_TXT("Exemples : %1.", "Examples: %1.").arg(sampleTexts.join(", ")));
    }
    if (readableSamples > 0 && unchangedSamples == readableSamples) {
        parts.append(KE_TXT("Les exemples n'ont pas bougé : la première recherche a probablement capturé des copies, une valeur miroir, ou une représentation qui ne suit pas la valeur affichée.",
                             "The examples haven't moved: the first search probably captured copies, a mirrored value, or a representation that doesn't follow the displayed value."));
    }

    parts.append(KE_TXT("Restaure la réduction précédente, puis essaie une réduction changed/increased ou une nouvelle recherche en Float32 / valeur x100.",
                         "Restore the previous reduction, then try a changed/increased reduction or a new search in Float32 / x100 value."));
    return parts.join(' ');
}


} // namespace

ScanningCoreManager::ScanningCoreManager(ApplicationController& controller, QObject* parent)
    : QObject(parent)
    , m_controller(controller) {}

bool ScanningCoreManager::isScanInProgress() const {
    return m_controller.m_scanInProgress;
}

void ScanningCoreManager::requestCancelActiveScan() {
    if (m_controller.m_activeScanCancellation) {
        m_controller.m_activeScanCancellation->cancel();
    }
}

ScanStateAccess ScanningCoreManager::scanState() {
    return m_controller.scanState();
}

ScanStateAccess ScanningCoreManager::scanState() const {
    return m_controller.scanState();
}

void ScanningCoreManager::clearCandidateUndo() {
    m_controller.clearCandidateUndo();
}

void ScanningCoreManager::clearCandidateValueHistory() {
    m_controller.clearCandidateValueHistory();
}

bool ScanningCoreManager::rememberCandidatesForUndo(QString* error) {
    return m_controller.rememberCandidatesForUndo(error);
}

void ScanningCoreManager::recordCandidateObservations(const QVariantList& observations) {
    m_controller.recordCandidateObservations(observations);
}

void ScanningCoreManager::detectStableCandidateGroup(killcore::NextScanMode mode, const QList<killcore::Candidate>& survivors, QVariantMap* result) {
    m_controller.detectStableCandidateGroup(mode, survivors, result);
}

void ScanningCoreManager::appendSmartSearchDebug(const QString& event, const QVariantMap& payload) const {
    m_controller.appendSmartSearchDebug(event, payload);
}

void ScanningCoreManager::appendScanTelemetry(const QString& event, const QVariantMap& payload) const {
    m_controller.appendScanTelemetry(event, payload);
}

bool ScanningCoreManager::hasAddressBeenWriteVerified(uint64_t address) const {
    return m_controller.hasAddressBeenWriteVerified(address);
}

void ScanningCoreManager::scanStarted() {
    emit m_controller.scanStarted();
}

void ScanningCoreManager::scanProgress(int percent) {
    emit m_controller.scanProgress(percent);
}

void ScanningCoreManager::scanStatsUpdated(int candidateCount) {
    emit m_controller.scanStatsUpdated(candidateCount);
}

void ScanningCoreManager::scanFinished(const QVariantMap& result) {
    emit m_controller.scanFinished(result);
}
QVariantMap ScanningCoreManager::startExactScan(const QString& value, const QString& valueType) {
    QVariantMap result;
    QVariantList matches;
    result["success"] = false;

    if (!m_controller.m_handle.isValid()) {
        result["error"] = KE_TXT("Aucun processus attaché.", "No process attached.");
        result["matches"] = matches;
        return result;
    }

    killcore::ValueType type;
    if (!killcore::parseValueType(valueType, &type)) {
        result["error"] = KE_TXT("Type de valeur invalide.", "Invalid value type.");
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
    killcore::ScanEngine scanner(m_controller.m_handle);
    const auto scan = scanner.exactScan(scanValue, options);
    const qint64 elapsedMs = timer.elapsed();
    emit scanProgress(90);
    clearCandidateUndo();
    clearCandidateValueHistory();
    auto& candidates = scanState().candidates();
    candidates.replaceFromScan(scan, killcore::scanValueToBytes(scanValue));

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
    result["candidateStoreSize"] = static_cast<qulonglong>(candidates.size());
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
    emit scanStatsUpdated(static_cast<int>(candidates.size()));
    emit scanProgress(100);
    return result;
}



QVariantMap ScanningCoreManager::startExactScanMultiType(const QString& value, const QString& valueType) {
    QVariantMap result;
    QVariantList matches;
    result["success"] = false;

    if (!m_controller.m_handle.isValid()) {
        result["error"] = KE_TXT("Aucun processus attaché.", "No process attached.");
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
        result["error"] = KE_TXT("Type de valeur invalide.", "Invalid value type.");
        result["matches"] = matches;
        return result;
    }

    const auto valueVariants = smartAuto
        ? smartAutoScanVariants(value)
        : killcore::generateScanVariants(value, explicitType, explicitTypeGiven);
    if (valueVariants.isEmpty()) {
        result["error"] = KE_TXT("Impossible de parser '%1' comme valeur numérique.", "Could not parse '%1' as a numeric value.").arg(value);
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
    killcore::ScanEngine scanner(m_controller.m_handle);
    const auto scan = scanner.exactScanMultiType(variants, options);
    const qint64 elapsedMs = timer.elapsed();
    emit scanProgress(90);
    clearCandidateUndo();
    clearCandidateValueHistory();
    auto& candidateStore = scanState().candidates();
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
    candidateStore.replaceCandidates(candidates);

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
    result["candidateStoreSize"] = static_cast<qulonglong>(candidateStore.size());
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
    emit scanStatsUpdated(static_cast<int>(candidateStore.size()));
    emit scanProgress(100);
    return result;
}



QVariantMap ScanningCoreManager::startExactScanExpert(
    const QString& value,
    const QString& valueType,
    const QVariantMap& expertOptions) {
    QVariantMap result;
    QVariantList matches;
    result["success"] = false;

    if (!m_controller.m_handle.isValid()) {
        result["error"] = KE_TXT("Aucun processus attaché.", "No process attached.");
        result["matches"] = matches;
        return result;
    }

    killcore::ValueType type;
    if (!killcore::parseValueType(valueType, &type)) {
        result["error"] = KE_TXT("Type de valeur invalide.", "Invalid value type.");
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
                result["error"] = KE_TXT("Adresse de début invalide.", "Invalid start address.");
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
                result["error"] = KE_TXT("Adresse de fin invalide.", "Invalid end address.");
                result["matches"] = matches;
                return result;
            }
        }
    }
    if (options.startAddress != 0
        && options.stopAddress != 0
        && options.stopAddress <= options.startAddress) {
        result["error"] = KE_TXT("La fin de plage doit être supérieure au début.", "The range end must be greater than the start.");
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
    killcore::ScanEngine scanner(m_controller.m_handle);
    const auto scan = scanner.exactScan(scanValue, options);
    const qint64 elapsedMs = timer.elapsed();
    emit scanProgress(90);
    clearCandidateUndo();
    clearCandidateValueHistory();
    auto& candidates = scanState().candidates();
    candidates.replaceFromScan(scan, killcore::scanValueToBytes(scanValue));

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
    result["candidateStoreSize"] = static_cast<qulonglong>(candidates.size());
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
    emit scanStatsUpdated(static_cast<int>(candidates.size()));
    emit scanProgress(100);
    return result;
}



QVariantMap ScanningCoreManager::scanEncryptedValue(const QString& value, const QString& valueType, const QVariantMap& optionsMap) {
    QVariantMap result;
    QVariantList matches;
    result["success"] = false;
    result["matches"] = matches;

    if (!m_controller.m_handle.isValid()) {
        result["error"] = KE_TXT("Aucun processus attaché.", "No process attached.");
        return result;
    }

    killcore::ValueType type;
    if (!killcore::parseValueType(valueType, &type)) {
        result["error"] = KE_TXT("Type invalide pour scan chiffré.", "Invalid type for encrypted scan.");
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
        result["error"] = KE_TXT("Le scan chiffré v1 supporte seulement les types entiers.", "Encrypted scan v1 only supports integer types.");
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
        result["error"] = KE_TXT("Mode chiffré invalide. Utilise xor, add, sub ou not.", "Invalid encrypted mode. Use xor, add, sub, or not.");
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
        result["error"] = KE_TXT("Clé invalide.", "Invalid key.");
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
    killcore::MemoryReader reader(m_controller.m_handle);
    const auto regions = killcore::MemoryMap::snapshot(m_controller.m_handle);
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



QVariantMap ScanningCoreManager::startExactScanAsync(
    const QString& value,
    const QString& valueType,
    const QVariantMap& expertOptions) {
    QVariantMap result;
    QVariantList matches;
    result["success"] = false;
    result["started"] = false;

    if (m_controller.m_scanInProgress) {
        result["error"] = KE_TXT("Un scan est déjà en cours.", "A scan is already in progress.");
        return result;
    }

    if (!m_controller.m_handle.isValid()) {
        result["error"] = KE_TXT("Aucun processus attaché.", "No process attached.");
        result["matches"] = matches;
        return result;
    }

    killcore::ValueType type;
    if (!killcore::parseValueType(valueType, &type)) {
        result["error"] = KE_TXT("Type de valeur invalide.", "Invalid value type.");
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
                result["error"] = KE_TXT("Adresse de début invalide.", "Invalid start address.");
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
                result["error"] = KE_TXT("Adresse de fin invalide.", "Invalid end address.");
                result["matches"] = matches;
                return result;
            }
            options.stopAddress = stopAddress;
        }
    }
    if (options.startAddress != 0
        && options.stopAddress != 0
        && options.stopAddress <= options.startAddress) {
        result["error"] = KE_TXT("La fin de plage doit être supérieure au début.", "The range end must be greater than the start.");
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

    const int requestId = m_controller.m_nextScanRequestId++;
    const int pid = m_controller.m_pid;
    const QByteArray scannedBytes = killcore::scanValueToBytes(scanValue);
    const QPointer<ApplicationController> self(&m_controller);
    auto cancellation = std::make_shared<killcore::CancellationToken>();

    m_controller.m_scanInProgress = true;
    m_controller.m_activeScanCancellation = cancellation;
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
            scan.errorMessage = KE_TXT("Impossible d'ouvrir le processus dans le worker de scan.", "Could not open the process in the scan worker.");
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
                self->scanState().candidates().replaceFromScan(scan, scannedBytes);
            }
            const auto& candidates = self->scanState().candidates();

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
            finished["candidateStoreSize"] = static_cast<qulonglong>(candidates.size());
            finished["elapsedMs"] = static_cast<qulonglong>(elapsedMs);
            finished["bytesPerSecond"] = ratePerSecond(scan.bytesScanned, elapsedMs);
            finished["matchesPerSecond"] = ratePerSecond(scan.matchesFound, elapsedMs);
            finished["candidateStoreFileBacked"] = candidates.isFileBacked();
            finished["candidateStoreBytes"] = static_cast<qulonglong>(candidates.storageBytes());
            finished["candidateStoreMemoryBytes"] = static_cast<qulonglong>(candidates.estimatedMemoryBytes());
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
            emit self->scanStatsUpdated(static_cast<int>(candidates.size()));
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



QVariantMap ScanningCoreManager::cancelActiveScan() {
    QVariantMap result;
    result["success"] = false;
    if (!m_controller.m_scanInProgress || !m_controller.m_activeScanCancellation) {
        result["error"] = KE_TXT("Aucun scan actif à annuler.", "No active scan to cancel.");
        return result;
    }
    m_controller.m_activeScanCancellation->cancel();
    result["success"] = true;
    result["error"] = "";
    return result;
}



QVariantMap ScanningCoreManager::nextScanAsync(const QString& mode, const QString& value) {
    QVariantMap result;
    result["success"] = false;
    result["started"] = false;

    if (m_controller.m_scanInProgress) {
        result["error"] = KE_TXT("Un scan est déjà en cours.", "A scan is already in progress.");
        return result;
    }
    if (!m_controller.m_handle.isValid()) {
        result["error"] = KE_TXT("Aucun processus attaché.", "No process attached.");
        return result;
    }
    auto& candidates = scanState().candidates();
    if (candidates.isEmpty()) {
        result["error"] = KE_TXT("Aucun candidat à filtrer. Lance d'abord un scan exact.", "No candidates to filter. Run an exact scan first.");
        return result;
    }

    killcore::NextScanMode scanMode;
    if (!killcore::parseNextScanMode(mode, &scanMode)) {
        result["error"] = KE_TXT("Mode de next scan invalide.", "Invalid next scan mode.");
        return result;
    }

    killcore::Candidate firstCandidate;
    if (!candidates.firstCandidate(&firstCandidate)) {
        result["error"] = KE_TXT("Impossible de lire le premier candidat.", "Could not read the first candidate.");
        return result;
    }

    const auto candidateSnapshot = candidates.streamSnapshot();
    const auto firstCandidateType = firstCandidate.type;
    const auto candidateThreshold = candidates.fileBackedThreshold();
    double targetNumber = 0.0;
    if (scanMode == killcore::NextScanMode::Exact && value.trimmed().isEmpty()) {
        result["error"] = KE_TXT("Valeur requise pour un next scan exact.", "A value is required for an exact next scan.");
        return result;
    }
    if (scanMode == killcore::NextScanMode::Delta) {
        bool ok = false;
        targetNumber = value.trimmed().replace(',', '.').toDouble(&ok);
        if (!ok) {
            result["error"] = KE_TXT("Valeur delta invalide.", "Invalid delta value.");
            return result;
        }
    }

    double rangeMin = 0.0;
    double rangeMax = 0.0;
    if (scanMode == killcore::NextScanMode::Between) {
        // Meme regle que la version synchrone de nextScan() : separateurs ','/';'
        // uniquement, pas de '-' (ambigu avec un nombre negatif).
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
            result["error"] = KE_TXT(
                "Plage invalide. Utilise le format \"min,max\" (ex. 50,100). "
                "Le séparateur '-' n'est pas supporté (ambigu avec un nombre négatif).",
                "Invalid range. Use the \"min,max\" format (e.g. 50,100). "
                "The '-' separator isn't supported (ambiguous with a negative number).");
            return result;
        }
        if (rangeMin > rangeMax) {
            result["error"] = KE_TXT("Plage invalide : min doit être ≤ max.", "Invalid range: min must be ≤ max.");
            return result;
        }
    }

    const int requestId = m_controller.m_nextScanRequestId++;
    const int pid = m_controller.m_pid;
    const QPointer<ApplicationController> self(&m_controller);
    auto cancellation = std::make_shared<killcore::CancellationToken>();
    // Copié sur le thread GUI (m_candidateValueHistory n'est pas thread-safe)
    // pour être capturé par valeur dans le worker — voir buildConfidenceValueHistory().
    const auto confidenceHistory = buildConfidenceValueHistory(m_controller.m_candidateValueHistory);

    m_controller.m_scanInProgress = true;
    m_controller.m_activeScanCancellation = cancellation;
    emit scanStarted();
    emit scanProgress(0);

    std::thread([self, requestId, pid, mode, value, scanMode, firstCandidateType, candidateSnapshot, candidateThreshold, targetNumber, rangeMin, rangeMax, cancellation, confidenceHistory]() mutable {
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
            error = KE_TXT("Impossible d'ouvrir le processus dans le worker de next scan.", "Could not open the process in the next scan worker.");
        } else {
            killcore::MemoryReader reader(workerHandle);
            QString streamError;
            const auto confidenceRegions = killcore::MemoryMap::snapshot(workerHandle);
            killcore::CandidateConfidenceContext confidenceContext;
            confidenceContext.regions = &confidenceRegions;
            confidenceContext.valueHistory = &confidenceHistory;
            const bool completed = killcore::CandidateStore::forEachCandidate(candidateSnapshot, [&](const killcore::Candidate& candidate) {
                if (cancellation->isCancelled()) {
                    cancelled = true;
                    error = KE_TXT("Next scan annulé.", "Next scan cancelled.");
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
                                ? KE_TXT("Impossible de construire la valeur cible pour un candidat.", "Could not build the target value for a candidate.")
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
                    case killcore::NextScanMode::Delta: {
                        const double scaledTarget = targetDeltaForCandidate(targetNumber, candidate);
                        const double tolerance = (candidate.type == killcore::ValueType::Float32
                            || candidate.type == killcore::ValueType::Float64) ? 0.0001 : 0.5;
                        keep = std::abs((currentNumber - previousNumber) - scaledTarget) < tolerance;
                        break;
                    }
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
                        valueHistoryUpdates.append(candidateObservationToVariantMap(updatedCandidate, current, "next_scan_async", true, true));
                    }
                    auto updated = updatedCandidate;
                    updated.lastValue = current;
                    confidenceContext.secondaryVariant = updated.secondaryVariant;
                    updated.confidence = killcore::computeCandidateConfidence(updated.address, updated.type, confidenceContext);
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
                error = streamError.isEmpty() ? KE_TXT("Next scan interrompu.", "Next scan interrupted.") : streamError;
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
                    finishError = undoError.isEmpty() ? KE_TXT("Impossible de préparer l'annulation du next scan.", "Could not prepare the next scan's cancellation.") : undoError;
                } else {
                    if (streamOutput) {
                        self->scanState().candidates() = std::move(survivors);
                    } else {
                        self->scanState().candidates().replaceCandidates(memorySurvivors);
                    }
                    self->recordCandidateObservations(valueHistoryUpdates);
                }
            }
            const auto& candidates = self->scanState().candidates();
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
            finished["remaining"] = static_cast<qulonglong>(cancelled ? candidateSnapshot.totalCount : candidates.size());
            finished["error"] = finishError;
            finished["debugBeforeCount"] = static_cast<qulonglong>(candidateSnapshot.totalCount);
            finished["debugMode"] = mode;
            finished["debugValue"] = value;
            finished["streamInput"] = streamInput;
            finished["streamOutput"] = streamOutput;
            finished["fileBacked"] = candidates.isFileBacked();
            finished["candidateStorePath"] = candidates.backingFilePath();
            finished["elapsedMs"] = static_cast<qulonglong>(elapsedMs);
            finished["candidatesPerSecond"] = ratePerSecond(checked, elapsedMs);
            finished["candidateStoreBytes"] = static_cast<qulonglong>(candidates.storageBytes());
            finished["candidateStoreMemoryBytes"] = static_cast<qulonglong>(candidates.estimatedMemoryBytes());
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
            emit self->scanStatsUpdated(static_cast<int>(candidates.size()));
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



QVariantMap ScanningCoreManager::nextScan(const QString& mode, const QString& value) {
    QVariantMap result;
    result["success"] = false;

    if (!m_controller.m_handle.isValid()) {
        result["error"] = KE_TXT("Aucun processus attaché.", "No process attached.");
        return result;
    }

    auto& candidates = scanState().candidates();
    if (candidates.isEmpty()) {
        result["error"] = KE_TXT("Aucun candidat à filtrer. Lance d'abord un scan exact.", "No candidates to filter. Run an exact scan first.");
        return result;
    }

    killcore::NextScanMode scanMode;
    if (!killcore::parseNextScanMode(mode, &scanMode)) {
        result["error"] = KE_TXT("Mode de next scan invalide.", "Invalid next scan mode.");
        return result;
    }

    double targetNumber = 0.0;
    const auto firstCandidateType = candidates.candidates().first().type;

    if (scanMode == killcore::NextScanMode::Exact && value.trimmed().isEmpty()) {
        result["error"] = KE_TXT("Valeur requise pour un next scan exact.", "A value is required for an exact next scan.");
        return result;
    }
    if (scanMode == killcore::NextScanMode::Delta) {
        bool ok = false;
        targetNumber = value.trimmed().replace(',', '.').toDouble(&ok);
        if (!ok) {
            result["error"] = KE_TXT("Valeur delta invalide.", "Invalid delta value.");
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
            result["error"] = KE_TXT(
                "Plage invalide. Utilise le format \"min,max\" (ex. 50,100). "
                "Le séparateur '-' n'est pas supporté (ambigu avec un nombre négatif).",
                "Invalid range. Use the \"min,max\" format (e.g. 50,100). "
                "The '-' separator isn't supported (ambiguous with a negative number).");
            return result;
        }
        if (rangeMin > rangeMax) {
            result["error"] = KE_TXT("Plage invalide : min doit être ≤ max.", "Invalid range: min must be ≤ max.");
            return result;
        }
    }

    emit scanStarted();
    emit scanProgress(0);
    killcore::MemoryReader reader(m_controller.m_handle);
    QList<killcore::Candidate> survivors;
    survivors.reserve(candidates.candidates().size());

    const size_t beforeCount = candidates.size();
    size_t checked = 0;
    size_t unreadable = 0;
    QVariantList debugSamples;
    QVariantList valueHistoryUpdates;

    // Score de confiance recalculé par candidat survivant : region snapshot +
    // historique de valeurs déjà accumulé par les next_scan précédents
    // (m_candidateValueHistory), voir buildConfidenceValueHistory().
    const auto confidenceRegions = killcore::MemoryMap::snapshot(m_controller.m_handle);
    const auto confidenceHistory = buildConfidenceValueHistory(m_controller.m_candidateValueHistory);
    killcore::CandidateConfidenceContext confidenceContext;
    confidenceContext.regions = &confidenceRegions;
    confidenceContext.valueHistory = &confidenceHistory;

    for (const auto& candidate : candidates.candidates()) {
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
                        ? KE_TXT("Impossible de construire la valeur cible pour un candidat.", "Could not build the target value for a candidate.")
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
            case killcore::NextScanMode::Delta: {
                const double scaledTarget = targetDeltaForCandidate(targetNumber, candidate);
                const double tolerance = (candidate.type == killcore::ValueType::Float32
                    || candidate.type == killcore::ValueType::Float64) ? 0.0001 : 0.5;
                keep = std::abs((currentNumber - previousNumber) - scaledTarget) < tolerance;
                break;
            }
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
            confidenceContext.secondaryVariant = updated.secondaryVariant;
            updated.confidence = killcore::computeCandidateConfidence(updated.address, updated.type, confidenceContext);
            survivors.append(updated);
        }
    }

    QString undoError;
    if (!rememberCandidatesForUndo(&undoError)) {
        result["error"] = undoError.isEmpty() ? KE_TXT("Impossible de préparer l'annulation du next scan.", "Could not prepare the next scan's cancellation.") : undoError;
        emit scanProgress(100);
        return result;
    }
    candidates.replaceCandidates(survivors);
    recordCandidateObservations(valueHistoryUpdates);
    detectStableCandidateGroup(scanMode, survivors, &result);

    result["success"] = true;
    result["checked"] = static_cast<qulonglong>(checked);
    result["unreadable"] = static_cast<qulonglong>(unreadable);
    result["remaining"] = static_cast<qulonglong>(candidates.size());
    result["error"] = "";
    result["debugBeforeCount"] = static_cast<qulonglong>(beforeCount);
    result["debugMode"] = mode;
    result["debugValue"] = value;
    if (candidates.size() == 0) {
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
    emit scanStatsUpdated(static_cast<int>(candidates.size()));
    emit scanProgress(100);
    return result;
}



QVariantMap ScanningCoreManager::undoCandidateScan() {
    QVariantMap result;
    result["success"] = false;
    result["restored"] = false;
    auto state = scanState();
    auto& candidates = state.candidates();
    auto& previousCandidates = state.previousCandidates();
    result["count"] = static_cast<qulonglong>(candidates.size());

    if (m_controller.m_scanInProgress) {
        result["error"] = KE_TXT("Impossible de restaurer pendant un scan actif.", "Cannot restore while a scan is active.");
        return result;
    }

    if (!m_controller.m_hasPreviousCandidates || previousCandidates.isEmpty()) {
        result["error"] = KE_TXT("Aucune réduction précédente à restaurer.", "No previous reduction to restore.");
        return result;
    }

    candidates = std::move(previousCandidates);
    m_controller.m_hasPreviousCandidates = false;

    result["success"] = true;
    result["restored"] = true;
    result["count"] = static_cast<qulonglong>(candidates.size());
    clearCandidateValueHistory();
    result["fileBacked"] = candidates.isFileBacked();
    result["candidateStorePath"] = candidates.backingFilePath();
    result["candidateStoreBytes"] = static_cast<qulonglong>(candidates.storageBytes());
    result["candidateStoreMemoryBytes"] = static_cast<qulonglong>(candidates.estimatedMemoryBytes());
    result["error"] = "";

    appendSmartSearchDebug("undo_candidate_scan", {
        {"restored", true},
        {"count", result.value("count")},
        {"fileBacked", result.value("fileBacked")},
        {"candidateStoreBytes", result.value("candidateStoreBytes")},
        {"candidateStoreMemoryBytes", result.value("candidateStoreMemoryBytes")},
    });
    emit scanStatsUpdated(static_cast<int>(candidates.size()));
    emit scanProgress(100);
    return result;
}



QVariantMap ScanningCoreManager::getCandidates(int pageIndex, int pageSize, const QString& addressFilter) const {
    QVariantMap result;
    QVariantList candidates;

    const int boundedPageIndex = std::max(0, pageIndex);
    const int boundedPageSize = std::clamp(pageSize, 1, 500);
    const bool hasAddressFilter = !addressFilter.trimmed().isEmpty();
    const auto& candidateStore = scanState().candidates();
    const bool displaySuppressed = !hasAddressFilter && candidateStore.size() > kCandidateDisplayLimit;

    if (displaySuppressed) {
        result["pageIndex"] = boundedPageIndex;
        result["pageSize"] = boundedPageSize;
        result["totalCount"] = static_cast<qulonglong>(candidateStore.size());
        result["displaySuppressed"] = true;
        result["displayLimit"] = static_cast<qulonglong>(kCandidateDisplayLimit);
        result["fileBacked"] = candidateStore.isFileBacked();
        result["candidateStorePath"] = candidateStore.backingFilePath();
        result["candidateStoreBytes"] = static_cast<qulonglong>(candidateStore.storageBytes());
        result["candidateStoreMemoryBytes"] = static_cast<qulonglong>(candidateStore.estimatedMemoryBytes());
        result["candidates"] = candidates;
        return result;
    }

    const auto page = candidateStore.page(
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
    result["fileBacked"] = candidateStore.isFileBacked();
    result["candidateStorePath"] = candidateStore.backingFilePath();
    result["candidateStoreBytes"] = static_cast<qulonglong>(candidateStore.storageBytes());
    result["candidateStoreMemoryBytes"] = static_cast<qulonglong>(candidateStore.estimatedMemoryBytes());
    result["candidates"] = candidates;
    return result;
}



QVariantMap ScanningCoreManager::captureUnknownSnapshot() {
    return captureUnknownSnapshotWithOptions({});
}



QVariantMap ScanningCoreManager::captureUnknownSnapshotWithOptions(const QVariantMap& expertOptions) {
    QVariantMap result;
    result["success"] = false;

    if (!m_controller.m_handle.isValid()) {
        result["error"] = KE_TXT("Aucun processus attaché.", "No process attached.");
        return result;
    }

    auto state = scanState();
    state.clearCandidates();
    clearCandidateUndo();
    clearCandidateValueHistory();
    m_controller.m_smartSearchActive = false;
    m_controller.m_smartSearchInitialValue.clear();
    m_controller.m_smartSearchTargetValue.clear();

    emit scanStarted();
    emit scanProgress(0);
    const auto options = scanOptionsFromSettingsAndExpertOptions(expertOptions);
    int suggestedDepthMb = 0;
    uint64_t relevantBytes = 0;
    const size_t maxSnapshotBytes = resolveUnknownSnapshotMaxBytes(
        expertOptions, m_controller.m_handle, options, &suggestedDepthMb, &relevantBytes);
    const auto snapshot = state.snapshot().capture(m_controller.m_handle, maxSnapshotBytes, nullptr, options);
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



QVariantMap ScanningCoreManager::captureUnknownSnapshotAsync() {
    return captureUnknownSnapshotAsyncWithOptions({});
}



QVariantMap ScanningCoreManager::captureUnknownSnapshotAsyncWithOptions(const QVariantMap& expertOptions) {
    QVariantMap result;
    result["success"] = false;
    result["started"] = false;

    if (m_controller.m_scanInProgress) {
        result["error"] = KE_TXT("Un scan est déjà en cours.", "A scan is already in progress.");
        return result;
    }
    if (!m_controller.m_handle.isValid()) {
        result["error"] = KE_TXT("Aucun processus attaché.", "No process attached.");
        return result;
    }

    scanState().clearCandidates();
    clearCandidateUndo();
    clearCandidateValueHistory();
    m_controller.m_smartSearchActive = false;
    m_controller.m_smartSearchInitialValue.clear();
    m_controller.m_smartSearchTargetValue.clear();

    const int requestId = m_controller.m_nextScanRequestId++;
    const int pid = m_controller.m_pid;
    int suggestedDepthMb = 0;
    uint64_t relevantBytes = 0;
    auto options = scanOptionsFromSettingsAndExpertOptions(expertOptions);
    const size_t maxSnapshotBytes = resolveUnknownSnapshotMaxBytes(
        expertOptions, m_controller.m_handle, options, &suggestedDepthMb, &relevantBytes);
    const QPointer<ApplicationController> self(&m_controller);
    auto cancellation = std::make_shared<killcore::CancellationToken>();

    m_controller.m_scanInProgress = true;
    m_controller.m_activeScanCancellation = cancellation;
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
            snapshot.errorMessage = KE_TXT("Impossible d'ouvrir le processus dans le worker unknown.", "Could not open the process in the unknown worker.");
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
                self->scanState().snapshot() = std::move(snapshotStore);
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



QVariantMap ScanningCoreManager::unknownNextScan(const QString& mode, const QString& valueType, const QString& deltaValue) {
    QVariantMap result;
    result["success"] = false;

    if (!m_controller.m_handle.isValid()) {
        result["error"] = KE_TXT("Aucun processus attaché.", "No process attached.");
        return result;
    }

    killcore::NextScanMode scanMode;
    if (!killcore::parseNextScanMode(mode, &scanMode)) {
        result["error"] = KE_TXT("Mode invalide.", "Invalid mode.");
        return result;
    }
    double displayedDelta = 0.0;
    if (scanMode == killcore::NextScanMode::Delta) {
        bool ok = false;
        displayedDelta = deltaValue.trimmed().replace(',', '.').toDouble(&ok);
        if (!ok) {
            result["error"] = KE_TXT("Valeur delta requise et invalide pour un unknown scan en mode delta.", "A delta value is required and invalid for an unknown scan in delta mode.");
            return result;
        }
    }
    auto state = scanState();
    auto& candidates = state.candidates();
    auto& snapshotStore = state.snapshot();
    if (!candidates.isEmpty()) {
        QVariantMap refined = nextScan(mode, scanMode == killcore::NextScanMode::Delta ? deltaValue : QString());
        refined["kind"] = "unknown_refine";
        refined["refinedFromCandidates"] = true;
        refined["checkedBytes"] = refined.value("checked");
        refined["matchesFound"] = refined.value("remaining");
        refined["stored"] = refined.value("remaining");
        return refined;
    }

    if (snapshotStore.isEmpty()) {
        result["error"] = KE_TXT("Aucun snapshot unknown capturé.", "No unknown snapshot captured.");
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
        result["error"] = KE_TXT("Type invalide.", "Invalid type.");
        emit scanProgress(100);
        return result;
    }

    const auto typesToRun = autoType ? compareTypes : QList<killcore::ValueType>{singleType};
    // Chaque type recoit un budget egal plutot qu'un plafond global partage :
    // sinon le premier type de la liste (Int32) peut a lui seul epuiser le
    // plafond et empecher les types suivants (Float32, Int16, ...) d'etre
    // testes du tout en mode Auto, ce qui fait disparaitre silencieusement
    // la bonne adresse si elle n'est pas du premier type essaye.
    const qsizetype perTypeCap = typesToRun.isEmpty()
        ? kUnknownAutoMaxReturnedMatches
        : std::max<qsizetype>(1, kUnknownAutoMaxReturnedMatches / typesToRun.size());
    QStringList truncatedTypes;
    for (const auto type : typesToRun) {
        // SC2-UNKNOWN-1 : en mode Delta, teste un delta AFFICHÉ (ex. "+7")
        // contre chaque représentation fixed-point du type courant
        // (x1/x10/x100/x1000/x4096/x65536, cf. generateDeltaVariants) — sinon
        // un score interne stocké en virgule fixe (135 affiché = 552960
        // brut) ne matcherait jamais un delta affiché "+7" (delta brut réel
        // 28672 pour x4096). Pour les autres modes, un seul passage "neutre"
        // (pas de targetDelta/label à appliquer).
        const auto deltaVariants = (scanMode == killcore::NextScanMode::Delta)
            ? killcore::generateDeltaVariants(displayedDelta, type)
            : QList<killcore::DeltaVariant>{killcore::DeltaVariant{}};

        bool typeSuccess = true;
        bool typePartial = false;
        size_t typeCheckedBytes = 0;
        size_t typeMatchesFound = 0;
        QString typeError;
        qsizetype addedForType = 0;

        for (const auto& deltaVariant : deltaVariants) {
            if (addedForType >= perTypeCap) {
                break; // Deja au plafond pour ce type : inutile de tester les echelles restantes.
            }

            killcore::ScanOptions scanOptions;
            if (scanMode == killcore::NextScanMode::Delta) {
                scanOptions.targetDelta = deltaVariant.rawDelta;
                scanOptions.matchVariantLabel = deltaVariant.label;
            }
            const auto scan = snapshotStore.compare(m_controller.m_handle, type, scanMode, nullptr, scanOptions);
            checkedBytes += scan.checkedBytes;
            matchesFound += scan.matchesFound;
            typeCheckedBytes += scan.checkedBytes;
            typeMatchesFound += scan.matchesFound;
            partial = partial || scan.partial;
            typePartial = typePartial || scan.partial;
            cancelled = cancelled || scan.cancelled;
            compareSuccess = compareSuccess && scan.success;
            typeSuccess = typeSuccess && scan.success;
            if (!scan.errorMessage.isEmpty() && compareError.isEmpty()) {
                compareError = scan.errorMessage;
            }
            if (!scan.errorMessage.isEmpty() && typeError.isEmpty()) {
                typeError = scan.errorMessage;
            }

            const auto typeCandidates = candidatesFromUnknownScan(m_controller.m_handle, scan);
            for (const auto& candidate : typeCandidates) {
                const QString key = QString::number(candidate.address, 16) + "|" + killcore::valueTypeToString(candidate.type);
                if (seenCandidates.contains(key)) {
                    continue;
                }
                if (addedForType >= perTypeCap) {
                    partial = true;
                    typePartial = true;
                    if (!truncatedTypes.contains(killcore::valueTypeToString(type))) {
                        truncatedTypes.append(killcore::valueTypeToString(type));
                    }
                    break;
                }
                seenCandidates.insert(key);
                unknownCandidates.append(candidate);
                ++addedForType;
            }

            if (cancelled) {
                break;
            }
        }

        typeSummaries.append(QVariantMap{
            {"type", killcore::valueTypeToString(type)},
            {"success", typeSuccess},
            {"partial", typePartial || addedForType >= perTypeCap},
            {"checkedBytes", static_cast<qulonglong>(typeCheckedBytes)},
            {"matchesFound", static_cast<qulonglong>(typeMatchesFound)},
            {"stored", addedForType},
            {"error", typeError},
        });
        if (cancelled) {
            break;
        }
    }
    if (!truncatedTypes.isEmpty() && compareError.isEmpty()) {
        compareError = KE_TXT(
            "Limite de %1 candidats/type atteinte pour : %2. Tous les types ont ete testes ; "
            "raffine avec changed/increased/decreased pour reduire.",
            "Limit of %1 candidates/type reached for: %2. All types have been tested; "
            "refine with changed/increased/decreased to narrow down.")
                            .arg(perTypeCap)
                            .arg(truncatedTypes.join(", "));
    }
    emit scanProgress(90);

    QString undoError;
    if (!candidates.isEmpty() && !rememberCandidatesForUndo(&undoError)) {
        result["error"] = undoError.isEmpty() ? KE_TXT("Impossible de préparer l'annulation de la comparaison.", "Could not prepare the comparison's cancellation.") : undoError;
        emit scanProgress(100);
        return result;
    }
    candidates.replaceCandidates(unknownCandidates);

    result["success"] = compareSuccess;
    result["partial"] = partial;
    result["cancelled"] = cancelled;
    result["checkedBytes"] = static_cast<qulonglong>(checkedBytes);
    result["matchesFound"] = static_cast<qulonglong>(matchesFound);
    result["stored"] = static_cast<qulonglong>(candidates.size());
    result["valueType"] = autoType ? QString("Auto") : valueType;
    result["typePasses"] = typeSummaries;
    result["error"] = compareError;
    if (candidates.size() == 0 && compareSuccess) {
        result["diagnostic"] = noCandidateDiagnosticMessage(result, "unknown_compare");
    }
    emit scanStatsUpdated(static_cast<int>(candidates.size()));
    emit scanProgress(100);
    return result;
}



QVariantMap ScanningCoreManager::unknownNextScanAsync(const QString& mode, const QString& valueType, const QString& deltaValue) {
    QVariantMap result;
    result["success"] = false;
    result["started"] = false;

    if (m_controller.m_scanInProgress) {
        result["error"] = KE_TXT("Un scan est déjà en cours.", "A scan is already in progress.");
        return result;
    }
    if (!m_controller.m_handle.isValid()) {
        result["error"] = KE_TXT("Aucun processus attaché.", "No process attached.");
        return result;
    }

    killcore::NextScanMode scanMode;
    if (!killcore::parseNextScanMode(mode, &scanMode)) {
        result["error"] = KE_TXT("Mode invalide.", "Invalid mode.");
        return result;
    }
    double displayedDelta = 0.0;
    if (scanMode == killcore::NextScanMode::Delta) {
        bool ok = false;
        displayedDelta = deltaValue.trimmed().replace(',', '.').toDouble(&ok);
        if (!ok) {
            result["error"] = KE_TXT("Valeur delta requise et invalide pour un unknown scan en mode delta.", "A delta value is required and invalid for an unknown scan in delta mode.");
            return result;
        }
    }
    auto state = scanState();
    auto& candidates = state.candidates();
    auto& snapshotStore = state.snapshot();
    if (!candidates.isEmpty()) {
        const size_t refineCandidateCount = candidates.size();
        appendScanTelemetry("unknown_refine_async_start", {
            {"mode", mode},
            {"valueType", valueType},
            {"candidateCount", static_cast<qulonglong>(refineCandidateCount)},
            {"fileBacked", candidates.isFileBacked()},
            {"candidateStoreBytes", static_cast<qulonglong>(candidates.storageBytes())},
            {"candidateStoreMemoryBytes", static_cast<qulonglong>(candidates.estimatedMemoryBytes())},
        });
        if (refineCandidateCount <= 20000) {
            QElapsedTimer timer;
            timer.start();
            QVariantMap refined = nextScan(mode, scanMode == killcore::NextScanMode::Delta ? deltaValue : QString());
            refined["requestId"] = m_controller.m_nextScanRequestId++;
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
        QVariantMap refined = nextScanAsync(mode, scanMode == killcore::NextScanMode::Delta ? deltaValue : QString());
        refined["refinedFromCandidates"] = true;
        return refined;
    }

    if (snapshotStore.isEmpty()) {
        result["error"] = KE_TXT("Aucun snapshot unknown capturé.", "No unknown snapshot captured.");
        return result;
    }
    const QString normalizedValueType = valueType.trimmed();
    const bool autoType = normalizedValueType.compare("Auto", Qt::CaseInsensitive) == 0
        || normalizedValueType.compare("SmartAuto", Qt::CaseInsensitive) == 0
        || normalizedValueType.isEmpty();
    killcore::ValueType singleType = killcore::ValueType::Int32;
    if (!autoType && !killcore::parseValueType(valueType, &singleType)) {
        result["error"] = KE_TXT("Type invalide.", "Invalid type.");
        return result;
    }
    const auto typesToRun = autoType ? unknownAutoValueTypes() : QList<killcore::ValueType>{singleType};

    const int requestId = m_controller.m_nextScanRequestId++;
    const int pid = m_controller.m_pid;
    const QPointer<ApplicationController> self(&m_controller);
    auto cancellation = std::make_shared<killcore::CancellationToken>();

    m_controller.m_scanInProgress = true;
    m_controller.m_activeScanCancellation = cancellation;
    emit scanStarted();
    emit scanProgress(0);

    std::thread([self, requestId, pid, mode, valueType, autoType, typesToRun, scanMode, displayedDelta, cancellation]() {
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
            compareError = KE_TXT("Impossible d'ouvrir le processus dans le worker unknown.", "Could not open the process in the unknown worker.");
        } else if (self) {
            // Compare "Unknown" : relit toute la mémoire capturée par le
            // snapshot, une fois par type testé (jusqu'à 6 en mode Auto) —
            // combine la position dans typesToRun et la progression par
            // région à l'intérieur de chaque type pour une seule barre
            // cohérente plutôt que de sauter par paliers de 1/totalTypes.
            const int totalTypes = std::max<int>(1, static_cast<int>(typesToRun.size()));
            int typeIndex = 0;
            int lastUnknownProgress = 0;
            // Voir la version synchrone (unknownNextScan) pour le detail :
            // un plafond global partage entre types ferait sortir la boucle
            // des le premier type qui deborde (typiquement Int32), sans que
            // les types suivants soient jamais testes en mode Auto.
            const qsizetype perTypeCap = typesToRun.isEmpty()
                ? kUnknownAutoMaxReturnedMatches
                : std::max<qsizetype>(1, kUnknownAutoMaxReturnedMatches / typesToRun.size());
            QStringList truncatedTypes;
            for (const auto type : typesToRun) {
                // SC2-UNKNOWN-1 : voir la version synchrone (unknownNextScan)
                // pour le detail — en mode Delta, un delta affiche est teste
                // contre chaque echelle fixed-point du type courant.
                const auto deltaVariants = (scanMode == killcore::NextScanMode::Delta)
                    ? killcore::generateDeltaVariants(displayedDelta, type)
                    : QList<killcore::DeltaVariant>{killcore::DeltaVariant{}};

                bool typeSuccess = true;
                bool typePartial = false;
                size_t typeCheckedBytes = 0;
                size_t typeMatchesFound = 0;
                QString typeError;
                qsizetype addedForType = 0;

                for (const auto& deltaVariant : deltaVariants) {
                    if (addedForType >= perTypeCap) {
                        break;
                    }

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
                    if (scanMode == killcore::NextScanMode::Delta) {
                        compareOptions.targetDelta = deltaVariant.rawDelta;
                        compareOptions.matchVariantLabel = deltaVariant.label;
                    }
                    const auto scan = self->scanState().snapshot().compare(workerHandle, type, scanMode, cancellation.get(), compareOptions);
                    checkedBytes += scan.checkedBytes;
                    matchesFound += scan.matchesFound;
                    typeCheckedBytes += scan.checkedBytes;
                    typeMatchesFound += scan.matchesFound;
                    partial = partial || scan.partial;
                    typePartial = typePartial || scan.partial;
                    cancelled = cancelled || scan.cancelled;
                    compareSuccess = compareSuccess && scan.success;
                    typeSuccess = typeSuccess && scan.success;
                    if (!scan.errorMessage.isEmpty() && compareError.isEmpty()) {
                        compareError = scan.errorMessage;
                    }
                    if (!scan.errorMessage.isEmpty() && typeError.isEmpty()) {
                        typeError = scan.errorMessage;
                    }

                    const auto typeCandidates = scan.success && !scan.cancelled
                        ? candidatesFromUnknownScan(workerHandle, scan)
                        : QList<killcore::Candidate>{};
                    for (const auto& candidate : typeCandidates) {
                        const QString key = QString::number(candidate.address, 16) + "|" + killcore::valueTypeToString(candidate.type);
                        if (seenCandidates.contains(key)) {
                            continue;
                        }
                        if (addedForType >= perTypeCap) {
                            partial = true;
                            typePartial = true;
                            if (!truncatedTypes.contains(killcore::valueTypeToString(type))) {
                                truncatedTypes.append(killcore::valueTypeToString(type));
                            }
                            break;
                        }
                        seenCandidates.insert(key);
                        unknownCandidates.append(candidate);
                        ++addedForType;
                    }

                    if (cancelled) {
                        break;
                    }
                }
                ++typeIndex;

                typeSummaries.append(QVariantMap{
                    {"type", killcore::valueTypeToString(type)},
                    {"success", typeSuccess},
                    {"partial", typePartial || addedForType >= perTypeCap},
                    {"checkedBytes", static_cast<qulonglong>(typeCheckedBytes)},
                    {"matchesFound", static_cast<qulonglong>(typeMatchesFound)},
                    {"stored", addedForType},
                    {"error", typeError},
                });
                if (cancelled) {
                    break;
                }
            }
            if (!truncatedTypes.isEmpty() && compareError.isEmpty()) {
                compareError = KE_TXT(
                    "Limite de %1 candidats/type atteinte pour : %2. Tous les types ont ete testes ; "
                    "raffine avec changed/increased/decreased pour reduire.",
                    "Limit of %1 candidates/type reached for: %2. All types have been tested; "
                    "refine with changed/increased/decreased to narrow down.")
                                    .arg(perTypeCap)
                                    .arg(truncatedTypes.join(", "));
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
                auto& candidates = self->scanState().candidates();
                if (!candidates.isEmpty() && !self->rememberCandidatesForUndo(&undoError)) {
                    finishSuccess = false;
                    finishError = undoError.isEmpty()
                        ? KE_TXT("Impossible de préparer l'annulation de la comparaison.", "Could not prepare the comparison's cancellation.")
                        : undoError;
                } else {
                    candidates.replaceCandidates(unknownCandidates);
                }
            }
            const auto& candidates = self->scanState().candidates();

            QVariantMap finished;
            finished["requestId"] = requestId;
            finished["kind"] = "unknown_next";
            finished["success"] = finishSuccess;
            finished["partial"] = partial;
            finished["cancelled"] = cancelled;
            finished["checkedBytes"] = static_cast<qulonglong>(checkedBytes);
            finished["matchesFound"] = static_cast<qulonglong>(matchesFound);
            finished["stored"] = static_cast<qulonglong>(candidates.size());
            finished["valueType"] = autoType ? QString("Auto") : valueType;
            finished["typePasses"] = typeSummaries;
            finished["error"] = finishError;
            if (candidates.size() == 0 && finishSuccess) {
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
            emit self->scanStatsUpdated(static_cast<int>(candidates.size()));
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



QVariantMap ScanningCoreManager::scanGroupScan(const QVariantList& entriesList, const QVariantMap& optionsMap) {
    QVariantMap result;
    QVariantList matches;
    result["success"] = false;
    result["matches"] = matches;

    if (!m_controller.m_handle.isValid()) {
        result["error"] = KE_TXT("Aucun processus attaché.", "No process attached.");
        return result;
    }

    killcore::GroupScanOptions groupOptions;
    const int maxResults = std::clamp(optionsMap.value("maxResults", 1000).toInt(), 1, 10000);
    groupOptions.maxResults = static_cast<size_t>(maxResults);
    groupOptions.maxDistance = std::clamp(optionsMap.value("maxDistance", 256).toInt(), 4, 4096);

    if (entriesList.isEmpty()) {
        result["error"] = KE_TXT("Aucune entree pour le scan groupe.", "No entry for the group scan.");
        return result;
    }

    for (const auto& entryVariant : entriesList) {
        const QVariantMap entryMap = entryVariant.toMap();
        killcore::GroupScanEntry entry;

        const QString typeText = entryMap.value("type", "Int32").toString();
        if (!killcore::parseValueType(typeText, &entry.type)) {
            result["error"] = KE_TXT("Type invalide pour une entree du scan groupe : %1", "Invalid type for a group scan entry: %1").arg(typeText);
            return result;
        }

        bool offsetOk = false;
        entry.offset = entryMap.value("offset").toLongLong(&offsetOk);
        if (!offsetOk) {
            result["error"] = KE_TXT("Offset invalide pour une entree du scan groupe.", "Invalid offset for a group scan entry.");
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
    killcore::MemoryReader reader(m_controller.m_handle);
    const auto regions = killcore::MemoryMap::snapshot(m_controller.m_handle);
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



} // namespace killengine
