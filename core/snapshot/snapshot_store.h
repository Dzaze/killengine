#pragma once

#include "memory/memory_map.h"
#include "memory/memory_reader.h"
#include "scanner/scan_types.h"

#include <QList>
#include <QString>
#include <QTemporaryFile>

#include <cstddef>
#include <cstdint>
#include <memory>

namespace killcore {

struct SnapshotRegion {
    uint64_t baseAddress{0};
    uint64_t size{0};
    qint64 mappedOffset{0};
    qsizetype storedSize{0};
    qsizetype originalSize{0};
    bool compressed{false};
};

struct SnapshotResult {
    bool success{false};
    bool partial{false};
    bool cancelled{false};
    size_t regionsCaptured{0};
    size_t bytesCaptured{0};
    size_t compressedBytes{0};
    size_t regionsSkipped{0};
    bool mappedStorage{false};
    QString errorMessage;
};

struct UnknownScanResult {
    bool success{false};
    bool partial{false};
    bool cancelled{false};
    size_t checkedBytes{0};
    size_t matchesFound{0};
    QString errorMessage;
    QList<ScanMatch> matches;
};

class SnapshotStore {
public:
    SnapshotStore() = default;
    ~SnapshotStore();

    SnapshotStore(const SnapshotStore&) = delete;
    SnapshotStore& operator=(const SnapshotStore&) = delete;
    SnapshotStore(SnapshotStore&& other) noexcept;
    SnapshotStore& operator=(SnapshotStore&& other) noexcept;

    SnapshotResult capture(
        const ProcessHandle& process,
        size_t maxBytes = 512 * 1024 * 1024,
        const CancellationToken* cancellation = nullptr,
        const ScanOptions& options = {});

    SnapshotResult capture(
        const ProcessHandle& process,
        size_t maxBytes,
        const CancellationToken* cancellation);

    UnknownScanResult compare(
        const ProcessHandle& process,
        ValueType type,
        NextScanMode mode,
        const CancellationToken* cancellation = nullptr,
        const ScanOptions& options = {}) const;

    void clear();
    bool isEmpty() const;
    size_t bytesCaptured() const;
    size_t compressedBytesCaptured() const;
    bool usesMappedStorage() const;

private:
    QList<SnapshotRegion> m_regions;
    size_t m_bytesCaptured{0};
    size_t m_compressedBytesCaptured{0};
    std::unique_ptr<QTemporaryFile> m_backingFile;
    uchar* m_mappedData{nullptr};
    qint64 m_mappedSize{0};
};

} // namespace killcore
