#include "process_enumerator.h"
#include "architecture.h"
#include "logging/logger.h"

#ifdef Q_OS_WIN
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#endif

#include <algorithm>
#include <unordered_set>

namespace killcore {

#ifdef Q_OS_WIN

// ---------------------------------------------------------------------------
// Helper: collecter les PIDs qui possèdent une fenêtre visible
// ---------------------------------------------------------------------------
struct WindowEnumData {
    std::unordered_set<uint32_t> pids;
};

static BOOL CALLBACK enumWindowsCallback(HWND hwnd, LPARAM lParam) {
    auto* data = reinterpret_cast<WindowEnumData*>(lParam);

    // Only windows that are visible
    if (!IsWindowVisible(hwnd)) {
        return TRUE;
    }

    // Must have a title
    wchar_t title[256] = {0};
    int titleLen = GetWindowTextW(hwnd, title, 256);
    if (titleLen == 0) {
        return TRUE;
    }

    // Get the PID
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid > 0) {
        data->pids.insert(static_cast<uint32_t>(pid));
    }

    return TRUE;
}

static std::unordered_set<uint32_t> collectPidsWithWindows() {
    WindowEnumData data;
    EnumWindows(enumWindowsCallback, reinterpret_cast<LPARAM>(&data));
    return data.pids;
}

static QList<ProcessModuleInfo> enumerateProcessModules(uint32_t pid) {
    QList<ProcessModuleInfo> modules;

    if (pid == 0) {
        return modules;
    }

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snapshot == INVALID_HANDLE_VALUE) {
        KE_LOG_DEBUG() << "ProcessEnumerator: module snapshot failed for PID " << pid
                       << " (error=" << GetLastError() << ")";
        return modules;
    }

    MODULEENTRY32W entry;
    entry.dwSize = sizeof(entry);

    if (Module32FirstW(snapshot, &entry)) {
        do {
            ProcessModuleInfo module;
            module.name = QString::fromWCharArray(entry.szModule);
            module.path = QString::fromWCharArray(entry.szExePath);
            module.baseAddress = reinterpret_cast<quint64>(entry.modBaseAddr);
            module.size = static_cast<quint64>(entry.modBaseSize);
            modules.append(std::move(module));
        } while (Module32NextW(snapshot, &entry));
    } else {
        KE_LOG_DEBUG() << "ProcessEnumerator: Module32FirstW failed for PID " << pid
                       << " (error=" << GetLastError() << ")";
    }

    CloseHandle(snapshot);
    return modules;
}

// ---------------------------------------------------------------------------
// ProcessEnumerator implementation
// ---------------------------------------------------------------------------

QList<ProcessInfo> ProcessEnumerator::enumerate() {
    QList<ProcessInfo> result;
    const auto windowPids = collectPidsWithWindows();

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        KE_LOG_ERROR() << "ProcessEnumerator: CreateToolhelp32Snapshot failed (error=" << GetLastError() << ")";
        return result;
    }

    PROCESSENTRY32W entry;
    entry.dwSize = sizeof(entry);

    if (Process32FirstW(snapshot, &entry)) {
        do {
            ProcessInfo info;
            info.pid = entry.th32ProcessID;
            info.name = QString::fromWCharArray(entry.szExeFile);
            info.hasWindow = windowPids.count(info.pid) > 0;

            // Get full path and architecture (requires opening the process)
            HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
            if (hProcess) {
                wchar_t buf[MAX_PATH] = {0};
                DWORD size = MAX_PATH;
                if (QueryFullProcessImageNameW(hProcess, 0, buf, &size)) {
                    info.executablePath = QString::fromWCharArray(buf, static_cast<int>(size));
                }
                info.arch = ArchitectureDetector::detect(hProcess);
                CloseHandle(hProcess);
            }

            info.modules = enumerateProcessModules(info.pid);

            result.append(std::move(info));
        } while (Process32NextW(snapshot, &entry));
    }

    CloseHandle(snapshot);

    KE_LOG_DEBUG() << "ProcessEnumerator: enumerated " << result.size() << " processes";
    return result;
}

QList<ProcessInfo> ProcessEnumerator::enumerateWithWindows() {
    QList<ProcessInfo> all = enumerate();
    QList<ProcessInfo> withWindows;

    for (auto& info : all) {
        if (info.hasWindow) {
            withWindows.append(std::move(info));
        }
    }

    // Sort by name
    std::sort(withWindows.begin(), withWindows.end(),
              [](const ProcessInfo& a, const ProcessInfo& b) {
                  return a.name.toLower() < b.name.toLower();
              });

    KE_LOG_DEBUG() << "ProcessEnumerator: " << withWindows.size() << " processes with windows";
    return withWindows;
}

ProcessInfo ProcessEnumerator::foregroundProcess() {
    ProcessInfo info;

    HWND hwnd = GetForegroundWindow();
    if (!hwnd) return info;

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0) return info;

    info.pid = pid;
    info.hasWindow = true;

    // Get process name and arch
    HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (hProcess) {
        wchar_t buf[MAX_PATH] = {0};
        DWORD size = MAX_PATH;
        if (QueryFullProcessImageNameW(hProcess, 0, buf, &size)) {
            info.executablePath = QString::fromWCharArray(buf, static_cast<int>(size));
            int lastSlash = info.executablePath.lastIndexOf(QChar('\\'));
            if (lastSlash >= 0) {
                info.name = info.executablePath.mid(lastSlash + 1);
            } else {
                info.name = info.executablePath;
            }
        }
        info.arch = ArchitectureDetector::detect(hProcess);
        info.modules = enumerateProcessModules(info.pid);
        CloseHandle(hProcess);
    }

    return info;
}

QList<ProcessModuleInfo> ProcessEnumerator::enumerateModules(uint32_t pid) {
    return enumerateProcessModules(pid);
}

#else
// Non-Windows stubs
QList<ProcessInfo> ProcessEnumerator::enumerate() { return {}; }
QList<ProcessInfo> ProcessEnumerator::enumerateWithWindows() { return {}; }
ProcessInfo ProcessEnumerator::foregroundProcess() { return {}; }
QList<ProcessModuleInfo> ProcessEnumerator::enumerateModules(uint32_t) { return {}; }
#endif

QString ProcessEnumerator::architectureToString(Architecture arch) {
    switch (arch) {
        case Architecture::x86: return "x86";
        case Architecture::x64: return "x64";
        default:                return "Unknown";
    }
}

} // namespace killcore
