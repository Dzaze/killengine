#include "scan_engine.h"

#include "logging/logger.h"

#include <algorithm>
#include <cstring>

namespace killcore {

namespace {

void scanBuffer(
    const QByteArray& buffer,
    uint64_t baseAddress,
    const QByteArray& needle,
    ValueType type,
    ScanResult* result,
    size_t maxResults) {
    if (!result || needle.isEmpty() || buffer.size() < needle.size()) {
        return;
    }

    const auto haystackSize = buffer.size();
    const auto needleSize = needle.size();
    const char* haystack = buffer.constData();
    const char* expected = needle.constData();

    for (qsizetype offset = 0; offset <= haystackSize - needleSize; ++offset) {
        if (std::memcmp(haystack + offset, expected, static_cast<size_t>(needleSize)) == 0) {
            ++result->matchesFound;
            if (result->matches.size() < static_cast<qsizetype>(maxResults)) {
                result->matches.append({baseAddress + static_cast<uint64_t>(offset), type});
            } else {
                result->partial = true;
            }
        }
    }
}

} // namespace

ScanEngine::ScanEngine(const ProcessHandle& process)
    : m_process(process) {
}

ScanResult ScanEngine::exactScan(
    const ScanValue& value,
    const ScanOptions& options,
    const CancellationToken* cancellation) const {
    ScanResult result;

    if (!m_process.isValid()) {
        result.errorMessage = "Process handle is not valid.";
        return result;
    }

    const QByteArray needle = scanValueToBytes(value);
    if (needle.isEmpty()) {
        result.errorMessage = "Scan value has no byte representation.";
        return result;
    }

    const auto regions = MemoryMap::snapshot(m_process);
    MemoryReader reader(m_process);
    const size_t chunkSize = std::max(options.chunkSize, static_cast<size_t>(needle.size()));

    for (const auto& region : regions) {
        if (cancellation && cancellation->isCancelled()) {
            result.cancelled = true;
            result.partial = true;
            result.errorMessage = "Scan cancelled.";
            return result;
        }

        if (!region.readable || region.guarded || region.size < static_cast<uint64_t>(needle.size())) {
            continue;
        }

        uint64_t offset = 0;
        QByteArray overlap;
        while (offset < region.size) {
            const size_t remaining = static_cast<size_t>(std::min<uint64_t>(
                region.size - offset,
                static_cast<uint64_t>(chunkSize)));
            const auto read = reader.readChunked(region.baseAddress + offset, remaining, chunkSize, cancellation);

            if (read.cancelled) {
                result.cancelled = true;
                result.partial = true;
                result.errorMessage = read.errorMessage;
                return result;
            }

            if (read.bytesRead == 0) {
                break;
            }

            QByteArray buffer = overlap + read.data;
            const uint64_t bufferBase = region.baseAddress + offset - static_cast<uint64_t>(overlap.size());
            scanBuffer(buffer, bufferBase, needle, value.type, &result, options.maxResults);

            result.bytesScanned += read.bytesRead;
            offset += static_cast<uint64_t>(read.bytesRead);

            const qsizetype overlapSize = std::min<qsizetype>(needle.size() - 1, buffer.size());
            overlap = buffer.right(overlapSize);

            if (read.partial) {
                result.partial = true;
                break;
            }
        }

        ++result.regionsScanned;
    }

    result.success = true;
    KE_LOG_INFO() << "Exact scan completed: type=" << valueTypeToString(value.type).toStdString()
                  << " matches=" << result.matchesFound
                  << " bytes=" << result.bytesScanned;
    return result;
}

} // namespace killcore
