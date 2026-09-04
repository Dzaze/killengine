#pragma once

#include "process/process_handle.h"

#include <QString>
#include <QVariantMap>

#include <functional>

namespace killengine {

class KernelDriverManager {
public:
    using TelemetryCallback = std::function<void(const QString&, const QVariantMap&)>;

    KernelDriverManager(const killcore::ProcessHandle& handle, TelemetryCallback telemetry);

    QVariantMap probeKernelDriver() const;
    QVariantMap startKernelDriver() const;
    QVariantMap readMemoryKernel(const QString& addressHex, int size) const;
    QVariantMap writeMemoryKernel(const QString& addressHex, const QString& hexBytes);
    QVariantMap handleTable(const QString& ownerPid, const QString& handleValue, bool hide) const;

private:
    const killcore::ProcessHandle& m_handle;
    TelemetryCallback m_appendScanTelemetry;
};

} // namespace killengine
