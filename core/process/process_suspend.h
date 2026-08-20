#pragma once

#include <QList>

#include <cstdint>

namespace killcore {

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
    explicit ProcessThreadsSuspendGuard(uint32_t pid);
    ~ProcessThreadsSuspendGuard();

    ProcessThreadsSuspendGuard(const ProcessThreadsSuspendGuard&) = delete;
    ProcessThreadsSuspendGuard& operator=(const ProcessThreadsSuspendGuard&) = delete;

    /// Nombre de threads effectivement suspendues (peut être inférieur au
    /// nombre total de threads du process si certaines n'ont pas pu être
    /// ouvertes — pas bloquant, la fenêtre reste réduite pour les autres).
    int suspendedCount() const { return m_suspendedCount; }

private:
    QList<void*> m_threadHandles; // HANDLE Win32, typé void* pour ne pas inclure windows.h ici
    int m_suspendedCount{0};
};

} // namespace killcore
