#include "process_suspend.h"
#include "logging/logger.h"

#include <windows.h>
#include <tlhelp32.h>

namespace killcore {

ProcessThreadsSuspendGuard::ProcessThreadsSuspendGuard(uint32_t pid, uint32_t extraExcludedThreadId) {
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
            if (extraExcludedThreadId != 0 && entry.th32ThreadID == extraExcludedThreadId) {
                continue;
            }
            // THREAD_GET_CONTEXT/SET_CONTEXT en plus de SUSPEND_RESUME : les
            // appelants qui veulent manipuler les registres de debug pendant
            // que tout est fige (ex: armer DR0-DR7 sans la course qui a fait
            // planter core/debug/inprocess_breakpoint_handler.cpp le
            // 19/08/2026 — suspendre une a une PENDANT que d'autres threads
            // continuent de tourner) reutilisent directement ce handle plutot
            // que de rouvrir la thread une deuxieme fois.
            const HANDLE threadHandle = OpenThread(
                THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT,
                FALSE, entry.th32ThreadID);
            if (!threadHandle) {
                continue;
            }
            if (SuspendThread(threadHandle) == static_cast<DWORD>(-1)) {
                CloseHandle(threadHandle);
                continue;
            }
            m_threads.append(SuspendedThreadHandle{entry.th32ThreadID, static_cast<void*>(threadHandle)});
            ++m_suspendedCount;
        } while (Thread32Next(snapshot, &entry));
    }

    CloseHandle(snapshot);
}

ProcessThreadsSuspendGuard::~ProcessThreadsSuspendGuard() {
    for (const auto& thread : m_threads) {
        const HANDLE threadHandle = static_cast<HANDLE>(thread.handle);
        ResumeThread(threadHandle);
        CloseHandle(threadHandle);
    }
}

} // namespace killcore
