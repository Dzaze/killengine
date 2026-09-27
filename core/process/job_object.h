#pragma once

#include <QtGlobal>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

#include <cstdint>

namespace killcore {

/**
 * @brief RAII wrapper autour d'un Job Object Windows configuré avec
 * JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE (UX-PRODUIT-15, docs/PHASE_TRACKER.md).
 *
 * Garantit qu'un processus enfant assigné meurt automatiquement si CE
 * process-ci (le propriétaire du JobObject) se termine -- normalement OU
 * après un crash -- sans nécessiter que le parent ait le temps d'exécuter un
 * quelconque code de nettoyage : c'est le noyau Windows qui ferme le handle
 * du job et applique la limite quand le dernier handle référençant ce job
 * disparaît, ce qui inclut la terminaison abrupte du process propriétaire.
 *
 * Un JobObject par niveau de parenté (instance normale -> enfant tutoriel ;
 * enfant tutoriel -> cible démo), jamais un seul partagé entre plusieurs
 * relations parent/enfant indépendantes.
 *
 * Confirmé absent du reste du dépôt avant cette fiche (aucun
 * CreateJobObject/AssignProcessToJobObject existant) -- nouvelle
 * infrastructure, pas un remplacement d'un mécanisme existant.
 */
class JobObject {
public:
    JobObject();
    ~JobObject();

    // Non-copyable, non-movable (RAII simple, une seule instance par usage).
    JobObject(const JobObject&) = delete;
    JobObject& operator=(const JobObject&) = delete;

    /// Le job a-t-il été créé et configuré avec succès ?
    bool isValid() const;

    /// Assigne un processus déjà démarré (par son PID) à ce job. Nécessite
    /// que ce processus n'appartienne à aucun autre job qui l'interdirait
    /// (cas normal pour un enfant tout juste spawné par QProcess).
    bool assignProcess(qint64 pid);

private:
#ifdef Q_OS_WIN
    HANDLE m_handle{nullptr};
#else
    void* m_handle{nullptr};
#endif
};

} // namespace killcore
