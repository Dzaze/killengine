#include "anti_debug.h"

#include "logging/logger.h"

#ifdef Q_OS_WIN
#include <windows.h>
#include <winternl.h>
#endif

namespace killcore {

#ifdef Q_OS_WIN
namespace {

/// Typedef pour NtQueryInformationProcess (variante 5 args, winternl).
typedef NTSTATUS(NTAPI* NtQueryInformationProcess_t)(
    HANDLE ProcessHandle,
    PROCESSINFOCLASS ProcessInformationClass,
    PVOID ProcessInformation,
    ULONG ProcessInformationLength,
    PULONG ReturnLength);

/// Récupère l'adresse du PEB du processus cible via
/// NtQueryInformationProcess(ProcessBasicInformation).
uint64_t getProcessPebAddress(HANDLE hProcess, QString* outError) {
    auto fail = [&](const QString& reason) {
        if (outError) *outError = reason;
        return 0;
    };

    HMODULE hNtdll = GetModuleHandleW(L"ntdll.dll");
    if (!hNtdll) return fail("Cannot find ntdll.dll");

    auto pNtQueryInformationProcess = reinterpret_cast<NtQueryInformationProcess_t>(
        GetProcAddress(hNtdll, "NtQueryInformationProcess"));
    if (!pNtQueryInformationProcess) return fail("Cannot find NtQueryInformationProcess");

    PROCESS_BASIC_INFORMATION pbi{};
    ULONG returnLength = 0;
    NTSTATUS status = pNtQueryInformationProcess(
        hProcess,
        ProcessBasicInformation,
        &pbi,
        sizeof(pbi),
        &returnLength);

    if (status != 0) {
        return fail(QString("NtQueryInformationProcess(ProcessBasicInformation) failed (NTSTATUS: 0x%1)")
                        .arg(static_cast<quint32>(status), 8, 16, QChar('0')));
    }
    if (!pbi.PebBaseAddress) return fail("PEB address is null");
    return reinterpret_cast<uint64_t>(pbi.PebBaseAddress);
}

/// Lit `size` octets à l'adresse distante donnée.
bool readRemote(HANDLE hProcess, uint64_t address, void* buffer, size_t size) {
    SIZE_T bytesRead = 0;
    return ReadProcessMemory(hProcess, reinterpret_cast<LPVOID>(address), buffer, size, &bytesRead)
        && bytesRead == size;
}

/// Écrit `size` octets à l'adresse distante donnée.
bool writeRemote(HANDLE hProcess, uint64_t address, const void* buffer, size_t size) {
    SIZE_T bytesWritten = 0;
    return WriteProcessMemory(hProcess, reinterpret_cast<LPVOID>(address), buffer, size, &bytesWritten)
        && bytesWritten == size;
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

    // Récupérer l'adresse du PEB de la cible.
    QString pebError;
    m_pebAddress = getProcessPebAddress(m_hProcess, &pebError);
    if (!m_pebAddress) {
        result.error = QString("Cannot read target PEB: %1").arg(pebError);
        return result;
    }
    result.pebAddress = m_pebAddress;

    // --- BeingDebugged (PEB+0x2, x64) ---
    if (!readRemote(m_hProcess, m_pebAddress + 0x2, &m_originalBeingDebugged, sizeof(m_originalBeingDebugged))) {
        result.error = QString("Failed to read PEB+0x2 (BeingDebugged) at 0x%1")
                           .arg(m_pebAddress + 0x2, 16, QChar('0'));
        return result;
    }
    const BYTE beingDebugged = 0;
    if (!writeRemote(m_hProcess, m_pebAddress + 0x2, &beingDebugged, sizeof(beingDebugged))) {
        result.error = QString("Failed to write PEB+0x2 (BeingDebugged) at 0x%1")
                           .arg(m_pebAddress + 0x2, 16, QChar('0'));
        return result;
    }
    result.fieldsPatched++;

    // --- NtGlobalFlag (PEB+0xBC, x64) : effacer les bits de check heap (0x70) ---
    DWORD ntGlobalFlag = 0;
    if (!readRemote(m_hProcess, m_pebAddress + 0xBC, &ntGlobalFlag, sizeof(ntGlobalFlag))) {
        result.error = QString("Failed to read PEB+0xBC (NtGlobalFlag) at 0x%1")
                           .arg(m_pebAddress + 0xBC, 16, QChar('0'));
        return result;
    }
    m_originalNtGlobalFlag = ntGlobalFlag;
    const DWORD patchedFlag = ntGlobalFlag & ~0x70;
    if (patchedFlag != ntGlobalFlag) {
        if (!writeRemote(m_hProcess, m_pebAddress + 0xBC, &patchedFlag, sizeof(patchedFlag))) {
            result.error = QString("Failed to write PEB+0xBC (NtGlobalFlag) at 0x%1")
                               .arg(m_pebAddress + 0xBC, 16, QChar('0'));
            return result;
        }
        result.fieldsPatched++;
    }

    // --- DebugObjectHandle (PEB+0x1C, x64) ---
    if (!readRemote(m_hProcess, m_pebAddress + 0x1C, &m_originalDebugObjectHandle, sizeof(m_originalDebugObjectHandle))) {
        result.error = QString("Failed to read PEB+0x1C (DebugObjectHandle) at 0x%1")
                           .arg(m_pebAddress + 0x1C, 16, QChar('0'));
        return result;
    }
    if (m_originalDebugObjectHandle != 0) {
        const uint64_t nullHandle = 0;
        if (!writeRemote(m_hProcess, m_pebAddress + 0x1C, &nullHandle, sizeof(nullHandle))) {
            result.error = QString("Failed to write PEB+0x1C (DebugObjectHandle) at 0x%1")
                               .arg(m_pebAddress + 0x1C, 16, QChar('0'));
            return result;
        }
        result.fieldsPatched++;
    }

    if (result.fieldsPatched == 0) {
        // PEB déjà propre (pas de debugger visible) — pas d'erreur, rien à faire.
        result.success = true;
        m_active = true;
        KE_LOG_INFO() << "AntiDebug: PEB already clean (PID " << m_pid << "), nothing to patch";
        return result;
    }

    m_active = true;
    result.success = true;
    KE_LOG_INFO() << "AntiDebug: patched PEB of PID " << m_pid
                  << " (fields: " << result.fieldsPatched
                  << ", BeingDebugged was " << static_cast<int>(m_originalBeingDebugged)
                  << ", NtGlobalFlag was 0x" << std::hex << m_originalNtGlobalFlag
                  << ", DebugObjectHandle was 0x" << m_originalDebugObjectHandle << ")";
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

    if (m_hProcess && m_pebAddress) {
        // Restaurer BeingDebugged.
        if (m_originalBeingDebugged != 0) {
            writeRemote(m_hProcess, m_pebAddress + 0x2, &m_originalBeingDebugged, sizeof(m_originalBeingDebugged));
        }
        // Restaurer NtGlobalFlag (seulement si on l'avait modifié).
        if ((m_originalNtGlobalFlag & 0x70) != 0) {
            writeRemote(m_hProcess, m_pebAddress + 0xBC, &m_originalNtGlobalFlag, sizeof(m_originalNtGlobalFlag));
        }
        // Restaurer DebugObjectHandle.
        if (m_originalDebugObjectHandle != 0) {
            writeRemote(m_hProcess, m_pebAddress + 0x1C, &m_originalDebugObjectHandle, sizeof(m_originalDebugObjectHandle));
        }
    }

    m_active = false;
    m_hProcess = nullptr;
    m_pebAddress = 0;
    m_originalBeingDebugged = 0;
    m_originalNtGlobalFlag = 0;
    m_originalDebugObjectHandle = 0;

    KE_LOG_INFO() << "AntiDebug: restored original PEB values";
#else
    // nothing to do
#endif
}

} // namespace killcore
