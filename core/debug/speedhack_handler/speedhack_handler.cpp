// KillEngineSpeedhackHandler.dll — handler minimal injecte dans le processus
// cible pour accelerer/ralentir le temps qu'il percoit (roadmap section J).
//
// Volontairement independant de Qt/killcore (meme raisonnement documente dans
// page_guard_handler.cpp) : cette DLL est chargee dans un processus tiers, pas
// dans KillEngine.exe.
//
// Principe : patch de l'IAT (Import Address Table), pas hook inline par
// patch de prologue. La cible est un ensemble fixe et connu de fonctions temps
// systeme. Le handler patche par NOM d'import, pas par DLL exacte, pour couvrir
// kernel32.dll, KERNELBASE.dll et les API-set modernes (`api-ms-win-*`) qui
// apparaissent souvent dans les apps UWP/MSIX.
//
// Limite acceptee (voir docs/POWER_UP_ROADMAP.md section J) : ne couvre pas un
// appel resolu dynamiquement via GetProcAddress + pointeur stocke a la main.
//
// 1. DllMain lance un thread (jamais de travail lourd directement dans
//    DllMain — loader lock) qui ouvre le mapping partage cree par KillEngine
//    avant l'injection, enumere les modules deja charges puis repasse
//    periodiquement pour les modules charges apres l'installation.
// 2. Chaque fonction hookee garde son propre etat "horloge virtuelle a delta
//    scale" (killcore::scaleClockDelta, core/debug/speedhack_clock.h) : lit
//    `factor` dans l'etat partage a chaque appel, jamais de saut de valeur
//    meme si KillEngine change le facteur en direct.
//
// Remarque sur l'absence de recursion : cette DLL APPELLE les vraies fonctions
// temps par son propre IAT intact. On exclut explicitement toutes les copies du
// handler speedhack pour ne jamais patcher nos propres imports.

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
ClockState g_fileTimeState;
ClockState g_preciseFileTimeState;

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

void WINAPI DetourGetSystemTimeAsFileTime(LPFILETIME fileTime) {
    FILETIME real{};
    GetSystemTimeAsFileTime(&real);
    if (!fileTime) return;

    ULARGE_INTEGER value{};
    value.LowPart = real.dwLowDateTime;
    value.HighPart = real.dwHighDateTime;
    value.QuadPart = static_cast<ULONGLONG>(ApplyScaling(g_fileTimeState, static_cast<int64_t>(value.QuadPart)));
    fileTime->dwLowDateTime = value.LowPart;
    fileTime->dwHighDateTime = value.HighPart;
}

void WINAPI DetourGetSystemTimePreciseAsFileTime(LPFILETIME fileTime) {
    FILETIME real{};
    GetSystemTimePreciseAsFileTime(&real);
    if (!fileTime) return;

    ULARGE_INTEGER value{};
    value.LowPart = real.dwLowDateTime;
    value.HighPart = real.dwHighDateTime;
    value.QuadPart = static_cast<ULONGLONG>(ApplyScaling(g_preciseFileTimeState, static_cast<int64_t>(value.QuadPart)));
    fileTime->dwLowDateTime = value.LowPart;
    fileTime->dwHighDateTime = value.HighPart;
}

struct HookTarget {
    const char* importName;
    void* detour;
    void* realAddresses[3];
    uint32_t maskBit;
};

bool ImportNameEquals(const char* left, const char* right) {
    if (!left || !right) return false;
    while (*left && *right) {
        char a = *left;
        char b = *right;
        if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
        if (a != b) return false;
        ++left;
        ++right;
    }
    return *left == '\0' && *right == '\0';
}

bool WideCharEqualsInsensitive(wchar_t left, wchar_t right) {
    if (left >= L'A' && left <= L'Z') left = static_cast<wchar_t>(left - L'A' + L'a');
    if (right >= L'A' && right <= L'Z') right = static_cast<wchar_t>(right - L'A' + L'a');
    return left == right;
}

bool WideContainsInsensitive(const wchar_t* text, const wchar_t* needle) {
    if (!text || !needle || !*needle) return false;
    for (const wchar_t* cursor = text; *cursor; ++cursor) {
        const wchar_t* a = cursor;
        const wchar_t* b = needle;
        while (*a && *b && WideCharEqualsInsensitive(*a, *b)) {
            ++a;
            ++b;
        }
        if (!*b) return true;
    }
    return false;
}

