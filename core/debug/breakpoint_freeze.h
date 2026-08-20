#pragma once

#include "debug/hardware_breakpoint.h"
#include "debug/breakpoint_arbiter.h"

#include <QObject>
#include <QByteArray>
#include <QList>

#include <atomic>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <thread>

namespace killcore {

/// Mode de freeze par hardware breakpoint.
enum class BreakpointFreezeMode {
    Capture,       ///< Capture simple (comme findWhatWrites)
    RewriteValue,  ///< Réécrit la valeur figée après chaque écriture du jeu
    BlockWrite,    ///< Tente d'annuler l'écriture en modifiant le contexte CPU
};

/// Configuration d'un freeze par breakpoint.
struct BreakpointFreezeConfig {
    uint64_t address{0};
    BreakpointSize size{BreakpointSize::DWord};
    QByteArray frozenValue;                 ///< Valeur à maintenir
    BreakpointFreezeMode mode{BreakpointFreezeMode::RewriteValue};
};

/// Statistiques de freeze.
struct BreakpointFreezeStats {
    uint64_t totalHits{0};
    uint64_t rewrites{0};
    uint64_t blocks{0};
    uint64_t errors{0};
};

/**
 * @brief Freeze invincible par hardware breakpoint.
 *
 * Contrairement au polling classique (FreezeManager), cette classe attache
 * KillEngine comme debugger et intercepte CHAQUE écriture du jeu à l'adresse
 * cible via un hardware breakpoint DR0-DR3.
 *
 * Modes :
 *   - RewriteValue : après chaque écriture du jeu, réécrit immédiatement la
 *     valeur figée via WriteProcessMemory avant de relâcher le thread.
 *     C'est le mode le plus robuste.
 *   - BlockWrite : modifie le contexte CPU (registre destination) pour annuler
 *     l'écriture. Plus rapide mais dépend de l'instruction.
 *   - Capture : ne modifie rien, juste compte les hits (debug).
 *
 * Avantage vs polling :
 *   - Zéro latence (réaction au cycle CPU près)
 *   - Tient même si le jeu réécrit 1000×/seconde
 *   - Le jeu ne voit jamais sa valeur modifiée persister
 */
class BreakpointFreezeManager : public QObject {
    Q_OBJECT
public:
    explicit BreakpointFreezeManager(QObject* parent = nullptr);
    ~BreakpointFreezeManager() override;

    /// Démarre le freeze sur une adresse.
    /// Attach au processus, pose le breakpoint, lance la boucle de monitoring.
    bool start(uint32_t pid, const BreakpointFreezeConfig& config);

    /// Démarre le freeze sur plusieurs adresses simultanément (max 4 = DR0-DR3).
    bool startMulti(uint32_t pid, const QList<BreakpointFreezeConfig>& configs);

    /// Arrête le freeze et détache proprement.
    void stop();

    /// Le freeze est-il actif ?
    bool isActive() const { return m_active.load(); }

    /// Retourne les statistiques courantes.
    BreakpointFreezeStats stats() const;

signals:
    /// Émis à chaque fois qu'une écriture du jeu est interceptée et corrigée.
    void writeIntercepted(uint64_t address, uint64_t gameValue, uint64_t rewrittenValue);

    /// Émis en cas d'erreur (perte de session, crash du jeu, etc.).
    void freezeError(const QString& error);

private:
    void freezeLoop(std::shared_ptr<std::promise<bool>> startSignal);
    void handleDebugEvent(const struct _DEBUG_EVENT& event, unsigned long* continueStatus);
    bool rewriteValue(uint64_t address, const QByteArray& value);
    bool blockWriteByContext(uint32_t threadId, uint64_t address);

    uint32_t m_pid{0};
    QList<BreakpointFreezeConfig> m_configs;
    BreakpointFreezeStats m_stats;
    mutable std::mutex m_statsMutex;
    std::atomic_bool m_active{false};
    std::atomic_bool m_stopRequested{false};
    std::thread m_freezeThread;

#ifdef Q_OS_WIN
    void* m_processHandle{nullptr};
    HardwareBreakpointSession* m_session{nullptr};
#endif
};

} // namespace killcore
