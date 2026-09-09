#include "write_freeze_core_manager.h"

#include "localization/localization.h"

#include "memory/memory_reader.h"
#include "memory/memory_writer.h"
#include "process/process_suspend.h"
#include "scanner/display_value_tracker.h"

#include <QElapsedTimer>

#include <algorithm>
#include <cstring>
#include <optional>
#include <utility>

namespace killengine {
namespace {

constexpr int kWriteWatchTicks = 8;
constexpr int kWriteWatchMaxEntries = 20;
constexpr int kWriteWatchConfirmMismatches = 2;

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

double bytesToDouble(const QByteArray& bytes, killcore::ValueType type) {
    if (bytes.size() < static_cast<int>(killcore::valueTypeSize(type))) {
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

} // namespace

WriteFreezeCoreManager::WriteFreezeCoreManager(
    const killcore::ProcessHandle& handle,
    AutoWriteStateCallback autoWriteState,
    IsAttachedCallback isAttached,
    PidCallback pid,
    TelemetryCallback telemetry,
    PersistWriteHistoryCallback persistWriteHistory,
    ResultCallback writeDidNotHold,
    int& lastBatchStartIndex,
    int& lastBatchEndIndex,
    QObject* parent)
    : QObject(parent)
    , m_handle(handle)
    , m_autoWriteState(std::move(autoWriteState))
    , m_isAttached(std::move(isAttached))
    , m_pid(std::move(pid))
    , m_appendScanTelemetry(std::move(telemetry))
    , m_persistWriteHistory(std::move(persistWriteHistory))
    , m_writeDidNotHold(std::move(writeDidNotHold))
    , m_lastBatchStartIndex(lastBatchStartIndex)
    , m_lastBatchEndIndex(lastBatchEndIndex) {
    m_writeWatchTimer.setInterval(1500);
    connect(&m_writeWatchTimer, &QTimer::timeout, this, &WriteFreezeCoreManager::applyWriteWatchTick);
}

void WriteFreezeCoreManager::clearSessionState() {
    m_writeWatchTimer.stop();
    m_writeWatchEntries.clear();
    m_lastWriteAddress = 0;
    m_lastWritePreviousValue.clear();
    auto state = m_autoWriteState();
    state.clearWriteHistory();
    state.clearLastTargets();
    state.clearChatTargets();
    m_lastBatchStartIndex = -1;
    m_lastBatchEndIndex = -1;
}

QVariantMap WriteFreezeCoreManager::writeMemoryValue(const QString& addressHex, const QString& valueType, const QString& value) {
    QVariantMap result;
    result["success"] = false;

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

    killcore::ProcessHandle writeHandle(static_cast<uint32_t>(m_pid()), killcore::ProcessAccess::ReadWrite);
    if (!writeHandle.isValid()) {
        result["error"] = KE_TXT("Impossible d'ouvrir le processus en écriture.", "Could not open the process for writing.");
        return result;
    }

    const QByteArray targetBytes = killcore::scanValueToBytes(scanValue);
    killcore::MemoryWriter writer(writeHandle);
    const auto write = writer.write(address, targetBytes, true);
    if (write.success) {
        m_lastWriteAddress = address;
        m_lastWritePreviousValue = write.previousValue;
        m_autoWriteState().appendWriteRecord({address, write.previousValue, targetBytes, type, value});
        registerWriteWatch(address, type, targetBytes);
    }

    result["success"] = write.success;
    result["verified"] = write.verified;
    result["protectionChanged"] = write.protectionChanged;
    result["bytesWritten"] = static_cast<int>(write.bytesWritten);
    result["error"] = write.errorMessage;
    return result;
}

QVariantMap WriteFreezeCoreManager::writeMemoryValuesAtomic(const QVariantList& targets, const QVariantMap& options) {
    QVariantMap result;
    result["success"] = false;

    if (!m_isAttached() || m_pid() <= 0) {
        result["error"] = KE_TXT("Aucun processus attaché.", "No process attached.");
        return result;
    }
    if (targets.isEmpty()) {
        result["error"] = KE_TXT("Aucune cible à écrire.", "No target to write.");
        return result;
    }
    constexpr int kMaxAtomicTargets = 32;
    if (targets.size() > kMaxAtomicTargets) {
        result["error"] = KE_TXT("Trop de cibles pour une écriture groupée (%1 maximum).", "Too many targets for a batched write (%1 maximum).").arg(kMaxAtomicTargets);
        return result;
    }

    killcore::ProcessHandle writeHandle(static_cast<uint32_t>(m_pid()), killcore::ProcessAccess::ReadWrite);
    if (!writeHandle.isValid()) {
        result["error"] = KE_TXT("Impossible d'ouvrir le processus en écriture.", "Could not open the process for writing.");
        return result;
    }

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
            parseError = KE_TXT("Adresse invalide.", "Invalid address.");
        } else if (!killcore::parseValueType(typeName, &type)) {
            parseError = KE_TXT("Type invalide.", "Invalid type.");
        } else if (!killcore::parseScanValue(valueText, type, &scanValue, &parseError) && parseError.isEmpty()) {
            parseError = KE_TXT("Valeur invalide.", "Invalid value.");
        }
        if (!parseError.isEmpty()) {
            QVariantMap errEntry;
            errEntry["address"] = addressHex;
            errEntry["error"] = parseError;
            parseErrors.append(errEntry);
            continue;
        }

        parsed.append({address, type, killcore::scanValueToBytes(scanValue), addressHex, typeName, valueText});
    }

