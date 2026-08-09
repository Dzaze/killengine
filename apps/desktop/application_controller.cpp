#include "application_controller.h"

#include "candidates/candidate_store.h"
#include "crash_handler.h"
#include "logging/logger.h"
#include "memory/memory_map.h"
#include "memory/memory_reader.h"
#include "memory/memory_writer.h"
#include "process/process_enumerator.h"
#include "process/process_handle.h"
#include "scanner/scan_engine.h"
#include "scanner/scan_types.h"
#include "snapshot/snapshot_store.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QPointer>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <thread>

namespace killengine {

namespace {

constexpr size_t kAutoWriteCandidateLimit = 4;
constexpr int kDefaultScanMaxResults = 1000000;
constexpr int kDefaultScanChunkSizeMb = 1;

enum class SmartSearchIntentKind {
    Unknown,
    ResetContext,
    ExactScan,
    GuidedScan,
    RefineScan,
    ActivateMemoryTargets,
    WriteMemoryTargets,
    RewriteLastTargets,
    WriteProfileTargets,
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
        case SmartSearchIntentKind::RewriteLastTargets:
            return "RewriteLastTargets";
        case SmartSearchIntentKind::WriteProfileTargets:
            return "WriteProfileTargets";
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

QVariantMap candidateToVariantMap(const killcore::Candidate& candidate) {
    QVariantMap entry;
    entry["address"] = QString::number(candidate.address, 16);
    entry["type"] = killcore::valueTypeToString(candidate.type);
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
    options.chunkSize = static_cast<size_t>(
        boundedSettingInt(settings, "scan/chunkSizeMb", kDefaultScanChunkSizeMb, 1, 64))
        * 1024
        * 1024;
    options.fastScan = settings.value("scan/fastScan", true).toBool();
    return options;
}

double bytesToDouble(const QByteArray& bytes, killcore::ValueType type) {
    if (bytes.size() < static_cast<qsizetype>(killcore::valueTypeSize(type))) {
        return 0.0;
    }

    switch (type) {
        case killcore::ValueType::Int32: {
            int32_t value = 0;
            std::memcpy(&value, bytes.constData(), sizeof(value));
            return static_cast<double>(value);
        }
        case killcore::ValueType::Int64: {
            int64_t value = 0;
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

QStringList numbersFromText(const QString& text) {
    QStringList values;
    const QRegularExpression re(R"([-+]?\d+(?:[\.,]\d+)?)");
    auto it = re.globalMatch(textWithoutHexAddresses(text));
    while (it.hasNext()) {
        values.append(it.next().captured(0).replace(',', '.'));
    }
    return values;
}

QString bytesToHex(const QByteArray& bytes) {
    return QString::fromLatin1(bytes.toHex(' '));
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
        || q.contains("ces adresse")
        || q.contains("ces adresses")
        || q.contains("les adresse")
        || q.contains("les adresses")
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

    if (intent.resetContext && numbers.isEmpty()) {
        intent.kind = SmartSearchIntentKind::ResetContext;
        intent.rationale = "L'utilisateur demande un nouveau contexte sans donner encore de valeur.";
    } else if (!addresses.isEmpty()) {
        intent.kind = hasOneNumber && wantsMemoryWrite
            ? SmartSearchIntentKind::WriteMemoryTargets
            : SmartSearchIntentKind::ActivateMemoryTargets;
        intent.rationale = "Le message contient une ou plusieurs adresses mémoire explicites.";
    } else if (hasChatMemoryTargets && hasOneNumber && !intent.resetContext) {
        intent.kind = SmartSearchIntentKind::WriteMemoryTargets;
        intent.rationale = "Des adresses mémoire sont actives dans la conversation.";
    } else if (hasLastAutoWriteTargets && hasOneNumber && !intent.resetContext && wantsLastRewrite) {
        intent.kind = SmartSearchIntentKind::RewriteLastTargets;
        intent.rationale = "L'utilisateur demande de modifier les dernières adresses écrites.";
    } else if (hasOneNumber && !intent.resetContext && wantsMemoryWrite) {
        intent.kind = SmartSearchIntentKind::WriteProfileTargets;
        intent.rationale = "L'utilisateur formule une intention d'écriture sur une cible nommée.";
    } else if (smartSearchActive && hasCandidates && hasOneNumber && !intent.resetContext) {
        intent.kind = SmartSearchIntentKind::RefineScan;
        intent.rationale = "Un scan guidé est actif et l'utilisateur donne une nouvelle valeur observée.";
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
        suggestions.append(suggestion);
    }
    return suggestions;
}

} // namespace

ApplicationController::ApplicationController(QObject* parent)
    : QObject(parent) {
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

    KE_LOG_INFO() << "Attached to PID " << pid << " (" << m_processName.toStdString() << ")";

    emit attachmentChanged();
    return true;
}

void ApplicationController::detachProcess() {
    KE_LOG_INFO() << "detachProcess()";

    m_handle.close();
    m_pid = 0;
    m_attached = false;
    m_processName.clear();
    m_candidates.clear();
    m_snapshot.clear();
    m_freeze.clear();
    m_freezeTimer.stop();
    m_lastWriteAddress = 0;
    m_lastWritePreviousValue.clear();
    m_writeHistory.clear();
    m_lastAutoWriteTargets.clear();
    m_chatMemoryTargets.clear();
    m_activeProfileTargets.clear();
    m_lastBatchStartIndex = -1;
    m_lastBatchEndIndex = -1;

    emit attachmentChanged();
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
    killcore::ScanEngine scanner(m_handle);
    const auto scan = scanner.exactScan(scanValue, options);
    emit scanProgress(90);
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
    appendSmartSearchDebug("exact_scan", {
        {"value", value},
        {"valueType", valueType},
        {"success", result.value("success")},
        {"partial", result.value("partial")},
        {"matchesFound", result.value("matchesFound")},
        {"candidateStoreSize", result.value("candidateStoreSize")},
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
    killcore::ScanEngine scanner(m_handle);
    const auto scan = scanner.exactScan(scanValue, options);
    emit scanProgress(90);
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

    m_scanInProgress = true;
    emit scanStarted();
    emit scanProgress(0);

    std::thread([self, requestId, pid, value, valueType, expertOptions, scanValue, options, scannedBytes]() {
        killcore::ScanResult scan;
        killcore::ProcessHandle workerHandle(static_cast<uint32_t>(pid), killcore::ProcessAccess::ReadOnly);
        if (!workerHandle.isValid()) {
            scan.success = false;
            scan.errorMessage = "Impossible d'ouvrir le processus dans le worker de scan.";
        } else {
            killcore::ScanEngine scanner(workerHandle);
            scan = scanner.exactScan(scanValue, options);
        }

        if (!self) {
            return;
        }
        QMetaObject::invokeMethod(self.data(), [self, requestId, value, valueType, expertOptions, scannedBytes, scan]() {
            if (!self) {
                return;
            }

            QVariantMap finished;
            QVariantList finishedMatches;
            self->m_candidates.replaceFromScan(scan, scannedBytes);

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
                {"error", finished.value("error")},
            });
            self->m_scanInProgress = false;
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

    killcore::ScanValue targetValue;
    QByteArray targetBytes;
    double targetNumber = 0.0;
    const auto candidateType = m_candidates.candidates().first().type;

    if (scanMode == killcore::NextScanMode::Exact || scanMode == killcore::NextScanMode::Delta) {
        QString parseError;
        if (!killcore::parseScanValue(value, candidateType, &targetValue, &parseError)) {
            result["error"] = parseError;
            return result;
        }
        targetBytes = killcore::scanValueToBytes(targetValue);
        targetNumber = bytesToDouble(targetBytes, candidateType);
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
        switch (scanMode) {
            case killcore::NextScanMode::Exact:
                keep = bytesEqual(current, targetBytes, candidate.type);
                break;
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
            auto updated = candidate;
            updated.lastValue = current;
            survivors.append(updated);
        }
    }

    m_candidates.replaceCandidates(survivors);

    result["success"] = true;
    result["checked"] = static_cast<qulonglong>(checked);
    result["unreadable"] = static_cast<qulonglong>(unreadable);
    result["remaining"] = static_cast<qulonglong>(m_candidates.size());
    result["error"] = "";
    result["debugBeforeCount"] = static_cast<qulonglong>(beforeCount);
    result["debugMode"] = mode;
    result["debugValue"] = value;
    result["debugSamples"] = debugSamples;
    appendSmartSearchDebug("next_scan", {
        {"mode", mode},
        {"value", value},
        {"candidateType", killcore::valueTypeToString(candidateType)},
        {"beforeCount", static_cast<qulonglong>(beforeCount)},
        {"checked", result.value("checked")},
        {"unreadable", result.value("unreadable")},
        {"remaining", result.value("remaining")},
        {"samples", debugSamples},
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
    result["candidates"] = candidates;
    return result;
}

QVariantMap ApplicationController::captureUnknownSnapshot() {
    QVariantMap result;
    result["success"] = false;

    if (!m_handle.isValid()) {
        result["error"] = "Aucun processus attaché.";
        return result;
    }

    emit scanStarted();
    emit scanProgress(0);
    const auto snapshot = m_snapshot.capture(m_handle);
    emit scanProgress(100);
    result["success"] = snapshot.success;
    result["partial"] = snapshot.partial;
    result["cancelled"] = snapshot.cancelled;
    result["regionsCaptured"] = static_cast<qulonglong>(snapshot.regionsCaptured);
    result["regionsSkipped"] = static_cast<qulonglong>(snapshot.regionsSkipped);
    result["bytesCaptured"] = static_cast<qulonglong>(snapshot.bytesCaptured);
    result["compressedBytes"] = static_cast<qulonglong>(snapshot.compressedBytes);
    result["mappedStorage"] = snapshot.mappedStorage;
    result["error"] = snapshot.errorMessage;
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

    killcore::ValueType type;
    if (!killcore::parseValueType(valueType, &type)) {
        result["error"] = "Type invalide.";
        return result;
    }

    emit scanStarted();
    emit scanProgress(0);
    const auto scan = m_snapshot.compare(m_handle, type, scanMode);
    emit scanProgress(90);
    killcore::ScanResult scanResult;
    scanResult.success = scan.success;
    scanResult.partial = scan.partial;
    scanResult.bytesScanned = scan.checkedBytes;
    scanResult.matchesFound = scan.matchesFound;
    scanResult.errorMessage = scan.errorMessage;
    scanResult.matches = scan.matches;

    m_candidates.replaceFromScan(scanResult, {});

    result["success"] = scan.success;
    result["partial"] = scan.partial;
    result["checkedBytes"] = static_cast<qulonglong>(scan.checkedBytes);
    result["matchesFound"] = static_cast<qulonglong>(scan.matchesFound);
    result["stored"] = static_cast<qulonglong>(m_candidates.size());
    result["error"] = scan.errorMessage;
    emit scanStatsUpdated(static_cast<int>(m_candidates.size()));
    emit scanProgress(100);
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
        m_writeHistory.append({address, write.previousValue});
    }

    result["success"] = write.success;
    result["verified"] = write.verified;
    result["bytesWritten"] = static_cast<int>(write.bytesWritten);
    result["error"] = write.errorMessage;
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
    const int batchEnd = std::min(m_lastBatchEndIndex, static_cast<int>(m_writeHistory.size()));
    for (int i = batchEnd - 1; i >= m_lastBatchStartIndex; --i) {
        const auto& rec = m_writeHistory.at(i);
        const auto write = writer.write(rec.address, rec.previousValue, true);
        if (write.success) {
            ++rolled;
        }
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
    m_smartSearchTargetValue = value;
    m_lastBatchStartIndex = m_writeHistory.size();

    for (const auto& target : m_lastAutoWriteTargets) {
        QVariantMap suggestion;
        suggestion["address"] = QString::number(target.address, 16);
        suggestion["type"] = killcore::valueTypeToString(target.type);
        suggestion["value"] = value;
        suggestions.append(suggestion);

        auto writeResult = writeMemoryValue(
            suggestion.value("address").toString(),
            suggestion.value("type").toString(),
            value);
        writeResult.insert("address", suggestion.value("address"));
        writeResult.insert("value", value);
        writeResult.insert("type", suggestion.value("type"));
        allWritesOk = allWritesOk && writeResult.value("success").toBool();
        writeResults.append(writeResult);
    }

    m_lastBatchEndIndex = m_writeHistory.size();
    if (m_lastBatchEndIndex == m_lastBatchStartIndex) {
        m_lastBatchStartIndex = -1;
        m_lastBatchEndIndex = -1;
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
    result["rollbackNote"] = "Tu peux annuler cette réécriture via le bouton rollback batch dans l'assistant.";
    result["message"] = allWritesOk
        ? QString("J'ai repris les %1 dernière(s) adresse(s) auto-écrite(s) et j'ai mis %2 dessus.")
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
    m_smartSearchTargetValue = value;
    m_lastBatchStartIndex = m_writeHistory.size();
    m_lastAutoWriteTargets.clear();

    for (const auto& target : m_chatMemoryTargets) {
        QVariantMap suggestion;
        suggestion["source"] = "chat_address";
        suggestion["address"] = QString::number(target.address, 16);
        suggestion["type"] = killcore::valueTypeToString(target.type);
        suggestion["value"] = value;
        suggestions.append(suggestion);

        auto writeResult = writeMemoryValue(
            suggestion.value("address").toString(),
            suggestion.value("type").toString(),
            value);
        writeResult.insert("source", suggestion.value("source"));
        writeResult.insert("address", suggestion.value("address"));
        writeResult.insert("value", value);
        writeResult.insert("type", suggestion.value("type"));
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
    result["rollbackNote"] = "Tu peux annuler cette écriture via le bouton rollback batch dans l'assistant.";
    result["message"] = allWritesOk
        ? QString("J'ai écrit %1 sur %2 adresse(s) mémoire sélectionnée(s) dans la conversation.")
              .arg(value)
              .arg(m_chatMemoryTargets.size())
        : QString("J'ai essayé d'écrire %1 sur les adresses mémoire sélectionnées, mais au moins une écriture a échoué.")
              .arg(value);
    appendSmartSearchDebug("chat_memory_write", result);
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

    QVariantMap result;
    result["success"] = true;
    result["cleared"] = cleared;
    result["targets"] = QVariantList{};
    appendSmartSearchDebug("chat_memory_targets_cleared", result);
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
        suggestions.append(suggestion);

        auto writeResult = writeMemoryValue(
            suggestion.value("address").toString(),
            suggestion.value("type").toString(),
            value);
        writeResult.insert("profile", suggestion.value("profile"));
        writeResult.insert("target", suggestion.value("target"));
        writeResult.insert("address", suggestion.value("address"));
        writeResult.insert("value", value);
        writeResult.insert("type", suggestion.value("type"));
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
    result["rollbackNote"] = "Tu peux annuler cette écriture via le bouton rollback batch dans l'assistant.";
    result["message"] = allWritesOk
        ? QString("J'ai utilisé le profil et j'ai mis %1 sur %2 cible(s) \"%3\".")
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
    });

    if (intent.resetContext) {
        m_smartSearchActive = false;
        m_smartSearchInitialValue.clear();
        m_smartSearchTargetValue.clear();
        m_candidates.clear();
        m_lastAutoWriteTargets.clear();
        m_chatMemoryTargets.clear();
        m_activeProfileTargets.clear();
        appendSmartSearchDebug("smart_search_reset", {
            {"query", query},
            {"reason", "new search request"},
        });
    }

    if (intent.kind == SmartSearchIntentKind::ActivateMemoryTargets
        || (intent.kind == SmartSearchIntentKind::WriteMemoryTargets && !chatAddresses.isEmpty())) {
        auto activation = activateChatMemoryTargetsFromQuery(query);
        if (intent.kind == SmartSearchIntentKind::WriteMemoryTargets
            && activation.value("success").toBool()
            && numbers.size() == 1) {
            return writeChatMemoryTargetsFromQuery(query, numbers.first());
        }
        return activation;
    }

    if (intent.kind == SmartSearchIntentKind::WriteMemoryTargets && numbers.size() == 1) {
        return writeChatMemoryTargetsFromQuery(query, numbers.first());
    }

    if (intent.kind == SmartSearchIntentKind::RewriteLastTargets && numbers.size() == 1) {
        return rewriteLastAutoWriteTargets(numbers.first(), query);
    }

    if (intent.kind == SmartSearchIntentKind::WriteProfileTargets && numbers.size() == 1) {
        auto profileWrite = writeProfileTargetsFromQuery(query, numbers.first());
        if (profileWrite.value("tool").toString() == "profile_write") {
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
        args["valueType"] = "Int32";
        result["status"] = "tool_call";
        result["tool"] = "exact_scan";
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
        args["valueType"] = "Int32";
        result["status"] = "tool_call";
        result["tool"] = "exact_scan";
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
        return result;
    }

    const QString tool = result.value("tool").toString();
    const QVariantMap args = result.value("args").toMap();
    QVariantMap actionResult;

    if (tool == "exact_scan") {
        actionResult = startExactScan(args.value("value").toString(), args.value("valueType").toString());
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
        return result;
    } else {
        result["actionStatus"] = "unsupported_tool";
        result["actionError"] = QString("Outil Smart Search non supporté: %1").arg(tool);
        return result;
    }

    result["actionStatus"] = actionResult.value("success").toBool() ? "executed" : "failed";
    result["actionResult"] = actionResult;

    if (tool == "exact_scan" && actionResult.value("success").toBool()) {
        m_smartSearchInitialValue = args.value("value").toString();
        m_smartSearchValueType = args.value("valueType", "Int32").toString();
        if (numbers.size() >= 2) {
            m_smartSearchTargetValue = numbers.at(1);
            m_smartSearchActive = true;
        }

        const auto count = actionResult.value("candidateStoreSize").toULongLong();
        result["workflowStatus"] = "awaiting_value_change";
        result["targetValue"] = m_smartSearchTargetValue;
        result["message"] = QString("J'ai trouvé %1 candidats pour %2. Fais bouger la valeur dans le jeu, puis donne-moi la nouvelle valeur pour réduire la liste.")
                                .arg(count)
                                .arg(m_smartSearchInitialValue);
    } else if ((tool == "next_scan" || tool == "unknown_compare") && actionResult.value("success").toBool()) {
        const auto remaining = tool == "next_scan"
                                   ? actionResult.value("remaining").toULongLong()
                                   : actionResult.value("stored").toULongLong();
        result["targetValue"] = m_smartSearchTargetValue;

        if (remaining >= 1 && remaining <= kAutoWriteCandidateLimit && !m_smartSearchTargetValue.isEmpty()) {
            const auto suggestions = suggestedWritesForCandidates(
                m_candidates,
                m_smartSearchTargetValue,
                kAutoWriteCandidateLimit);
            QVariantList writeResults;
            bool allWritesOk = true;
            m_lastBatchStartIndex = m_writeHistory.size();
            m_lastAutoWriteTargets.clear();

            for (const auto& item : suggestions) {
                const auto suggestion = item.toMap();
                auto writeResult = writeMemoryValue(
                    suggestion.value("address").toString(),
                    suggestion.value("type").toString(),
                    suggestion.value("value").toString());
                // Enrichit le résultat avec l'adresse/valeur pour l'affichage UI.
                writeResult.insert("address", suggestion.value("address"));
                writeResult.insert("value", suggestion.value("value"));
                writeResult.insert("type", suggestion.value("type"));
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

            result["workflowStatus"] = allWritesOk ? "auto_write_done" : "auto_write_partial_or_failed";
            result["suggestedWrites"] = suggestions;
            result["suggestedWrite"] = suggestions.isEmpty() ? QVariantMap{} : suggestions.first().toMap();
            result["autoWriteResults"] = writeResults;
            result["autoWriteResult"] = writeResults.isEmpty() ? QVariantMap{} : writeResults.last().toMap();
            result["autoWriteCount"] = writeResults.size();
            result["rollbackNote"] = "Tu peux annuler toutes les écritures via le bouton rollback batch dans l'assistant.";
            result["message"] = allWritesOk
                                    ? QString("Il reste %1 candidat(s). J'ai écrit automatiquement %2 sur toutes les adresses finales.")
                                          .arg(remaining)
                                          .arg(m_smartSearchTargetValue)
                                    : QString("Il reste %1 candidat(s), mais au moins une écriture automatique a échoué.")
                                          .arg(remaining);
        } else if (remaining > 1) {
            result["workflowStatus"] = "needs_more_refinement";
            result["message"] = QString("Il reste %1 candidats. Refais varier le score, puis indique-moi la nouvelle valeur.")
                                    .arg(remaining);
        } else {
            result["workflowStatus"] = "no_candidate";
            result["message"] = "Aucun candidat restant. Il faut repartir sur un nouveau scan exact.";
        }
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
        settings, "scan/chunkSizeMb", kDefaultScanChunkSizeMb, 1, 64);
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
        std::clamp(incoming.value("scanChunkSizeMb", kDefaultScanChunkSizeMb).toInt(), 1, 64));
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
    manifest["logFilePath"] = getLogFilePath();
    manifest["smartSearchDebugFilePath"] = smartSearchDebugFilePath();
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

    QFile file(smartSearchDebugFilePath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        KE_LOG_WARN() << "Unable to open Smart Search debug file: "
                      << smartSearchDebugFilePath().toStdString();
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

} // namespace killengine
