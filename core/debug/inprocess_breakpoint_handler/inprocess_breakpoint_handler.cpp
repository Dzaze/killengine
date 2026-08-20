// KillEngineInProcessBreakpointHandler.dll — handler minimal injecte dans le
// processus cible pour poser un hardware breakpoint (DR0-DR7) depuis
// l'interieur, sans passer par le canal de debug Win32
// (voir core/debug/inprocess_breakpoint_ipc.h pour la justification).
//
// Volontairement independant de Qt/killcore, meme principe que
// page_guard_handler.cpp : cette DLL est chargee dans un processus tiers (le
// logiciel analyse), pas dans KillEngine.exe.
//
// Principe (version 1, stabilite avant couverture maximale — voir le piege
// documente plus bas et docs/STRATEGY_ROOM.md) :
//   1. DllMain(DLL_PROCESS_ATTACH) lance un thread dedie (jamais de travail
//      lourd dans DllMain — loader lock) qui ouvre le mapping partage cree
//      par KillEngine avant l'injection, lit l'adresse/config a surveiller,
//      installe le VEH, puis arme DR0 sur ELLE-MEME uniquement (pas les
//      autres threads deja existantes de la cible).
//   2. DllMain(DLL_THREAD_ATTACH) arme DR0 sur chaque NOUVELLE thread creee
//      dans le processus par la suite — Windows notifie une DLL chargee de
//      chaque nouvelle thread automatiquement, ce qui couvre les threads
//      creees apres l'installation sans avoir besoin d'un evenement de debug
//      externe (CREATE_THREAD_DEBUG_EVENT, reserve a DebugActiveProcess).
//   3. Le VEH capture EXCEPTION_SINGLE_STEP declenche par DR0 : en mode
//      Capture, rapporte juste le hit (RIP/thread) via l'etat partage, pour
//      de l'instrumentation/Find What Writes ; en mode RewriteValue, ecrit
//      aussi la valeur figee directement a l'adresse surveillee, DANS le
//      processus cible, avant de reprendre l'execution.
//
// Contrairement a page_guard_handler.cpp, aucun re-armement n'est necessaire
// apres un hit : un hardware breakpoint DR0-DR7 n'est pas consomme par une
// violation (a la difference de PAGE_GUARD que Windows leve automatiquement),
// il reste arme tant que DR7 n'est pas modifie.
//
// PIEGE CORRIGE LE 19/08/2026 (voir docs/STRATEGY_ROOM.md) : la toute
// premiere version armait DR0 sur TOUTES les threads deja existantes de la
// cible au moment de l'installation, en les suspendant une par une
// (SuspendThread + GetThreadContext + SetThreadContext + ResumeThread) via
// une enumeration ToolHelp32. Teste sur une cible reelle chargee (process Qt
// multi-thread), ca a fait crasher la cible de facon quasi-immediate et
// silencieuse (fichier de crash vide, echec avant meme qu'un rapporteur de
// crash ait pu ecrire quoi que ce soit) — suspendre une thread au hasard
// pendant qu'elle tient potentiellement un verrou OS critique (heap,
// loader), pendant que la thread d'installation continue elle-meme a faire
// des appels Win32, est un risque reel de corruption sur une cible chargee.
// Contrairement au hardware breakpoint EXTERNE (hardware_breakpoint.cpp),
// qui beneficie du fait qu'un evenement de debug Win32 arrete deja TOUTE la
// cible avant de toucher les registres, rien ici ne garantit un etat sain
// pendant l'arment en masse d'une cible qui continue a tourner. Corrige en
// n'armant plus QUE la thread appelante au demarrage — limitation acceptee
// explicitement : les threads deja existantes au moment de l'injection ne
// sont pas instrumentees tant qu'elles ne sont pas recreees. Une strategie
// plus large et sure pour couvrir progressivement les threads preexistantes
// (sans suspension globale brutale) reste a concevoir plus tard.
//
// Regle de test (demandee explicitement) : ne jamais tester cette DLL
// directement sur KillEngine.exe. Toujours valider sur
// KillEngineTestTarget.exe (plusieurs cycles injection -> breakpoint ->
// desarmement -> reinjection sans crash) avant d'envisager une cible reelle.

#include "../inprocess_breakpoint_ipc.h"

#include <windows.h>
#include <cstdio>

