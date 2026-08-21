// KillEngineSpeedhackHandler.dll — handler minimal injecte dans le processus
// cible pour accelerer/ralentir le temps qu'il percoit (roadmap section J).
//
// Volontairement independant de Qt/killcore (meme raisonnement documente dans
// page_guard_handler.cpp) : cette DLL est chargee dans un processus tiers, pas
// dans KillEngine.exe.
//
// Principe : patch de l'IAT (Import Address Table), pas hook inline par
// patch de prologue. La cible est un ensemble fixe et connu de 4 fonctions
// systeme (QueryPerformanceCounter/GetTickCount/GetTickCount64/timeGetTime) :
// remplacer le pointeur de 8 octets dans la table d'imports de chaque module
// deja charge suffit a intercepter tout appel `call [IAT_slot]` genere par le
// compilateur pour un import statique — pas besoin de decoder la longueur des
// instructions du prologue (donc pas besoin de lier Zydis ici, contrairement
// a instruction_patch_suggester.cpp cote KillEngine).
//
// Limites acceptees (v1, voir docs/POWER_UP_ROADMAP.md section J) : ne couvre
// pas un appel resolu dynamiquement via GetProcAddress + pointeur stocke a la
// main, et ne re-patch pas les modules charges APRES l'installation (nouveau
// LoadLibrary ulterieur dans la cible).
//
// 1. DllMain lance un thread (jamais de travail lourd directement dans
//    DllMain — loader lock) qui ouvre le mapping partage cree par KillEngine
//    avant l'injection, resout les adresses reelles des 4 fonctions (via son
//    PROPRE import kernel32/winmm — voir remarque plus bas sur l'absence de
//    recursion), enumere les modules deja charges et patche leur IAT.
// 2. Chaque fonction hookee garde son propre etat "horloge virtuelle a delta
//    scale" (killcore::scaleClockDelta, core/debug/speedhack_clock.h) : lit
//    `factor` dans l'etat partage a chaque appel, jamais de saut de valeur
//    meme si KillEngine change le facteur en direct.
//
// Remarque sur l'absence de recursion : cette DLL APPELLE les vraies
// QueryPerformanceCounter/GetTickCount/GetTickCount64/timeGetTime par leur nom
// (liees normalement via kernel32.lib/winmm.lib) pour obtenir la valeur reelle
// avant de la scaler. Cela ne boucle jamais sur nos propres detours : l'IAT
// qu'on patche est celle des AUTRES modules de la cible (l'executable
// principal, les DLL du moteur...), pas celle de KillEngineSpeedhackHandler.dll
// lui-meme, dont l'import reste intact et pointe vers la vraie implementation.

#include "../speedhack_ipc.h"
#include "../speedhack_clock.h"

#include <windows.h>
#include <mmsystem.h>
#include <tlhelp32.h>

#pragma comment(lib, "winmm.lib")

namespace {

killcore::SpeedhackIpcState* g_state = nullptr;
HANDLE g_mapping = nullptr;

// Etat "horloge virtuelle" d'une fonction hookee. Un exemplaire par fonction
// (pas par thread appelant : le compteur qu'elles retournent est un etat
// process-wide monotone, comme la fonction reelle qu'elles remplacent).
struct ClockState {
    volatile long lock{0};
    int64_t realBase{0};
    int64_t virtualBase{0};
    volatile long initialized{0};
};

ClockState g_qpcState;
ClockState g_gtcState;
ClockState g_gtc64State;
ClockState g_tgtState;

// Spinlock leger : les appels sont courts (quelques instructions) et
// frequents, une CRITICAL_SECTION serait une charge disproportionnee par
// rapport a la contention reelle attendue.
void SpinLock(volatile long* lock) {
    while (InterlockedCompareExchange(lock, 1, 0) != 0) {
        // busy-wait
    }
}
void SpinUnlock(volatile long* lock) {
    InterlockedExchange(lock, 0);
}

int64_t ApplyScaling(ClockState& state, int64_t real) {
    const double factor = g_state ? g_state->factor : 1.0;
    SpinLock(&state.lock);
    if (!state.initialized) {
        state.realBase = real;
        state.virtualBase = real;
        state.initialized = 1;
    }
    const int64_t virtualNow = killcore::scaleClockDelta(state.realBase, real, state.virtualBase, factor);
    state.realBase = real;
    state.virtualBase = virtualNow;
    SpinUnlock(&state.lock);
    return virtualNow;
}

BOOL WINAPI DetourQueryPerformanceCounter(LARGE_INTEGER* counter) {
    LARGE_INTEGER real;
    const BOOL ok = QueryPerformanceCounter(&real);
    if (counter) {
        counter->QuadPart = ok ? ApplyScaling(g_qpcState, real.QuadPart) : 0;
    }
    return ok;
}

DWORD WINAPI DetourGetTickCount() {
    return static_cast<DWORD>(ApplyScaling(g_gtcState, static_cast<int64_t>(GetTickCount())));
}

ULONGLONG WINAPI DetourGetTickCount64() {
    return static_cast<ULONGLONG>(ApplyScaling(g_gtc64State, static_cast<int64_t>(GetTickCount64())));
}

DWORD WINAPI DetourTimeGetTime() {
    return static_cast<DWORD>(ApplyScaling(g_tgtState, static_cast<int64_t>(timeGetTime())));
}

struct HookTarget {
    const wchar_t* importedFromDll; // nom du module tel qu'il apparait dans le descripteur d'import cible
    FARPROC realAddress;
    void* detour;
    uint32_t maskBit;
};

// Patche toutes les entrees IAT de `module` qui pointent vers une des
// fonctions de `targets` — ne touche jamais le module qui exporte reellement
// la fonction (kernel32.dll/winmm.dll n'importent pas leurs propres exports).
void PatchModuleImports(HMODULE module, HookTarget* targets, int targetCount, volatile long* mask) {
    if (!module) return;

    auto base = reinterpret_cast<uintptr_t>(module);
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return;

    const auto& importDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (importDir.VirtualAddress == 0 || importDir.Size == 0) return;

    auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + importDir.VirtualAddress);
    for (; descriptor->Name != 0; ++descriptor) {
        const char* dllNameA = reinterpret_cast<const char*>(base + descriptor->Name);
        wchar_t dllNameW[64]{};
        int i = 0;
        for (; dllNameA[i] != '\0' && i < 63; ++i) {
            dllNameW[i] = static_cast<wchar_t>(dllNameA[i]);
        }
        dllNameW[i] = L'\0';

        // FirstThunk pointe vers les vraies adresses (une fois le loader
        // passe) ; OriginalFirstThunk vers les noms — on ne modifie que le
        // premier, jamais le second.
        auto* thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->FirstThunk);
        for (; thunk->u1.Function != 0; ++thunk) {
            auto currentTarget = reinterpret_cast<FARPROC>(static_cast<uintptr_t>(thunk->u1.Function));
            for (int t = 0; t < targetCount; ++t) {
                if (currentTarget != targets[t].realAddress) continue;
                if (_wcsicmp(dllNameW, targets[t].importedFromDll) != 0) continue;

                DWORD oldProtect = 0;
                if (!VirtualProtect(&thunk->u1.Function, sizeof(uintptr_t), PAGE_READWRITE, &oldProtect)) {
                    continue;
                }
                thunk->u1.Function = reinterpret_cast<uintptr_t>(targets[t].detour);
                DWORD ignored = 0;
                VirtualProtect(&thunk->u1.Function, sizeof(uintptr_t), oldProtect, &ignored);
                InterlockedOr(mask, static_cast<LONG>(targets[t].maskBit));
                break;
            }
        }
    }
}