bool WideStartsWithInsensitive(const wchar_t* text, const wchar_t* prefix) {
    if (!text || !prefix) return false;
    while (*prefix) {
        if (!*text || !WideCharEqualsInsensitive(*text, *prefix)) return false;
        ++text;
        ++prefix;
    }
    return true;
}

void DirectoryPrefixOf(const wchar_t* path, wchar_t* out, size_t outCount) {
    if (!out || outCount == 0) return;
    out[0] = L'\0';
    if (!path || !*path) return;

    size_t lastSlash = 0;
    size_t i = 0;
    for (; path[i] && i + 1 < outCount; ++i) {
        out[i] = path[i];
        if (path[i] == L'\\' || path[i] == L'/') {
            lastSlash = i;
        }
    }
    const size_t end = lastSlash > 0 ? lastSlash + 1 : i;
    out[end < outCount ? end : outCount - 1] = L'\0';
}

bool IsSensitiveUiOrAdModule(const MODULEENTRY32W& entry) {
    const wchar_t* name = entry.szModule;
    const wchar_t* path = entry.szExePath;
    return WideContainsInsensitive(name, L"webview") ||
           WideContainsInsensitive(path, L"webview") ||
           WideContainsInsensitive(name, L"xaml") ||
           WideContainsInsensitive(path, L"xaml") ||
           WideContainsInsensitive(name, L"dcomp") ||
           WideContainsInsensitive(name, L"directcomposition") ||
           WideContainsInsensitive(name, L"coremessaging") ||
           WideContainsInsensitive(name, L"uiautomation") ||
           WideContainsInsensitive(name, L"textinput") ||
           WideContainsInsensitive(name, L"ads") ||
           WideContainsInsensitive(name, L"advert");
}

bool ShouldPatchModule(const MODULEENTRY32W& entry, HMODULE mainModule, const wchar_t* mainModuleDir) {
    if (wcsstr(entry.szModule, L"KillEngineSpeedhackHandler.dll") != nullptr) {
        return false;
    }
    if (entry.hModule == mainModule) {
        return true;
    }
    if (!entry.szExePath[0] || !mainModuleDir || !*mainModuleDir) {
        return false;
    }
    if (!WideStartsWithInsensitive(entry.szExePath, mainModuleDir)) {
        return false;
    }
    if (IsSensitiveUiOrAdModule(entry)) {
        return false;
    }
    return true;
}

bool TargetMatchesImportedFunction(const HookTarget& target, const char* importName, uintptr_t thunkFunction) {
    if (ImportNameEquals(importName, target.importName)) {
        return true;
    }
    for (void* realAddress : target.realAddresses) {
        if (realAddress && thunkFunction == reinterpret_cast<uintptr_t>(realAddress)) {
            return true;
        }
    }
    return false;
}

void ResolveTargetAddresses(HookTarget* targets, int targetCount) {
    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    HMODULE kernelBase = GetModuleHandleW(L"KERNELBASE.dll");
    HMODULE winmm = GetModuleHandleW(L"winmm.dll");

    for (int t = 0; t < targetCount; ++t) {
        targets[t].realAddresses[0] = kernel32 ? reinterpret_cast<void*>(GetProcAddress(kernel32, targets[t].importName)) : nullptr;
        targets[t].realAddresses[1] = kernelBase ? reinterpret_cast<void*>(GetProcAddress(kernelBase, targets[t].importName)) : nullptr;
        targets[t].realAddresses[2] = winmm ? reinterpret_cast<void*>(GetProcAddress(winmm, targets[t].importName)) : nullptr;
    }
}

