#pragma once

#include "scanner/scan_types.h"

#include <QByteArray>
#include <QList>

#include <cstdint>

namespace killcore {

struct FreezeEntry {
    uint64_t address{0};
    ValueType type{ValueType::Int32};
    QByteArray value;
    bool enabled{true};
};

class FreezeManager {
public:
    void clear();
    void setEntry(uint64_t address, ValueType type, const QByteArray& value);
    void remove(uint64_t address);
    bool isEmpty() const;
    const QList<FreezeEntry>& entries() const;

private:
    QList<FreezeEntry> m_entries;
};

} // namespace killcore
