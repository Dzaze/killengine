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

    /// Phase 13 : scan multi-type + variantes de représentation.
    /// Fusionne les résultats de plusieurs ScanValue, en marquant la confiance
    /// et le libellé de variante pour chaque match.
    struct MultiTypeMatch {
        ScanValue value;
        QString   label;
        bool      secondary{false};
    };

    ScanResult exactScanMultiType(
        const QList<MultiTypeMatch>& variants,
        const ScanOptions& options = {},
        const CancellationToken* cancellation = nullptr) const;

private:
    const ProcessHandle& m_process;
};

} // namespace killcore