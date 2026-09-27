#include "activity_registry.h"

#include <QDateTime>

#include <algorithm>

namespace killcore {

namespace {

qint64 nowMs() {
    return QDateTime::currentMSecsSinceEpoch();
}

bool isTerminalState(ActivityState state) {
    switch (state) {
        case ActivityState::Completed:
        case ActivityState::Cancelled:
        case ActivityState::Failed:
        case ActivityState::Interrupted:
            return true;
        case ActivityState::Running:
        case ActivityState::CancelRequested:
            return false;
    }
    return false;
}

} // namespace

QString activityKindToString(ActivityKind kind) {
    switch (kind) {
        case ActivityKind::ScanExact: return QStringLiteral("scan_exact");
        case ActivityKind::ScanAuto: return QStringLiteral("scan_auto");
        case ActivityKind::ScanNext: return QStringLiteral("scan_next");
        case ActivityKind::ScanCaptureUnknown: return QStringLiteral("scan_capture_unknown");
        case ActivityKind::ScanUnknownNext: return QStringLiteral("scan_unknown_next");
        case ActivityKind::TimelineCollection: return QStringLiteral("timeline_collection");
        case ActivityKind::LuaScript: return QStringLiteral("lua_script");
        case ActivityKind::ModuleInstall: return QStringLiteral("module_install");
        case ActivityKind::SaveFileWatch: return QStringLiteral("save_file_watch");
        case ActivityKind::CandidateComparison: return QStringLiteral("candidate_comparison");
    }
    return QStringLiteral("unknown");
}

QString activityStateToString(ActivityState state) {
    switch (state) {
        case ActivityState::Running: return QStringLiteral("running");
        case ActivityState::CancelRequested: return QStringLiteral("cancel_requested");
        case ActivityState::Completed: return QStringLiteral("completed");
        case ActivityState::Cancelled: return QStringLiteral("cancelled");
        case ActivityState::Failed: return QStringLiteral("failed");
        case ActivityState::Interrupted: return QStringLiteral("interrupted");
    }
    return QStringLiteral("unknown");
}

QString ActivityRegistry::beginActivity(ActivityKind kind,
                                         const QString& summary,
                                         bool canCancel,
                                         std::optional<ActivityTarget> target,
                                         const QString& requestId) {
    ActivityEntry entry;
    entry.operationId = QStringLiteral("act-%1").arg(m_nextOperationSeq++);
    entry.kind = kind;
    entry.state = ActivityState::Running;
    entry.revision = ++m_globalRevision;
    entry.startedAtMs = nowMs();
    entry.finishedAtMs = 0;
    entry.summary = summary;
    entry.canCancel = canCancel;
    entry.target = std::move(target);
    entry.requestId = requestId;

    const QString id = entry.operationId;
    m_entries.insert(id, entry);
    m_order.append(id);
    return id;
}

bool ActivityRegistry::updateProgress(const QString& operationId, QVariant progress, const QString& summary) {
    auto it = m_entries.find(operationId);
    if (it == m_entries.end()) {
        return false;
    }
    // Une entrée terminale ne redevient jamais running : une poussée de
    // progression en retard (arrivée après finish()) est ignorée plutôt que
    // de rouvrir l'activité (voir fiche : "progression après fin").
    if (isTerminalState(it->state)) {
        return false;
    }
    it->progress = std::move(progress);
    if (!summary.isEmpty()) {
        it->summary = summary;
    }
    it->revision = ++m_globalRevision;
    return true;
}

bool ActivityRegistry::markCancelRequested(const QString& operationId) {
    auto it = m_entries.find(operationId);
    if (it == m_entries.end()) {
        return false;
    }
    if (isTerminalState(it->state)) {
        // Idempotent : annuler une activité déjà terminée n'est pas une erreur.
        return true;
    }
    if (it->state == ActivityState::CancelRequested) {
        return true;
    }
    it->state = ActivityState::CancelRequested;
    it->revision = ++m_globalRevision;
    return true;
}

bool ActivityRegistry::finish(const QString& operationId,
                               ActivityState terminalState,
                               const QString& summary,
                               const QString& errorMessage,
                               const QString& resultRef) {
    if (!isTerminalState(terminalState)) {
        return false;
    }
    auto it = m_entries.find(operationId);
    if (it == m_entries.end()) {
        return false;
    }
    if (isTerminalState(it->state)) {
        // Déjà terminée (double notification) : ne pas re-terminer, ne pas
        // re-bump la révision pour une mutation qui n'a pas réellement eu lieu.
        return false;
    }

    it->state = terminalState;
    it->finishedAtMs = nowMs();
    if (!summary.isEmpty()) {
        it->summary = summary;
    }
    it->errorMessage = errorMessage;
    it->resultRef = resultRef;
    it->progress = 100;
    it->canCancel = false;
    it->revision = ++m_globalRevision;

    evictTerminalIfNeeded();
    return true;
}

bool ActivityRegistry::acknowledge(const QString& operationId) {
    auto it = m_entries.find(operationId);
    if (it == m_entries.end()) {
        return false;
    }
    if (it->acknowledged) {
        return true;
    }
    it->acknowledged = true;
    it->revision = ++m_globalRevision;
    return true;
}

std::optional<ActivityEntry> ActivityRegistry::find(const QString& operationId) const {
    auto it = m_entries.constFind(operationId);
    if (it == m_entries.constEnd()) {
        return std::nullopt;
    }
    return it.value();
}

QList<ActivityEntry> ActivityRegistry::snapshot() const {
    QList<ActivityEntry> running;
    QList<ActivityEntry> terminal;
    for (const QString& id : m_order) {
        auto it = m_entries.constFind(id);
        if (it == m_entries.constEnd()) {
            continue; // évincée
        }
        if (isTerminalState(it->state)) {
            terminal.append(it.value());
        } else {
            running.append(it.value());
        }
    }
    std::reverse(terminal.begin(), terminal.end()); // plus récente d'abord
    running.append(terminal);
    return running;
}

qint64 ActivityRegistry::globalRevision() const {
    return m_globalRevision;
}

int ActivityRegistry::runningCount() const {
    int count = 0;
    for (const auto& entry : m_entries) {
        if (!isTerminalState(entry.state)) {
            ++count;
        }
    }
    return count;
}

int ActivityRegistry::unacknowledgedTerminalCount() const {
    int count = 0;
    for (const auto& entry : m_entries) {
        if (isTerminalState(entry.state) && !entry.acknowledged) {
            ++count;
        }
    }
    return count;
}

void ActivityRegistry::evictTerminalIfNeeded() {
    int terminalCount = 0;
    for (const auto& entry : m_entries) {
        if (isTerminalState(entry.state)) {
            ++terminalCount;
        }
    }
    if (terminalCount <= kActivityTerminalCap) {
        return;
    }

    int toEvict = terminalCount - kActivityTerminalCap;
    for (qsizetype i = 0; i < m_order.size() && toEvict > 0; ) {
        const QString& id = m_order.at(i);
        auto it = m_entries.constFind(id);
        if (it == m_entries.constEnd()) {
            m_order.removeAt(i);
            continue;
        }
        if (isTerminalState(it->state)) {
            m_entries.remove(id);
            m_order.removeAt(i);
            --toEvict;
            ++m_globalRevision;
            continue;
        }
        ++i;
    }
}

} // namespace killcore
