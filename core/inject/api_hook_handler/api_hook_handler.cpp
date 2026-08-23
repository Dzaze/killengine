// KillEngineApiHookHandler.dll — handler injecté dans le processus cible pour
// intercepter des appels de fonctions via MinHook (roadmap section B).
//
// Volontairement indépendant de Qt/killcore (même raisonnement documenté dans
// page_guard_handler.cpp) : cette DLL est chargée dans un processus tiers, pas
// dans KillEngine.exe.
//
// Principe : KillEngine écrit la config (module!fonction, mode) dans le mapping
// partagé AVANT l'injection. Une fois chargée, la DLL :
//   1. Ouvre le mapping partagé (nom dérivé de son propre PID).
//   2. Résout module!fonction in-process (GetModuleHandleW + GetProcAddress —
//      ici on EST dans la cible, pas besoin de lire la table d'export à
//      distance comme export_resolver.cpp).
//   3. Pose un hook MinHook générique qui compte les appels (et éventuellement
//      force la valeur de retour en mode ForceReturn).
//   4. Surveille removeRequested pour retirer proprement le hook.
//
// Convention d'appel : le hook compte les appels SANS connaître la signature
// réelle de la fonction. En Microsoft x64, le nettoyage de pile est TOUJOURS à
// charge de l'appelant : un wrapper 4 arguments peut envelopper toute fonction
// sans en connaître la signature tant qu'il ne modifie pas les arguments. Le
// tail-call vers le trampoline MinHook retransmet les arguments registres à
// l'identique (les arguments pile restent en place sous le return address).
//
// Limitations explicites (v1) :
//   - ForceReturn force RAX sans appeler l'original : adapté aux fonctions
//     retournant un entier/pointeur/bool, PAS aux fonctions retournant des
//     structs par valeur > 8 octets (RAX+RDX, rare).
//   - Une seule fonction hookée par session/mapping (le state IPC est pensé
//     pour une cible à la fois, comme page_guard).
//   - Le handler ne se décharge jamais (même limitation assumée que les trois
//     autres handlers injectés) : removeRequested désactive le hook, la DLL
//     reste chargée jusqu'à la fin du processus cible.

#include "../api_hook_ipc.h"

#include <windows.h>

#include "MinHook.h"

namespace {

killcore::ApiHookIpcState* g_state = nullptr;
HANDLE g_mapping = nullptr;

// Signature générique d'une fonction x64 : en Microsoft x64, les 4 premiers
// arguments passent dans RCX/RDX/R8/R9, les suivants sur la pile (et y
// restent — c'est l'appelant qui nettoie). Ce wrapper couvre donc toute
// fonction dont on ne connaît pas la signature, sans risque de pile.
using GenericFunction = int64_t(WINAPI*)(int64_t, int64_t, int64_t, int64_t);

GenericFunction g_original = nullptr;

int64_t WINAPI GenericDetour(int64_t a, int64_t b, int64_t c, int64_t d) {
    if (g_state != nullptr) {
        InterlockedIncrement64(&g_state->callCount);
        if (g_state->mode == static_cast<int32_t>(killcore::ApiHookMode::ForceReturn)) {
            return g_state->forcedReturnValue;
        }
    }
    // Tail-call vers le trampoline MinHook : les arguments registres sont
    // retransmis à l'identique, les arguments pile restent en place sous le
    // return address. Le trampoline ré-exécute les instructions originales
    // déplacées puis saute vers la suite de la fonction réelle.
    return g_original(a, b, c, d);
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
    killcore::buildApiHookMappingName(GetCurrentProcessId(), name, 64);

    g_mapping = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, name);
    if (!g_mapping) {
        return 1;
    }
    g_state = static_cast<killcore::ApiHookIpcState*>(
        MapViewOfFile(g_mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(killcore::ApiHookIpcState)));
    if (!g_state) {
        CloseHandle(g_mapping);
        g_mapping = nullptr;
        return 1;
    }

    // Conversion wchar_t -> char (GetProcAddress prend un LPCSTR).
    char functionName[128];
    const int written = WideCharToMultiByte(
        CP_ACP, 0, g_state->functionName, -1, functionName, sizeof(functionName), nullptr, nullptr);
    if (written <= 0) {
        g_state->resolveError = 1;
        CloseMapping();
        return 1;
    }

    // Résolution in-process : GetModuleHandleW échoue si le module demandé
    // n'est pas encore chargé — on réessaie quelques secondes pour couvrir les
    // modules chargés tardivement (même esprit que la boucle de sweep IAT du
    // speedhack_handler).
    void* target = nullptr;
    for (int attempt = 0; attempt < 40 && target == nullptr; ++attempt) {
        HMODULE module = GetModuleHandleW(g_state->moduleName);
        if (module) {
            target = reinterpret_cast<void*>(GetProcAddress(module, functionName));
        }
        if (target == nullptr) {
            Sleep(100);
        }
    }

    if (target == nullptr) {
        g_state->resolveError = 1;
        CloseMapping();
        return 1;
    }

    if (MH_Initialize() != MH_OK) {
        g_state->installError = 1;
        CloseMapping();
        return 1;
    }

    if (MH_CreateHook(target, reinterpret_cast<void*>(&GenericDetour),
                      reinterpret_cast<LPVOID*>(&g_original)) != MH_OK) {
        g_state->installError = 1;
        CloseMapping();
        return 1;
    }

    if (MH_EnableHook(target) != MH_OK) {
        MH_RemoveHook(target);
        g_state->installError = 1;
        CloseMapping();
        return 1;
    }

    InterlockedExchange(&g_state->active, 1);

    // Boucle de surveillance : attend removeRequested, puis retire le hook
    // proprement. Le mapping reste mappé tant que la DLL vit (KillEngine garde
    // sa propre vue dessus ; l'objet disparaît quand les deux côtés ferment).
    while (g_state && !g_state->removeRequested) {
        Sleep(100);
    }

    if (g_state) {
        MH_DisableHook(target);
        MH_RemoveHook(target);
        InterlockedExchange(&g_state->active, 0);
    }
    return 0;
}

} // namespace

BOOL APIENTRY DllMain(HMODULE /*module*/, DWORD reason, LPVOID /*reserved*/) {
    if (reason == DLL_PROCESS_ATTACH) {
        // Jamais de travail lourd dans DllMain (loader lock) — même
        // précaution que page_guard_handler.cpp / speedhack_handler.cpp.
        HANDLE thread = CreateThread(nullptr, 0, InstallThread, nullptr, 0, nullptr);
        if (thread) {
            CloseHandle(thread);
        }
    }
    return TRUE;
}