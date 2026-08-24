#pragma once

#include "process/process_handle.h"
#include "breakpoint_arbiter.h"

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QList>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <thread>

namespace killcore {

class CancellationToken;

/// Type de breakpoint matériel.
enum class BreakpointType : uint32_t {
    Execute    = 0, ///< Break sur exécution d'instruction (DR7 bits 16-17 = 00)
    Write      = 1, ///< Break sur écriture mémoire (DR7 bits 16-17 = 01)
    Access     = 3, ///< Break sur lecture ou écriture (DR7 bits 16-17 = 11)
};

/// Taille de la région surveillée par le breakpoint.
enum class BreakpointSize : uint32_t {
    Byte   = 0, ///< 1 octet
    Word   = 1, ///< 2 octets
    DWord  = 3, ///< 4 octets
    QWord  = 2, ///< 8 octets
};

/// Une capture d'instruction qui a déclenché un breakpoint.
struct BreakpointHit {
    uint64_t address{0};        ///< Adresse surveillée
    uint64_t instructionPointer{0}; ///< RIP de l'instruction qui a écrit
    uint64_t valueBefore{0};    ///< Valeur avant l'écriture (si BreakpointType::Write)
    uint64_t valueAfter{0};     ///< Valeur après l'écriture
    uint64_t threadId{0};       ///< Thread qui a déclenché
    QString module;             ///< Module contenant l'instruction (ex: "game.exe")
    uint64_t moduleOffset{0};   ///< Offset dans le module
    uint64_t rax{0};
    uint64_t rbx{0};
    uint64_t rcx{0};
    uint64_t rdx{0};
    uint64_t rsi{0};
    uint64_t rdi{0};
    uint64_t rbp{0};
    uint64_t rsp{0};
    uint64_t r8{0};
    uint64_t r9{0};
    uint64_t r10{0};
    uint64_t r11{0};
    uint64_t r12{0};
    uint64_t r13{0};
    uint64_t r14{0};
    uint64_t r15{0};
    QByteArray xmm0;
};

/// Configuration d'un breakpoint matériel.
struct BreakpointConfig {
    uint64_t address{0};
    BreakpointType type{BreakpointType::Write};
    BreakpointSize size{BreakpointSize::DWord};
};

/// État d'une session de debug.
enum class DebugSessionState {
    Idle,
    Attaching,
    Active,
    Detaching,
    Error,
};

/**
 * @brief Session de debugging utilisant les hardware breakpoints (debug registers DR0-DR3).
 *
 * C'est LA technologie qui permet de :
 * 1. Trouver l'instruction qui écrit une valeur ("Find What Writes")
 * 2. Figer une valeur en bloquant l'écriture du jeu (freeze sans polling)
 * 3. Remonter à la source gameplay depuis une valeur affichée
 *
 * Principe :
 *   - DebugActiveProcess(pid) attache KillEngine comme debugger
 *   - SetThreadContext pose un breakpoint DR0-DR3 sur l'adresse cible
 *   - WaitForDebugEvent capture EXCEPTION_SINGLE_STEP quand le jeu écrit
 *   - On lit RIP pour identifier l'instruction fautive
 *
 * Limitations x64 :
 *   - 4 breakpoints max (DR0, DR1, DR2, DR3)
 *   - Nécessite SeDebugPrivilege
 *   - Le jeu peut détecter l'attachement debugger (anti-debug)
 */
class HardwareBreakpointSession : public QObject {
    Q_OBJECT

public:
    explicit HardwareBreakpointSession(QObject* parent = nullptr);
    ~HardwareBreakpointSession() override;

    /// Attache au processus cible comme debugger.
    /// Nécessite PROCESS_ALL_ACCESS + SeDebugPrivilege.
    bool attach(uint32_t pid);

    /// Détache proprement et restaure tous les breakpoints.
    void detach();

    /// Pose un breakpoint matériel sur une adresse.
    /// Retourne l'index du breakpoint (0-3) ou -1 si échec/tous occupés.
    int setBreakpoint(const BreakpointConfig& config);

    /// Retire un breakpoint par index.
    bool removeBreakpoint(int index);

    /// Retire tous les breakpoints.
    void clearBreakpoints();

    /// État actuel de la session.
    DebugSessionState state() const { return m_state; }

    /// Nombre de hits capturés jusqu'à présent.
    size_t hitCount() const { return m_hitCount.load(); }

    /// Retourne les hits capturés et vide le buffer.
    QList<BreakpointHit> takeHits();

    /// Lance la boucle de monitoring dans un thread dédié.
    /// Capture les hits jusqu'à stopMonitoring() ou maxHits atteint.
    void startMonitoring(size_t maxHits = 100, int timeoutMs = 5000);

