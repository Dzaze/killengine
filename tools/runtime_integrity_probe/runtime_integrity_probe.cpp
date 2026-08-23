#include <windows.h>
#include <winternl.h>
#include <MinHook.h>

namespace {

using NtQueryInformationProcessFn = NTSTATUS(NTAPI*)(
    HANDLE process,
    PROCESSINFOCLASS informationClass,
    PVOID information,
    ULONG informationLength,
    PULONG returnLength);

constexpr NTSTATUS kStatusSuccess = static_cast<NTSTATUS>(0x00000000L);
constexpr int kProcessDebugPort = 7;
constexpr int kProcessDebugObjectHandle = 30;
constexpr int kProcessDebugFlags = 31;

NtQueryInformationProcessFn g_originalNtQueryInformationProcess = nullptr;
LONG g_hookInstalled = 0;
LONG g_observedDebugQueries = 0;

const char* DebugClassName(PROCESSINFOCLASS informationClass) {
    switch (static_cast<int>(informationClass)) {
    case kProcessDebugPort:
        return "ProcessDebugPort";
    case kProcessDebugObjectHandle:
        return "ProcessDebugObjectHandle";
    case kProcessDebugFlags:
        return "ProcessDebugFlags";
    default:
        return "Other";
    }
}

bool IsDebugInformationClass(PROCESSINFOCLASS informationClass) {
    const int value = static_cast<int>(informationClass);
    return value == kProcessDebugPort ||
           value == kProcessDebugObjectHandle ||
           value == kProcessDebugFlags;
}

unsigned long long ReadObservedValue(PVOID information, ULONG informationLength) {
    if (!information || informationLength == 0) {
        return 0;
    }

    if (informationLength >= sizeof(unsigned long long)) {
        return *reinterpret_cast<unsigned long long*>(information);
    }
    if (informationLength >= sizeof(unsigned long)) {
        return *reinterpret_cast<unsigned long*>(information);
    }
    if (informationLength >= sizeof(unsigned short)) {
        return *reinterpret_cast<unsigned short*>(information);
    }
    return *reinterpret_cast<unsigned char*>(information);
}

void AppendProbeLog(const char* line) {
    char tempPath[MAX_PATH]{};
    if (GetTempPathA(static_cast<DWORD>(sizeof(tempPath)), tempPath) == 0) {
        return;
    }

    char logPath[MAX_PATH]{};
    wsprintfA(logPath, "%skillengine_runtime_integrity_probe_%lu.log", tempPath, GetCurrentProcessId());

    HANDLE file = CreateFileA(
        logPath,
        FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);

    if (file == INVALID_HANDLE_VALUE) {
        return;
    }

    DWORD written = 0;
    WriteFile(file, line, static_cast<DWORD>(lstrlenA(line)), &written, nullptr);
    WriteFile(file, "\r\n", 2, &written, nullptr);
    CloseHandle(file);
}

void LogObservedDebugQuery(
    PROCESSINFOCLASS informationClass,
    NTSTATUS status,
    PVOID information,
    ULONG informationLength,
    PULONG returnLength,
    bool falsified) {
    const LONG count = InterlockedIncrement(&g_observedDebugQueries);
    const unsigned long long value =
        status == kStatusSuccess ? ReadObservedValue(information, informationLength) : 0;
    const ULONG returnedLength = returnLength ? *returnLength : 0;

    char line[512]{};
    wsprintfA(
        line,
        "event=debug_query_observed pid=%lu count=%ld class=%s(%d) status=0x%08lX value=0x%llX length=%lu returnLength=%lu action=%s",
        GetCurrentProcessId(),
        count,
        DebugClassName(informationClass),
        static_cast<int>(informationClass),
        static_cast<unsigned long>(status),
        value,
        informationLength,
        returnedLength,
        falsified ? "FALSIFIED_SUCCESS" : "PASSTHROUGH");

    OutputDebugStringA(line);
    OutputDebugStringA("\n");
    AppendProbeLog(line);
}

// Le Hook modifié avec la logique de falsification active
NTSTATUS NTAPI ObservedNtQueryInformationProcess(
    HANDLE process,
    PROCESSINFOCLASS informationClass,
    PVOID information,
    ULONG informationLength,
    PULONG returnLength) {
    
    // 1. On laisse d'abord la vraie fonction s'exécuter
    const NTSTATUS status = g_originalNtQueryInformationProcess(
        process,
        informationClass,
        information,
        informationLength,
        returnLength);

    bool falsified = false;

    // 2. Si l'appel a réussi et concerne une classe liée au debug, on applique le correctif
    if (status == kStatusSuccess && IsDebugInformationClass(informationClass) && information) {
        const int infoClassValue = static_cast<int>(informationClass);

        if (infoClassValue == kProcessDebugPort) {
            // Un port à 0 signifie "aucun debugger attaché"
            if (informationLength >= sizeof(DWORD_PTR)) {
                *reinterpret_cast<DWORD_PTR*>(information) = 0;
                falsified = true;
            }
        }
        else if (infoClassValue == kProcessDebugObjectHandle) {
            // Un handle à 0 (NULL) signifie "aucun objet de debug ouvert"
            if (informationLength >= sizeof(HANDLE)) {
                *reinterpret_cast<HANDLE*>(information) = nullptr;
                falsified = true;
            }
        }
        else if (infoClassValue == kProcessDebugFlags) {
            // Le flag NoDebugInherit doit être à 1 (TRUE) pour simuler l'absence de debug
            if (informationLength >= sizeof(DWORD)) {
                *reinterpret_cast<DWORD*>(information) = 1; 
                falsified = true;
            }
        }
    }

    // 3. Log de l'activité (Utile pour vérifier dans le fichier .log si le bypass a fonctionné)
    if (IsDebugInformationClass(informationClass)) {
        LogObservedDebugQuery(informationClass, status, information, informationLength, returnLength, falsified);
    }

    return status;
}

DWORD WINAPI InstallRuntimeIntegrityProbeThread(LPVOID) {
    if (MH_Initialize() != MH_OK) {
        AppendProbeLog("event=probe_install_failed reason=MH_Initialize");
        return 1;
    }

    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) {
        AppendProbeLog("event=probe_install_failed reason=GetModuleHandle_ntdll");
        MH_Uninitialize();
        return 1;
    }

