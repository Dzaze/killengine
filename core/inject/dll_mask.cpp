#include "dll_mask.h"
#include "logging/logger.h"

#ifdef Q_OS_WIN
#include <windows.h>
#include <winternl.h>
#include <psapi.h>
#endif

#include <vector>
#include <string>
#include <atomic>

namespace killcore {

#ifdef Q_OS_WIN
namespace {

/// Indique si le masquage DLL est actif.
std::atomic<bool> g_dllMaskActive{false};

/// Liste des DLLs masquées (nom -> handle du module).
struct MaskedDllEntry {
    QString dllName;
    HMODULE moduleHandle;
    std::wstring originalPath;
};
QList<MaskedDllEntry> g_maskedDlls;

/// Typedef pour NtUnmapViewOfSection.
typedef NTSTATUS(NTAPI* NtUnmapViewOfSection_t)(
    HANDLE ProcessHandle,
    PVOID BaseAddress);

/// Typedef pour NtSetInformationProcess.
typedef NTSTATUS(NTAPI* NtSetInformationProcess_t)(
    HANDLE ProcessHandle,
    PROCESSINFOCLASS ProcessInformationClass,
    PVOID ProcessInformation,
    ULONG ProcessInformationLength);

/// Masque une DLL dans le processus cible en la déchargeant de la liste des modules.
/// Cette technique utilise NtUnmapViewOfSection pour décharger la DLL du processus cible.
/// Cela rend la DLL invisible pour EnumProcessModules et GetModuleFileNameExW.
bool maskDllInProcess(HANDLE hProcess, const QString& dllName, HMODULE* outModule) {
    HMODULE hModules[1024];
    DWORD cbNeeded;
    if (!EnumProcessModules(hProcess, hModules, sizeof(hModules), &cbNeeded)) {
        return false;
    }

    int moduleCount = cbNeeded / sizeof(HMODULE);
    for (int i = 0; i < moduleCount; i++) {
        wchar_t moduleName[MAX_PATH];
        if (GetModuleFileNameExW(hProcess, hModules[i], moduleName, MAX_PATH)) {
            QString currentName = QString::fromWCharArray(moduleName);
            if (currentName.contains(dllName, Qt::CaseInsensitive)) {
                // Module trouvé — on le décharge du processus cible.
                HMODULE hNtdll = GetModuleHandleW(L"ntdll.dll");
                if (!hNtdll) return false;

                auto pNtUnmapViewOfSection = reinterpret_cast<NtUnmapViewOfSection_t>(
                    GetProcAddress(hNtdll, "NtUnmapViewOfSection"));
                if (!pNtUnmapViewOfSection) return false;

                NTSTATUS status = pNtUnmapViewOfSection(hProcess, hModules[i]);
                if (status != 0) {
                    KE_LOG_WARN() << "DllMask: NtUnmapViewOfSection failed for '"
                                  << dllName.toStdString() << "' (NTSTATUS: " << status << ")";
                    return false;
                }

                if (outModule) *outModule = hModules[i];
                KE_LOG_INFO() << "DllMask: unmaped DLL '" << dllName.toStdString()
                              << "' from process (module: " << currentName.toStdString() << ")";
                return true;
            }
        }
    }

    return false;
}

/// Restaure une DLL masquée en la rechargeant dans le processus cible.
/// Cette technique est plus complexe car il faut recharger la DLL.
/// Pour l'instant, on utilise une approche simplifiée : on charge la DLL
/// via CreateRemoteThread + LoadLibraryW.
bool restoreDllInProcess(HANDLE hProcess, const QString& dllName, HMODULE moduleHandle) {
    // Pour restaurer une DLL déchargée, on doit la recharger.
    // On utilise CreateRemoteThread + LoadLibraryW.
    // Cependant, le chemin de la DLL doit être accessible depuis le processus cible.

    // Trouver le chemin complet de la DLL.
    wchar_t dllPath[MAX_PATH];
    if (!GetModuleFileNameExW(hProcess, moduleHandle, dllPath, MAX_PATH)) {
        // Le module a été déchargé, on ne peut pas récupérer le chemin.
        // On essaie de charger la DLL par son nom simple.
        HMODULE hKernel32 = GetModuleHandleW(L"kernel32.dll");
        if (!hKernel32) return false;

        auto pLoadLibraryW = reinterpret_cast<LPVOID>(
            GetProcAddress(hKernel32, "LoadLibraryW"));
        if (!pLoadLibraryW) return false;

        // Convertir le nom de la DLL en wide string.
        std::wstring wDllName = dllName.toStdWString();

        // Allouer de la mémoire dans le processus cible pour le nom de la DLL.
        SIZE_T nameSize = (wDllName.size() + 1) * sizeof(wchar_t);
        LPVOID remoteName = VirtualAllocEx(hProcess, nullptr, nameSize,
                                           MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!remoteName) return false;

        SIZE_T bytesWritten = 0;
        if (!WriteProcessMemory(hProcess, remoteName, wDllName.c_str(), nameSize, &bytesWritten)) {
            VirtualFreeEx(hProcess, remoteName, 0, MEM_RELEASE);
            return false;
        }

        // Créer un thread distant pour charger la DLL.
        HANDLE hThread = CreateRemoteThread(hProcess, nullptr, 0,
            reinterpret_cast<LPTHREAD_START_ROUTINE>(pLoadLibraryW),
            remoteName, 0, nullptr);
        if (!hThread) {
            VirtualFreeEx(hProcess, remoteName, 0, MEM_RELEASE);
            return false;
        }

        WaitForSingleObject(hThread, 5000);
        CloseHandle(hThread);
        VirtualFreeEx(hProcess, remoteName, 0, MEM_RELEASE);

        return true;
    }

    // Si on a le chemin, on peut le charger directement.
    HMODULE hKernel32 = GetModuleHandleW(L"kernel32.dll");
    if (!hKernel32) return false;

    auto pLoadLibraryW = reinterpret_cast<LPVOID>(
        GetProcAddress(hKernel32, "LoadLibraryW"));
    if (!pLoadLibraryW) return false;

    SIZE_T pathSize = (wcslen(dllPath) + 1) * sizeof(wchar_t);
    LPVOID remotePath = VirtualAllocEx(hProcess, nullptr, pathSize,
                                       MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remotePath) return false;

    SIZE_T bytesWritten = 0;
    if (!WriteProcessMemory(hProcess, remotePath, dllPath, pathSize, &bytesWritten)) {
        VirtualFreeEx(hProcess, remotePath, 0, MEM_RELEASE);
        return false;
    }

    HANDLE hThread = CreateRemoteThread(hProcess, nullptr, 0,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(pLoadLibraryW),
        remotePath, 0, nullptr);
    if (!hThread) {
        VirtualFreeEx(hProcess, remotePath, 0, MEM_RELEASE);
        return false;
    }

    WaitForSingleObject(hThread, 5000);
    CloseHandle(hThread);
    VirtualFreeEx(hProcess, remotePath, 0, MEM_RELEASE);

    return true;
}

} // namespace
#endif

DllMaskResult DllMask::maskDll(uint32_t pid, const QString& dllName) {
    DllMaskResult result;

#ifdef Q_OS_WIN
    HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION, FALSE, pid);
    if (!hProcess) {
        result.error = "Cannot open process (PID: " + QString::number(pid) + ")";
        return result;
    }

