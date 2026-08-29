#include "auto_write_state_access.h"

namespace killengine {

AutoWriteStateAccess::AutoWriteStateAccess(
    QList<WriteRecord>& writeHistory,
    QList<AutoWriteTarget>& lastTargets,
    QList<AutoWriteTarget>& chatTargets)
    : m_writeHistory(writeHistory)
    , m_lastTargets(lastTargets)
    , m_chatTargets(chatTargets) {}

int AutoWriteStateAccess::writeHistorySize() const {
    return m_writeHistory.size();
}

bool AutoWriteStateAccess::writeHistoryEmpty() const {
    return m_writeHistory.isEmpty();
}

const WriteRecord& AutoWriteStateAccess::writeHistoryAt(int index) const {
    return m_writeHistory.at(index);
}

void AutoWriteStateAccess::appendWriteRecord(const WriteRecord& record) {
    m_writeHistory.append(record);
}

void AutoWriteStateAccess::removeWriteHistoryAt(int index) {
    m_writeHistory.removeAt(index);
}

bool AutoWriteStateAccess::hasLastTargets() const {
    return !m_lastTargets.isEmpty();
}

int AutoWriteStateAccess::lastTargetCount() const {
    return m_lastTargets.size();
}

const QList<AutoWriteTarget>& AutoWriteStateAccess::lastTargets() const {
    return m_lastTargets;
}

QList<AutoWriteTarget>& AutoWriteStateAccess::lastTargets() {
    return m_lastTargets;
}

void AutoWriteStateAccess::appendLastTarget(const AutoWriteTarget& target) {
    m_lastTargets.append(target);
}

void AutoWriteStateAccess::clearLastTargets() {
    m_lastTargets.clear();
}

bool AutoWriteStateAccess::hasChatTargets() const {
    return !m_chatTargets.isEmpty();
}

int AutoWriteStateAccess::chatTargetCount() const {
    return m_chatTargets.size();
}

const QList<AutoWriteTarget>& AutoWriteStateAccess::chatTargets() const {
    return m_chatTargets;
}

QList<AutoWriteTarget>& AutoWriteStateAccess::chatTargets() {
    return m_chatTargets;
}

void AutoWriteStateAccess::appendChatTarget(const AutoWriteTarget& target) {
    m_chatTargets.append(target);
}

void AutoWriteStateAccess::clearChatTargets() {
    m_chatTargets.clear();
}

void AutoWriteStateAccess::replaceChatTargetsWithLastTargets() {
    m_chatTargets = m_lastTargets;
}

} // namespace killengine
