#include "process_mask.h"

#include "logging/logger.h"

#ifdef Q_OS_WIN
#include <windows.h>
#include <winternl.h>
#endif

#include <atomic>

namespace killcore {

#ifdef Q_OS_WIN
namespace {

/// Nom original du processus (sauvegardé au démarrage).
QString g_originalProcessName;

/// Typedef pour NtSetInformationProcess.
typedef NTSTATUS(NTAPI* NtSetInformationProcess_t)(
    HANDLE ProcessHandle,
    PROCESSINFOCLASS ProcessInformationClass,
    PVOID ProcessInformation,
    ULONG ProcessInformationLength);

/// Typedef pour NtQueryInformationProcess.
typedef NTSTATUS(NTAPI* NtQueryInformationProcess_t)(
    HANDLE ProcessHandle,
    PROCESSINFOCLASS ProcessInformationClass,
    PVOID ProcessInformation,
    ULONG ProcessInformationLength,
    PULONG ReturnLength);

/// Typedef pour RtlInitUnicodeString.
typedef void(NTAPI* RtlInitUnicodeString_t)(
    PUNICODE_STRING DestinationString,
    PCWSTR SourceString);

/// Typedef pour NtSetInformationProcess avec ProcessBasicInformation.
/// ProcessBasicInformation = 0
/// PEB contient le nom du processus dans PEB->ProcessParameters->ImagePathName.

/// Structure PEB partiellement définie pour accéder au nom du processus.
/// On utilise une approche indirecte : on modifie le nom dans le PEB
/// en écrivant directement dans la mémoire du processus cible.

/// Adresse du PEB du processus cible (récupérée via NtQueryInformationProcess).
uint64_t g_targetPebAddress{0};

/// Nom original sauvegardé (wide string).
std::wstring g_originalProcessNameW;

/// Nom masqué actuel.
std::wstring g_maskedProcessNameW;

/// Indique si le masquage est actif.
std::atomic<bool> g_maskActive{false};

/// Récupère l'adresse du PEB du processus cible.
uint64_t getProcessPebAddress(HANDLE hProcess) {
    HMODULE hNtdll = GetModuleHandleW(L"ntdll.dll");
    if (!hNtdll) return 0;

    auto pNtQueryInformationProcess = reinterpret_cast<NtQueryInformationProcess_t>(
        GetProcAddress(hNtdll, "NtQueryInformationProcess"));
    if (!pNtQueryInformationProcess) return 0;

    PROCESS_BASIC_INFORMATION pbi{};
    ULONG returnLength = 0;
    NTSTATUS status = pNtQueryInformationProcess(
        hProcess,
        ProcessBasicInformation,
        &pbi,
        sizeof(pbi),
        &returnLength);

    if (status != 0) return 0;
    return reinterpret_cast<uint64_t>(pbi.PebBaseAddress);
}

/// Modifie le nom du processus dans le PEB du processus cible.
/// Le PEB contient ProcessParameters->ImagePathName (UNICODE_STRING).
/// On écrit le nouveau nom directement dans la mémoire du processus cible.
bool modifyProcessNameInPeb(HANDLE hProcess, uint64_t pebAddress, const std::wstring& newName) {
    if (!pebAddress) return false;

    // Lire le PEB pour trouver ProcessParameters.
    // PEB offset 0x20 = PRTL_USER_PROCESS_PARAMETERS ProcessParameters (sur x64)
    // ProcessParameters offset 0x60 = UNICODE_STRING ImagePathName (sur x64)
    // UNICODE_STRING: Length (2 bytes), MaximumLength (2 bytes), Buffer (8 bytes)

    // Lire l'adresse de ProcessParameters depuis le PEB.
    SIZE_T bytesRead = 0;
    uint64_t processParametersAddr = 0;
    if (!ReadProcessMemory(hProcess, reinterpret_cast<LPVOID>(pebAddress + 0x20),
                           &processParametersAddr, sizeof(processParametersAddr), &bytesRead) ||
        bytesRead != sizeof(processParametersAddr)) {
        return false;
    }

    if (!processParametersAddr) return false;

    // Lire l'UNICODE_STRING ImagePathName depuis ProcessParameters.
    // Offset 0x60 sur x64.
    UNICODE_STRING imagePathName{};
    if (!ReadProcessMemory(hProcess, reinterpret_cast<LPVOID>(processParametersAddr + 0x60),
                           &imagePathName, sizeof(imagePathName), &bytesRead) ||
        bytesRead != sizeof(imagePathName)) {
        return false;
    }

    // Vérifier que le buffer est lisible.
    if (!imagePathName.Buffer || imagePathName.Length == 0) return false;

    // Préparer le nouveau nom (en UTF-16).
    USHORT newLength = static_cast<USHORT>(newName.size() * sizeof(wchar_t));
    USHORT newMaxLen = newLength + sizeof(wchar_t); // +1 pour le null terminator

    // Si le nouveau nom est plus court ou égal au buffer existant, on peut écrire directement.
    if (newMaxLen <= imagePathName.MaximumLength) {
        // Écrire le nouveau nom dans le buffer existant.
        SIZE_T bytesWritten = 0;
        if (!WriteProcessMemory(hProcess, imagePathName.Buffer,
                                newName.c_str(), newLength, &bytesWritten) ||
            bytesWritten != newLength) {
            return false;
        }

        // Écrire le null terminator.
        wchar_t nullChar = L'\0';
        WriteProcessMemory(hProcess, reinterpret_cast<LPVOID>(
            reinterpret_cast<uint64_t>(imagePathName.Buffer) + newLength),
            &nullChar, sizeof(nullChar), &bytesWritten);

        // Mettre à jour la longueur dans l'UNICODE_STRING.
        imagePathName.Length = newLength;
        imagePathName.MaximumLength = newMaxLen;
        WriteProcessMemory(hProcess, reinterpret_cast<LPVOID>(processParametersAddr + 0x60),
                           &imagePathName, sizeof(imagePathName), &bytesWritten);

        return true;
    }

    // Si le nouveau nom est plus long, on ne peut pas l'écrire dans le buffer existant.
    // On pourrait allouer un nouveau buffer, mais c'est plus complexe.
    // Pour l'instant, on tronque le nom.
    newLength = imagePathName.MaximumLength - sizeof(wchar_t);
    newMaxLen = imagePathName.MaximumLength;
    std::wstring truncatedName = newName.substr(0, newLength / sizeof(wchar_t));

    SIZE_T bytesWritten = 0;
    if (!WriteProcessMemory(hProcess, imagePathName.Buffer,
                            truncatedName.c_str(), newLength, &bytesWritten) ||
        bytesWritten != newLength) {
        return false;
    }

    // Écrire le null terminator.
    wchar_t nullChar = L'\0';
    WriteProcessMemory(hProcess, reinterpret_cast<LPVOID>(
        reinterpret_cast<uint64_t>(imagePathName.Buffer) + newLength),
        &nullChar, sizeof(nullChar), &bytesWritten);

    // Mettre à jour la longueur dans l'UNICODE_STRING.
    imagePathName.Length = newLength;
    imagePathName.MaximumLength = newMaxLen;
    WriteProcessMemory(hProcess, reinterpret_cast<LPVOID>(processParametersAddr + 0x60),
                       &imagePathName, sizeof(imagePathName), &bytesWritten);

    return true;
}

/// Restaure le nom original dans le PEB.
bool restoreProcessNameInPeb(HANDLE hProcess, uint64_t pebAddress, const std::wstring& originalName) {
    return modifyProcessNameInPeb(hProcess, pebAddress, originalName);
}

} // namespace
#endif

ProcessMaskResult ProcessMask::maskCurrentProcess(const QString& newName) {
    ProcessMaskResult result;

#ifdef Q_OS_WIN
    // Sauvegarder le nom original si pas encore fait.
    if (g_originalProcessName.isEmpty()) {
        wchar_t buffer[MAX_PATH];
        GetModuleFileNameW(nullptr, buffer, MAX_PATH);
        g_originalProcessName = QString::fromWCharArray(buffer);
        g_originalProcessNameW = buffer;
    }

    // Récupérer l'adresse du PEB du processus actuel.
    HANDLE hProcess = GetCurrentProcess();
    g_targetPebAddress = getProcessPebAddress(hProcess);
    if (!g_targetPebAddress) {
        result.error = "Cannot get PEB address";
        return result;
    }

    // Convertir le nouveau nom en wide string.
    g_maskedProcessNameW = newName.toStdWString();

    // Modifier le nom dans le PEB.
    if (!modifyProcessNameInPeb(hProcess, g_targetPebAddress, g_maskedProcessNameW)) {
        result.error = "Failed to modify process name in PEB";
        return result;
    }

    g_maskActive = true;
    KE_LOG_INFO() << "ProcessMask: masked process name from '"
                  << g_originalProcessName.toStdString() << "' to '"
                  << newName.toStdString() << "'";

    result.success = true;
#else
    (void)newName;
    result.error = "Process masking is Windows-only";
#endif

    return result;
}

ProcessMaskResult ProcessMask::restoreOriginalName() {
    ProcessMaskResult result;

#ifdef Q_OS_WIN
    if (g_originalProcessName.isEmpty()) {
        result.error = "No original process name saved";
        return result;
    }

    if (!g_maskActive) {
        result.error = "Process masking is not active";
        return result;
    }

    // Restaurer le nom original dans le PEB.
    HANDLE hProcess = GetCurrentProcess();
    if (!restoreProcessNameInPeb(hProcess, g_targetPebAddress, g_originalProcessNameW)) {
        result.error = "Failed to restore process name in PEB";
        return result;
    }

    g_maskActive = false;
    KE_LOG_INFO() << "ProcessMask: restored original process name '"
                  << g_originalProcessName.toStdString() << "'";

    result.success = true;
#else
    result.error = "Process masking is Windows-only";
#endif

    return result;
}

} // namespace killcore