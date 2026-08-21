#pragma once

// Etat partage entre KillEngine.exe (killcore/SpeedhackSession) et la petite DLL
// injectee dans le processus cible (speedhack_handler.cpp). Meme principe que
// page_guard_ipc.h/inprocess_breakpoint_ipc.h : volontairement libre de toute
// dependance Qt/killcore, layout POD fixe sans pointeurs.
//
// Contrairement aux deux IPC existantes (ou le seul champ reecrit apres
// injection est un booleen one-shot "arrete-toi"), `factor` est un vrai champ
// vivant : KillEngine peut le reecrire a volonte pendant que le mapping reste
// ouvert (curseur de slider en direct), sans jamais avoir besoin de re-injecter.

#include <cstdint>
#include <cstdio>

namespace killcore {

// Bits de hooksInstalledMask, un par fonction hookee (diagnostic uniquement).
constexpr uint32_t kSpeedhackHookQueryPerformanceCounter = 1u << 0;
constexpr uint32_t kSpeedhackHookGetTickCount = 1u << 1;
constexpr uint32_t kSpeedhackHookGetTickCount64 = 1u << 2;
constexpr uint32_t kSpeedhackHookTimeGetTime = 1u << 3;
constexpr uint32_t kSpeedhackHookGetSystemTimeAsFileTime = 1u << 4;
constexpr uint32_t kSpeedhackHookGetSystemTimePreciseAsFileTime = 1u << 5;

#pragma pack(push, 1)
struct SpeedhackIpcState {
    volatile long active{0};          // 1 une fois au moins un hook installe dans la cible
    volatile long installError{0};    // 1 si aucun hook n'a pu etre installe (echec total)
    volatile long stopRequested{0};   // 1 si KillEngine abandonne une installation en cours
    volatile long hooksInstalledMask{0}; // OR des kSpeedhackHook* reussis
    // factor est le seul champ que KillEngine reecrit apres l'injection, en
    // direct (curseur de slider) — voir commentaire en tete de fichier. 1.0 =
    // vitesse normale (etat de repos/"stop"), 0.0 = pause, >1 = accelere,
    // <1 = ralenti. Pas de section critique cote handler : une lecture
    // legerement en retard d'un cran de slider est sans consequence, une
    // ecriture de 8 octets alignee est deja atomique sur x64.
    double factor{1.0};
};
#pragma pack(pop)

/// Nom de mapping partage, derive du PID cible — connu des deux cotes sans
/// echange prealable (KillEngine cree le mapping avant d'injecter ; la DLL
/// injectee l'ouvre via son propre GetCurrentProcessId() une fois chargee).
inline void buildSpeedhackMappingName(uint32_t pid, wchar_t* buffer, size_t bufferCount) {
    swprintf_s(buffer, bufferCount, L"Local\\KillEngineSpeedhack_%u", pid);
}

} // namespace killcore
