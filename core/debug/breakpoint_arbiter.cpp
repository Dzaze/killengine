#include "breakpoint_arbiter.h"

#include "logging/logger.h"

namespace killcore {

namespace {
QString lifecycleToString(HwBreakpointLifecycle state) {
    switch (state) {
        case HwBreakpointLifecycle::Idle: return "Idle";
        case HwBreakpointLifecycle::Arming: return "Arming";
        case HwBreakpointLifecycle::Active: return "Active";
        case HwBreakpointLifecycle::Disarming: return "Disarming";
        case HwBreakpointLifecycle::Error: return "Error";
    }
    return "?";
}

QString ownerToString(HwBreakpointOwner owner) {
    switch (owner) {
        case HwBreakpointOwner::None: return "None";
        case HwBreakpointOwner::InProcess: return "InProcess";
        case HwBreakpointOwner::ExternalDebug: return "ExternalDebug";
    }
    return "?";
}
} // namespace

HwBreakpointArbiter& HwBreakpointArbiter::instance() {
    static HwBreakpointArbiter s_instance;
    return s_instance;
}

bool HwBreakpointArbiter::tryAcquire(
    uint32_t pid,
    HwBreakpointOwner owner,
    const QString& mechanismLabel,
    uint64_t address,
    int drSlot,
    QString* error) {
    std::lock_guard<std::mutex> lock(m_mutex);

    const auto it = m_state.constFind(pid);
    if (it != m_state.constEnd() && it->state != HwBreakpointLifecycle::Idle) {
        const QString msg = QString(
            "Registres de debug deja detenus pour PID %1 par %2 (%3, etat %4) -- "
            "refuse pour eviter un conflit sur DR0-DR7 (voir docs/STRATEGY_ROOM.md, "
            "incident du 19-20/08/2026).")
            .arg(pid)
            .arg(ownerToString(it->owner))
            .arg(it->mechanismLabel)
            .arg(lifecycleToString(it->state));
        if (error) *error = msg;
        KE_LOG_WARN() << "HwBreakpointArbiter: tryAcquire REFUSED pid=" << pid
                      << " requestedBy=" << mechanismLabel.toStdString()
                      << " currentOwner=" << ownerToString(it->owner).toStdString()
                      << " currentState=" << lifecycleToString(it->state).toStdString();
        return false;
    }

    HwBreakpointOwnershipSnapshot snap;
    snap.owner = owner;
    snap.state = HwBreakpointLifecycle::Arming;
    snap.address = address;
    snap.drSlot = drSlot;
    snap.mechanismLabel = mechanismLabel;
    m_state.insert(pid, snap);

    KE_LOG_INFO() << "HwBreakpointArbiter: ACQUIRED pid=" << pid
                  << " owner=" << ownerToString(owner).toStdString()
                  << " mechanism=" << mechanismLabel.toStdString()
                  << " address=0x" << std::hex << address << std::dec
                  << " drSlot=" << drSlot
                  << " state=Idle->Arming";
    return true;
}

void HwBreakpointArbiter::markActive(uint32_t pid) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_state.find(pid);
    if (it == m_state.end()) return;
    KE_LOG_INFO() << "HwBreakpointArbiter: pid=" << pid
                  << " mechanism=" << it->mechanismLabel.toStdString()
                  << " state=" << lifecycleToString(it->state).toStdString() << "->Active";
    it->state = HwBreakpointLifecycle::Active;
}

void HwBreakpointArbiter::markDisarming(uint32_t pid) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_state.find(pid);
    if (it == m_state.end()) return;
    KE_LOG_INFO() << "HwBreakpointArbiter: pid=" << pid
                  << " mechanism=" << it->mechanismLabel.toStdString()
                  << " state=" << lifecycleToString(it->state).toStdString() << "->Disarming";
    it->state = HwBreakpointLifecycle::Disarming;
}

void HwBreakpointArbiter::release(uint32_t pid, bool disarmConfirmed) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_state.find(pid);
    if (it == m_state.end()) return;

    const QString mechanism = it->mechanismLabel;
    const QString prevState = lifecycleToString(it->state);

    if (disarmConfirmed) {
        KE_LOG_INFO() << "HwBreakpointArbiter: RELEASED pid=" << pid
                      << " mechanism=" << mechanism.toStdString()
                      << " state=" << prevState.toStdString() << "->Idle (desarmement confirme)";
        m_state.erase(it);
    } else {
        KE_LOG_ERROR() << "HwBreakpointArbiter: RELEASE WITHOUT CONFIRMED DISARM pid=" << pid
                       << " mechanism=" << mechanism.toStdString()
                       << " state=" << prevState.toStdString() << "->Error -- "
                       << "PID empoisonne, tout futur tryAcquire() sera refuse jusqu'a resetForPid()";
        it->state = HwBreakpointLifecycle::Error;
    }
}

void HwBreakpointArbiter::resetForPid(uint32_t pid) {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto it = m_state.constFind(pid);
    if (it != m_state.constEnd()) {
        KE_LOG_INFO() << "HwBreakpointArbiter: RESET pid=" << pid
                      << " mechanism=" << it->mechanismLabel.toStdString()
                      << " state=" << lifecycleToString(it->state).toStdString() << "->Idle "
                      << "(cible detachee/morte -- registres de debug de l'ancien process n'existent plus)";
    }
    m_state.remove(pid);
}

HwBreakpointOwnershipSnapshot HwBreakpointArbiter::snapshot(uint32_t pid) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_state.value(pid);
}

bool HwBreakpointArbiter::isPoisoned(uint32_t pid) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto it = m_state.constFind(pid);
    return it != m_state.constEnd() && it->state == HwBreakpointLifecycle::Error;
}

// ---------------------------------------------------------------------------

HwBreakpointOwnershipGuard::HwBreakpointOwnershipGuard(
    uint32_t pid,
    HwBreakpointOwner owner,
    const QString& mechanismLabel,
    uint64_t address,
    int drSlot)
    : m_pid(pid) {
    QString err;
    m_acquired = HwBreakpointArbiter::instance().tryAcquire(pid, owner, mechanismLabel, address, drSlot, &err);
    if (!m_acquired) {
        m_error = err;
    }
}

HwBreakpointOwnershipGuard::~HwBreakpointOwnershipGuard() {
    if (m_acquired && !m_released) {
        HwBreakpointArbiter::instance().release(m_pid, m_disarmConfirmed);
    }
}

void HwBreakpointOwnershipGuard::markActive() {
    if (m_acquired) HwBreakpointArbiter::instance().markActive(m_pid);
}

void HwBreakpointOwnershipGuard::markDisarming() {
    if (m_acquired) HwBreakpointArbiter::instance().markDisarming(m_pid);
}

void HwBreakpointOwnershipGuard::confirmDisarmed(bool confirmed) {
    m_disarmConfirmed = confirmed;
}

} // namespace killcore