    if (parsed.isEmpty()) {
        result["error"] = KE_TXT("Aucune cible valide à écrire.", "No valid target to write.");
        result["parseErrors"] = parseErrors;
        return result;
    }

    const bool suspendThreads = options.value("suspendThreads", true).toBool();
    int suspendedThreadCount = 0;
    QVariantList writeResults;
    int written = 0;
    auto writeState = m_autoWriteState();
    const int batchStartIndex = writeState.writeHistorySize();

    {
        std::optional<killcore::ProcessThreadsSuspendGuard> suspendGuard;
        if (suspendThreads) {
            suspendGuard.emplace(static_cast<uint32_t>(m_pid()));
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
                m_lastWriteAddress = pt.address;
                m_lastWritePreviousValue = write.previousValue;
                writeState.appendWriteRecord({pt.address, write.previousValue, pt.bytes, pt.type, pt.valueText});
                registerWriteWatch(pt.address, pt.type, pt.bytes);
            }
            writeResults.append(writeResult);
        }
    }

    result["success"] = written > 0 && written == parsed.size();
    result["results"] = writeResults;
    result["written"] = written;
    result["total"] = targets.size();
    result["suspendedThreadCount"] = suspendedThreadCount;
    if (written > 0) {
        m_lastBatchStartIndex = batchStartIndex;
        m_lastBatchEndIndex = writeState.writeHistorySize();
    }
    if (!parseErrors.isEmpty()) {
        result["parseErrors"] = parseErrors;
    }
    m_appendScanTelemetry("write_atomic_multi", result);
    return result;
}

