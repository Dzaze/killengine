#pragma once

#include <cstdint>

#include <QByteArray>
#include <QString>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace killcore {

enum class KernelDriverProbeStatus {
    Unavailable,
    Connected,
    AccessDenied,
    Incompatible,
    Error,
};

struct KernelDriverCapabilities {
    std::uint32_t protocolVersion = 1; // Définis la version du protocole que le driver supporte
    bool healthProbe = false; // Permet au driver de répondre aux requêtes de santé
    bool processMemoryAccess = false; // Permet au driver de lire et écrire dans la mémoire des processus
    bool privilegedInstrumentation = false; // Permet au driver d'utiliser des fonctionnalités d'instrumentation privilégiées
    bool handleTable = false; // Permet au driver de masquer/restaurer des handles dans la table d'un process
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

#ifdef Q_OS_WIN
    /// Write raw bytes into another process's memory via the kernel driver
    /// (kKillEngineKernelIoctlWriteMemory). Returns false on failure (driver
    /// not loaded, IOCTL rejected, or a short write).
    bool writeMemory(HANDLE processId, uint64_t address, const QByteArray& data) const;

    /// Read `size` bytes from another process's memory via the kernel driver
    /// (kKillEngineKernelIoctlReadMemory). Returns an empty QByteArray on
    /// failure rather than a partially-filled buffer.
    QByteArray readMemory(HANDLE processId, uint64_t address, size_t size) const;

    /// Hide or restore a specific handle in a process's handle table via the
    /// kernel driver (kKillEngineKernelIoctlHandleTable). `action` is 0 = hide,
    /// 1 = restore. Returns true if the IOCTL succeeded (the entry may or may
    /// not have existed — see the output parameters).
    bool handleTable(HANDLE ownerPid, uint64_t handleValue, int action,
                     bool& found, uint64_t& entryIndex, uint64_t& originalObject) const;
#endif

    static QString statusToString(KernelDriverProbeStatus status);

private:
    QString m_devicePath;
};

} // namespace killcore
