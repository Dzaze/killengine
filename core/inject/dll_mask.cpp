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

/// Typedef pour NtQueryInformationProcess (variante 5 args, winternl).
typedef NTSTATUS(NTAPI* NtQueryInformationProcess_t)(
    HANDLE ProcessHandle,
    PROCESSINFOCLASS ProcessInformationClass,
    PVOID ProcessInformation,
    ULONG ProcessInformationLength,
    PULONG ReturnLength);

/// Layout x64 du PEB / PEB_LDR_DATA / LDR_DATA_TABLE_ENTRY (offsets standards,
/// stables depuis Windows XP x64).
constexpr uint64_t kPebLdrOffset = 0x30;      ///< PEB->Ldr (PPEB_LDR_DATA)
constexpr uint64_t kLdrLoadOrderHead = 0x20;  ///< Ldr->InLoadOrderModuleList
constexpr uint64_t kLdrMemoryOrderHead = 0x30; ///< Ldr->InMemoryOrderModuleList
constexpr uint64_t kLdrInitOrderHead = 0x38;   ///< Ldr->InInitializationOrderModuleList
constexpr uint64_t kEntryLoadOrderLinks = 0x00; ///< entry->InLoadOrderLinks
constexpr uint64_t kEntryMemoryOrderLinks = 0x10; ///< entry->InMemoryOrderLinks
constexpr uint64_t kEntryInitOrderLinks = 0x20;   ///< entry->InInitializationOrderLinks
constexpr uint64_t kEntryDllBase = 0x30;          ///< entry->DllBase

/// Un lien de liste sauvegardé pour un module masqué : l'adresse du nœud et
/// des nœuds voisins (prev/next), pour pouvoir ré-lier le module à la restauration.
struct SavedListLink {
    bool found{false};
    uint64_t nodeAddr{0};   ///< Adresse du LIST_ENTRY du module dans cette liste
    uint64_t prevNode{0};   ///< Adresse du LIST_ENTRY précédent (ou de la tête de liste)
    uint64_t nextNode{0};   ///< Adresse du LIST_ENTRY suivant (ou de la tête de liste)
};

/// Liste des DLLs masquées (nom -> liens sauvegardés).
struct MaskedDllEntry {
    QString dllName;
    uint64_t moduleBase{0};
    SavedListLink loadOrder;
    SavedListLink memoryOrder;
    SavedListLink initOrder;
};
std::vector<MaskedDllEntry> g_maskedDlls;

/// Indique si le masquage DLL est actif.
std::atomic<bool> g_dllMaskActive{false};

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

/// Récupère l'adresse du PEB du processus cible.
uint64_t getProcessPebAddress(HANDLE hProcess, QString* outError) {
    auto fail = [&](const QString& reason) {
        if (outError) *outError = reason;
        return 0;
    };

    HMODULE hNtdll = GetModuleHandleW(L"ntdll.dll");
    if (!hNtdll) return fail("Cannot find ntdll.dll");

    auto pNtQuery = reinterpret_cast<NtQueryInformationProcess_t>(
        GetProcAddress(hNtdll, "NtQueryInformationProcess"));
    if (!pNtQuery) return fail("Cannot find NtQueryInformationProcess");

    PROCESS_BASIC_INFORMATION pbi{};
    ULONG returnLength = 0;
    NTSTATUS status = pNtQuery(hProcess, ProcessBasicInformation, &pbi, sizeof(pbi), &returnLength);
    if (status != 0) {
        return fail(QString("NtQueryInformationProcess(ProcessBasicInformation) failed (NTSTATUS: 0x%1)")
                        .arg(static_cast<quint32>(status), 8, 16, QChar('0')));
    }
    if (!pbi.PebBaseAddress) return fail("PEB address is null");
    return reinterpret_cast<uint64_t>(pbi.PebBaseAddress);
}

/// Trouve la base du module dont le nom contient `dllName` (insensible à la
/// casse) dans le processus cible. Retourne 0 si absent.
uint64_t findModuleBaseByName(HANDLE hProcess, const QString& dllName, QString* outError) {
    auto fail = [&](const QString& reason) {
        if (outError) *outError = reason;
        return 0;
    };

    HMODULE hModules[1024];
    DWORD cbNeeded = 0;
    if (!EnumProcessModules(hProcess, hModules, sizeof(hModules), &cbNeeded)) {
        return fail(QString("EnumProcessModules failed (error: %1)").arg(GetLastError()));
    }

    const int moduleCount = cbNeeded / sizeof(HMODULE);
    for (int i = 0; i < moduleCount; i++) {
        wchar_t moduleName[MAX_PATH];
        if (GetModuleFileNameExW(hProcess, hModules[i], moduleName, MAX_PATH)) {
            const QString currentName = QString::fromWCharArray(moduleName);
            if (currentName.contains(dllName, Qt::CaseInsensitive)) {
                return reinterpret_cast<uint64_t>(hModules[i]);
            }
        }
    }
    return fail(QString("DLL '%1' not found among %2 loaded modules").arg(dllName).arg(moduleCount));
}

