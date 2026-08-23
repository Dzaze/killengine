#include <windows.h>
#include <winternl.h> // Contient les structures de base NT
#include <MinHook.h>

// Définition du type NTSTATUS si non inclus
typedef LONG NTSTATUS;
#define STATUS_SUCCESS ((NTSTATUS)0x00000000L)

// Redéfinition propre de la fonction native
typedef NTSTATUS(NTAPI* NtQueryInformationProcess_t)(
    HANDLE ProcessHandle,
    PROCESSINFOCLASS ProcessInformationClass,
    PVOID ProcessInformation,
    ULONG ProcessInformationLength,
    PULONG ReturnLength
);

NtQueryInformationProcess_t OriginalNtQueryInformationProcess = nullptr;

// Le Hook corrigé
NTSTATUS NTAPI HookedNtQueryInformationProcess(
    HANDLE ProcessHandle,
    PROCESSINFOCLASS ProcessInformationClass,
    PVOID ProcessInformation,
    ULONG ProcessInformationLength,
    PULONG ReturnLength
) {
    // On laisse d'abord la vraie fonction s'exécuter pour remplir les structures légitimement
    NTSTATUS status = OriginalNtQueryInformationProcess(ProcessHandle, ProcessInformationClass, ProcessInformation, ProcessInformationLength, ReturnLength);

    // Si la fonction a réussi et qu'on demande le DebugPort, on falsifie le résultat
    if (status == STATUS_SUCCESS && ProcessInformationClass == ProcessDebugPort) {
        if (ProcessInformation && ProcessInformationLength >= sizeof(DWORD_PTR)) {
            *(DWORD_PTR*)ProcessInformation = 0;
        }
    }

    return status; // On retourne le vrai status (STATUS_SUCCESS)
}

// Gestion des hooks
void InstallHook() {
    if (MH_Initialize() == MH_OK) {
        // Récupération dynamique de la fonction dans ntdll.dll
        HMODULE hNtDll = GetModuleHandleA("ntdll.dll");
        if (hNtDll) {
            LPVOID pTarget = (LPVOID)GetProcAddress(hNtDll, "NtQueryInformationProcess");
            
            if (pTarget) {
                MH_CreateHook(pTarget, (LPVOID)&HookedNtQueryInformationProcess, reinterpret_cast<LPVOID*>(&OriginalNtQueryInformationProcess));
                MH_EnableHook(pTarget);
            }
        }
    }
}

void RemoveHook() {
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
}

// Point d'entrée pour une DLL injectable
BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    switch (ul_reason_for_call) {
        case DLL_PROCESS_ATTACH:
            DisableThreadLibraryCalls(hModule);
            InstallHook();
            break;
        case DLL_PROCESS_DETACH:
            RemoveHook();
            break;
    }
    return TRUE;
}