    HMODULE foundModule = nullptr;
    bool success = maskDllInProcess(hProcess, dllName, &foundModule);
    CloseHandle(hProcess);

    if (success) {
        g_dllMaskActive = true;
        MaskedDllEntry entry;
        entry.dllName = dllName;
        entry.moduleHandle = foundModule;
        g_maskedDlls.append(entry);
        result.success = true;
        KE_LOG_INFO() << "DllMask: successfully masked DLL '" << dllName.toStdString()
                      << "' in process " << pid;
    } else {
        result.error = "Failed to mask DLL '" + dllName + "' in process " + QString::number(pid);
    }
#else
    (void)pid;
    (void)dllName;
    result.error = "DLL masking is Windows-only";
#endif

    return result;
}

DllMaskResult DllMask::restoreDll(uint32_t pid, const QString& dllName) {
    DllMaskResult result;

#ifdef Q_OS_WIN
    // Trouver l'entrée masquée.
    int foundIndex = -1;
    for (int i = 0; i < g_maskedDlls.size(); i++) {
        if (g_maskedDlls[i].dllName.compare(dllName, Qt::CaseInsensitive) == 0) {
            foundIndex = i;
            break;
        }
    }

    if (foundIndex < 0) {
        result.error = "DLL '" + dllName + "' is not currently masked";
        return result;
    }

    HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION, FALSE, pid);
    if (!hProcess) {
        result.error = "Cannot open process (PID: " + QString::number(pid) + ")";
        return result;
    }

    bool success = restoreDllInProcess(hProcess, dllName, g_maskedDlls[foundIndex].moduleHandle);
    CloseHandle(hProcess);

    if (success) {
        g_maskedDlls.removeAt(foundIndex);
        if (g_maskedDlls.isEmpty()) g_dllMaskActive = false;
        result.success = true;
        KE_LOG_INFO() << "DllMask: successfully restored DLL '" << dllName.toStdString()
                      << "' in process " << pid;
    } else {
        result.error = "Failed to restore DLL '" + dllName + "' in process " + QString::number(pid);
    }
#else
    (void)pid;
    (void)dllName;
    result.error = "DLL masking is Windows-only";
#endif

    return result;
}

} // namespace killcore