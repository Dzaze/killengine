#pragma once

#include "memory/memory_region.h"
#include "process/process_handle.h"

#include <QList>
#include <cstdint>

namespace killcore {

struct MemoryMapStats {
    int      regionCount{0};
    int      committedCount{0};
    int      readableCount{0};
    int      writableCount{0};
    int      executableCount{0};
    uint64_t totalBytes{0};
    uint64_t committedBytes{0};
    uint64_t readableBytes{0};
    uint64_t writableBytes{0};
    uint64_t executableBytes{0};
};

class MemoryMap {
public:
    static QList<MemoryRegion> snapshot(const ProcessHandle& process);
    static MemoryMapStats stats(const QList<MemoryRegion>& regions);
};

} // namespace killcore
