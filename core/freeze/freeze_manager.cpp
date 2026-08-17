#include "freeze_manager.h"

namespace killcore {

void FreezeManager::clear() {
    m_entries.clear();
}

void FreezeManager::setEntry(uint64_t address, ValueType type, const QByteArray& value, FreezeMode mode) {
    for (auto& entry : m_entries) {
        if (entry.address == address) {
            entry.type = type;
            entry.value = value;
            entry.enabled = true;
            entry.mode = mode;
            // Nouveau freeze pour cette adresse = etat de fiabilite reinitialise :
            // une reussite/echec passe ne doit pas influencer le prochain diagnostic.
            entry.consecutiveDriftTicks = 0;
            entry.totalDriftTicks = 0;
            entry.totalTicks = 0;
            entry.flaggedUnstable = false;
            return;
        }
    }

    m_entries.append({address, type, value, true, mode});
}

void FreezeManager::remove(uint64_t address) {
    for (qsizetype i = m_entries.size() - 1; i >= 0; --i) {
        if (m_entries.at(i).address == address) {
            m_entries.removeAt(i);
        }
    }
}

void FreezeManager::removeByMode(FreezeMode mode) {
    for (qsizetype i = m_entries.size() - 1; i >= 0; --i) {
        if (m_entries.at(i).mode == mode) {
            m_entries.removeAt(i);
        }
    }
}

bool FreezeManager::isEmpty() const {
    return m_entries.isEmpty();
}

bool FreezeManager::hasMode(FreezeMode mode) const {
    for (const auto& entry : m_entries) {
        if (entry.enabled && entry.mode == mode) {
            return true;
        }
    }
    return false;
}

const QList<FreezeEntry>& FreezeManager::entries() const {
    return m_entries;
}

QList<FreezeEntry> FreezeManager::entriesForMode(FreezeMode mode) const {
    QList<FreezeEntry> filtered;
    for (const auto& entry : m_entries) {
        if (entry.enabled && entry.mode == mode) {
            filtered.append(entry);
        }
    }
    return filtered;
}

bool FreezeManager::recordPollTick(uint64_t address, bool valueMatchedBeforeRewrite) {
    for (auto& entry : m_entries) {
        if (entry.address != address) {
            continue;
        }

        entry.totalTicks++;
        if (valueMatchedBeforeRewrite) {
            entry.consecutiveDriftTicks = 0;
            return false;
        }

        entry.totalDriftTicks++;
        entry.consecutiveDriftTicks++;

        if (entry.consecutiveDriftTicks >= kFreezePollDriftThreshold && !entry.flaggedUnstable) {
            entry.flaggedUnstable = true;
            return true;
        }
        return false;
    }
    return false;
}

} // namespace killcore
