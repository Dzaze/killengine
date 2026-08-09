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
    size_t maxResults,
    size_t alignment,
    uint64_t rangeStart,
    uint64_t rangeEnd) {
    if (!result || needle.isEmpty() || buffer.size() < needle.size()) {
        return;
    }

    const auto haystackSize = buffer.size();
    const auto needleSize = needle.size();
    const char* haystack = buffer.constData();
    const char* expected = needle.constData();
    const uint64_t step = static_cast<uint64_t>(std::max<size_t>(alignment, 1));

    uint64_t firstAddress = std::max(baseAddress, rangeStart);
    if (rangeEnd != 0 && firstAddress >= rangeEnd) {
        return;
    }

    const uint64_t remainder = firstAddress % step;
    if (remainder != 0) {
        firstAddress += step - remainder;
    }
    if (firstAddress < baseAddress) {
        return;
    }

    const uint64_t firstOffset = firstAddress - baseAddress;
    if (firstOffset > static_cast<uint64_t>(haystackSize - needleSize)) {
        return;
    }

    for (qsizetype offset = static_cast<qsizetype>(firstOffset);
         offset <= haystackSize - needleSize;
         offset += static_cast<qsizetype>(step)) {
        const uint64_t candidateAddress = baseAddress + static_cast<uint64_t>(offset);
        if (rangeEnd != 0 && candidateAddress >= rangeEnd) {
            break;
        }
        if (std::memcmp(haystack + offset, expected, static_cast<size_t>(needleSize)) == 0) {
            ++result->matchesFound;
            if (result->matches.size() < static_cast<qsizetype>(maxResults)) {
                result->matches.append({candidateAddress, type});
            } else {
                result->partial = true;
            }
        }
    }
}

bool isCopyOnWriteProtection(uint32_t protection) {
    const auto flags = protectionToString(protection).split('|');
    return flags.contains("WC") || flags.contains("XWC");
}

bool regionMatchesScanOptions(
    const MemoryRegion& region,
    const ScanOptions& options,
    size_t needleSize,
    uint64_t* scanStart = nullptr,
    uint64_t* scanEnd = nullptr) {
    if (!region.readable || region.guarded || region.size < static_cast<uint64_t>(needleSize)) {
        return false;
    }
    if (options.writableOnly && !region.writable) return false;
    if (options.executableOnly && !region.executable) return false;
    if (options.copyOnWriteOnly && !isCopyOnWriteProtection(region.protection)) return false;

    const uint64_t regionStart = region.baseAddress;
    const uint64_t regionEnd = region.baseAddress + region.size;
    if (options.stopAddress != 0 && regionStart >= options.stopAddress) return false;
    if (options.startAddress != 0 && options.startAddress >= regionEnd) return false;

    const uint64_t effectiveStart = std::max(regionStart, options.startAddress);
    const uint64_t effectiveEnd = options.stopAddress == 0 ? regionEnd : std::min(regionEnd, options.stopAddress);
    if (effectiveEnd <= effectiveStart || effectiveEnd - effectiveStart < static_cast<uint64_t>(needleSize)) {
        return false;
    }

    if (scanStart) *scanStart = effectiveStart;
    if (scanEnd) *scanEnd = effectiveEnd;
    return true;
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
    size_t progressRegionsTotal = 0;
    size_t progressBytesTotal = 0;
    for (const auto& region : regions) {
        uint64_t scanStart = 0;
        uint64_t scanEnd = 0;
        if (regionMatchesScanOptions(region, options, static_cast<size_t>(needle.size()), &scanStart, &scanEnd)) {
            ++progressRegionsTotal;
            progressBytesTotal += static_cast<size_t>(scanEnd - scanStart);
        }
    }

    auto reportProgress = [&]() {
        if (!options.progressCallback) {
            return;
        }
        options.progressCallback({
            progressRegionsTotal,
            result.regionsScanned,
            progressBytesTotal,
            result.bytesScanned,
            result.matchesFound,
        });
    };
    reportProgress();

    // Fast scan : aligne automatiquement sur la taille du type si l'utilisateur
    // n'a pas forcé un alignement explicite (alignment > 1).
    size_t effectiveAlignment = options.alignment;
    if (effectiveAlignment <= 1 && options.fastScan) {
        effectiveAlignment = valueTypeSize(value.type);
    }

    for (const auto& region : regions) {
        if (cancellation && cancellation->isCancelled()) {
            result.cancelled = true;
            result.partial = true;
            result.errorMessage = "Scan cancelled.";
            return result;
        }

        if (!regionMatchesScanOptions(region, options, static_cast<size_t>(needle.size()))) {
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
            scanBuffer(
                buffer,
                bufferBase,
                needle,
                value.type,
                &result,
                options.maxResults,
                effectiveAlignment,
                options.startAddress,
                options.stopAddress);

            result.bytesScanned += read.bytesRead;
            reportProgress();
            offset += static_cast<uint64_t>(read.bytesRead);

            const qsizetype overlapSize = std::min<qsizetype>(needle.size() - 1, buffer.size());
            overlap = buffer.right(overlapSize);

            if (read.partial) {
                result.partial = true;
                break;
            }
        }

        ++result.regionsScanned;
        reportProgress();
    }

    result.success = true;
    KE_LOG_INFO() << "Exact scan completed: type=" << valueTypeToString(value.type).toStdString()
                  << " matches=" << result.matchesFound
                  << " bytes=" << result.bytesScanned;
    return result;
}

} // namespace killcore
