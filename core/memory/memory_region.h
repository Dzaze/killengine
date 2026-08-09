#pragma once

#include <QString>
#include <cstdint>

namespace killcore {

enum class MemoryState {
    Unknown,
    Committed,
    Reserved,
    Free,
};

enum class MemoryType {
    Unknown,
    Image,
    Mapped,
    Private,
};

struct MemoryRegion {
    uint64_t    baseAddress{0};
    uint64_t    allocationBase{0};
    uint64_t    size{0};
    uint32_t    protection{0};
    uint32_t    allocationProtection{0};
    MemoryState state{MemoryState::Unknown};
    MemoryType  type{MemoryType::Unknown};
    bool        readable{false};
    bool        writable{false};
    bool        executable{false};
    bool        guarded{false};
};

QString memoryStateToString(MemoryState state);
QString memoryTypeToString(MemoryType type);
QString protectionToString(uint32_t protection);

} // namespace killcore
