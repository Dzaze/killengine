#include "process_suspend.h"
#include "logging/logger.h"

#include <windows.h>
#include <tlhelp32.h>

namespace killcore {

ProcessThreadsSuspendGuard::ProcessThreadsSuspendGuard(uint32_t pid) {
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        KE_LOG_WARN() << "ProcessThreadsSuspendGuard: CreateToolhelp32Snapshot failed, error="
                      << GetLastError();
        return;
    }

    const DWORD callingThreadId = GetCurrentThreadId();

    THREADENTRY32 entry{};
    entry.dwSize = sizeof(THREADENTRY32);

    if (Thread32First(snapshot, &entry)) {
        do {
            if (entry.th32OwnerProcessID != pid) {
                continue;
            }
            // Ne jamais suspendre le thread appelant : si la cible attachee
            // est KillEngine lui-meme (auto-attache, deliberement ou par
            // accident), suspendre ce thread bloquerait a jamais l'execution
            // qui doit justement le reprendre plus tard (auto-deadlock
            // constate en test le 19/08/2026 — voir docs/PHASE_TRACKER.md).
            if (entry.th32ThreadID == callingThreadId) {
                continue;
            }
            const HANDLE threadHandle = OpenThread(THREAD_SUSPEND_RESUME, FALSE, entry.th32ThreadID);
            if (!threadHandle) {
                continue;
            }
            if (SuspendThread(threadHandle) == static_cast<DWORD>(-1)) {
                CloseHandle(threadHandle);
                continue;
            }
            m_threadHandles.append(static_cast<void*>(threadHandle));
            ++m_suspendedCount;
        } while (Thread32Next(snapshot, &entry));
    }

    CloseHandle(snapshot);
}

ProcessThreadsSuspendGuard::~ProcessThreadsSuspendGuard() {
    for (void* rawHandle : m_threadHandles) {
        const HANDLE threadHandle = static_cast<HANDLE>(rawHandle);
        ResumeThread(threadHandle);
        CloseHandle(threadHandle);
    }
}

} // namespace killcore
