// KillEnginePageGuardHandler.dll — handler minimal injecte dans le processus
// cible pour la surveillance par PAGE_GUARD, sans canal de debug Win32 (voir core/debug/page_guard.h).
//
// Volontairement independant de Qt/killcore : cette DLL est chargee dans un
// processus tiers (le jeu surveille), pas dans KillEngine.exe. Toute
// dependance supplementaire (Qt, nlohmann/json...) serait une charge de
// chargement inutile dans un processus qu'on ne controle pas, et un point de
// defaillance en plus si cette dependance manque ou echoue a se resoudre.
//
// Principe (identique a PageGuardSession::vectoredHandler, la version
// in-process de ce meme mecanisme) :
//   1. DllMain lance un thread (jamais de travail lourd directement dans
//      DllMain — loader lock) qui ouvre le mapping partage cree par
//      KillEngine avant l'injection, lit l'adresse/taille a surveiller, pose
//      PAGE_GUARD sur la page et installe le VEH.
//   2. A chaque violation PAGE_GUARD : capture RIP/adresse/type d'acces dans
//      l'etat partage, pose le trap flag (single-step) pour laisser
//      l'instruction fautive se rejouer normalement une fois la garde levee.
//   3. A l'exception single-step suivante : re-arme PAGE_GUARD (sauf si
//      KillEngine a demande l'arret entretemps) puis reprend l'execution.

#include "../page_guard_ipc.h"

#include <windows.h>

namespace {

killcore::PageGuardIpcState* g_state = nullptr;
HANDLE g_mapping = nullptr;
void* g_vehHandle = nullptr;
uint64_t g_pageBase = 0;
DWORD g_guardProtect = PAGE_READWRITE | PAGE_GUARD;
bool g_rearmPending = false;

constexpr DWORD kStatusGuardPageViolation = 0x80000001;

LONG WINAPI VectoredHandler(EXCEPTION_POINTERS* ep) {
    if (!g_state || !ep || !ep->ExceptionRecord || !ep->ContextRecord) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const auto* record = ep->ExceptionRecord;

    if (record->ExceptionCode == EXCEPTION_SINGLE_STEP && g_rearmPending) {
        g_rearmPending = false;
        if (g_state->stopRequested) {
            // Arret demande : on laisse la garde levee (pas de re-arm), le VEH
            // reste installe mais devient inerte pour cette page — best-effort,
            // pas de RemoveVectoredExceptionHandler ici (le thread d'install
            // n'existe plus, et retirer un VEH depuis un contexte d'exception
            // arbitraire est plus risque que de simplement rester silencieux).
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        DWORD oldProtect = 0;
        VirtualProtect(reinterpret_cast<LPVOID>(g_pageBase), 4096, g_guardProtect, &oldProtect);
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    if (record->ExceptionCode != kStatusGuardPageViolation) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    const uint64_t exceptionAddress = static_cast<uint64_t>(record->ExceptionInformation[1]);
    const bool isWrite = (record->ExceptionInformation[0] == 1);
    const uint64_t rip = reinterpret_cast<uint64_t>(record->ExceptionAddress);

    const bool inRange = exceptionAddress >= g_state->watchAddress &&
                          exceptionAddress < g_state->watchAddress + g_state->watchSize;
    const bool wanted = (isWrite && g_state->captureWrites) || (!isWrite && g_state->captureReads);

    if (inRange && wanted) {
        g_state->lastHitRip = rip;
        g_state->lastHitAccessAddress = exceptionAddress;
        g_state->lastHitIsWrite = isWrite ? 1u : 0u;
        g_state->lastHitThreadId = GetCurrentThreadId();
        InterlockedIncrement(&g_state->hitCount);
    }

    // La garde est one-shot : Windows la leve automatiquement pour laisser
    // l'instruction fautive se rejouer. On pose le trap flag pour recevoir un
    // single-step juste apres cette instruction, et c'est seulement a ce
    // moment-la (cas ci-dessus) qu'on re-arme — sinon l'instruction qui vient
    // de re-declencher la garde re-boucle dessus indefiniment.
    ep->ContextRecord->EFlags |= 0x100;
    g_rearmPending = true;
    return EXCEPTION_CONTINUE_EXECUTION;
}

DWORD WINAPI InstallThread(LPVOID) {
    wchar_t name[64];
    killcore::buildPageGuardMappingName(GetCurrentProcessId(), name, 64);

    constexpr DWORD kMappingAccess = FILE_MAP_READ | FILE_MAP_WRITE;
    g_mapping = OpenFileMappingW(kMappingAccess, FALSE, name);
    if (!g_mapping) {
        return 1;
    }
    g_state = static_cast<killcore::PageGuardIpcState*>(
        MapViewOfFile(g_mapping, kMappingAccess, 0, 0, sizeof(killcore::PageGuardIpcState)));
    if (!g_state) {
        CloseHandle(g_mapping);
        g_mapping = nullptr;
        return 1;
    }

    g_pageBase = g_state->watchAddress & ~static_cast<uint64_t>(0xFFF);

    g_vehHandle = AddVectoredExceptionHandler(1, VectoredHandler);
    if (!g_vehHandle) {
        g_state->installErrorStep = 1;
        g_state->installLastError = GetLastError();
        InterlockedExchange(&g_state->installError, 1);
        return 1;
    }

    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(reinterpret_cast<LPCVOID>(g_pageBase), &mbi, sizeof(mbi))) {
        g_state->installErrorStep = 2;
        g_state->installLastError = GetLastError();
        InterlockedExchange(&g_state->installError, 1);
        RemoveVectoredExceptionHandler(g_vehHandle);
        g_vehHandle = nullptr;
        return 1;
    }

    const DWORD baseProtect = mbi.Protect & ~PAGE_GUARD;
    g_guardProtect = baseProtect | PAGE_GUARD;

    DWORD oldProtect = 0;
    if (!VirtualProtect(reinterpret_cast<LPVOID>(g_pageBase), 4096, g_guardProtect, &oldProtect)) {
        g_state->installErrorStep = 2;
        g_state->installLastError = GetLastError();
        InterlockedExchange(&g_state->installError, 1);
        RemoveVectoredExceptionHandler(g_vehHandle);
        g_vehHandle = nullptr;
        return 1;
    }

    InterlockedExchange(&g_state->active, 1);
    return 0;
}

} // namespace

BOOL APIENTRY DllMain(HMODULE /*module*/, DWORD reason, LPVOID /*reserved*/) {
    if (reason == DLL_PROCESS_ATTACH) {
        // Jamais de travail lourd (OpenFileMapping, AddVectoredExceptionHandler,
        // VirtualProtect) directement dans DllMain : le loader lock est tenu ici,
        // un thread dedie evite tout risque de deadlock avec d'autres DLL du
        // processus cible en cours de chargement.
        HANDLE thread = CreateThread(nullptr, 0, InstallThread, nullptr, 0, nullptr);
        if (thread) {
            CloseHandle(thread);
        }
    }
    return TRUE;
}