    auto* target = reinterpret_cast<LPVOID>(GetProcAddress(ntdll, "NtQueryInformationProcess"));
    if (!target) {
        AppendProbeLog("event=probe_install_failed reason=GetProcAddress_NtQueryInformationProcess");
        MH_Uninitialize();
        return 1;
    }

    if (MH_CreateHook(
            target,
            reinterpret_cast<LPVOID>(&ObservedNtQueryInformationProcess),
            reinterpret_cast<LPVOID*>(&g_originalNtQueryInformationProcess)) != MH_OK) {
        AppendProbeLog("event=probe_install_failed reason=MH_CreateHook");
        MH_Uninitialize();
        return 1;
    }

    if (MH_EnableHook(target) != MH_OK) {
        AppendProbeLog("event=probe_install_failed reason=MH_EnableHook");
        MH_RemoveHook(target);
        MH_Uninitialize();
        return 1;
    }

    InterlockedExchange(&g_hookInstalled, 1);
    AppendProbeLog("event=probe_installed target=NtQueryInformationProcess mode=active_bypass");
    return 0;
}

void InstallRuntimeIntegrityProbe() {
    HANDLE thread = CreateThread(nullptr, 0, InstallRuntimeIntegrityProbeThread, nullptr, 0, nullptr);
    if (thread) {
        CloseHandle(thread);
    }
}

void RemoveRuntimeIntegrityProbe() {
    if (InterlockedExchange(&g_hookInstalled, 0) == 1) {
        MH_DisableHook(MH_ALL_HOOKS);
        MH_RemoveHook(MH_ALL_HOOKS);
        MH_Uninitialize();
        AppendProbeLog("event=probe_removed target=NtQueryInformationProcess");
    }
}

} // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(module);
        InstallRuntimeIntegrityProbe();
        break;
    case DLL_PROCESS_DETACH:
        RemoveRuntimeIntegrityProbe();
        break;
    default:
        break;
    }

    return TRUE;
}
