#pragma once

#include "process/process_handle.h"

#include <QByteArray>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

#include <cstdint>
#include <functional>
#include <memory>

class QProcess;

namespace killengine {

class ClrInspectorBridge {
public:
    using TelemetryCallback = std::function<void(const QString&, const QVariantMap&)>;
    using IsAttachedCallback = std::function<bool()>;
    using PidCallback = std::function<int()>;
    using ProcessNameCallback = std::function<QString()>;

    ClrInspectorBridge(
        const killcore::ProcessHandle& handle,
        TelemetryCallback telemetry,
        IsAttachedCallback isAttached,
        PidCallback pid,
        ProcessNameCallback processName);
    ~ClrInspectorBridge();

    QVariantMap getClrInspectorStatus() const;
    QVariantMap attachClrInspector();
    QVariantMap detachClrInspector();
    QVariantMap shutdownClrInspector();
    QVariantMap flushClrInspectorCache();
    QVariantMap findClrObjectsByType(const QString& typeSubstring);
    QVariantMap findClrObjectsByFieldValue(const QString& typeSubstring, const QString& fieldName, const QString& expectedValue, int maxResults);
    QVariantMap readClrObject(const QString& addressHex);
    QVariantMap writeClrPrimitiveField(const QString& objectAddressHex, const QString& fieldName, const QString& value);
    QVariantMap writeClrPrimitivePath(const QString& objectAddressHex, const QString& path, const QString& value);
    QVariantMap writeClrPrimitivePathBatch(const QString& objectAddressHex, const QVariantList& operations);
    QVariantMap enumerateClrRoots(const QString& typeSubstring);
    QVariantMap writeClrPrimitivePathByLocator(const QString& typeSubstring, const QString& identityField, const QString& identityValue, const QString& path, const QString& value);
    QVariantMap writeClrPrimitivePathBatchByLocator(const QString& typeSubstring, const QString& identityField, const QString& identityValue, const QVariantList& operations);
    QVariantMap writeClrPrimitivePathBatchAtomic(const QString& objectAddressHex, const QVariantList& operations);
    QVariantMap callClrInstanceMethod(const QString& objectAddressHex, const QString& methodName, const QString& valueText, const QString& valueType);
    QVariantMap findClrGcRootPath(const QString& targetObjectAddressHex, int maxDepth, int maxRootsScanned);
    QVariantMap generateClrObjectReport(const QString& objectAddressHex, int maxDepth, int maxNodes, bool includeGcRootChain);
    QVariantMap disassembleClrMethod(const QString& objectAddressHex, const QString& methodName, int instructionCount);

private:
    QString clrInspectorPipeName() const;
    QString findClrInspectorExecutable() const;
    bool ensureClrInspectorStarted(QString* error = nullptr);
    QVariantMap callClrInspectorRpc(const QString& method, const QVariantList& params, int timeoutMs = 5000);

    const killcore::ProcessHandle& m_handle;
    TelemetryCallback m_appendScanTelemetry;
    IsAttachedCallback m_isAttached;
    PidCallback m_pid;
    ProcessNameCallback m_processName;
    std::unique_ptr<QProcess> m_process;
    int m_requestId{1};
};

} // namespace killengine