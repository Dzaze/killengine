#include "freeze_manager.h"

namespace killcore {

void FreezeManager::clear() {
    m_entries.clear();
}

void FreezeManager::setEntry(uint64_t address, ValueType type, const QByteArray& value) {
    for (auto& entry : m_entries) {
        if (entry.address == address) {
            entry.type = type;
            entry.value = value;
            entry.enabled = true;
            return;
        }
    }

    m_entries.append({address, type, value, true});
}

void FreezeManager::remove(uint64_t address) {
    for (qsizetype i = m_entries.size() - 1; i >= 0; --i) {
        if (m_entries.at(i).address == address) {
            m_entries.removeAt(i);
        }
    }
}

bool FreezeManager::isEmpty() const {
    return m_entries.isEmpty();
}

const QList<FreezeEntry>& FreezeManager::entries() const {
    return m_entries;
}

} // namespace killcore
