#include "architecture.h"
#include "logging/logger.h"

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace killcore {

#ifdef Q_OS_WIN

Architecture ArchitectureDetector::detect(HANDLE processHandle) {
    if (!processHandle || processHandle == INVALID_HANDLE_VALUE) {
        return Architecture::Unknown;
    }

    // Try IsWow64Process2 (available on Windows 10+
    USHORT processMachine = 0;
    USHORT nativeMachine  = 0;

    // IsWow64Process2 is not available on all Windows versions
    // We try dynamically loading it
    typedef BOOL(WINAPI * IsWow64Process2Func)(HANDLE, PUSHORT, PUSHORT);
    static auto pIsWow64Process2 = reinterpret_cast<IsWow64Process2Func>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "IsWow64Process2"));

    if (pIsWow64Process2) {
        if (pIsWow64Process2(processHandle, &processMachine, &nativeMachine)) {
            // IMAGE_FILE_MACHINE_I386 = 0x014c (32-bit process on 64-bit OS)
            if (processMachine == IMAGE_FILE_MACHINE_I386) {
                return Architecture::x86;
            }
            // processMachine == 0 means native (not WOW64) → 64-bit on 64-bit OS
            if (processMachine == 0 && nativeMachine == IMAGE_FILE_MACHINE_AMD64) {
                return Architecture::x64;
            }
            return Architecture::x64; // default for other native scenarios
        }
    }

    // Fallback: IsWow64Process (less precise)
    BOOL isWow64 = FALSE;
    if (IsWow64Process(processHandle, &isWow64)) {
        if (isWow64) {
            return Architecture::x86; // 32-bit process on 64-bit OS
        }
        // Not WOW64 — either native 64-bit on 64-bit OS, or 32-bit on 32-bit OS
        // Since KillEngine is 64-bit only, if we're here it's likely x64
        return Architecture::x64;
    }

    return Architecture::Unknown;
}

Architecture ArchitectureDetector::detectByPid(uint32_t pid) {
    if (pid == 0) return Architecture::Unknown;

    HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!handle) {
        KE_LOG_DEBUG() << "ArchitectureDetector: OpenProcess failed for PID " << pid
                       << " (error=" << GetLastError() << ")";
        return Architecture::Unknown;
    }

    Architecture result = detect(handle);
    CloseHandle(handle);
    return result;
}

Architecture ArchitectureDetector::current() {
    // KillEngine itself is x64 only
    return Architecture::x64;
}

#else

Architecture ArchitectureDetector::detect(HANDLE) {
    return Architecture::Unknown;
}

Architecture ArchitectureDetector::detectByPid(uint32_t) {
    return Architecture::Unknown;
}

Architecture ArchitectureDetector::current() {
    return Architecture::Unknown;
}

#endif

} // namespace killcore