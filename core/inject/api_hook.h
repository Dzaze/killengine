#pragma once

#include "process/process_handle.h"
#include "api_hook_ipc.h"

#include <QString>

#include <cstdint>

namespace killcore {

struct ApiHookStats {
    bool active{false};
    bool installError{false};
    bool resolveError{false};
    bool unsupportedConv{false};
    uint64_t callCount{0};
};

struct ApiHookConfig {
    QString moduleName;      // ex: "kernel32.dll"
    QString functionName;    // ex: "CreateFileW"
    ApiHookMode mode{ApiHookMode::Count};
    int64_t forcedReturnValue{0};
};

/**
 * @brief Intercepte les appels d'une fonction arbitraire de la cible via un
 * composant injecté (KillEngineApiHookHandler.dll) qui pose un hook MinHook
 * in-process (roadmap section B, cas d'usage "interception de fonctions").
 *
 * Différence avec les primitives déjà présentes dans core/inject :
 *   - function_hook.cpp pose un JMP écrit à distance vers une fonction qui DOIT
 *     déjà exister dans la cible (le detour doit être injecté au préalable) ;
 *   - le patch IAT du speedhack ne couvre que les imports statiques par nom ;
 *   - ICI le handler apporte son propre moteur de hook (MinHook, lié
 *     statiquement dans la DLL handler) : couvre aussi les appels résolus par
 *     GetProcAddress et n'exige aucune préparation côté cible.
 *
 * Le mode Count est une interception passive (compteur d'appels, l'original
 * est toujours appelé) : utile pour confirmer qu'une fonction est réellement
 * utilisée par la cible avant de décider d'un patch. Le mode ForceReturn
 * simule une valeur de retour (QA/simulation de pannes) sans appeler
 * l'original — voir les limitations de convention d'appel documentées dans
 * api_hook_handler.cpp.
 *
 * `stop()` demande au handler de retirer le hook (removeRequested via IPC),
 * attend sa confirmation, puis ferme la vue locale du mapping. Comme les
 * autres handlers injectés, la DLL reste chargée dans la cible jusqu'à la fin
 * du processus — une réinjection ultérieure sur le même PID n'est pas
 * supportée (DllMain ne se rejoue pas) : refus propre avec message explicite.
 */
class ApiHookSession {
public:
    ApiHookSession() = default;
    ~ApiHookSession();

    ApiHookSession(const ApiHookSession&) = delete;
    ApiHookSession& operator=(const ApiHookSession&) = delete;

    /// Injecte le handler et pose le hook sur config.moduleName!config.functionName.
    bool start(const ProcessHandle& process, const ApiHookConfig& config,
               const QString& handlerPath, QString* error);

    /// Demande au handler de retirer le hook (le handler reste chargé).
    void stop();

    ApiHookStats stats() const;
    bool isActive() const { return m_active; }

private:
    void* m_mapping{nullptr};
    ApiHookIpcState* m_state{nullptr};
    uint32_t m_pid{0};
    bool m_active{false};
};

} // namespace killcore