/// Parcourt une liste chaînée Ldr (tête à `headAddr`) et cherche le nœud dont
/// DllBase == moduleBase. Si trouvé : sauvegarde les adresses prev/next et
/// délie le nœud de la liste (prev->Flink = next, next->Blink = prev).
///
/// `entryLinksOffset` est l'offset du LIST_ENTRY concerné dans
/// LDR_DATA_TABLE_ENTRY (0x00/0x10/0x20) — le nœud parcouru pointe sur ce
/// champ, donc DllBase est à `node + (kEntryDllBase - entryLinksOffset)`.
///
/// Retourne true si le module a été trouvé ET délié, false s'il est absent de
/// cette liste (pas une erreur).
bool unlinkModuleFromList(HANDLE hProcess, uint64_t headAddr, uint64_t entryLinksOffset,
                          uint64_t moduleBase, SavedListLink* outLink, QString* outError) {
    const uint64_t dllBaseOffset = kEntryDllBase - entryLinksOffset;

    uint64_t node = 0;
    if (!readRemote(hProcess, headAddr, &node, sizeof(node))) {
        if (outError) *outError = QString("Failed to read list head at 0x%1").arg(headAddr, 16, QChar('0'));
        return false;
    }

    uint64_t prev = headAddr;
    int guard = 0;
    while (node != headAddr && node != 0) {
        if (++guard > 4096) {
            if (outError) *outError = QString("Module list walk exceeded 4096 nodes (corrupt list?)");
            return false;
        }
        uint64_t dllBase = 0;
        if (!readRemote(hProcess, node + dllBaseOffset, &dllBase, sizeof(dllBase))) {
            if (outError) *outError = QString("Failed to read entry DllBase at 0x%1").arg(node + dllBaseOffset, 16, QChar('0'));
            return false;
        }
        if (dllBase == moduleBase) {
            uint64_t next = 0;
            if (!readRemote(hProcess, node, &next, sizeof(next))) {
                if (outError) *outError = QString("Failed to read entry Flink at 0x%1").arg(node, 16, QChar('0'));
                return false;
            }
            // Délier : prev->Flink = next, next->Blink = prev.
            if (!writeRemote(hProcess, prev, &next, sizeof(next)) ||
                !writeRemote(hProcess, next + 0x8, &prev, sizeof(prev))) {
                if (outError) *outError = QString("Failed to unlink module node at 0x%1").arg(node, 16, QChar('0'));
                return false;
            }
            outLink->found = true;
            outLink->nodeAddr = node;
            outLink->prevNode = prev;
            outLink->nextNode = next;
            return true;
        }
        prev = node;
        if (!readRemote(hProcess, node, &node, sizeof(node))) {
            if (outError) *outError = QString("Failed to read entry Flink at 0x%1").arg(node, 16, QChar('0'));
            return false;
        }
    }
    return false; // module absent de cette liste
}

