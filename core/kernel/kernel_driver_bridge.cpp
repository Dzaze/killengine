#include "kernel/kernel_driver_bridge.h"

#include <string>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winioctl.h>
#endif

namespace killcore {

namespace {

constexpr std::uint32_t kKillEngineKernelDeviceType = 0x8000;
constexpr std::uint32_t kIoctlIndexHealth = 0x801;
constexpr std::uint32_t kIoctlIndexReadMemory = 0x802;
constexpr std::uint32_t kIoctlIndexWriteMemory = 0x803;

#ifdef _WIN32
DWORD ioctlHealthProbe() {
    return CTL_CODE(kKillEngineKernelDeviceType, kIoctlIndexHealth, METHOD_BUFFERED, FILE_READ_DATA);
}

DWORD ioctlReadMemory() {
    return CTL_CODE(kKillEngineKernelDeviceType, kIoctlIndexReadMemory, METHOD_BUFFERED, FILE_READ_DATA);
}

DWORD ioctlWriteMemory() {
    return CTL_CODE(kKillEngineKernelDeviceType, kIoctlIndexWriteMemory, METHOD_BUFFERED, FILE_WRITE_DATA);
}

QString systemErrorMessage(DWORD errorCode) {
    LPWSTR raw = nullptr;
    const DWORD size = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                         FORMAT_MESSAGE_IGNORE_INSERTS,
                                     nullptr,
                                     errorCode,
                                     MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                                     reinterpret_cast<LPWSTR>(&raw),
                                     0,
                                     nullptr);
    QString message = size > 0 && raw ? QString::fromWCharArray(raw).trimmed() : QStringLiteral("Erreur Windows inconnue");
    if (raw) {
        LocalFree(raw);
    }
    return message;
}
#endif

} // namespace

KernelDriverBridge::KernelDriverBridge(QString devicePath)
    : m_devicePath(std::move(devicePath)) {}

QString KernelDriverBridge::statusToString(KernelDriverProbeStatus status) {
    switch (status) {
        case KernelDriverProbeStatus::Unavailable:  return QStringLiteral("unavailable");
        case KernelDriverProbeStatus::Connected:    return QStringLiteral("connected");
        case KernelDriverProbeStatus::AccessDenied: return QStringLiteral("access_denied");
        case KernelDriverProbeStatus::Incompatible: return QStringLiteral("incompatible");
        case KernelDriverProbeStatus::Error:        return QStringLiteral("error");
    }
    return QStringLiteral("error");
}

KernelDriverProbeResult KernelDriverBridge::probe() const {
    KernelDriverProbeResult result;
    result.devicePath = m_devicePath;

#ifndef _WIN32
    result.status = KernelDriverProbeStatus::Unavailable;
    result.message = QStringLiteral("Le connecteur driver noyau est disponible uniquement sur Windows.");
    return result;
#else
    const std::wstring widePath = m_devicePath.toStdWString();
    HANDLE device = CreateFileW(widePath.c_str(),
                                GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE,
                                nullptr,
                                OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL,
                                nullptr);
    if (device == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
            result.status = KernelDriverProbeStatus::Unavailable;
            result.message = QStringLiteral("Driver KillEngineKernel non chargé.");
        } else if (error == ERROR_ACCESS_DENIED) {
            result.status = KernelDriverProbeStatus::AccessDenied;
            result.message = QStringLiteral("Accès refusé au device driver. Vérifie les droits administrateur et l'ACL du device.");
        } else {
            result.status = KernelDriverProbeStatus::Error;
            result.message = QStringLiteral("Ouverture du device driver échouée: %1").arg(systemErrorMessage(error));
        }
        return result;
    }

    struct HealthResponse {
        std::uint32_t protocolVersion;
        std::uint32_t flags;
        bool processMemoryRead : 1;
        bool processMemoryWrite : 1;
    };

    HealthResponse response{};
    DWORD bytesReturned = 0;
    const BOOL ok = DeviceIoControl(device,
                                    ioctlHealthProbe(),
                                    nullptr,
                                    0,
                                    &response,
                                    sizeof(response),
                                    &bytesReturned,
                                    nullptr);
    const DWORD ioctlError = ok ? ERROR_SUCCESS : GetLastError();
    CloseHandle(device);

    if (!ok || bytesReturned < sizeof(HealthResponse)) {
        result.status = KernelDriverProbeStatus::Error;
        result.message = QStringLiteral("Le driver a été ouvert, mais le probe de santé a échoué: %1").arg(systemErrorMessage(ioctlError));
        return result;
    }

    result.capabilities.protocolVersion = response.protocolVersion;
    result.capabilities.healthProbe = (response.flags & 0x1u) != 0;
    result.capabilities.processMemoryAccess = response.processMemoryRead && response.processMemoryWrite;
    result.capabilities.privilegedInstrumentation = true;

    if (response.protocolVersion != kProtocolVersion) {
        result.status = KernelDriverProbeStatus::Incompatible;
        result.message = QStringLiteral("Driver KillEngineKernel incompatible: protocole %1, attendu %2.")
                             .arg(response.protocolVersion)
                             .arg(kProtocolVersion);
        return result;
    }

    result.status = KernelDriverProbeStatus::Connected;
    result.message = QStringLiteral("Driver KillEngineKernel connecté en mode probe uniquement.");
    return result;