    /// Lance la boucle de monitoring dans le thread courant.
    /// À utiliser quand le même thread a appelé DebugActiveProcess.
    void monitorBlocking(size_t maxHits = 100, int timeoutMs = 5000);

    /// Demande l'arrêt du monitoring.
    void stopMonitoring();

    /// Vérifie si le monitoring est en cours.
    bool isMonitoring() const { return m_monitoring.load(); }

    /// Tente d'activer SeDebugPrivilege sur le processus courant.
    /// Retourne true si le privilège a été accordé.
    static bool enableDebugPrivilege();

signals:
    /// Émis quand un breakpoint est déclenché.
    void breakpointHit(const killcore::BreakpointHit& hit);

    /// Émis quand l'état de la session change.
    void stateChanged(killcore::DebugSessionState state);

    /// Émis quand le monitoring se termine.
    void monitoringFinished(size_t totalHits);

private:
    void setState(DebugSessionState state);
    void monitorLoop(size_t maxHits, int timeoutMs);
    bool applyBreakpointsToThread(uint32_t threadId);
    bool captureHitFromDebugEvent(const struct _EXCEPTION_DEBUG_INFO& exceptionInfo,
                                   uint64_t instructionAddress,
                                   uint32_t threadId,
                                   BreakpointHit* outHit);

    uint32_t m_pid{0};
    DebugSessionState m_state{DebugSessionState::Idle};
    BreakpointConfig m_breakpoints[4];
    bool m_breakpointActive[4]{false, false, false, false};
    std::atomic_size_t m_hitCount{0};
    std::atomic_bool m_monitoring{false};
    std::atomic_bool m_stopRequested{false};
    std::thread m_monitorThread;
    QList<BreakpointHit> m_pendingHits;
    // Proprietaire unique des registres de debug pour m_pid (voir
    // breakpoint_arbiter.h) -- acquis dans attach(), libere dans detach().
    std::unique_ptr<HwBreakpointOwnershipGuard> m_ownership;

#ifdef Q_OS_WIN
    void* m_processHandle{nullptr}; // HANDLE du processus debuggé
#endif
};

/**
 * @brief Utilitaire de haut niveau : trouve l'instruction qui écrit une adresse.
 *
 * Lance une session de debug, pose un breakpoint Write sur l'adresse,
 * attend que le jeu écrive, capture l'instruction, puis détache.
 *
 * @param pid Processus cible
 * @param address Adresse à surveiller
 * @param size Taille de la valeur (4 pour Int32)
 * @param timeoutMs Temps max d'attente (défaut 5 secondes)
 * @param maxHits Nombre max de hits à capturer (défaut 10)
 * @return Liste des hits (instructions qui écrivent à cette adresse)
 */
QList<BreakpointHit> findWhatWrites(
    uint32_t pid,
    uint64_t address,
    BreakpointSize size = BreakpointSize::DWord,
    int timeoutMs = 5000,
    size_t maxHits = 10);

QList<BreakpointHit> findWhatWrites(
    uint32_t pid,
    uint64_t address,
    BreakpointSize size,
    int timeoutMs,
    size_t maxHits,
    const CancellationToken* cancellation);

/**
 * @brief Utilitaire de haut niveau : trouve les instructions qui lisent une adresse.
 *
 * Variante "Find What Accesses" : pose un breakpoint Access (lecture/ecriture).
 * C'est souvent l'instruction qui LIT la valeur qui revele la structure proprietaire
 * (boucle de rendu UI, calcul gameplay), pas celle qui l'ecrit.
 */
QList<BreakpointHit> findWhatAccesses(
    uint32_t pid,
    uint64_t address,
    BreakpointSize size = BreakpointSize::DWord,
    int timeoutMs = 5000,
    size_t maxHits = 10);

QList<BreakpointHit> findWhatAccesses(
    uint32_t pid,
    uint64_t address,
    BreakpointSize size,
    int timeoutMs,
    size_t maxHits,
    const CancellationToken* cancellation);

/**
 * @brief Utilitaire de haut niveau : capture quand une instruction s'exécute.
 *
 * Variante "break on execute" utile quand on connait un RIP mais pas encore
 * la destination memoire runtime (ex: movups [r8], xmm0 dans un moteur UI).
 * Les hits incluent les registres généraux et XMM0 au moment du trap.
 */
QList<BreakpointHit> findWhatExecutes(
    uint32_t pid,
    uint64_t instructionAddress,
    int timeoutMs = 5000,
    size_t maxHits = 10);

QList<BreakpointHit> findWhatExecutes(
    uint32_t pid,
    uint64_t instructionAddress,
    int timeoutMs,
    size_t maxHits,
    const CancellationToken* cancellation);

} // namespace killcore

Q_DECLARE_METATYPE(killcore::BreakpointHit)
Q_DECLARE_METATYPE(killcore::DebugSessionState)