/// Ré-lier un module délié dans une liste chaînée Ldr.
bool relinkModuleToList(HANDLE hProcess, const SavedListLink& link, QString* outError) {
    if (!link.found) return true; // rien à faire pour cette liste
    // prev->Flink = node, next->Blink = node, node->Flink = next, node->Blink = prev.
    if (!writeRemote(hProcess, link.prevNode, &link.nodeAddr, sizeof(link.nodeAddr)) ||
        !writeRemote(hProcess, link.nextNode + 0x8, &link.nodeAddr, sizeof(link.nodeAddr)) ||
        !writeRemote(hProcess, link.nodeAddr, &link.nextNode, sizeof(link.nextNode)) ||
        !writeRemote(hProcess, link.nodeAddr + 0x8, &link.prevNode, sizeof(link.prevNode))) {
        if (outError) *outError = QString("Failed to relink module node at 0x%1").arg(link.nodeAddr, 16, QChar('0'));
        return false;
    }
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

    QString stepError;
    const uint64_t moduleBase = findModuleBaseByName(hProcess, dllName, &stepError);
    if (!moduleBase) {
        result.error = QString("Failed to mask DLL '%1' in process %2: %3")
                           .arg(dllName).arg(pid).arg(stepError);
        CloseHandle(hProcess);
        return result;
    }

    const uint64_t peb = getProcessPebAddress(hProcess, &stepError);
    if (!peb) {
        result.error = QString("Failed to mask DLL '%1' in process %2: %3")
                           .arg(dllName).arg(pid).arg(stepError);
        CloseHandle(hProcess);
        return result;
    }

    uint64_t ldr = 0;
    if (!readRemote(hProcess, peb + kPebLdrOffset, &ldr, sizeof(ldr)) || !ldr) {
        result.error = QString("Failed to read PEB->Ldr at 0x%1").arg(peb + kPebLdrOffset, 16, QChar('0'));
        CloseHandle(hProcess);
        return result;
    }

    MaskedDllEntry entry;
    entry.dllName = dllName;
    entry.moduleBase = moduleBase;

    QStringList errors;
    int listsPatched = 0;
    if (unlinkModuleFromList(hProcess, ldr + kLdrLoadOrderHead, kEntryLoadOrderLinks, moduleBase, &entry.loadOrder, &stepError)) {
        listsPatched++;
    } else if (!stepError.isEmpty()) {
        errors << stepError;
    }
    if (unlinkModuleFromList(hProcess, ldr + kLdrMemoryOrderHead, kEntryMemoryOrderLinks, moduleBase, &entry.memoryOrder, &stepError)) {
        listsPatched++;
    } else if (!stepError.isEmpty()) {
        errors << stepError;
    }
    if (unlinkModuleFromList(hProcess, ldr + kLdrInitOrderHead, kEntryInitOrderLinks, moduleBase, &entry.initOrder, &stepError)) {
        listsPatched++;
    } else if (!stepError.isEmpty()) {
        errors << stepError;
    }

    if (listsPatched == 0) {
        result.error = QString("Failed to mask DLL '%1' in process %2: module found at 0x%3 but not present in any Ldr list (%4)")
                           .arg(dllName).arg(pid).arg(moduleBase, 16, QChar('0'))
                           .arg(errors.join("; "));
        CloseHandle(hProcess);
        return result;
    }

    g_maskedDlls.push_back(entry);
    g_dllMaskActive = true;
    result.success = true;
    if (!errors.isEmpty()) {
        result.error = errors.join("; "); // warnings partiels (module absent d'une liste sur trois)
    }
    KE_LOG_INFO() << "DllMask: unlinked DLL '" << dllName.toStdString()
                  << "' (base 0x" << std::hex << moduleBase << ") from " << listsPatched
                  << " Ldr list(s) in process " << std::dec << pid;
    CloseHandle(hProcess);
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
    for (size_t i = 0; i < g_maskedDlls.size(); i++) {
        if (g_maskedDlls[i].dllName.compare(dllName, Qt::CaseInsensitive) == 0) {
            foundIndex = static_cast<int>(i);
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

    const MaskedDllEntry& entry = g_maskedDlls[foundIndex];
    QStringList errors;
    QString stepError;
    if (!relinkModuleToList(hProcess, entry.loadOrder, &stepError)) {
        errors << "load order relink failed";
    }
    if (!relinkModuleToList(hProcess, entry.memoryOrder, &stepError)) {
        errors << "memory order relink failed";
    }
    if (!relinkModuleToList(hProcess, entry.initOrder, &stepError)) {
        errors << "init order relink failed";
    }

    if (!errors.isEmpty()) {
        // Laisser l'entrée en place pour une nouvelle tentative de restauration.
        result.error = QString("Failed to restore DLL '%1' in process %2: %3")
                           .arg(dllName).arg(pid).arg(errors.join("; "));
        CloseHandle(hProcess);
        return result;
    }

    g_maskedDlls.erase(g_maskedDlls.begin() + foundIndex);
    if (g_maskedDlls.empty()) g_dllMaskActive = false;
    result.success = true;
    KE_LOG_INFO() << "DllMask: relinked DLL '" << dllName.toStdString()
                  << "' (base 0x" << std::hex << entry.moduleBase << ") in process " << std::dec << pid;
    CloseHandle(hProcess);
#else
    (void)pid;
    (void)dllName;
    result.error = "DLL masking is Windows-only";
#endif

    return result;
}

} // namespace killcore
