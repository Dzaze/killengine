#pragma once

#include "process/process_handle.h"
#include "inprocess_breakpoint_ipc.h"
#include "breakpoint_arbiter.h"

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QString>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>

namespace killcore {

/// Hit capturé par une session de breakpoint in-process (mode Capture).
struct InProcessBreakpointHit {
    uint64_t instructionPointer{0}; ///< RIP de l'instruction qui a écrit
    uint64_t threadId{0};
    QString module;
    uint64_t moduleOffset{0};
    uint64_t rax{0};
    uint64_t rcx{0};
    uint64_t rdx{0};
    uint64_t rbp{0};
    uint64_t rsp{0};
    uint64_t r8{0};
    uint64_t r9{0};
    QByteArray xmm0;
};

/// Configuration d'une capture in-process (mode Capture, borné).
struct InProcessBreakpointConfig {
    uint64_t address{0};
    size_t size{4};
    bool captureWrites{true};  ///< true = écritures uniquement ; false = lecture/écriture
    bool captureExecute{false}; ///< true = breakpoint d'execution sur address (ignore captureWrites)
    int timeoutMs{5000};
    size_t maxHits{10};
    /// Chemin de KillEngineInProcessBreakpointHandler.dll — requis, ce mode
    /// ne couvre que la surveillance d'un processus externe (voir .cpp).
    QString injectedHandlerPath;
    /// Arme aussi les threads DEJA EXISTANTES au moment de l'injection (pas
    /// seulement les nouvelles, comportement par défaut historique — voir le
    /// piège documenté dans inprocess_breakpoint_handler.cpp). Fait depuis
    /// KillEngine.exe (process externe), pas depuis la DLL injectée : suspend
    /// TOUT le process cible d'un coup (ProcessThreadsSuspendGuard) avant
    /// d'écrire les registres de debug sur chaque thread, puis reprend tout —
    /// évite la course qui avait fait planter une cible réelle en 19/08/2026
    /// (suspendre une thread a la fois PENDANT que d'autres continuent de
    /// tourner). Opt-in explicite : reste false pour freeze/watch classiques,
    /// activé seulement quand couvrir les threads préexistantes est demandé
    /// explicitement.
    bool armExistingThreads{false};
};

struct InProcessBreakpointResult {
    bool success{false};
    bool timedOut{false};
    QString error;
    QList<InProcessBreakpointHit> hits;
    /// Nombre de threads préexistantes armées en plus de la thread
    /// d'installation, quand config.armExistingThreads était actif (0 sinon,
    /// y compris si l'option était activée mais qu'aucune autre thread
    /// n'existait/n'a pu être ouverte).
    int existingThreadsArmed{0};
};

/// Stats en direct d'un freeze in-process actif (lecture synchrone de la
/// mémoire partagée, pas de minuteur dédié — même philosophie que
/// getBreakpointFreezeStats() pour la variante externe).
struct InProcessBreakpointFreezeStats {
    bool active{false};
    bool installError{false};
    uint64_t hitCount{0};
    uint32_t armedThreadCount{0};
};

/**
 * @brief Hardware breakpoint posé depuis l'intérieur du processus cible, sans
 * passer par le canal de debug Win32.
 *
 * Roadmap section F, niveau 2 : contrairement à HardwareBreakpointSession
 * (core/debug/hardware_breakpoint.*, qui utilise DebugActiveProcess +
 * WaitForDebugEvent — un canal exclusif : un seul débogueur peut posséder le
 * port de debug d'un process à la fois, ce qui échoue si l'utilisateur
 * débogue déjà sa propre cible avec un autre outil), cette classe injecte un
 * petit composant (KillEngineInProcessBreakpointHandler.dll) qui arme
 * lui-même DR0 sur ses propres threads (SetThreadContext depuis l'intérieur)
 * et capture EXCEPTION_SINGLE_STEP via un handler d'exception vectoré (VEH)
 * — même principe d'architecture que PageGuardSession/page_guard_handler.cpp,
 * avec des registres de debug (adresse exacte) plutôt que PAGE_GUARD
 * (granularité page de 4 Ko).
 *
 * Usage prévu : instrumentation/QA automatisée sur un logiciel que
 * l'utilisateur possède ou est autorisé à analyser, notamment quand un autre
 * débogueur est déjà attaché à la cible (le canal Win32 externe est alors
 * indisponible) ou quand une précision au octet près est nécessaire là où
 * Page Guard (page entière) est trop grossier.
 *
 * **Limitations connues et acceptées (version 1, voir docs/STRATEGY_ROOM.md)** :
 * - seule la thread appelante est armée à l'installation ; les threads DÉJÀ
 *   existantes dans la cible au moment de l'injection ne sont PAS instrumentées
 *   tant qu'elles ne sont pas recréées — seules les threads créées APRÈS
 *   l'installation sont couvertes (armées automatiquement via DLL_THREAD_ATTACH
 *   côté composant injecté). Choix délibéré suite à un crash constaté en test
 *   réel : armer toutes les threads existantes nécessite de les suspendre une
 *   par une, risqué sur une cible chargée (suspendre une thread qui tient un
 *   verrou OS critique). Une stratégie plus large et sûre reste à concevoir.
 * - une seule capture/freeze par cible et par lancement de KillEngine : une
 *   fois le composant injecté dans une cible, une deuxième tentative sur la
 *   MÊME cible (même PID) est refusée proprement plutôt que tentée, même
 *   après un stop() propre — LoadLibraryW sur un module déjà chargé ne
 *   relance pas DllMain(DLL_PROCESS_ATTACH), donc rien ne réinstalle l'état
 *   partagé. Redémarrer la cible (nouveau PID) permet une nouvelle capture.
 *   Choix délibéré suite à un crash constaté en test réel en réutilisant
 *   l'état partagé d'une injection précédente sans réinstallation.
 *
 * Deux modes, deux cycles de vie distincts :
 *   - **Capture** (monitor/startAsync) : borné par timeout/maxHits, pour une
 *     capture ponctuelle façon "Find What Writes".
 *   - **RewriteValue** (startFreeze) : non borné, actif jusqu'à stop() — le
 *     composant injecté réécrit lui-même la valeur figée juste après chaque
 *     écriture interceptée, sans jamais passer par le canal de debug Win32
 *     (contrairement à BreakpointFreezeManager qui, lui, en dépend).
 */
class InProcessBreakpointSession : public QObject {
    Q_OBJECT
public:
    explicit InProcessBreakpointSession(QObject* parent = nullptr);
    ~InProcessBreakpointSession() override;

