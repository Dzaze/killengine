#pragma once

// État partagé entre KillEngine.exe et la DLL injectée lag_switch_handler.
// Pattern POD fixe sans dépendance Qt/killcore, comme api_hook_ipc.h.

#include <cstdint>
#include <cstdio>
#include <cwchar>

namespace killcore {

#pragma pack(push, 1)
struct LagSwitchIpcState {
    // Config écrite par KillEngine AVANT l'injection.
    int32_t delayMs;  // Délai en ms (0 = pas de délai, hook passif)

    // État vivant écrit par le handler dans la cible.
    volatile long active;       // 1 une fois les hooks posés
    volatile long installError; // 1 si la pose des hooks a échoué
    volatile long long totalCalls; // Nombre total d'appels recv/WSARecv interceptés
    volatile long long totalDelayed; // Nombre d'appels effectivement retardés

    // Contrôle écrit par KillEngine PENDANT la session.
    volatile long removeRequested; // 1 pour demander au handler de retirer les hooks
};
#pragma pack(pop)

/// Nom du mapping partagé, dérivé du PID cible.
inline void buildLagSwitchMappingName(uint32_t pid, wchar_t* buffer, size_t bufferCount) {
    swprintf_s(buffer, bufferCount, L"Local\\KillEngineLagSwitch_%u", pid);
}

} // namespace killcore
