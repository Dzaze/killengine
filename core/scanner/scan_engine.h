#pragma once

#include "memory/memory_map.h"
#include "memory/memory_reader.h"
#include "scanner/scan_types.h"

namespace killcore {

class ScanEngine {
public:
    explicit ScanEngine(const ProcessHandle& process);

    ScanResult exactScan(
        const ScanValue& value,
        const ScanOptions& options = {},
        const CancellationToken* cancellation = nullptr) const;

private:
    const ProcessHandle& m_process;
};

} // namespace killcore