    /// Démarre une capture bornée. Bloquant jusqu'à timeout ou maxHits.
    InProcessBreakpointResult monitor(const ProcessHandle& process, const InProcessBreakpointConfig& config);

    /// Version asynchrone : lance la capture dans un thread.
    void startAsync(const ProcessHandle& process, const InProcessBreakpointConfig& config);

    /// Démarre un freeze in-process (non bloquant, reste actif jusqu'à stop()).
    /// `frozenBytes` doit faire au plus 8 octets (contrainte de l'état IPC).
    bool startFreeze(
        const ProcessHandle& process,
        uint64_t address,
        size_t size,
        bool captureWrites,
        const QByteArray& frozenBytes,
        const QString& injectedHandlerPath,
        QString* error);

    /// Stats en direct du freeze actif (lecture directe de la mémoire
    /// partagée, valide uniquement pendant que isFreezing() est vrai).
    InProcessBreakpointFreezeStats freezeStats() const;

    /// Arrête la capture async en cours OU le freeze actif.
    void stop();

    bool isMonitoring() const { return m_monitoring.load(); }
    bool isFreezing() const { return m_freezing.load(); }

signals:
    void hitCaptured(const killcore::InProcessBreakpointHit& hit);
    void monitoringFinished(size_t totalHits);

private:
    std::atomic_bool m_monitoring{false};
    std::atomic_bool m_freezing{false};
    std::atomic_bool m_stopRequested{false};
    std::thread m_monitorThread;
    QList<InProcessBreakpointHit> m_pendingHits;
    std::mutex m_hitsMutex;

    // Mapping IPC gardé ouvert tant que le freeze est actif (fermé par stop()) —
    // void* pour éviter d'exposer HANDLE/windows.h dans ce header public.
    void* m_freezeMapping{nullptr};
    InProcessBreakpointIpcState* m_freezeState{nullptr};
    uint32_t m_freezePid{0};
    std::unique_ptr<HwBreakpointOwnershipGuard> m_freezeOwnership;
};

} // namespace killcore

Q_DECLARE_METATYPE(killcore::InProcessBreakpointHit)
