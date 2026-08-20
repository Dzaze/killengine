#pragma once

#include "process/process_handle.h"

#include <QObject>
#include <QList>
#include <QString>

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>

namespace killcore {

/// Hit capturé par une page guard.
struct PageGuardHit {
    uint64_t monitoredAddress{0};   ///< Adresse surveillée
    uint64_t instructionPointer{0}; ///< RIP de l'instruction qui a accédé
    uint64_t accessAddress{0};      ///< Adresse exacte accédée
    uint64_t threadId{0};           ///< Thread qui a déclenché
    bool isWrite{false};            ///< True si écriture, false si lecture
    QString module;                 ///< Module contenant l'instruction
    uint64_t moduleOffset{0};
};

/// Configuration d'une page guard.
struct PageGuardConfig {
    uint64_t address{0};            ///< Adresse à surveiller
    size_t size{4};                 ///< Taille en octets
    bool captureWrites{true};       ///< Capturer les écritures
    bool captureReads{false};       ///< Capturer les lectures
    int timeoutMs{5000};            ///< Timeout de capture
    size_t maxHits{100};            ///< Nombre max de hits
    /// Chemin de KillEnginePageGuardHandler.dll, requis pour surveiller un
    /// processus externe (le cas réel — un AddVectoredExceptionHandler posé
    /// dans KillEngine.exe ne reçoit que les exceptions de KillEngine
    /// lui-même). Ignoré si le processus surveillé est KillEngine.exe.
    QString injectedHandlerPath;
};

/// Résultat d'une session de page guard.
struct PageGuardResult {
    bool success{false};
    bool timedOut{false};
    QString error;
    QList<PageGuardHit> hits;
};

/**
 * @brief Surveillance par page guards — alternative aux hardware breakpoints qui ne
 * passe pas par le canal de debug Win32.
 *
 * Contrairement aux hardware breakpoints (qui nécessitent DebugActiveProcess — un canal
 * exclusif, indisponible si un autre outil débogue déjà la cible), les page guards
 * utilisent VirtualProtectEx avec le flag PAGE_GUARD et AddVectoredExceptionHandler.
 * Aucun attachement debugger requis.
 *
 * Principe :
 *   1. VirtualProtectEx(addr, PAGE_READWRITE | PAGE_GUARD)
 *   2. AddVectoredExceptionHandler capture STATUS_GUARD_PAGE_VIOLATION
 *   3. On lit RIP depuis EXCEPTION_POINTERS
 *   4. On restaure la protection (la garde est one-shot)
 *   5. On re-pose la garde si on veut continuer à capturer
 *
 * Avantage clé : ne dépend pas du canal de debug Win32 exclusif, donc utilisable même
 * quand un autre débogueur est déjà attaché à la cible.
 *
 * Inconvénient : moins précis (page entière = 4 Ko) et one-shot (il faut re-poser).
 */
class PageGuardSession : public QObject {
    Q_OBJECT
public:
    explicit PageGuardSession(QObject* parent = nullptr);
    ~PageGuardSession() override;

    /// Démarre la surveillance. Bloquant jusqu'à timeout ou maxHits.
    PageGuardResult monitor(const ProcessHandle& process, const PageGuardConfig& config);

    /// Version asynchrone : lance le monitoring dans un thread.
    void startAsync(const ProcessHandle& process, const PageGuardConfig& config);

    /// Demande l'arrêt du monitoring async.
    void stop();

    /// Le monitoring est-il actif ?
    bool isMonitoring() const { return m_monitoring.load(); }

    /// Récupère les hits capturés.
    QList<PageGuardHit> takeHits();

signals:
    void hitCaptured(const killcore::PageGuardHit& hit);
    void monitoringFinished(size_t totalHits);

private:
    static void* s_currentSession; // Pour le callback VEH statique (limite: 1 session à la fois)
    static LONG WINAPI vectoredHandler(struct _EXCEPTION_POINTERS* ep);

    void handleViolation(uint64_t exceptionAddress, uint64_t instructionPointer, bool isWrite, uint32_t threadId);

    /// Surveillance d'un processus externe via handler injecté + IPC mémoire partagée
    /// (le VEH in-process ci-dessus ne peut pas observer les exceptions d'un autre processus).
    PageGuardResult monitorRemote(const ProcessHandle& process, const PageGuardConfig& config);

    std::atomic_bool m_monitoring{false};
    std::atomic_bool m_stopRequested{false};
    std::thread m_monitorThread;
    QList<PageGuardHit> m_pendingHits;

    uint64_t m_targetAddress{0};
    size_t m_targetSize{4};
    uint64_t m_pageBase{0};
    void* m_processHandle{nullptr};
    uint32_t m_originalProtection{0};
    bool m_captureWrites{true};
    bool m_captureReads{false};
    bool m_rearmPending{false};
    std::mutex m_hitsMutex;
};

} // namespace killcore

Q_DECLARE_METATYPE(killcore::PageGuardHit)
