#pragma once

#include "process/process_handle.h"
#include "inject/lag_switch_ipc.h"

#include <QString>

#include <cstdint>

namespace killcore {

struct LagSwitchStats {
    bool active{false};
    bool installError{false};
    int32_t delayMs{0};
    long long totalCalls{0};
    long long totalDelayed{0};
};

/**
 * @brief Retarde les fonctions recv/WSARecv du processus cible via injection
 * DLL + MinHook (lag switch).
 *
 * Le handler injecté hook recv et WSARecv de ws2_32.dll. Si delayMs > 0,
 * il appelle Sleep(delayMs) avant de passer à l'original. Si delayMs == 0,
 * il passe directement à l'original (hook passif, juste comptage).
 *
 * Comme ApiHookSession : la DLL reste chargée dans la cible jusqu'à la fin
 * du processus — une réinjection sur le même PID n'est pas supportée.
 */
class LagSwitchSession {
public:
    LagSwitchSession() = default;
    ~LagSwitchSession();

    LagSwitchSession(const LagSwitchSession&) = delete;
    LagSwitchSession& operator=(const LagSwitchSession&) = delete;

    /// Injecte le handler et pose les hooks sur recv/WSARecv.
    bool start(const ProcessHandle& process, int delayMs,
               const QString& handlerPath, QString* error);

    /// Demande au handler de retirer les hooks (le handler reste chargé).
    void stop();

    /// Met à jour le délai en temps réel (0 = passif, >0 = délai en ms).
    void setDelayMs(int delayMs);

    LagSwitchStats stats() const;
    bool isActive() const { return m_active; }

private:
    void* m_mapping{nullptr};
    LagSwitchIpcState* m_state{nullptr};
    uint32_t m_pid{0};
    bool m_active{false};
};

} // namespace killcore
