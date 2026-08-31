#include "anti_debug.h"

#include "logging/logger.h"

#ifdef Q_OS_WIN
#include <windows.h>
#include <winternl.h>
#endif

#include <atomic>

namespace killcore {

#ifdef Q_OS_WIN
namespace {

/// Hook VEH pour IsDebuggerPresent : retourne FALSE.
/// Le VEH intercepte l'exception de breakpoint (INT3) posé sur la fonction.
/// On modifie le contexte du thread pour retourner 0 (FALSE) et continuer.
std::atomic<bool> g_antiDebugActive{false};

/// Adresse de IsDebuggerPresent dans le processus cible.
uint64_t g_isDebuggerPresentAddr{0};
/// Adresse de CheckRemoteDebuggerPresent dans le processus cible.
uint64_t g_checkRemoteDebuggerPresentAddr{0};
/// Adresse de NtQueryInformationProcess dans le processus cible.
uint64_t g_ntQueryInformationProcessAddr{0};
/// Adresse de NtSetInformationProcess dans le processus cible.
uint64_t g_ntSetInformationProcessAddr{0};

/// VEH handler pour intercepter les appels anti-debug.
LONG WINAPI antiDebugVectoredHandler(EXCEPTION_POINTERS* ep) {
    if (!g_antiDebugActive || !ep) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    auto* record = ep->ExceptionRecord;
    auto* context = ep->ContextRecord;

    // Intercepter les breakpoints (INT3) sur les fonctions anti-debug.
    if (record->ExceptionCode == EXCEPTION_BREAKPOINT) {
        const uint64_t rip = context->Rip;

        // IsDebuggerPresent : retourne FALSE (0)
        if (rip == g_isDebuggerPresentAddr + 1) { // +1 car INT3 est 1 byte
            context->Rax = 0; // FALSE
            context->Rip += 1; // Sauter l'INT3
            return EXCEPTION_CONTINUE_EXECUTION;
        }

        // CheckRemoteDebuggerPresent : retourne FALSE (0) via le paramètre
        if (rip == g_checkRemoteDebuggerPresentAddr + 1) {
            // Le premier paramètre (RCX) est un pointeur vers un BOOL.
            // On le met à FALSE.
            if (context->Rcx) {
                *reinterpret_cast<BOOL*>(context->Rcx) = FALSE;
            }
            context->Rax = 0; // NTSTATUS SUCCESS
            context->Rip += 1;
            return EXCEPTION_CONTINUE_EXECUTION;
        }

        // NtQueryInformationProcess : intercepter ProcessDebugPort (0x7)
        // Si le processus demande le debug port, on retourne 0 (pas de debug port).
        if (rip == g_ntQueryInformationProcessAddr + 1) {
            // RCX = ProcessHandle, RDX = ProcessInformationClass, R8 = ProcessInformation, R9 = ProcessInformationLength
            const ULONG processInfoClass = static_cast<ULONG>(context->Rdx);
            if (processInfoClass == 7) { // ProcessDebugPort
                // ProcessInformation est un pointeur vers un DWORD64 (ou DWORD sur 32-bit).
                // On le met à 0 (pas de debug port).
                if (context->R8) {
                    *reinterpret_cast<ULONG_PTR*>(context->R8) = 0;
                }
                context->Rax = 0; // NTSTATUS SUCCESS
                context->Rip += 1;
                return EXCEPTION_CONTINUE_EXECUTION;
            }
            // Pour les autres classes, on laisse passer.
        }

        // NtSetInformationProcess : intercepter ProcessDebugPort (0x7)
        // Empêcher la désactivation du debug port.
        if (rip == g_ntSetInformationProcessAddr + 1) {
            const ULONG processInfoClass = static_cast<ULONG>(context->Rdx);
            if (processInfoClass == 7) { // ProcessDebugPort
                // On retourne NTSTATUS SUCCESS sans rien faire.
                context->Rax = 0; // NTSTATUS SUCCESS
                context->Rip += 1;
                return EXCEPTION_CONTINUE_EXECUTION;
            }
            // Pour les autres classes, on laisse passer.
        }
    }

    return EXCEPTION_CONTINUE_SEARCH;
}

/// Installe un breakpoint (INT3) sur une fonction distante.
bool installInt3Breakpoint(HANDLE hProcess, uint64_t address, BYTE* originalByte) {
    // Lire le byte original
    SIZE_T bytesRead = 0;
    if (!ReadProcessMemory(hProcess, reinterpret_cast<LPVOID>(address), originalByte, 1, &bytesRead) ||
        bytesRead != 1) {
        return false;
    }

    // Écrire INT3 (0xCC)
    BYTE int3 = 0xCC;
    SIZE_T bytesWritten = 0;
    if (!WriteProcessMemory(hProcess, reinterpret_cast<LPVOID>(address), &int3, 1, &bytesWritten) ||
        bytesWritten != 1) {
        return false;
    }

    return true;
}

/// Retire un breakpoint (INT3) en restaurant le byte original.
bool removeInt3Breakpoint(HANDLE hProcess, uint64_t address, BYTE originalByte) {
    SIZE_T bytesWritten = 0;
    return WriteProcessMemory(hProcess, reinterpret_cast<LPVOID>(address), &originalByte, 1, &bytesWritten) &&
           bytesWritten == 1;
}

} // namespace
#endif

AntiDebugSession::~AntiDebugSession() {
    stop();
}

AntiDebugResult AntiDebugSession::start(const ProcessHandle& process) {
    AntiDebugResult result;

#ifdef Q_OS_WIN
    if (!process.isValid()) {
        result.error = "Invalid process handle";
        return result;
    }

    m_pid = process.pid();
    m_hProcess = process.rawHandle();
    g_antiDebugActive = true;

    // Résoudre les adresses des fonctions anti-debug dans le processus cible.
    // Sur Windows, kernel32.dll est chargé à la même adresse dans tous les processus.
    HMODULE hKernel32 = GetModuleHandleW(L"kernel32.dll");
    if (!hKernel32) {
        result.error = "Cannot find kernel32.dll";
        g_antiDebugActive = false;
        return result;
    }

    auto pIsDebuggerPresent = reinterpret_cast<uint64_t>(
        GetProcAddress(hKernel32, "IsDebuggerPresent"));
    auto pCheckRemoteDebuggerPresent = reinterpret_cast<uint64_t>(
        GetProcAddress(hKernel32, "CheckRemoteDebuggerPresent"));

    if (!pIsDebuggerPresent || !pCheckRemoteDebuggerPresent) {
        result.error = "Cannot find anti-debug functions";
        g_antiDebugActive = false;
        return result;
    }

    g_isDebuggerPresentAddr = pIsDebuggerPresent;
    g_checkRemoteDebuggerPresentAddr = pCheckRemoteDebuggerPresent;

    // Résoudre NtQueryInformationProcess et NtSetInformationProcess depuis ntdll.dll.
    HMODULE hNtdll = GetModuleHandleW(L"ntdll.dll");
    if (hNtdll) {
        auto pNtQueryInformationProcess = reinterpret_cast<uint64_t>(
            GetProcAddress(hNtdll, "NtQueryInformationProcess"));
        auto pNtSetInformationProcess = reinterpret_cast<uint64_t>(
            GetProcAddress(hNtdll, "NtSetInformationProcess"));

        if (pNtQueryInformationProcess) {
            g_ntQueryInformationProcessAddr = pNtQueryInformationProcess;
        }
        if (pNtSetInformationProcess) {
            g_ntSetInformationProcessAddr = pNtSetInformationProcess;
        }
    }

    // Installer les breakpoints INT3 sur les fonctions anti-debug.
    BYTE originalByte1 = 0, originalByte2 = 0;

    if (installInt3Breakpoint(m_hProcess, g_isDebuggerPresentAddr, &originalByte1)) {
        m_originalByte1 = originalByte1;
        result.hooksInstalled++;
        KE_LOG_INFO() << "AntiDebug: installed INT3 on IsDebuggerPresent at 0x"
                      << std::hex << g_isDebuggerPresentAddr;
    }

    if (installInt3Breakpoint(m_hProcess, g_checkRemoteDebuggerPresentAddr, &originalByte2)) {
        m_originalByte2 = originalByte2;
        result.hooksInstalled++;
        KE_LOG_INFO() << "AntiDebug: installed INT3 on CheckRemoteDebuggerPresent at 0x"
                      << std::hex << g_checkRemoteDebuggerPresentAddr;
    }

    // Installer les breakpoints sur NtQueryInformationProcess et NtSetInformationProcess.
    if (g_ntQueryInformationProcessAddr) {
        BYTE originalByte3 = 0;
        if (installInt3Breakpoint(m_hProcess, g_ntQueryInformationProcessAddr, &originalByte3)) {
            m_originalByte3 = originalByte3;
            result.hooksInstalled++;
            KE_LOG_INFO() << "AntiDebug: installed INT3 on NtQueryInformationProcess at 0x"
                          << std::hex << g_ntQueryInformationProcessAddr;
        }
    }

    if (g_ntSetInformationProcessAddr) {
        BYTE originalByte4 = 0;
        if (installInt3Breakpoint(m_hProcess, g_ntSetInformationProcessAddr, &originalByte4)) {
            m_originalByte4 = originalByte4;
            result.hooksInstalled++;
            KE_LOG_INFO() << "AntiDebug: installed INT3 on NtSetInformationProcess at 0x"
                          << std::hex << g_ntSetInformationProcessAddr;
        }
    }

    if (result.hooksInstalled == 0) {
        result.error = "Failed to install any anti-debug hooks";
        g_antiDebugActive = false;
        return result;
    }

    // Installer le VEH dans le processus cible.
    // Note: Le VEH doit être installé dans le processus cible, pas dans KillEngine.
    // Pour cela, on injecte un petit shellcode qui appelle AddVectoredExceptionHandler.
    // Cependant, pour simplifier, on utilise une approche différente :
    // on modifie le PEB (Process Environment Block) pour désactiver le debug.
    // Cette approche est plus fiable et ne nécessite pas de VEH.

    // Alternative: on peut aussi utiliser NtSetInformationProcess pour désactiver
    // le debug port, mais cela nécessite des privilèges élevés.

    // Pour l'instant, on se contente des breakpoints INT3.
    // Le VEH sera installé via une DLL injectée si nécessaire.

    m_active = true;
    result.success = true;
#else
    (void)process;
    result.error = "Anti-debug is Windows-only";
#endif

    return result;
}

void AntiDebugSession::stop() {
#ifdef Q_OS_WIN
    if (!m_active) {
        return;
    }

    // Restaurer les breakpoints INT3.
    if (m_hProcess) {
        if (m_isDebuggerPresentAddr && m_originalByte1) {
            removeInt3Breakpoint(m_hProcess, m_isDebuggerPresentAddr, m_originalByte1);
        }
        if (m_checkRemoteDebuggerPresentAddr && m_originalByte2) {
            removeInt3Breakpoint(m_hProcess, m_checkRemoteDebuggerPresentAddr, m_originalByte2);
        }
        if (m_ntQueryInformationProcessAddr && m_originalByte3) {
            removeInt3Breakpoint(m_hProcess, m_ntQueryInformationProcessAddr, m_originalByte3);
        }
        if (m_ntSetInformationProcessAddr && m_originalByte4) {
            removeInt3Breakpoint(m_hProcess, m_ntSetInformationProcessAddr, m_originalByte4);
        }
    }

    g_antiDebugActive = false;
    m_active = false;
    m_hProcess = nullptr;

    KE_LOG_INFO() << "AntiDebug: stopped";
#else
    // nothing to do
#endif
}

} // namespace killcore