namespace {

killcore::InProcessBreakpointIpcState* g_state = nullptr;
HANDLE g_mapping = nullptr;
void* g_vehHandle = nullptr;

/// Log fichier minimal, sans dependance (voir en-tete de fichier) — ouvre,
/// ecrit, flush et referme a chaque appel plutot que de garder un handle
/// ouvert : si la cible crashe juste apres un appel, la ligne est deja sur
/// le disque. Ecrit dans %TEMP%\killengine_inprocess_bp_<pid>.log,
/// specifiquement pour pouvoir localiser precisement une future defaillance
/// d'installation (demande explicite suite au piege documente plus haut).
void LogStep(const char* step) {
    wchar_t tempPath[MAX_PATH];
    if (GetTempPathW(MAX_PATH, tempPath) == 0) {
        return;
    }
    wchar_t fullPath[MAX_PATH + 64];
    swprintf_s(fullPath, L"%skillengine_inprocess_bp_%u.log", tempPath, GetCurrentProcessId());

    const HANDLE file = CreateFileW(fullPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    SYSTEMTIME st;
    GetLocalTime(&st);
    char buffer[256];
    const int len = sprintf_s(buffer, "[%02d:%02d:%02d.%03d] tid=%lu %s\r\n",
                              st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                              GetCurrentThreadId(), step);
    if (len > 0) {
        DWORD written = 0;
        WriteFile(file, buffer, static_cast<DWORD>(len), &written, nullptr);
        FlushFileBuffers(file);
    }
    CloseHandle(file);
}

/// Bits DR7 pour DR0 uniquement (slot 0) — meme encodage que computeDr7Bits()
/// dans hardware_breakpoint.cpp (L0=bit0, R/W0=bits16-17, LEN0=bits18-19),
/// duplique ici volontairement : cette DLL ne doit dependre de rien d'autre
/// que windows.h (voir en-tete de fichier).
uint32_t ComputeDr7() {
    uint32_t dr7 = 1u; // L0 (bit 0) : active DR0 localement
    dr7 |= (g_state->rwCode & 0x3u) << 16;
    dr7 |= (g_state->sizeCode & 0x3u) << 18;
    return dr7;
}

/// Arme DR0 sur une thread donnee. `mustSuspend` doit etre false pour la
/// thread appelante elle-meme (SuspendThread sur soi-meme bloquerait pour
/// toujours) — GetThreadContext/SetThreadContext fonctionnent sur la thread
/// courante sans suspension, technique standard pour un self-arming depuis
/// DLL_THREAD_ATTACH. Ne suspend plus jamais une AUTRE thread (voir piege
/// documente en tete de fichier) : mustSuspend n'est conserve que pour
/// documenter explicitement pourquoi ce chemin n'est plus emprunte.
bool ArmThread(HANDLE threadHandle, bool mustSuspend) {
    if (mustSuspend) {
        // Volontairement jamais appele avec true depuis le 19/08/2026 (voir
        // piege en tete de fichier) — garde defensive si jamais reintroduit
        // par erreur plus tard, plutot que de re-suspendre en silence.
        LogStep("armThread:suspend-path-disabled");
        return false;
    }

    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    const bool ok = GetThreadContext(threadHandle, &ctx) != FALSE;
    if (ok) {
        ctx.Dr0 = g_state->watchAddress;
        ctx.Dr6 = 0;
        ctx.Dr7 = ComputeDr7();
        SetThreadContext(threadHandle, &ctx);
    }
    return ok;
}

/// Enregistre une thread armee dans la liste bornee de l'etat partage, pour
/// permettre un desarmement deterministe plus tard (voir RecordArmedThread
/// dans inprocess_breakpoint_ipc.h). Le compteur reel peut depasser la
/// capacite du tableau (8) -- au-dela, la thread reste armee mais n'est plus
/// individuellement desarmable a l'arret (limite bornee, documentee).
void RecordArmedThread(DWORD tid) {
    if (!g_state) return;
    const long idx = InterlockedIncrement(&g_state->armedThreadIdCount) - 1;
    if (idx >= 0 && idx < 8) {
        g_state->armedThreadIds[idx] = tid;
    } else {
        LogStep("armedThread:tracking-capacity-exceeded");
    }
}

/// Desarme DR7 sur la thread courante (jamais de suspension sur soi-meme --
/// GetThreadContext/SetThreadContext fonctionnent directement).
void DisarmSelf() {
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    if (GetThreadContext(GetCurrentThread(), &ctx)) {
        ctx.Dr0 = 0;
        ctx.Dr7 = 0;
        SetThreadContext(GetCurrentThread(), &ctx);
    }
}

/// Desarme DR7 sur une AUTRE thread specifique et deja connue (jamais une
/// enumeration "toutes les threads du process" -- c'est precisement le piege
/// du premier crash documente en tete de fichier). Surface de risque bornee
/// au strict sous-ensemble de threads que CE composant a lui-meme armees.
void DisarmOtherThread(DWORD tid) {
    HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, tid);
    if (!h) {
        // Thread deja terminee entre-temps -- rien a desarmer, pas une erreur.
        return;
    }
    const DWORD suspendCount = SuspendThread(h);
    if (suspendCount != static_cast<DWORD>(-1)) {
        CONTEXT ctx{};
        ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        if (GetThreadContext(h, &ctx)) {
            ctx.Dr0 = 0;
            ctx.Dr7 = 0;
            SetThreadContext(h, &ctx);
        }
        ResumeThread(h);
    }
    CloseHandle(h);
}

/// Desarme deterministiquement TOUTES les threads trackees (armedThreadIds),
/// y compris la thread appelante elle-meme si elle en fait partie, puis
/// confirme via state->disarmed. Appele uniquement depuis la boucle de
/// surveillance de InstallThread, jamais depuis le VEH (qui ne desarmait
/// auparavant que "si un nouveau hit survient" -- non deterministe, piege
/// corrige le 20/08/2026).
void DisarmAllTrackedThreadsAndConfirm() {
    const DWORD selfTid = GetCurrentThreadId();
    const long rawCount = g_state->armedThreadIdCount;
    const long count = rawCount < 8 ? rawCount : 8;
    for (long i = 0; i < count; ++i) {
        const DWORD tid = g_state->armedThreadIds[i];
        if (tid == selfTid) {
            DisarmSelf();
        } else if (tid != 0) {
            DisarmOtherThread(tid);
        }
    }
    InterlockedExchange(&g_state->disarmed, 1);
}

LONG WINAPI VectoredHandler(EXCEPTION_POINTERS* ep) {
    if (!g_state || !ep || !ep->ExceptionRecord || !ep->ContextRecord) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    // Confirme que c'est bien DR0 (bit 0 de Dr6) qui a declenche, pas un
    // single-step d'une autre origine (trap flag pose ailleurs, etc.) —
    // Windows ne vide pas Dr6 automatiquement, on le fait nous-memes ensuite.
    if ((ep->ContextRecord->Dr6 & 0x1) == 0) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    ep->ContextRecord->Dr6 = 0;

    if (g_state->stopRequested) {
        // Arret demande entre-temps : desarme cette thread (DR7=0) au lieu
        // de continuer a intercepter, gestion propre de l'arret plutot que
        // de laisser le breakpoint arme indefiniment sur les threads qui
        // continuent a le declencher.
        ep->ContextRecord->Dr7 = 0;
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    if (g_state->mode == 1 && g_state->freezeValueSize > 0 && g_state->freezeValueSize <= 8) {
        // Mode RewriteValue : l'ecriture qui vient de declencher le
        // breakpoint a deja eu lieu (DR0 sur une ecriture trappe apres,
        // comme un watchpoint materiel classique) — on la recouvre
        // immediatement avec la valeur figee, en memoire locale (on est
        // DANS le processus cible), avant que l'execution ne reprenne.
        memcpy(reinterpret_cast<void*>(g_state->watchAddress), g_state->freezeValueBytes, g_state->freezeValueSize);
    }

    g_state->lastHitRip = reinterpret_cast<uint64_t>(ep->ExceptionRecord->ExceptionAddress);
    g_state->lastHitThreadId = GetCurrentThreadId();
    InterlockedIncrement(&g_state->hitCount);

    return EXCEPTION_CONTINUE_EXECUTION;
}

DWORD WINAPI InstallThread(LPVOID) {
    LogStep("install:start");

    wchar_t name[64];
    killcore::buildInProcessBreakpointMappingName(GetCurrentProcessId(), name, 64);

    g_mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name);
    if (!g_mapping) {
        LogStep("install:openFileMapping:failed");
        return 1;
    }
    g_state = static_cast<killcore::InProcessBreakpointIpcState*>(
        MapViewOfFile(g_mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(killcore::InProcessBreakpointIpcState)));
    if (!g_state) {
        LogStep("install:mapViewOfFile:failed");
        CloseHandle(g_mapping);
        g_mapping = nullptr;
        return 1;
    }
    LogStep("install:ipcMapped");

    g_vehHandle = AddVectoredExceptionHandler(1, VectoredHandler);
    if (!g_vehHandle) {
        LogStep("install:addVectoredExceptionHandler:failed");
        InterlockedExchange(&g_state->installError, 1);
        return 1;
    }
    LogStep("install:vehInstalled");

    // N'arme QUE la thread appelante (celle-ci), jamais les autres threads
    // deja existantes de la cible — voir le piege documente en tete de
    // fichier. Les threads creees APRES ce point sont couvertes par
    // DLL_THREAD_ATTACH ci-dessous ; celles qui existaient deja avant
    // l'injection ne le sont pas, limitation acceptee explicitement pour
    // cette version.
    const bool armedSelf = ArmThread(GetCurrentThread(), /*mustSuspend=*/false);
    g_state->armedThreadCount = armedSelf ? 1u : 0u;
    if (armedSelf) {
        RecordArmedThread(GetCurrentThreadId());
    }
    LogStep(armedSelf ? "install:armedSelf:ok" : "install:armedSelf:failed");

    InterlockedExchange(&g_state->active, 1);
    LogStep("install:done");

    // Cette thread ne se termine plus ici (piege corrige le 20/08/2026, voir
    // docs/STRATEGY_ROOM.md) : elle reste en boucle de surveillance jusqu'a
    // stopRequested, puis desarme ELLE-MEME activement toutes les threads
    // trackees avant de confirmer state->disarmed -- au lieu de compter sur
    // un hit futur hypothetique pour le faire paresseusement dans le VEH. Un
    // timeout de capture sans aucune ecriture interceptee doit desarmer DR7
    // de façon aussi sure qu'un arret apres un hit reel.
    while (!g_state->stopRequested) {
        Sleep(20);
    }
    LogStep("watcher:stopRequested-observed");
    DisarmAllTrackedThreadsAndConfirm();
    LogStep("watcher:disarmed-confirmed");
    return 0;
}

} // namespace

