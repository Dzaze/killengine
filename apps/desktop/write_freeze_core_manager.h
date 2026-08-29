#pragma once

#include "auto_write_state_access.h"
#include "process/process_handle.h"
#include "scanner/scan_types.h"

#include <QObject>
#include <QByteArray>
#include <QList>
#include <QString>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

#include <cstdint>
#include <functional>

namespace killengine {

class WriteFreezeCoreManager : public QObject {
public:
    using AutoWriteStateCallback = std::function<AutoWriteStateAccess()>;
    using IsAttachedCallback = std::function<bool()>;
    using PidCallback = std::function<int()>;
    using TelemetryCallback = std::function<void(const QString&, const QVariantMap&)>;
    using PersistWriteHistoryCallback = std::function<void(uint64_t, killcore::ValueType, const QString&)>;
    using ResultCallback = std::function<void(const QVariantMap&)>;

    WriteFreezeCoreManager(
        const killcore::ProcessHandle& handle,
        AutoWriteStateCallback autoWriteState,
        IsAttachedCallback isAttached,
        PidCallback pid,
        TelemetryCallback telemetry,
        PersistWriteHistoryCallback persistWriteHistory,
        ResultCallback writeDidNotHold,
        int& lastBatchStartIndex,
        int& lastBatchEndIndex,
        QObject* parent = nullptr);

    void clearSessionState();

    QVariantMap writeMemoryValue(const QString& addressHex, const QString& valueType, const QString& value);
    QVariantMap writeMemoryValuesAtomic(const QVariantList& targets, const QVariantMap& options);
    QVariantMap writeMemoryValuesWithVariants(const QVariantList& targets, const QString& value);
    QVariantMap writeMemoryValueConfirmed(const QString& addressHex, const QString& valueType, const QString& value, bool persistHistory = true);
    QVariantMap rollbackLastWriteBatch();
    QVariantMap rollbackLastWrite();
    void watchSuccessfulWrite(uint64_t address, killcore::ValueType type, const QByteArray& expectedBytes);

private:
    struct WriteWatchEntry {
        uint64_t address{0};
        killcore::ValueType type{killcore::ValueType::Int32};
        QByteArray expectedBytes;
        int ticksRemaining{0};
        int consecutiveMismatches{0};
    };

    void registerWriteWatch(uint64_t address, killcore::ValueType type, const QByteArray& expectedBytes);
    void applyWriteWatchTick();

    const killcore::ProcessHandle& m_handle;
    AutoWriteStateCallback m_autoWriteState;
    IsAttachedCallback m_isAttached;
    PidCallback m_pid;
    TelemetryCallback m_appendScanTelemetry;
    PersistWriteHistoryCallback m_persistWriteHistory;
    ResultCallback m_writeDidNotHold;
    int& m_lastBatchStartIndex;
    int& m_lastBatchEndIndex;
    QTimer m_writeWatchTimer;
    QList<WriteWatchEntry> m_writeWatchEntries;
    uint64_t m_lastWriteAddress{0};
    QByteArray m_lastWritePreviousValue;
};

} // namespace killengine
