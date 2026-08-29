#include "kernel_driver_manager.h"

#include "kernel/kernel_driver_bridge.h"
#include "logging/logger.h"

#include <QByteArray>

#include <algorithm>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace killengine {
namespace {
#ifdef Q_OS_WIN
QString windowsErrorMessage(DWORD errorCode) {
    LPWSTR raw = nullptr;
    const DWORD size = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                         FORMAT_MESSAGE_IGNORE_INSERTS,
                                     nullptr,
                                     errorCode,
                                     MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                                     reinterpret_cast<LPWSTR>(&raw),
                                     0,
                                     nullptr);
    QString message = size > 0 && raw ? QString::fromWCharArray(raw).trimmed() : QStringLiteral("erreur %1").arg(errorCode);
    if (raw) {
        LocalFree(raw);
    }
    return message;
}
#endif
} // namespace

KernelDriverManager::KernelDriverManager(const killcore::ProcessHandle& handle, TelemetryCallback telemetry)
    : m_handle(handle)
    , m_appendScanTelemetry(std::move(telemetry)) {}

QVariantMap KernelDriverManager::probeKernelDriver() const {
    const killcore::KernelDriverBridge bridge;
    const auto probe = bridge.probe();

    QVariantMap capabilities;
    capabilities["protocolVersion"] = static_cast<int>(probe.capabilities.protocolVersion);
    capabilities["healthProbe"] = probe.capabilities.healthProbe;
    capabilities["processMemoryAccess"] = probe.capabilities.processMemoryAccess;
    capabilities["privilegedInstrumentation"] = probe.capabilities.privilegedInstrumentation;

    QVariantMap result;
    result["success"] = probe.status == killcore::KernelDriverProbeStatus::Connected;
    result["status"] = killcore::KernelDriverBridge::statusToString(probe.status);
    result["devicePath"] = probe.devicePath;
    result["message"] = probe.message;
    result["capabilities"] = capabilities;

    KE_LOG_INFO() << "probeKernelDriver: status=" << result.value("status").toString().toStdString()
                  << " message=" << probe.message.toStdString();
    return result;
}

QVariantMap KernelDriverManager::startKernelDriver() const {
    QVariantMap result;
    result["success"] = false;
    result["serviceName"] = QStringLiteral("KillEngineKernel");
    result["started"] = false;
    result["alreadyRunning"] = false;

#ifndef Q_OS_WIN
    result["status"] = QStringLiteral("unavailable");
    result["devicePath"] = QString::fromWCharArray(killcore::KernelDriverBridge::kDefaultDevicePath);
    result["message"] = QStringLiteral("Démarrage du driver disponible uniquement sur Windows.");
    m_appendScanTelemetry("kernel_driver_start", result);
    return result;
#else
    auto addEmptyCapabilities = [&result]() {
        QVariantMap capabilities;
        capabilities["protocolVersion"] = static_cast<int>(killcore::KernelDriverBridge::kProtocolVersion);
        capabilities["healthProbe"] = false;
        capabilities["processMemoryAccess"] = false;
        capabilities["privilegedInstrumentation"] = false;
        result["capabilities"] = capabilities;
    };

    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) {
        const DWORD error = GetLastError();
        result["status"] = error == ERROR_ACCESS_DENIED ? QStringLiteral("access_denied") : QStringLiteral("error");
        result["devicePath"] = QString::fromWCharArray(killcore::KernelDriverBridge::kDefaultDevicePath);
        result["message"] = QStringLiteral("Impossible d'ouvrir le Service Control Manager: %1").arg(windowsErrorMessage(error));
        result["error"] = result["message"];
        addEmptyCapabilities();
        m_appendScanTelemetry("kernel_driver_start", result);
        return result;
    }

    SC_HANDLE service = OpenServiceW(scm, L"KillEngineKernel", SERVICE_START | SERVICE_QUERY_STATUS);
    if (!service) {
        const DWORD error = GetLastError();
        result["status"] = error == ERROR_SERVICE_DOES_NOT_EXIST ? QStringLiteral("unavailable")
                            : (error == ERROR_ACCESS_DENIED ? QStringLiteral("access_denied") : QStringLiteral("error"));
        result["devicePath"] = QString::fromWCharArray(killcore::KernelDriverBridge::kDefaultDevicePath);
        result["message"] = error == ERROR_SERVICE_DOES_NOT_EXIST
            ? QStringLiteral("Service KillEngineKernel introuvable. Installe d'abord le driver depuis un terminal administrateur.")
            : QStringLiteral("Impossible d'ouvrir le service KillEngineKernel: %1").arg(windowsErrorMessage(error));
        result["error"] = result["message"];
        addEmptyCapabilities();
        CloseServiceHandle(scm);
        m_appendScanTelemetry("kernel_driver_start", result);
        return result;
    }

    SERVICE_STATUS_PROCESS status{};
    DWORD bytesNeeded = 0;
    if (QueryServiceStatusEx(service,
                             SC_STATUS_PROCESS_INFO,
                             reinterpret_cast<LPBYTE>(&status),
                             sizeof(status),
                             &bytesNeeded) &&
        status.dwCurrentState == SERVICE_RUNNING) {
        result["alreadyRunning"] = true;
    } else if (!StartServiceW(service, 0, nullptr)) {
        const DWORD error = GetLastError();
        if (error == ERROR_SERVICE_ALREADY_RUNNING) {
            result["alreadyRunning"] = true;
        } else {
            result["status"] = error == ERROR_ACCESS_DENIED ? QStringLiteral("access_denied") : QStringLiteral("error");
            result["devicePath"] = QString::fromWCharArray(killcore::KernelDriverBridge::kDefaultDevicePath);
            result["message"] = QStringLiteral("Démarrage du service KillEngineKernel échoué: %1").arg(windowsErrorMessage(error));
            result["error"] = result["message"];
            addEmptyCapabilities();
            CloseServiceHandle(service);
            CloseServiceHandle(scm);
            m_appendScanTelemetry("kernel_driver_start", result);
            return result;
        }
    } else {
        result["started"] = true;
    }

    for (int attempt = 0; attempt < 30; ++attempt) {
        if (QueryServiceStatusEx(service,
                                 SC_STATUS_PROCESS_INFO,
                                 reinterpret_cast<LPBYTE>(&status),
                                 sizeof(status),
                                 &bytesNeeded) &&
            status.dwCurrentState == SERVICE_RUNNING) {
            break;
        }
        Sleep(100);
    }

    result["serviceState"] = static_cast<int>(status.dwCurrentState);
    CloseServiceHandle(service);
    CloseServiceHandle(scm);

    const QVariantMap probe = probeKernelDriver();
    const bool serviceStarted = result.value("started").toBool();
    const bool serviceAlreadyRunning = result.value("alreadyRunning").toBool();
    const int serviceState = result.value("serviceState").toInt();
    for (auto it = probe.cbegin(); it != probe.cend(); ++it) {
        result[it.key()] = it.value();
    }
    result["serviceName"] = QStringLiteral("KillEngineKernel");
    result["started"] = serviceStarted;
    result["alreadyRunning"] = serviceAlreadyRunning;
    result["serviceState"] = serviceState;
    if (result.value("success").toBool()) {
        result["message"] = serviceStarted
            ? QStringLiteral("Driver KillEngineKernel démarré et connecté.")
            : QStringLiteral("Driver KillEngineKernel déjà démarré et connecté.");
    } else if (!result.contains("error")) {
        result["error"] = result.value("message");
    }

    KE_LOG_INFO() << "startKernelDriver: status=" << result.value("status").toString().toStdString()
                  << " started=" << result.value("started").toBool()
                  << " alreadyRunning=" << result.value("alreadyRunning").toBool();
    m_appendScanTelemetry("kernel_driver_start", result);
    return result;
