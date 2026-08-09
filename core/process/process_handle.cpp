#include "process_handle.h"
#include "architecture.h"
#include "logging/logger.h"

#ifdef Q_OS_WIN
#include <windows.h>
#include <psapi.h>
#endif

namespace killcore {

#ifdef Q_OS_WIN

DWORD ProcessHandle::accessFlags(ProcessAccess access) {
    switch (access) {
        case ProcessAccess::ReadOnly:
            return PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ;
        case ProcessAccess::ReadWrite:
            return PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ
                 | PROCESS_VM_WRITE | PROCESS_VM_OPERATION;
        case ProcessAccess::AllAccess:
            return PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ
                 | PROCESS_VM_WRITE | PROCESS_VM_OPERATION;
        default:
            return PROCESS_QUERY_LIMITED_INFORMATION;
    }
}

#else
// Non-Windows stubs
#endif

ProcessHandle::ProcessHandle() = default;

ProcessHandle::ProcessHandle(uint32_t pid, ProcessAccess access) {
    open(pid, access);
}

ProcessHandle::~ProcessHandle() {
    close();
}

ProcessHandle::ProcessHandle(ProcessHandle&& other) noexcept
    : m_handle(other.m_handle)
    , m_pid(other.m_pid)
    , m_access(other.m_access)
    , m_arch(other.m_arch)
    , m_executableName(std::move(other.m_executableName)) {
    other.m_handle = nullptr;
    other.m_pid = 0;
}

ProcessHandle& ProcessHandle::operator=(ProcessHandle&& other) noexcept {
    if (this != &other) {
        close();
        m_handle = other.m_handle;
        m_pid = other.m_pid;
        m_access = other.m_access;
        m_arch = other.m_arch;
        m_executableName = std::move(other.m_executableName);
        other.m_handle = nullptr;
        other.m_pid = 0;
    }
    return *this;
}

bool ProcessHandle::open(uint32_t pid, ProcessAccess access) {
    close();

    if (pid == 0) return false;

#ifdef Q_OS_WIN
    DWORD flags = accessFlags(access);
    HANDLE h = OpenProcess(flags, FALSE, pid);
    if (!h) {
        DWORD err = GetLastError();
        KE_LOG_DEBUG() << "ProcessHandle: OpenProcess failed for PID " << pid
                       << " (flags=0x" << std::hex << flags << ", error=" << std::dec << err << ")";
        return false;
    }

    m_handle = h;
    m_pid = pid;
    m_access = access;

    refreshInfo();

    KE_LOG_INFO() << "ProcessHandle: opened PID " << pid
                  << " (" << m_executableName.toStdString() << ")"
                  << " arch=" << ProcessEnumerator::architectureToString(m_arch).toStdString();
    return true;
#else
    return false;
#endif
}

void ProcessHandle::close() {
#ifdef Q_OS_WIN
    if (m_handle) {
        CloseHandle(m_handle);
        m_handle = nullptr;
    }
#endif
    m_pid = 0;
    m_arch = Architecture::Unknown;
    m_executableName.clear();
}

bool ProcessHandle::isValid() const {
#ifdef Q_OS_WIN
    return m_handle != nullptr;
#else
    return false;
#endif
}

QString ProcessHandle::executablePath() const {
#ifdef Q_OS_WIN
    if (!isValid()) return {};

    wchar_t buf[MAX_PATH] = {0};
    DWORD size = MAX_PATH;
    if (QueryFullProcessImageNameW(m_handle, 0, buf, &size)) {
        return QString::fromWCharArray(buf, static_cast<int>(size));
    }
    return {};
#else
    return {};
#endif
}

void ProcessHandle::refreshInfo() {
    if (!isValid()) return;

#ifdef Q_OS_WIN
    // Get executable name
    wchar_t buf[MAX_PATH] = {0};
    DWORD size = MAX_PATH;
    if (QueryFullProcessImageNameW(m_handle, 0, buf, &size)) {
        QString fullPath = QString::fromWCharArray(buf, static_cast<int>(size));
        int lastSlash = fullPath.lastIndexOf(QChar('\\'));
        if (lastSlash >= 0) {
            m_executableName = fullPath.mid(lastSlash + 1);
        } else {
            m_executableName = fullPath;
        }
    }

    // Detect architecture
    m_arch = ArchitectureDetector::detect(m_handle);
#endif
}

} // namespace killcore