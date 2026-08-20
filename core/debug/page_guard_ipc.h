#pragma once

// Etat partage entre KillEngine.exe (killcore/PageGuardSession) et la petite DLL
// injectee dans le processus cible (page_guard_handler.cpp). Volontairement
// libre de toute dependance Qt/killcore : ce header est inclus tel quel par la
// DLL injectee, qui doit rester minimale (pas de chargeur Qt dans un processus
// tiers, pas de dependance qui pourrait echouer a charger et planter la cible).
//
// Layout POD fixe, sans pointeurs (memoire partagee inter-processus) — les
// deux cotes doivent voir exactement la meme structure.

#include <cstdint>
#include <cstdio>

namespace killcore {

#pragma pack(push, 1)
struct PageGuardIpcState {
    volatile long active{0};          // 1 une fois le VEH installe et la garde armee dans la cible
    volatile long installError{0};    // 1 si VirtualProtect/AddVectoredExceptionHandler a echoue cote cible
    volatile long stopRequested{0};   // KillEngine demande l'arret (best-effort, voir page_guard_handler.cpp)
    volatile long hitCount{0};        // nombre total de hits (monotone, InterlockedIncrement)
    uint64_t watchAddress{0};         // adresse exacte surveillee (pas necessairement alignee page)
    uint64_t watchSize{0};            // taille en octets
    uint32_t captureWrites{1};
    uint32_t captureReads{0};
    uint64_t lastHitRip{0};
    uint64_t lastHitAccessAddress{0};
    uint32_t lastHitIsWrite{0};
    uint32_t lastHitThreadId{0};
};
#pragma pack(pop)

/// Nom de mapping partage, derive du PID cible — connu des deux cotes sans
/// echange prealable (KillEngine cree le mapping avant d'injecter ; la DLL
/// injectee l'ouvre via son propre GetCurrentProcessId() une fois chargee).
inline void buildPageGuardMappingName(uint32_t pid, wchar_t* buffer, size_t bufferCount) {
    swprintf_s(buffer, bufferCount, L"Local\\KillEnginePageGuard_%u", pid);
}

} // namespace killcore
