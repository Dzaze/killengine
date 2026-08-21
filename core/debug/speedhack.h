#pragma once

#include "process/process_handle.h"
#include "speedhack_ipc.h"

#include <QString>

#include <cstdint>

namespace killcore {

struct SpeedhackStats {
    bool active{false};
    bool installError{false};
    uint32_t hooksInstalledMask{0};
    double factor{1.0};
};

/**
 * @brief Accélère/ralentit le temps perçu d'un processus cible (roadmap
 * section J), en hookant QueryPerformanceCounter/GetTickCount/GetTickCount64/
 * timeGetTime depuis un composant injecté (KillEngineSpeedhackHandler.dll,
 * voir speedhack_handler.cpp pour le détail du hook par patch d'IAT).
 *
 * Contrairement aux deux composants injectés existants (Page Guard, breakpoint
 * in-process), ce chantier a un vrai besoin de reconfiguration EN DIRECT : le
 * facteur est piloté par un slider côté UI et doit changer sans réinjecter.
 * `setFactor()` réécrit donc directement le champ `factor` du mapping partagé
 * pendant que la session reste active, seul endroit du repo qui fait ça.
 *
 * `start()` réutilise un mapping déjà actif pour le même PID s'il en existe
 * un (le composant, une fois injecté, reste installé et lit `factor` en
 * continu — inutile de réinjecter juste pour réactiver après un stop()) :
 * contrairement à InProcessBreakpointSession, où une deuxième tentative sur
 * le même PID est refusée (l'état partagé devrait être réinstallé, ce que
 * DllMain ne refait pas), ici il n'y a rien à réinstaller — seul `factor`
 * change de valeur.
 *
 * `stop()` ne tente jamais de dé-injecter (impossible proprement, même
 * limitation documentée dans inprocess_breakpoint.h) : il remet simplement
 * `factor` à 1.0 (passthrough transparent via la valeur réelle sauvegardée
 * par chaque détour) avant de fermer son propre handle de mapping. L'objet
 * mémoire partagée reste vivant tant que la cible tourne (son propre handle,
 * ouvert côté DLL injectée, le garde en vie) — une réactivation ultérieure
 * sur la même cible n'a donc besoin que de rouvrir ce mapping.
 */
class SpeedhackSession {
public:
    SpeedhackSession() = default;
    ~SpeedhackSession();

    SpeedhackSession(const SpeedhackSession&) = delete;
    SpeedhackSession& operator=(const SpeedhackSession&) = delete;

    /// Démarre (ou réactive) le speedhack sur `process`. `injectedHandlerPath`
    /// requis uniquement si aucune session n'existe déjà pour ce PID.
    bool start(const ProcessHandle& process, double factor, const QString& injectedHandlerPath, QString* error);

    /// Change le facteur en direct. Session déjà active requise.
    bool setFactor(double factor);

    /// Remet le facteur à 1.0 (vitesse normale) et referme le mapping côté
    /// KillEngine — ne dé-injecte jamais (voir commentaire de classe).
    void stop();

    SpeedhackStats stats() const;
    bool isActive() const { return m_active; }

private:
    void* m_mapping{nullptr};
    SpeedhackIpcState* m_state{nullptr};
    uint32_t m_pid{0};
    bool m_active{false};
};

} // namespace killcore