#endif
}

#ifdef Q_OS_WIN
bool KernelDriverBridge::writeMemory(HANDLE processId, uint64_t address, const QByteArray& data) const {
    #ifndef _WIN32
        return false;
    #else
        const std::wstring widePath = m_devicePath.toStdWString();
        HANDLE device = CreateFileW(widePath.c_str(),
                                    GENERIC_READ | GENERIC_WRITE,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    nullptr,
                                    OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
        if (device == INVALID_HANDLE_VALUE) {
            return false;
        }
    
        struct MemoryRequest {
            HANDLE ProcessId;
            PVOID Address;
            PVOID Buffer;
            SIZE_T Size;
        };
    
        MemoryRequest request{};
        request.ProcessId = processId;
        request.Address = reinterpret_cast<PVOID>(address);
        QByteArray dataCopy = data;  // Crée une copie non-constante de data
        request.Buffer = dataCopy.data();  // Utilise la copie non-constante
        request.Size = static_cast<SIZE_T>(data.size());
    
        DWORD bytesReturned = 0;
        const BOOL ok = DeviceIoControl(device,
                                        ioctlWriteMemory(),
                                        &request,
                                        sizeof(request),
                                        nullptr,
                                        0,
                                        &bytesReturned,
                                        nullptr);
        CloseHandle(device);
        return ok && bytesReturned == request.Size;
    #endif
}
#endif // Q_OS_WIN

#ifdef Q_OS_WIN
QByteArray KernelDriverBridge::readMemory(HANDLE processId, uint64_t address, size_t size) const {
        #ifndef _WIN32
            return QByteArray();
        #else
            const std::wstring widePath = m_devicePath.toStdWString();
            HANDLE device = CreateFileW(widePath.c_str(),
                                        GENERIC_READ | GENERIC_WRITE,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE,
                                        nullptr,
                                        OPEN_EXISTING,
                                        FILE_ATTRIBUTE_NORMAL,
                                        nullptr);
            if (device == INVALID_HANDLE_VALUE) {
                return QByteArray();
            }
        
            struct MemoryRequest {
                HANDLE ProcessId;
                PVOID Address;
                PVOID Buffer;
                SIZE_T Size;
            };
        
            MemoryRequest request{};
            request.ProcessId = processId;
            request.Address = reinterpret_cast<PVOID>(address);
            request.Buffer = nullptr;
            request.Size = static_cast<SIZE_T>(size);
        
            QByteArray buffer(size, '\0');
            DWORD bytesReturned = 0;
            const BOOL ok = DeviceIoControl(device,
                                            ioctlReadMemory(),
                                            &request,
                                            sizeof(request),
                                            buffer.data(),
                                            static_cast<DWORD>(size),
                                            &bytesReturned,
                                            nullptr);
            CloseHandle(device);
            if (ok) {
                buffer.resize(static_cast<int>(bytesReturned));
            }
            return ok ? buffer : QByteArray();
        #endif
        }
#endif // Q_OS_WIN

} // namespace killcore