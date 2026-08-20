#pragma once

// Etat partage entre KillEngine.exe (killcore/InProcessBreakpointSession) et
// la petite DLL injectee dans le processus cible
// (inprocess_breakpoint_handler.cpp). Meme esprit que page_guard_ipc.h : POD
// fixe sans pointeurs, volontairement libre de toute dependance Qt/killcore
// puisque ce header est inclus tel quel par la DLL injectee dans un
// processus tiers (le logiciel analyse), pas dans KillEngine.exe.
//
// Roadmap section F, niveau 2 — breakpoint materiel pose depuis un composant
// charge DANS le processus cible (SetThreadContext sur ses propres threads),
// plutot que via core/debug/hardware_breakpoint.cpp qui attache un debugger
// externe (DebugActiveProcess/WaitForDebugEvent). Justification technique :
// Windows n'autorise qu'un seul proprietaire du port de debug d'un process a
// la fois — si l'utilisateur debug deja sa propre cible avec WinDbg, Visual
// Studio ou x64dbg, DebugActiveProcess echoue. Un composant charge dans le
// process observe les acces memoire sans jamais reclamer ce port, et reste
// disponible pour de l'instrumentation/QA automatisee independamment de
// l'etat du canal de debug Win32. Meme principe d'architecture que
// page_guard_ipc.h/page_guard_handler.cpp, avec des registres de debug DR0-
// DR7 (adresse exacte) plutot que PAGE_GUARD (granularite page de 4 Ko).

#include <cstdint>
#include <cstdio>

namespace killcore {

#pragma pack(push, 1)
struct InProcessBreakpointIpcState {
    volatile long active{0};          // 1 une fois le VEH installe et DR0 arme dans la cible
    volatile long installError{0};    // 1 si SetThreadContext/AddVectoredExceptionHandler a echoue
    volatile long stopRequested{0};   // KillEngine demande l'arret (best-effort)
    volatile long hitCount{0};        // nombre total de hits (monotone, InterlockedIncrement)
    uint64_t watchAddress{0};         // adresse exacte surveillee (DR0)
    // Encodage identique aux bits LEN/R-W de DR7 (voir computeDr7Bits dans
    // hardware_breakpoint.cpp) : sizeCode 0=1o,1=2o,3=4o,2=8o ; rwCode
    // 1=ecriture, 3=lecture/ecriture (l'execution, rwCode 0, n'est pas geree
    // par ce mode observation/freeze memoire, hors de son cas d'usage).
    uint32_t sizeCode{3};
    uint32_t rwCode{1};
    // 0 = Capture (observe seulement, pour Find What Writes / instrumentation) ;
    // 1 = RewriteValue (freeze : reecrit freezeValueBytes juste apres chaque
    // ecriture interceptee, depuis l'interieur de la cible).
    uint32_t mode{0};
    uint32_t freezeValueSize{0};      // octets valides dans freezeValueBytes (mode RewriteValue)
    uint8_t  freezeValueBytes[8]{};
    uint64_t lastHitRip{0};
    uint32_t lastHitThreadId{0};
    uint32_t armedThreadCount{0};     // threads effectivement armees a l'installation (diagnostic)

    // Desarmement deterministe (piege corrige le 20/08/2026 -- voir
    // docs/STRATEGY_ROOM.md) : la thread d'installation ne se contente plus
    // de retourner apres l'armement, elle reste en boucle a attendre
    // stopRequested puis desarme elle-meme, activement, chacune des threads
    // qu'elle a armees (liste bornee ci-dessous, jamais "toutes les threads
    // du process" -- voir le piege du premier crash documente dans
    // inprocess_breakpoint_handler.cpp). Sans ca, un timeout sans aucun hit
    // laissait DR7 arme indefiniment (le VEH ne le desarme que "au prochain
    // hit", jamais garanti) alors que KillEngine croyait deja la ressource
    // libre.
    uint32_t armedThreadIds[8]{};     // TIDs effectivement armes, bornes a 8
    volatile long armedThreadIdCount{0}; // peut depasser 8 (compteur reel), le tableau lui reste borne
    volatile long disarmed{0};        // 1 une fois TOUTES les threads trackees confirmees desarmees
};
#pragma pack(pop)

/// Nom de mapping partage, derive du PID cible — connu des deux cotes sans
/// echange prealable (meme principe que buildPageGuardMappingName).
inline void buildInProcessBreakpointMappingName(uint32_t pid, wchar_t* buffer, size_t bufferCount) {
    swprintf_s(buffer, bufferCount, L"Local\\KillEngineInProcessBp_%u", pid);
}

} // namespace killcore
