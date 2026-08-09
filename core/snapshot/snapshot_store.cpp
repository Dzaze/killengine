#include "snapshot_store.h"

#include "logging/logger.h"
#include "snapshot_codec.h"

#include <QDir>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

namespace killcore {

namespace {

double bytesToDouble(const char* data, ValueType type) {
    switch (type) {
        case ValueType::Int32: {
            int32_t value = 0;
            std::memcpy(&value, data, sizeof(value));
            return static_cast<double>(value);
        }
        case ValueType::Int64: {
            int64_t value = 0;
            std::memcpy(&value, data, sizeof(value));
            return static_cast<double>(value);
        }
        case ValueType::Float32: {
            float value = 0;
            std::memcpy(&value, data, sizeof(value));
            return static_cast<double>(value);
        }
        case ValueType::Float64: {
            double value = 0;
            std::memcpy(&value, data, sizeof(value));
            return value;
        }
    }
    return 0.0;
}

bool equalsValue(const char* a, const char* b, ValueType type) {
    const size_t size = valueTypeSize(type);
    if (type == ValueType::Float32 || type == ValueType::Float64) {
        return std::abs(bytesToDouble(a, type) - bytesToDouble(b, type)) < 0.000001;
    }
    return std::memcmp(a, b, size) == 0;
}

bool matchesMode(const char* previous, const char* current, ValueType type, NextScanMode mode) {
    const double prev = bytesToDouble(previous, type);
    const double now = bytesToDouble(current, type);

    switch (mode) {
        case NextScanMode::Changed:
            return !equalsValue(previous, current, type);
        case NextScanMode::Unchanged:
            return equalsValue(previous, current, type);
        case NextScanMode::Increased:
            return now > prev;
        case NextScanMode::Decreased:
            return now < prev;
        case NextScanMode::Exact:
        case NextScanMode::Delta:
            return false;
    }
    return false;
}

} // namespace

SnapshotStore::~SnapshotStore() {
    clear();
}

SnapshotStore::SnapshotStore(SnapshotStore&& other) noexcept
    : m_regions(std::move(other.m_regions)),
      m_bytesCaptured(other.m_bytesCaptured),
      m_compressedBytesCaptured(other.m_compressedBytesCaptured),
      m_backingFile(std::move(other.m_backingFile)),
      m_mappedData(other.m_mappedData),
      m_mappedSize(other.m_mappedSize) {
    other.m_bytesCaptured = 0;
    other.m_compressedBytesCaptured = 0;
    other.m_mappedData = nullptr;
    other.m_mappedSize = 0;
}

SnapshotStore& SnapshotStore::operator=(SnapshotStore&& other) noexcept {
    if (this == &other) {
        return *this;
    }

    clear();
    m_regions = std::move(other.m_regions);
    m_bytesCaptured = other.m_bytesCaptured;
    m_compressedBytesCaptured = other.m_compressedBytesCaptured;
    m_backingFile = std::move(other.m_backingFile);
    m_mappedData = other.m_mappedData;
    m_mappedSize = other.m_mappedSize;

    other.m_bytesCaptured = 0;
    other.m_compressedBytesCaptured = 0;
    other.m_mappedData = nullptr;
    other.m_mappedSize = 0;
    return *this;
}

SnapshotResult SnapshotStore::capture(
    const ProcessHandle& process,
    size_t maxBytes,
    const CancellationToken* cancellation) {
    clear();

    SnapshotResult result;
    if (!process.isValid()) {
        result.errorMessage = "Process handle is not valid.";
        return result;
    }

    MemoryReader reader(process);
    const auto regions = MemoryMap::snapshot(process);
    m_backingFile = std::make_unique<QTemporaryFile>();
    m_backingFile->setFileTemplate(QDir::tempPath() + "/killengine_snapshot_XXXXXX.kesnap");
    m_backingFile->setAutoRemove(true);
    if (!m_backingFile->open()) {
        result.errorMessage = "Unable to create mapped snapshot backing file.";
        clear();
        return result;
    }

    for (const auto& region : regions) {
        if (cancellation && cancellation->isCancelled()) {
            result.cancelled = true;
            result.partial = true;
            result.errorMessage = "Snapshot capture cancelled.";
            return result;
        }

        if (!region.readable || region.guarded || region.size == 0) {
            ++result.regionsSkipped;
            continue;
        }

        if (m_bytesCaptured + static_cast<size_t>(region.size) > maxBytes) {
            ++result.regionsSkipped;
            result.partial = true;
            continue;
        }

        const auto read = reader.readChunked(region.baseAddress, static_cast<size_t>(region.size), 1024 * 1024, cancellation);
        if (!(read.success || read.partial) || read.bytesRead == 0) {
            ++result.regionsSkipped;
            continue;
        }

        auto compressed = SnapshotCodec::compressLz4(read.data);
        if (!compressed.success) {
            ++result.regionsSkipped;
            result.partial = true;
            result.errorMessage = compressed.errorMessage;
            continue;
        }

        const qint64 mappedOffset = m_backingFile->pos();
        const qint64 written = m_backingFile->write(compressed.block.data);
        if (written != compressed.block.data.size()) {
            result.errorMessage = "Unable to write compressed snapshot block.";
            clear();
            return result;
        }

        SnapshotRegion snapshotRegion;
        snapshotRegion.baseAddress = region.baseAddress;
        snapshotRegion.size = static_cast<uint64_t>(read.bytesRead);
        snapshotRegion.mappedOffset = mappedOffset;
        snapshotRegion.storedSize = compressed.block.data.size();
        snapshotRegion.originalSize = compressed.block.originalSize;
        snapshotRegion.compressed = compressed.block.compressed;
        m_regions.append(std::move(snapshotRegion));
        m_bytesCaptured += read.bytesRead;
        m_compressedBytesCaptured += static_cast<size_t>(compressed.block.data.size());

        ++result.regionsCaptured;
        result.bytesCaptured = m_bytesCaptured;
        result.compressedBytes = m_compressedBytesCaptured;

        if (read.partial) {
            result.partial = true;
        }
    }

    result.success = !m_regions.isEmpty();
    if (!result.success) {
        result.errorMessage = "No readable regions captured.";
        clear();
        return result;
    }

    if (!m_backingFile->flush()) {
        result.success = false;
        result.errorMessage = "Unable to flush mapped snapshot backing file.";
        clear();
        return result;
    }

    m_mappedSize = m_backingFile->size();
    if (m_mappedSize <= 0) {
        result.success = false;
        result.errorMessage = "Mapped snapshot backing file is empty.";
        clear();
        return result;
    }

    m_mappedData = m_backingFile->map(0, m_mappedSize);
    if (!m_mappedData) {
        result.success = false;
        result.errorMessage = "Unable to map compressed snapshot backing file.";
        clear();
        return result;
    }
    result.mappedStorage = true;

    KE_LOG_INFO() << "Snapshot captured: regions=" << result.regionsCaptured
                  << " bytes=" << result.bytesCaptured
                  << " compressedBytes=" << result.compressedBytes
                  << " mappedStorage=" << result.mappedStorage;
    return result;
}

UnknownScanResult SnapshotStore::compare(
    const ProcessHandle& process,
    ValueType type,
    NextScanMode mode,
    const CancellationToken* cancellation) const {
    UnknownScanResult result;

    if (!process.isValid()) {
        result.errorMessage = "Process handle is not valid.";
        return result;
    }

    if (m_regions.isEmpty()) {
        result.errorMessage = "No snapshot captured.";
        return result;
    }

    if (!m_mappedData || m_mappedSize <= 0) {
        result.errorMessage = "Snapshot mapped storage is not available.";
        return result;
    }

    if (mode == NextScanMode::Exact || mode == NextScanMode::Delta) {
        result.errorMessage = "Unknown scan supports changed/unchanged/increased/decreased.";
        return result;
    }

    const size_t valueSize = valueTypeSize(type);
    MemoryReader reader(process);

    for (const auto& region : m_regions) {
        if (cancellation && cancellation->isCancelled()) {
            result.cancelled = true;
            result.partial = true;
            result.errorMessage = "Unknown scan cancelled.";
            return result;
        }

        if (region.mappedOffset < 0
            || region.storedSize < 0
            || region.mappedOffset + region.storedSize > m_mappedSize) {
            result.partial = true;
            result.errorMessage = "Snapshot mapped block is invalid.";
            continue;
        }

        const auto previous = SnapshotCodec::decompressLz4(
            reinterpret_cast<const char*>(m_mappedData + region.mappedOffset),
            region.storedSize,
            region.originalSize);
        if (!previous.success) {
            result.partial = true;
            result.errorMessage = previous.errorMessage;
            continue;
        }

        const auto read = reader.readChunked(region.baseAddress, static_cast<size_t>(region.size), 1024 * 1024, cancellation);
        if (!(read.success || read.partial) || read.bytesRead < valueSize) {
            result.partial = true;
            continue;
        }

        const qsizetype comparable = std::min(previous.block.data.size(), read.data.size());
        for (qsizetype offset = 0; offset <= comparable - static_cast<qsizetype>(valueSize); ++offset) {
            if (matchesMode(previous.block.data.constData() + offset, read.data.constData() + offset, type, mode)) {
                ++result.matchesFound;
                if (result.matches.size() < 1000000) {
                    result.matches.append({region.baseAddress + static_cast<uint64_t>(offset), type});
                } else {
                    result.partial = true;
                }
            }
        }
        result.checkedBytes += static_cast<size_t>(comparable);
    }

    result.success = true;
    return result;
}

void SnapshotStore::clear() {
    if (m_backingFile && m_mappedData) {
        m_backingFile->unmap(m_mappedData);
    }
    m_mappedData = nullptr;
    m_mappedSize = 0;
    m_backingFile.reset();
    m_regions.clear();
    m_bytesCaptured = 0;
    m_compressedBytesCaptured = 0;
}

bool SnapshotStore::isEmpty() const {
    return m_regions.isEmpty();
}

size_t SnapshotStore::bytesCaptured() const {
    return m_bytesCaptured;
}

size_t SnapshotStore::compressedBytesCaptured() const {
    return m_compressedBytesCaptured;
}

bool SnapshotStore::usesMappedStorage() const {
    return m_mappedData != nullptr && m_mappedSize > 0;
}

} // namespace killcore
