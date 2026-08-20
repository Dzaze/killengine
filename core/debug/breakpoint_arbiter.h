#pragma once

// Proprietaire logique unique des registres de debug materiels (DR0-DR7) par
// PID cible. KillEngine a PLUSIEURS mecanismes independants qui posent des
// hardware breakpoints sur un processus cible :
//   - core/debug/hardware_breakpoint.cpp (externe, DebugActiveProcess) —
//     findWhatWrites/findWhatAccesses.
//   - core/debug/breakpoint_freeze.cpp (externe, DebugActiveProcess aussi,
//     mais sa propre boucle independante) — freezeWithBreakpoint.
//   - core/debug/inprocess_breakpoint.cpp (in-process, VEH + SetThreadContext
//     depuis l'interieur de la cible) — startInProcessBreakpointWatchAsync/
//     startInProcessBreakpointFreeze.
//
// Incident du 19-20/08/2026 (voir docs/STRATEGY_ROOM.md) : un breakpoint
// in-process laisse arme (DR0 jamais desarme de façon deterministe, voir le
// piege documente dans inprocess_breakpoint_handler.cpp) puis un appel a
// findWhatWrites externe sur la MEME cible ont ete enchaines sans aucune
// coordination — Solitaire.exe a crashe juste apres. La cause exacte n'a
// jamais ete prouvee par dump (hypothese, pas certitude), mais l'absence de
// coordination entre mecanismes qui touchent tous les memes registres CPU
// par thread est une faiblesse structurelle reelle independamment de la
// preuve : cet arbitre existe pour la fermer, pas pour confirmer l'hypothese.
//
// Regle : un seul mecanisme peut detenir les registres de debug d'un PID a
// la fois. tryAcquire() refuse proprement (pas d'attente, pas de file
// d'attente) si un autre mecanisme est deja Idle->Arming->Active->Disarming
// pour ce PID. L'appelant doit liberer explicitement via release() une fois
// le desarmement reellement confirme (pas suppose).

#include <QHash>
#include <QString>

#include <cstdint>
#include <mutex>

namespace killcore {

enum class HwBreakpointOwner {
    None,
    InProcess,     // core/debug/inprocess_breakpoint.cpp
    ExternalDebug, // core/debug/hardware_breakpoint.cpp ou breakpoint_freeze.cpp
};

enum class HwBreakpointLifecycle {
    Idle,
    Arming,
    Active,
    Disarming,
    Error, // desarmement jamais confirme -- etat "empoisonne" pour ce PID
};

struct HwBreakpointOwnershipSnapshot {
    HwBreakpointOwner owner{HwBreakpointOwner::None};
    HwBreakpointLifecycle state{HwBreakpointLifecycle::Idle};
    uint64_t address{0};
    int drSlot{-1};
    uint32_t ownerTid{0};
    QString mechanismLabel; // texte libre pour les logs ("in-process", "findWhatWrites", ...)
};

class HwBreakpointArbiter {
public:
    static HwBreakpointArbiter& instance();

    HwBreakpointArbiter(const HwBreakpointArbiter&) = delete;
    HwBreakpointArbiter& operator=(const HwBreakpointArbiter&) = delete;

    /// Tente de prendre possession des registres de debug pour ce PID. Refuse
    /// immediatement (jamais d'attente) si le PID est deja dans un etat
    /// autre que Idle -- y compris Error (empoisonne tant que resetForPid()
    /// n'a pas ete appele explicitement, ex: changement de PID/mort cible).
    /// Passe l'etat a Arming en cas de succes. Logge PID/mecanisme/adresse.
    bool tryAcquire(
        uint32_t pid,
        HwBreakpointOwner owner,
        const QString& mechanismLabel,
        uint64_t address,
        int drSlot,
        QString* error);

    /// Transition Arming -> Active : le breakpoint est reellement pose.
    void markActive(uint32_t pid);

    /// Transition -> Disarming : debut du teardown, avant toute tentative de
    /// desarmement reel.
    void markDisarming(uint32_t pid);

    /// Libere la possession. `disarmConfirmed` doit refleter un etat
    /// REELLEMENT verifie (ex: DR7 relu a 0, ou desarmement jamais tente car
    /// on a echoue avant meme d'armer quoi que ce soit) -- jamais suppose.
    /// Si false, le PID passe en Error et tout futur tryAcquire() pour ce
    /// PID est refuse tant que resetForPid() n'est pas appele explicitement.
    void release(uint32_t pid, bool disarmConfirmed);

    /// Retour forcé à Idle, sans passer par une confirmation de desarmement.
    /// Reserve aux cas ou le PID lui-meme a disparu (process mort/redemarre,
    /// changement de cible attachee) -- a ce moment les registres de debug
    /// de l'ancien process n'existent plus, il n'y a plus rien a proteger.
    void resetForPid(uint32_t pid);

    HwBreakpointOwnershipSnapshot snapshot(uint32_t pid) const;
    bool isPoisoned(uint32_t pid) const;

private:
    HwBreakpointArbiter() = default;

    mutable std::mutex m_mutex;
    QHash<uint32_t, HwBreakpointOwnershipSnapshot> m_state;
};

/// RAII : tryAcquire() au constructeur, release() garanti au destructeur
/// meme sur un retour anticipe/une exception -- evite d'oublier de liberer
/// sur l'un des multiples chemins de sortie de monitor()/startFreeze()/etc.
/// Par defaut, considere le desarmement NON confirme (le pire cas, celui qui
/// bloque a tort plutot que de rater une vraie fuite de DR7) tant que
/// confirmDisarmed() n'a pas ete appele explicitement avec le vrai resultat.
class HwBreakpointOwnershipGuard {
public:
    HwBreakpointOwnershipGuard(
        uint32_t pid,
        HwBreakpointOwner owner,
        const QString& mechanismLabel,
        uint64_t address,
        int drSlot);
    ~HwBreakpointOwnershipGuard();

    HwBreakpointOwnershipGuard(const HwBreakpointOwnershipGuard&) = delete;
    HwBreakpointOwnershipGuard& operator=(const HwBreakpointOwnershipGuard&) = delete;

    bool acquired() const { return m_acquired; }
    const QString& error() const { return m_error; }

    /// A appeler une fois le breakpoint reellement pose (avant ca, l'etat
    /// reste Arming).
    void markActive();

    /// A appeler juste avant le teardown, avant toute tentative reelle de
    /// desarmement.
    void markDisarming();

    /// A appeler avec le vrai resultat observe du desarmement, avant que le
    /// destructeur ne s'execute.
    void confirmDisarmed(bool confirmed);

private:
    uint32_t m_pid{0};
    bool m_acquired{false};
    bool m_released{false};
    bool m_disarmConfirmed{true}; // true tant qu'on n'a jamais rien arme reellement
    QString m_error;
};

} // namespace killcore
