// KillEngineLagSwitchHandler.dll — handler injecté dans le processus cible pour
// retarder les appels à recv/WSARecv de ws2_32.dll via MinHook.
//
// Volontairement indépendant de Qt/killcore (même raisonnement documenté dans
// api_hook_handler.cpp) : cette DLL est chargée dans un processus tiers.
//
// Principe : KillEngine écrit le délai (delayMs) dans le mapping partagé AVANT
// l'injection. Une fois chargée, la DLL :
//   1. Ouvre le mapping partagé (nom dérivé de son propre PID).
//   2. Résout recv et WSARecv de ws2_32.dll in-process.
//   3. Pose des hooks MinHook qui appellent Sleep(delayMs) avant l'original.
//   4. Surveille removeRequested pour retirer les hooks.

#include "../lag_switch_ipc.h"

#include <windows.h>
#include <winsock2.h>

#include "MinHook.h"

namespace {

killcore::LagSwitchIpcState* g_state = nullptr;
HANDLE g_mapping = nullptr;

// Pointeurs vers les fonctions originales
typedef int (WINAPI* RecvFn)(SOCKET s, char* buf, int len, int flags);
typedef int (WINAPI* WSARecvFn)(SOCKET s, LPWSABUF lpBuffers, DWORD dwBufferCount,
                                 LPDWORD lpNumberOfBytesRecvd, LPDWORD lpFlags,
                                 LPWSAOVERLAPPED lpOverlapped,
                                 LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine);

RecvFn g_originalRecv = nullptr;
WSARecvFn g_originalWSARecv = nullptr;

int WINAPI RecvDetour(SOCKET s, char* buf, int len, int flags) {
    if (g_state && g_state->delayMs > 0) {
        Sleep(static_cast<DWORD>(g_state->delayMs));
        InterlockedIncrement64(&g_state->totalDelayed);
    }
    if (g_state) {
        InterlockedIncrement64(&g_state->totalCalls);
    }
    return g_originalRecv(s, buf, len, flags);
}

int WINAPI WSARecvDetour(SOCKET s, LPWSABUF lpBuffers, DWORD dwBufferCount,
                          LPDWORD lpNumberOfBytesRecvd, LPDWORD lpFlags,
                          LPWSAOVERLAPPED lpOverlapped,
                          LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine) {
    if (g_state && g_state->delayMs > 0) {
        Sleep(static_cast<DWORD>(g_state->delayMs));
        InterlockedIncrement64(&g_state->totalDelayed);
    }
    if (g_state) {
        InterlockedIncrement64(&g_state->totalCalls);
    }
    return g_originalWSARecv(s, lpBuffers, dwBufferCount, lpNumberOfBytesRecvd,
                              lpFlags, lpOverlapped, lpCompletionRoutine);
}

void CloseMapping() {
    if (g_state) {
        UnmapViewOfFile(g_state);
        g_state = nullptr;
    }
    if (g_mapping) {
        CloseHandle(g_mapping);
        g_mapping = nullptr;
    }
}

DWORD WINAPI InstallThread(LPVOID) {
    wchar_t name[64];
    killcore::buildLagSwitchMappingName(GetCurrentProcessId(), name, 64);

    g_mapping = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, name);
    if (!g_mapping) {
        return 1;
    }
    g_state = static_cast<killcore::LagSwitchIpcState*>(
        MapViewOfFile(g_mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(killcore::LagSwitchIpcState)));
    if (!g_state) {
        CloseHandle(g_mapping);
        return 1;
    }

    // Résolution de ws2_32.dll in-process
    HMODULE ws2 = GetModuleHandleW(L"ws2_32.dll");
    if (!ws2) {
        g_state->installError = 1;
        CloseMapping();
        return 1;
    }

    void* recvTarget = reinterpret_cast<void*>(GetProcAddress(ws2, "recv"));
    void* wsaRecvTarget = reinterpret_cast<void*>(GetProcAddress(ws2, "WSARecv"));

    if (!recvTarget || !wsaRecvTarget) {
        g_state->installError = 1;
        CloseMapping();
        return 1;
    }

    if (MH_Initialize() != MH_OK) {
        g_state->installError = 1;
        CloseMapping();
        return 1;
    }

    if (MH_CreateHook(recvTarget, reinterpret_cast<void*>(&RecvDetour),
                      reinterpret_cast<LPVOID*>(&g_originalRecv)) != MH_OK) {
        g_state->installError = 1;
        CloseMapping();
        return 1;
    }

    if (MH_CreateHook(wsaRecvTarget, reinterpret_cast<void*>(&WSARecvDetour),
                      reinterpret_cast<LPVOID*>(&g_originalWSARecv)) != MH_OK) {
        g_state->installError = 1;
        CloseMapping();
        return 1;
    }

    if (MH_EnableHook(recvTarget) != MH_OK || MH_EnableHook(wsaRecvTarget) != MH_OK) {
        MH_RemoveHook(recvTarget);
        MH_RemoveHook(wsaRecvTarget);
        g_state->installError = 1;
        CloseMapping();
        return 1;
    }

    InterlockedExchange(&g_state->active, 1);

    // Boucle de surveillance
    while (InterlockedExchange(&g_state->removeRequested, 0) == 0) {
        // Mettre à jour delayMs depuis le state (peut changer en temps réel)
        Sleep(100);
    }

    MH_RemoveHook(recvTarget);
    MH_RemoveHook(wsaRecvTarget);
    MH_Uninitialize();
    InterlockedExchange(&g_state->active, 0);

    return 0;
}

} // namespace

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        CreateThread(nullptr, 0, InstallThread, nullptr, 0, nullptr);
    }
    return TRUE;
}