// Patche toutes les entrees IAT de `module` dont le NOM d'import correspond a
// une fonction cible. Le nom est plus robuste que le module importeur sur
// Windows moderne (kernel32/kernelbase/api-ms-win-*).
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
        if (descriptor->FirstThunk == 0) continue;
        auto* original = descriptor->OriginalFirstThunk != 0
            ? reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->OriginalFirstThunk)
            : nullptr;
        auto* thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->FirstThunk);
        for (; thunk->u1.Function != 0; ++thunk) {
            const char* importName = nullptr;
            if (original) {
                if (original->u1.AddressOfData == 0) break;
                if (IMAGE_SNAP_BY_ORDINAL(original->u1.Ordinal)) {
                    ++original;
                    continue;
                }
                auto* import = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + original->u1.AddressOfData);
                importName = reinterpret_cast<const char*>(import->Name);
                ++original;
            }

            for (int t = 0; t < targetCount; ++t) {
                if (!TargetMatchesImportedFunction(targets[t], importName, thunk->u1.Function)) continue;
                if (thunk->u1.Function == reinterpret_cast<uintptr_t>(targets[t].detour)) {
                    InterlockedOr(mask, static_cast<LONG>(targets[t].maskBit));
                    break;
                }

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

void PatchModuleImportsGuarded(HMODULE module, HookTarget* targets, int targetCount, volatile long* mask) {
    __try {
        PatchModuleImports(module, targets, targetCount, mask);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // La cible peut charger/decharger des modules pendant notre sweep IAT.
        // Un module partiellement initialise ne doit jamais faire tomber le
        // process cible : on ignore ce module et on retentera au prochain pass.
    }
}

void PatchLoadedModules(HookTarget* targets, int targetCount, HMODULE mainModule, const wchar_t* mainModuleDir) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
    if (snapshot == INVALID_HANDLE_VALUE) return;

    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Module32FirstW(snapshot, &entry)) {
        do {
            if (!ShouldPatchModule(entry, mainModule, mainModuleDir)) {
                continue;
            }
            PatchModuleImportsGuarded(entry.hModule, targets, targetCount, &g_state->hooksInstalledMask);
        } while (Module32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
}

DWORD WINAPI InstallThread(LPVOID) {
    wchar_t name[64];
    killcore::buildSpeedhackMappingName(GetCurrentProcessId(), name, 64);

    g_mapping = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, name);
    if (!g_mapping) {
        return 1;
    }
    g_state = static_cast<killcore::SpeedhackIpcState*>(
        MapViewOfFile(g_mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(killcore::SpeedhackIpcState)));
    if (!g_state) {
        CloseHandle(g_mapping);
        g_mapping = nullptr;
        return 1;
    }

    HookTarget targets[6] = {
        {"QueryPerformanceCounter", reinterpret_cast<void*>(&DetourQueryPerformanceCounter),
         {nullptr, nullptr, nullptr},
         killcore::kSpeedhackHookQueryPerformanceCounter},
        {"GetTickCount", reinterpret_cast<void*>(&DetourGetTickCount),
         {nullptr, nullptr, nullptr},
         killcore::kSpeedhackHookGetTickCount},
        {"GetTickCount64", reinterpret_cast<void*>(&DetourGetTickCount64),
         {nullptr, nullptr, nullptr},
         killcore::kSpeedhackHookGetTickCount64},
        {"timeGetTime", reinterpret_cast<void*>(&DetourTimeGetTime),
         {nullptr, nullptr, nullptr},
         killcore::kSpeedhackHookTimeGetTime},
        {"GetSystemTimeAsFileTime", reinterpret_cast<void*>(&DetourGetSystemTimeAsFileTime),
         {nullptr, nullptr, nullptr},
         killcore::kSpeedhackHookGetSystemTimeAsFileTime},
        {"GetSystemTimePreciseAsFileTime", reinterpret_cast<void*>(&DetourGetSystemTimePreciseAsFileTime),
         {nullptr, nullptr, nullptr},
         killcore::kSpeedhackHookGetSystemTimePreciseAsFileTime},
    };
    ResolveTargetAddresses(targets, 6);

    HMODULE mainModule = GetModuleHandleW(nullptr);
    wchar_t mainModulePath[MAX_PATH]{};
    wchar_t mainModuleDir[MAX_PATH]{};
    GetModuleFileNameW(nullptr, mainModulePath, MAX_PATH);
    DirectoryPrefixOf(mainModulePath, mainModuleDir, MAX_PATH);

    for (int pass = 0; pass < 120; ++pass) {
        if (g_state->stopRequested) {
            g_state->factor = 1.0;
            return 0;
        }
        PatchLoadedModules(targets, 6, mainModule, mainModuleDir);
        if (g_state->hooksInstalledMask != 0) {
            InterlockedExchange(&g_state->active, 1);
        } else if (pass == 20) {
            InterlockedExchange(&g_state->installError, 1);
        }
        Sleep(500);
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
