#pragma once

// État partagé entre KillEngine.exe (killcore/ApiHookSession) et la DLL injectée
// dans le processus cible (api_hook_handler.cpp). Même principe que
// speedhack_ipc.h/page_guard_ipc.h : volontairement libre de toute dépendance
// Qt/killcore, layout POD fixe sans pointeurs.
//
// But : compter les appels à une fonction arbitraire de la cible (interception
// passive d'abord, modification ensuite). Le handler utilise MinHook pour poser
// un inline hook robuste (trampoline géré par la lib, pas de shellcode maison).

#include <cstdint>
#include <cstdio>
#include <cwchar>

namespace killcore {

// Mode d'interception d'une fonction hookée.
enum class ApiHookMode : int32_t {
    Count = 0,      // Appelle l'original et compte (interception passive, zéro impact)
    ForceReturn = 1, // Force une valeur de retour (QA/simulation de pannes)
};

#pragma pack(push, 1)
struct ApiHookIpcState {
    // Config écrite par KillEngine AVANT l'injection, lue une fois par le handler.
    wchar_t moduleName[64];     // ex: L"kernel32.dll"
    wchar_t functionName[128];  // ex: L"CreateFileW"
    int32_t mode;               // ApiHookMode
    int64_t forcedReturnValue;  // Valeur de retour forcée si mode==ForceReturn

    // État vivant écrit par le handler dans la cible.
    volatile long active;           // 1 une fois le hook MinHook posé
    volatile long installError;     // 1 si la pose du hook a échoué
    volatile long resolveError;     // 1 si le symbole n'a pas été résolu in-process
    volatile long unsupportedConv;  // 1 si la convention d'appel n'est pas gérée
    volatile long long callCount;   // Nombre d'appels interceptés

    // Contrôle écrit par KillEngine PENDANT la session.
    volatile long removeRequested;  // 1 pour demander au handler de retirer le hook
};
#pragma pack(pop)

/// Nom du mapping partagé, dérivé du PID cible — connu des deux côtés sans
/// échange préalable (KillEngine crée le mapping avant d'injecter ; la DLL
/// injectée l'ouvre via son propre GetCurrentProcessId() une fois chargée).
inline void buildApiHookMappingName(uint32_t pid, wchar_t* buffer, size_t bufferCount) {
    swprintf_s(buffer, bufferCount, L"Local\\KillEngineApiHook_%u", pid);
}

} // namespace killcore