#endif
}

QVariantMap KernelDriverManager::readMemoryKernel(const QString& addressHex, int size) const {
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

    const int boundedSize = std::clamp(size, 1, 4096);

#ifdef Q_OS_WIN
    const killcore::KernelDriverBridge bridge;
    const auto targetPid = reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(m_handle.pid()));
    const QByteArray data = bridge.readMemory(targetPid, address, static_cast<size_t>(boundedSize));
    if (data.isEmpty()) {
        result["error"] = "Lecture kernel échouée (driver non chargé/non connecté, adresse invalide côté cible, ou accès refusé).";
        KE_LOG_WARN() << "readMemoryKernel: échec pid=" << m_handle.pid() << " address=0x" << QString::number(address, 16).toStdString();
        return result;
    }
    result["success"] = true;
    result["bytesRead"] = data.size();
    result["hex"] = QString::fromLatin1(data.toHex(' ').toUpper());
    KE_LOG_INFO() << "readMemoryKernel: pid=" << m_handle.pid() << " address=0x" << QString::number(address, 16).toStdString()
                  << " bytesRead=" << data.size();
#else
    result["error"] = "Fonctionnalité Windows uniquement.";
#endif
    return result;
}

QVariantMap KernelDriverManager::writeMemoryKernel(const QString& addressHex, const QString& hexBytes) {
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

    QString hexOnly = hexBytes;
    hexOnly.remove(' ');
    const QByteArray data = QByteArray::fromHex(hexOnly.toLatin1());
    if (data.isEmpty()) {
        result["error"] = "Octets invalides (format hexadécimal attendu, ex: \"90 90 90\").";
        return result;
    }

#ifdef Q_OS_WIN
    const killcore::KernelDriverBridge bridge;
    const auto targetPid = reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(m_handle.pid()));
    const bool written = bridge.writeMemory(targetPid, address, data);
    result["success"] = written;
    if (written) {
        result["bytesWritten"] = data.size();
        KE_LOG_INFO() << "writeMemoryKernel: pid=" << m_handle.pid() << " address=0x" << QString::number(address, 16).toStdString()
                      << " bytesWritten=" << data.size();
    } else {
        result["error"] = "Écriture kernel échouée (driver non chargé/non connecté, adresse invalide côté cible, ou accès refusé).";
        KE_LOG_WARN() << "writeMemoryKernel: échec pid=" << m_handle.pid() << " address=0x" << QString::number(address, 16).toStdString();
    }
#else
    result["error"] = "Fonctionnalité Windows uniquement.";
#endif
    return result;
}

} // namespace killengine
