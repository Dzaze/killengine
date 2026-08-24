#pragma once

#include <QList>

#include <cstdint>

namespace killcore {

/// Une thread suspendue par ProcessThreadsSuspendGuard, avec son HANDLE ouvert
/// (THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT — assez
/// pour suspendre/reprendre ET lire/écrire son contexte, ex: poser des
/// registres de debug DR0-DR7 pendant que tout est figé).
struct SuspendedThreadHandle {
    uint32_t threadId{0};
    void* handle{nullptr}; // HANDLE Win32, typé void* pour ne pas inclure windows.h ici
};

/// Suspend toutes les threads d'un process (énumération via
/// CreateToolhelp32Snapshot), pour une opération qui doit s'exécuter sans
/// qu'aucune thread de la cible ne puisse lire/écrire pendant la fenêtre —
/// utilisé pour les écritures multi-adresses "atomiques" (roadmap I, session
/// Solitaire du 19/08/2026 : une cible qui compare deux copies redondantes
/// d'une même valeur peut détecter et annuler une écriture isolée si l'autre
/// copie n'est pas mise à jour dans la même fenêtre — deux appels
/// WriteProcessMemory séparés, même rapprochés, laissent une fenêtre ouverte
/// à l'ordonnanceur Windows ; suspendre toutes les threads de la cible la
/// ferme).
///
/// RAII : le destructeur reprend automatiquement toutes les threads
/// suspendues, y compris en cas de sortie anticipée (return/exception) au
/// milieu de l'opération protégée — ne jamais laisser un process cible figé.
class ProcessThreadsSuspendGuard {
public:
    /// extraExcludedThreadId : thread supplémentaire à ne jamais suspendre
    /// (0 = aucune), en plus du thread appelant (toujours exclu). Utile quand
    /// une thread de la cible mérite d'être exclue même si elle diffère du
    /// thread appelant — ex: la thread d'installation d'un composant injecté
    /// dans CE MÊME process cible, déjà armée par ses propres moyens.
    explicit ProcessThreadsSuspendGuard(uint32_t pid, uint32_t extraExcludedThreadId = 0);
    ~ProcessThreadsSuspendGuard();

    ProcessThreadsSuspendGuard(const ProcessThreadsSuspendGuard&) = delete;
    ProcessThreadsSuspendGuard& operator=(const ProcessThreadsSuspendGuard&) = delete;

    /// Nombre de threads effectivement suspendues (peut être inférieur au
    /// nombre total de threads du process si certaines n'ont pas pu être
    /// ouvertes — pas bloquant, la fenêtre reste réduite pour les autres).
    int suspendedCount() const { return m_suspendedCount; }

    /// Threads effectivement suspendues avec leur handle (voir
    /// SuspendedThreadHandle) — utilisé pour manipuler leur contexte (DR0-DR7)
    /// pendant que tout est figé, en plus du simple suspend/resume RAII.
    const QList<SuspendedThreadHandle>& suspendedThreads() const { return m_threads; }

private:
    QList<SuspendedThreadHandle> m_threads;
    int m_suspendedCount{0};
};

} // namespace killcore
