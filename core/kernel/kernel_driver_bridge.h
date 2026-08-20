#pragma once

#include <cstdint>

#include <QString>

namespace killcore {

enum class KernelDriverProbeStatus {
    Unavailable,
    Connected,
    AccessDenied,
    Incompatible,
    Error,
};

struct KernelDriverCapabilities {
    std::uint32_t protocolVersion = 0;
    bool healthProbe = false;
    bool processMemoryAccess = false;
    bool privilegedInstrumentation = false;
};

struct KernelDriverProbeResult {
    KernelDriverProbeStatus status = KernelDriverProbeStatus::Unavailable;
    QString devicePath;
    QString message;
    KernelDriverCapabilities capabilities;
};

class KernelDriverBridge {
public:
    static constexpr std::uint32_t kProtocolVersion = 1;
    static constexpr const wchar_t* kDefaultDevicePath = L"\\\\.\\KillEngineKernel";

    explicit KernelDriverBridge(QString devicePath = QString::fromWCharArray(kDefaultDevicePath));

    const QString& devicePath() const noexcept { return m_devicePath; }

    /// Probe the optional kernel driver without enabling privileged memory operations.
    KernelDriverProbeResult probe() const;

    static QString statusToString(KernelDriverProbeStatus status);

private:
    QString m_devicePath;
};

} // namespace killcore
