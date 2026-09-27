#include "activity_manager.h"

#include "application_controller.h"

namespace killengine {

ActivityManager::ActivityManager(ApplicationController& controller, QObject* parent)
    : QObject(parent)
    , m_controller(controller) {
}

killcore::ActivityRegistry& ActivityManager::registry() {
    return m_registry;
}

const killcore::ActivityRegistry& ActivityManager::registry() const {
    return m_registry;
}

QVariantMap ActivityManager::entryToVariantMap(const killcore::ActivityEntry& entry) const {
    QVariantMap map;
    map["operationId"] = entry.operationId;
    map["kind"] = killcore::activityKindToString(entry.kind);
    map["state"] = killcore::activityStateToString(entry.state);
    map["revision"] = static_cast<qlonglong>(entry.revision);
    map["startedAtMs"] = static_cast<qlonglong>(entry.startedAtMs);
    map["finishedAtMs"] = static_cast<qlonglong>(entry.finishedAtMs);
    map["progress"] = entry.progress;
    map["summary"] = entry.summary;
    map["errorMessage"] = entry.errorMessage;
    map["canCancel"] = entry.canCancel;
    map["acknowledged"] = entry.acknowledged;
    map["resultRef"] = entry.resultRef;
    map["requestId"] = entry.requestId;
    if (entry.target.has_value()) {
        QVariantMap target;
        target["pid"] = entry.target->pid;
        target["processName"] = entry.target->processName;
        target["attachmentGeneration"] = entry.target->attachmentGeneration;
        map["target"] = target;
    } else {
        map["target"] = QVariant();
    }
    return map;
}

QVariantMap ActivityManager::getActivitySnapshot() const {
    QVariantMap result;
    result["globalRevision"] = static_cast<qlonglong>(m_registry.globalRevision());
    result["runningCount"] = m_registry.runningCount();
    result["unacknowledgedTerminalCount"] = m_registry.unacknowledgedTerminalCount();

    QVariantList entries;
    for (const auto& entry : m_registry.snapshot()) {
        entries.append(entryToVariantMap(entry));
    }
    result["entries"] = entries;
    return result;
}

void ActivityManager::notifyUpdated(const QString& operationId) {
    const auto entry = m_registry.find(operationId);
    if (!entry.has_value()) {
        return;
    }
    QVariantMap payload;
    payload["globalRevision"] = static_cast<qlonglong>(m_registry.globalRevision());
    payload["entry"] = entryToVariantMap(entry.value());
    emit activityUpdated(payload);
}

} // namespace killengine