QVariantMap WriteFreezeCoreManager::writeMemoryValuesWithVariants(const QVariantList& targets, const QString& value) {
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
        result["error"] = KE_TXT("Aucune cible à écrire.", "No target to write.");
        m_appendScanTelemetry("ui_string_sources_write", {
            {"success", false},
            {"error", result.value("error")},
            {"displayValue", value},
            {"targetCount", targets.size()},
        });
        return result;
    }
    if (rawValue.isEmpty()) {
        result["error"] = KE_TXT("Valeur vide.", "Empty value.");
        m_appendScanTelemetry("ui_string_sources_write", {
            {"success", false},
            {"error", result.value("error")},
            {"displayValue", value},
            {"targetCount", targets.size()},
        });
        return result;
    }

    killcore::ProcessHandle writeHandle(static_cast<uint32_t>(m_pid()), killcore::ProcessAccess::ReadWrite);
    if (!writeHandle.isValid()) {
        result["error"] = KE_TXT("Impossible d'ouvrir le processus en écriture.", "Could not open the process for writing.");
        m_appendScanTelemetry("ui_string_sources_write", {
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
    auto writeState = m_autoWriteState();
    m_lastBatchStartIndex = writeState.writeHistorySize();

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
            writeResult["error"] = KE_TXT("Adresse invalide.", "Invalid address.");
            allWritesOk = false;
            writeResults.append(writeResult);
            continue;
        }

        killcore::ValueType type;
        if (!killcore::parseValueType(typeName, &type)) {
            writeResult["error"] = KE_TXT("Type invalide.", "Invalid type.");
            allWritesOk = false;
            writeResults.append(writeResult);
            continue;
        }

        QString parseError;
        const QByteArray targetBytes = killcore::targetBytesForTypeAndVariant(rawValue, type, variantLabel, &parseError);
        if (targetBytes.isEmpty()) {
            writeResult["error"] = parseError.isEmpty() ? KE_TXT("Valeur incompatible avec cette variante.", "Value incompatible with this variant.") : parseError;
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
            writeState.appendWriteRecord({address, write.previousValue, targetBytes, type, rawValue});
        } else {
            allWritesOk = false;
        }
        writeResults.append(writeResult);
    }

    m_lastBatchEndIndex = writeState.writeHistorySize();
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
        : KE_TXT("Écriture partielle: %1/%2 réussie(s).", "Partial write: %1/%2 succeeded.").arg(written).arg(targets.size());
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
    m_appendScanTelemetry("ui_string_sources_write", {
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

QVariantMap WriteFreezeCoreManager::writeMemoryValueConfirmed(
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

    killcore::ProcessHandle writeHandle(static_cast<uint32_t>(m_pid()), killcore::ProcessAccess::ReadWrite);
    if (!writeHandle.isValid()) {
        result["error"] = KE_TXT("Impossible d'ouvrir le processus en écriture.", "Could not open the process for writing.");
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
            ? KE_TXT("La confirmation temporaire de l'adresse a échoué.", "The address's temporary confirmation failed.")
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
                ? KE_TXT("La restauration après confirmation temporaire a échoué.", "The restore after temporary confirmation failed.")
                : restore.errorMessage;
            return result;
        }
    } else {
        result["error"] = KE_TXT("Impossible de restaurer l'ancienne valeur après confirmation.", "Could not restore the previous value after confirmation.");
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
        m_autoWriteState().appendWriteRecord({address, previousValue, targetBytes, type, value});
        registerWriteWatch(address, type, targetBytes);
        if (persistHistory) {
            m_persistWriteHistory(address, type, value);
        }
    }

    return result;
}

QVariantMap WriteFreezeCoreManager::rollbackLastWriteBatch() {
    QVariantMap result;
    result["success"] = false;

    if (m_lastBatchStartIndex < 0
        || m_lastBatchEndIndex <= m_lastBatchStartIndex
        || m_lastBatchStartIndex >= m_autoWriteState().writeHistorySize()) {
        result["error"] = KE_TXT("Aucun batch d'écritures automatiques à restaurer.", "No batch of automatic writes to restore.");
        return result;
    }

    killcore::ProcessHandle writeHandle(static_cast<uint32_t>(m_pid()), killcore::ProcessAccess::ReadWrite);
    if (!writeHandle.isValid()) {
        result["error"] = KE_TXT("Impossible d'ouvrir le processus en écriture.", "Could not open the process for writing.");
        return result;
    }

    killcore::MemoryWriter writer(writeHandle);
    int rolled = 0;
    QVariantList restoredWrites;
    auto writeState = m_autoWriteState();
    const int batchEnd = std::min(m_lastBatchEndIndex, writeState.writeHistorySize());
    for (int i = batchEnd - 1; i >= m_lastBatchStartIndex; --i) {
        const auto& rec = writeState.writeHistoryAt(i);
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
        writeState.removeWriteHistoryAt(i);
    }
    m_lastBatchStartIndex = -1;
    m_lastBatchEndIndex = -1;
    writeState.clearLastTargets();
    if (writeState.writeHistoryEmpty()) {
        m_lastWriteAddress = 0;
        m_lastWritePreviousValue.clear();
    }

    result["success"] = (rolled == total);
    result["rolledBack"] = rolled;
    result["total"] = total;
    result["restoredWrites"] = restoredWrites;
    result["error"] = (rolled == total) ? QString() : KE_TXT("Seulement %1/%2 restaurées.", "Only %1/%2 restored.").arg(rolled).arg(total);
    return result;
}

QVariantMap WriteFreezeCoreManager::rollbackLastWrite() {
    QVariantMap result;
    result["success"] = false;

    if (m_lastWriteAddress == 0 || m_lastWritePreviousValue.isEmpty()) {
        result["error"] = KE_TXT("Aucune écriture à restaurer.", "No write to restore.");
        return result;
    }

    killcore::ProcessHandle writeHandle(static_cast<uint32_t>(m_pid()), killcore::ProcessAccess::ReadWrite);
    if (!writeHandle.isValid()) {
        result["error"] = KE_TXT("Impossible d'ouvrir le processus en écriture.", "Could not open the process for writing.");
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

void WriteFreezeCoreManager::watchSuccessfulWrite(uint64_t address, killcore::ValueType type, const QByteArray& expectedBytes) {
    registerWriteWatch(address, type, expectedBytes);
}

void WriteFreezeCoreManager::registerWriteWatch(uint64_t address, killcore::ValueType type, const QByteArray& expectedBytes) {
    if (address == 0 || expectedBytes.isEmpty()) {
        return;
    }

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

void WriteFreezeCoreManager::applyWriteWatchTick() {
    if (m_writeWatchEntries.isEmpty() || m_pid() <= 0 || !m_handle.isValid()) {
        m_writeWatchTimer.stop();
        return;
    }

    killcore::MemoryReader reader(m_handle);
    for (int i = m_writeWatchEntries.size() - 1; i >= 0; --i) {
        auto& entry = m_writeWatchEntries[i];
        const auto read = reader.read(entry.address, entry.expectedBytes.size());
        const bool matched = (read.success || read.partial)
            && read.bytesRead == static_cast<size_t>(entry.expectedBytes.size())
            && read.data == entry.expectedBytes;
        if (matched) {
            entry.consecutiveMismatches = 0;
        } else {
            ++entry.consecutiveMismatches;
        }
        --entry.ticksRemaining;

        if (entry.consecutiveMismatches >= kWriteWatchConfirmMismatches) {
            const QString addressHex = QString::number(entry.address, 16).toUpper();
            QVariantMap info;
            info["address"] = addressHex;
            info["type"] = killcore::valueTypeToString(entry.type);
            info["message"] = KE_TXT(
                "La valeur écrite à 0x%1 a déjà changé toute seule, quelques secondes après l'écriture — quelque "
                "chose la recalcule ou la réécrit depuis une source que tu n'as pas encore trouvée. Une simple "
                "écriture directe ne suffira pas ici.",
                "The value written at 0x%1 has already changed on its own, just a few seconds after the write — "
                "something is recomputing or rewriting it from a source you haven't found yet. A simple direct "
                "write won't be enough here.")
                .arg(addressHex);
            info["suggestion"] = KE_TXT("Capture l'instruction qui écrit dessus pour trouver la vraie source, ou pose un freeze si tu veux juste bloquer cette valeur.",
                                         "Capture the instruction that writes to it to find the real source, or set a freeze if you just want to lock this value.");
            m_appendScanTelemetry("write_did_not_hold", info);
            m_writeDidNotHold(info);
            m_writeWatchEntries.removeAt(i);
            continue;
        }

        if (entry.ticksRemaining <= 0) {
            m_writeWatchEntries.removeAt(i);
        }
    }

    if (m_writeWatchEntries.isEmpty()) {
        m_writeWatchTimer.stop();
    }
}

} // namespace killengine