BOOL APIENTRY DllMain(HMODULE /*module*/, DWORD reason, LPVOID /*reserved*/) {
    if (reason == DLL_PROCESS_ATTACH) {
        // Jamais de travail lourd (OpenFileMapping, AddVectoredExceptionHandler)
        // directement dans DllMain : le loader lock est tenu ici, un thread
        // dedie evite tout risque de deadlock avec d'autres DLL du processus
        // cible en cours de chargement.
        HANDLE thread = CreateThread(nullptr, 0, InstallThread, nullptr, 0, nullptr);
        if (thread) {
            CloseHandle(thread);
        }
    } else if (reason == DLL_THREAD_ATTACH) {
        // Nouvelle thread creee dans la cible apres l'installation : on
        // l'arme immediatement, sur elle-meme (pas de suspension necessaire,
        // on EST cette thread, synchrone avant qu'elle n'execute du code de
        // la cible). Si l'installation n'est pas encore terminee (g_state pas
        // encore mappe) ou si un arret a ete demande entre-temps, cette
        // thread n'est simplement pas armee.
        if (g_state && g_state->active && !g_state->stopRequested) {
            if (ArmThread(GetCurrentThread(), /*mustSuspend=*/false)) {
                RecordArmedThread(GetCurrentThreadId());
                InterlockedIncrement(&g_state->armedThreadCount);
            }
        }
    } else if (reason == DLL_PROCESS_DETACH) {
        // Gestion propre du retrait, meme si aucun chemin actuel ne
        // provoque un FreeLibrary explicite sur cette DLL (best-effort,
        // meme compromis documente dans page_guard_handler.cpp) : si jamais
        // elle est dechargee, ne pas laisser un VEH pointant vers un module
        // demappe.
        if (g_vehHandle) {
            RemoveVectoredExceptionHandler(g_vehHandle);
            g_vehHandle = nullptr;
        }
        if (g_state) {
            UnmapViewOfFile(g_state);
            g_state = nullptr;
        }
        if (g_mapping) {
            CloseHandle(g_mapping);
            g_mapping = nullptr;
        }
    }
    return TRUE;
}