DWORD WINAPI InstallThread(LPVOID) {
    wchar_t name[64];
    killcore::buildSpeedhackMappingName(GetCurrentProcessId(), name, 64);

    g_mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name);
    if (!g_mapping) {
        return 1;
    }
    g_state = static_cast<killcore::SpeedhackIpcState*>(
        MapViewOfFile(g_mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(killcore::SpeedhackIpcState)));
    if (!g_state) {
        CloseHandle(g_mapping);
        g_mapping = nullptr;
        return 1;
    }

    HookTarget targets[4] = {
        {L"kernel32.dll", reinterpret_cast<FARPROC>(&QueryPerformanceCounter),
         reinterpret_cast<void*>(&DetourQueryPerformanceCounter), killcore::kSpeedhackHookQueryPerformanceCounter},
        {L"kernel32.dll", reinterpret_cast<FARPROC>(&GetTickCount),
         reinterpret_cast<void*>(&DetourGetTickCount), killcore::kSpeedhackHookGetTickCount},
        {L"kernel32.dll", reinterpret_cast<FARPROC>(&GetTickCount64),
         reinterpret_cast<void*>(&DetourGetTickCount64), killcore::kSpeedhackHookGetTickCount64},
        {L"winmm.dll", reinterpret_cast<FARPROC>(&timeGetTime),
         reinterpret_cast<void*>(&DetourTimeGetTime), killcore::kSpeedhackHookTimeGetTime},
    };

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
    if (snapshot != INVALID_HANDLE_VALUE) {
        MODULEENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        if (Module32FirstW(snapshot, &entry)) {
            do {
                // Ne jamais patcher notre propre DLL (son import reste la
                // vraie implementation, voir commentaire en tete de fichier)
                // ni les modules qui EXPORTENT ces fonctions (ils ne les
                // importent pas d'eux-memes).
                if (_wcsicmp(entry.szModule, L"KillEngineSpeedhackHandler.dll") == 0 ||
                    _wcsicmp(entry.szModule, L"kernel32.dll") == 0 ||
                    _wcsicmp(entry.szModule, L"winmm.dll") == 0) {
                    continue;
                }
                PatchModuleImports(entry.hModule, targets, 4, &g_state->hooksInstalledMask);
            } while (Module32NextW(snapshot, &entry));
        }
        CloseHandle(snapshot);
    }

    if (g_state->hooksInstalledMask != 0) {
        InterlockedExchange(&g_state->active, 1);
    } else {
        InterlockedExchange(&g_state->installError, 1);
    }
    return 0;
}

} // namespace

BOOL APIENTRY DllMain(HMODULE /*module*/, DWORD reason, LPVOID /*reserved*/) {
    if (reason == DLL_PROCESS_ATTACH) {
        // Jamais de travail lourd (OpenFileMapping, enumeration de modules,
        // VirtualProtect) directement dans DllMain : le loader lock est tenu
        // ici, un thread dedie evite tout risque de deadlock avec d'autres
        // DLL du processus cible en cours de chargement — meme precaution que
        // page_guard_handler.cpp/inprocess_breakpoint_handler.cpp.
        HANDLE thread = CreateThread(nullptr, 0, InstallThread, nullptr, 0, nullptr);
        if (thread) {
            CloseHandle(thread);
        }
    }
    return TRUE;
}
