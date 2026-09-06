#include "scan_engine.h"

#include "logging/logger.h"
#include "scanner/candidate_confidence.h"
#include "scanner/performance_profile.h"
#include "scanner/worker_pool.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <vector>

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
    uint64_t rangeEnd,
    double confidence = 1.0,
    const QString& variantLabel = {}) {
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
                ScanMatch match;
                match.address = candidateAddress;
                match.type = type;
                match.confidence = confidence;
                match.variantLabel = variantLabel;
                result->matches.append(match);
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

PerformanceProfile profileFromScanOptions(const ScanOptions& options) {
    PerformanceProfile profile = makePerformanceProfile(options.performanceMode);
    if (options.chunkSize > 0) {
        profile.chunkSize = options.chunkSize;
    }
    if (options.maxInFlightBytes > 0) {
        profile.maxInFlightBytes = options.maxInFlightBytes;
    }
    if (options.maxWorkerThreads > 0) {
        profile.workerThreads = std::clamp(options.maxWorkerThreads, static_cast<size_t>(1), profile.workerThreads);
    }
    if (profile.chunkSize > 0 && profile.maxInFlightBytes > 0) {
        const size_t workersByMemory = std::max<size_t>(
            1,
            static_cast<size_t>(profile.maxInFlightBytes / static_cast<uint64_t>(profile.chunkSize)));
        profile.workerThreads = std::min(profile.workerThreads, workersByMemory);
    }
    return profile;
}

struct ScanRegionTask {
    MemoryRegion region;
    uint64_t scanStart{0};
    uint64_t scanEnd{0};
};

std::vector<ScanRegionTask> collectScanRegions(
    const QList<MemoryRegion>& regions,
    const ScanOptions& options,
    size_t needleSize,
    size_t* bytesTotal) {
    std::vector<ScanRegionTask> scanRegions;
    scanRegions.reserve(static_cast<size_t>(regions.size()));
    if (bytesTotal) {
        *bytesTotal = 0;
    }

    for (const auto& region : regions) {
        uint64_t scanStart = 0;
        uint64_t scanEnd = 0;
        if (regionMatchesScanOptions(region, options, needleSize, &scanStart, &scanEnd)) {
            scanRegions.push_back({region, scanStart, scanEnd});
            if (bytesTotal) {
                *bytesTotal += static_cast<size_t>(scanEnd - scanStart);
            }
        }
    }

    return scanRegions;
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
    const PerformanceProfile performance = profileFromScanOptions(options);
    const size_t chunkSize = std::max(performance.chunkSize, static_cast<size_t>(needle.size()));

    size_t progressBytesTotal = 0;
    const auto scanRegions = collectScanRegions(
        regions,
        options,
        static_cast<size_t>(needle.size()),
        &progressBytesTotal);

    const size_t progressRegionsTotal = scanRegions.size();
    std::atomic_size_t regionsScanned{0};
    std::atomic_size_t bytesScanned{0};
    std::atomic_size_t matchesFound{0};
    std::atomic_bool stopRequested{false};
    std::mutex resultMutex;
    std::mutex progressMutex;

    auto reportProgress = [&]() {
        if (!options.progressCallback) {
            return;
        }
        std::lock_guard<std::mutex> lock(progressMutex);
        options.progressCallback({
            progressRegionsTotal,
            regionsScanned.load(std::memory_order_relaxed),
            progressBytesTotal,
            bytesScanned.load(std::memory_order_relaxed),
            matchesFound.load(std::memory_order_relaxed),
        });
    };
    reportProgress();

    // Fast scan : aligne automatiquement sur la taille du type si l'utilisateur
    // n'a pas forcé un alignement explicite (alignment > 1).
    size_t effectiveAlignment = options.alignment;
    if (effectiveAlignment <= 1 && options.fastScan) {
        effectiveAlignment = valueTypeSize(value.type);
    }

    auto markCancelled = [&](const QString& message) {
        stopRequested.store(true, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(resultMutex);
        result.cancelled = true;
        result.partial = true;
        if (result.errorMessage.isEmpty()) {
            result.errorMessage = message;
        }
    };

    auto mergeChunkMatches = [&](const ScanResult& chunkResult) {
        if (chunkResult.matchesFound == 0) {
            return;
        }

        matchesFound.fetch_add(chunkResult.matchesFound, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(resultMutex);
        for (const auto& match : chunkResult.matches) {
            if (result.matches.size() < static_cast<qsizetype>(options.maxResults)) {
                result.matches.append(match);
            } else {
                result.partial = true;
                break;
            }
        }
        if (chunkResult.matches.size() < static_cast<qsizetype>(chunkResult.matchesFound)) {
            result.partial = true;
        }
    };

    WorkerPool::runBlocking(performance.workerThreads, scanRegions.size(), [&](size_t taskIndex) {
        if (stopRequested.load(std::memory_order_relaxed)) {
            return;
        }
        if (cancellation && cancellation->isCancelled()) {
            markCancelled("Scan cancelled.");
            return;
        }

        const auto& task = scanRegions[taskIndex];
        const uint64_t scanSize = task.scanEnd - task.scanStart;
        // Score de région calculé une fois par région balayée (pas par match ni
        // par candidat) : task.region est déjà la MemoryRegion exacte, aucun
        // lookup nécessaire.
        const double regionConfidence = computeScanTimeConfidence(
            computeRegionScore(task.region, CandidateConfidenceContext{}),
            /*secondaryVariant=*/false);
        MemoryReader reader(m_process);
        uint64_t offset = 0;
        QByteArray overlap;
        while (offset < scanSize) {
            if (stopRequested.load(std::memory_order_relaxed)) {
                return;
            }
            if (cancellation && cancellation->isCancelled()) {
                markCancelled("Scan cancelled.");
                return;
            }

            const size_t remaining = static_cast<size_t>(std::min<uint64_t>(
                scanSize - offset,
                static_cast<uint64_t>(chunkSize)));
            const auto read = reader.readChunked(task.scanStart + offset, remaining, chunkSize, cancellation);

            if (read.cancelled) {
                markCancelled(read.errorMessage.isEmpty() ? QString("Scan cancelled.") : read.errorMessage);
                return;
            }

            if (read.bytesRead == 0) {
                break;
            }

            QByteArray buffer = overlap + read.data;
            const uint64_t bufferBase = task.scanStart + offset - static_cast<uint64_t>(overlap.size());
            ScanResult chunkResult;
            scanBuffer(
                buffer,
                bufferBase,
                needle,
                value.type,
                &chunkResult,
                options.maxResults,
                effectiveAlignment,
                options.startAddress,
                options.stopAddress,
                regionConfidence);
            mergeChunkMatches(chunkResult);

            bytesScanned.fetch_add(read.bytesRead, std::memory_order_relaxed);
            reportProgress();
            offset += static_cast<uint64_t>(read.bytesRead);

            const qsizetype overlapSize = std::min<qsizetype>(needle.size() - 1, buffer.size());
            overlap = buffer.right(overlapSize);

            if (read.partial) {
                std::lock_guard<std::mutex> lock(resultMutex);
                result.partial = true;
                break;
            }
        }

        regionsScanned.fetch_add(1, std::memory_order_relaxed);
        reportProgress();
    });

    result.regionsScanned = regionsScanned.load(std::memory_order_relaxed);
    result.bytesScanned = bytesScanned.load(std::memory_order_relaxed);
    result.matchesFound = matchesFound.load(std::memory_order_relaxed);

    if (result.cancelled) {
        return result;
    }

    std::sort(result.matches.begin(), result.matches.end(),
              [](const ScanMatch& a, const ScanMatch& b) {
                  if (a.address != b.address) return a.address < b.address;
                  return static_cast<int>(a.type) < static_cast<int>(b.type);
              });

    result.success = true;
    KE_LOG_INFO() << "Exact scan completed: type=" << valueTypeToString(value.type).toStdString()
                  << " matches=" << result.matchesFound
                  << " bytes=" << result.bytesScanned
                  << " mode=" << performanceModeToString(performance.resolvedMode)
                  << " workers=" << performance.workerThreads
                  << " chunk=" << chunkSize;
    return result;
}

ScanResult ScanEngine::exactScanMultiType(
    const QList<MultiTypeMatch>& variants,
    const ScanOptions& options,
    const CancellationToken* cancellation) const {
    ScanResult result;

    if (!m_process.isValid()) {
        result.errorMessage = "Process handle is not valid.";
        return result;
    }

    if (variants.isEmpty()) {
        result.errorMessage = "No scan variant provided.";
        return result;
    }

    // Déterminer la plus grande taille d'aiguille pour découper les chunks correctement.
    size_t maxNeedleSize = 1;
    for (const auto& variant : variants) {
        const QByteArray needle = scanValueToBytes(variant.value);
        maxNeedleSize = std::max(maxNeedleSize, static_cast<size_t>(needle.size()));
    }

    const PerformanceProfile performance = profileFromScanOptions(options);
    const auto regions = MemoryMap::snapshot(m_process);
    const size_t chunkSize = std::max(performance.chunkSize, maxNeedleSize);

    size_t progressBytesTotal = 0;
    const auto scanRegions = collectScanRegions(regions, options, maxNeedleSize, &progressBytesTotal);
    const size_t progressRegionsTotal = scanRegions.size();
    std::atomic_size_t regionsScanned{0};
    std::atomic_size_t bytesScanned{0};
    std::atomic_size_t rawMatchesFound{0};
    std::atomic_bool stopRequested{false};
    std::mutex resultMutex;
    std::mutex progressMutex;

    auto reportProgress = [&]() {
        if (!options.progressCallback) {
            return;
        }
        std::lock_guard<std::mutex> lock(progressMutex);
        options.progressCallback({
            progressRegionsTotal,
            regionsScanned.load(std::memory_order_relaxed),
            progressBytesTotal,
            bytesScanned.load(std::memory_order_relaxed),
            rawMatchesFound.load(std::memory_order_relaxed),
        });
    };
    reportProgress();

    auto markCancelled = [&](const QString& message) {
        stopRequested.store(true, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(resultMutex);
        result.cancelled = true;
        result.partial = true;
        if (result.errorMessage.isEmpty()) {
            result.errorMessage = message;
        }
    };

    auto mergeChunkMatches = [&](const ScanResult& chunkResult) {
        if (chunkResult.matchesFound == 0) {
            return;
        }

        rawMatchesFound.fetch_add(chunkResult.matchesFound, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(resultMutex);
        for (const auto& match : chunkResult.matches) {
            if (result.matches.size() < static_cast<qsizetype>(options.maxResults)) {
                result.matches.append(match);
            } else {
                result.partial = true;
                break;
            }
        }
        if (chunkResult.matches.size() < static_cast<qsizetype>(chunkResult.matchesFound)) {
            result.partial = true;
        }
    };

    WorkerPool::runBlocking(performance.workerThreads, scanRegions.size(), [&](size_t taskIndex) {
        if (stopRequested.load(std::memory_order_relaxed)) {
            return;
        }
        if (cancellation && cancellation->isCancelled()) {
            markCancelled("Scan cancelled.");
            return;
        }

        const auto& task = scanRegions[taskIndex];
        const uint64_t scanSize = task.scanEnd - task.scanStart;
        // Score de région calculé une fois par région balayée (pas par variante
        // ni par match) : task.region est déjà la MemoryRegion exacte, aucun
        // lookup nécessaire.
        const double regionScore = computeRegionScore(task.region, CandidateConfidenceContext{});
        MemoryReader reader(m_process);
        uint64_t offset = 0;
        QByteArray overlap;
        while (offset < scanSize) {
            if (stopRequested.load(std::memory_order_relaxed)) {
                return;
            }
            if (cancellation && cancellation->isCancelled()) {
                markCancelled("Scan cancelled.");
                return;
            }

            const size_t remaining = static_cast<size_t>(std::min<uint64_t>(
                scanSize - offset,
                static_cast<uint64_t>(chunkSize)));
            const auto read = reader.readChunked(task.scanStart + offset, remaining, chunkSize, cancellation);

            if (read.cancelled) {
                markCancelled(read.errorMessage.isEmpty() ? QString("Scan cancelled.") : read.errorMessage);
                return;
            }

            if (read.bytesRead == 0) {
                break;
            }

            QByteArray buffer = overlap + read.data;
            const uint64_t bufferBase = task.scanStart + offset - static_cast<uint64_t>(overlap.size());

            for (const auto& variant : variants) {
                const QByteArray needle = scanValueToBytes(variant.value);
                if (needle.isEmpty()) {
                    continue;
                }

                size_t effectiveAlignment = options.alignment;
                if (effectiveAlignment <= 1 && options.fastScan) {
                    effectiveAlignment = valueTypeSize(variant.value.type);
                }

                // Les variantes secondaires (×100, unsigned...) reçoivent un malus de confiance,
                // appliqué seulement si l'adresse n'a pas déjà été matchée par une variante primaire.
                const double confidence = computeScanTimeConfidence(regionScore, variant.secondary);

                ScanResult chunkResult;
                scanBuffer(
                    buffer,
                    bufferBase,
                    needle,
                    variant.value.type,
                    &chunkResult,
                    options.maxResults,
                    effectiveAlignment,
                    options.startAddress,
                    options.stopAddress,
                    confidence,
                    variant.label);
                mergeChunkMatches(chunkResult);
            }

            bytesScanned.fetch_add(read.bytesRead, std::memory_order_relaxed);
            reportProgress();
            offset += static_cast<uint64_t>(read.bytesRead);

            const qsizetype overlapSize = std::min<qsizetype>(static_cast<qsizetype>(maxNeedleSize) - 1, buffer.size());
            overlap = buffer.right(overlapSize);

            if (read.partial) {
                std::lock_guard<std::mutex> lock(resultMutex);
                result.partial = true;
                break;
            }
        }

        regionsScanned.fetch_add(1, std::memory_order_relaxed);
        reportProgress();
    });

    result.regionsScanned = regionsScanned.load(std::memory_order_relaxed);
    result.bytesScanned = bytesScanned.load(std::memory_order_relaxed);

    if (result.cancelled) {
        return result;
    }

    // Dédoublonner : si une adresse est matchée par plusieurs variantes,
    // on conserve celle avec la plus haute confiance (et son label).
    std::sort(result.matches.begin(), result.matches.end(),
              [](const ScanMatch& a, const ScanMatch& b) {
                  if (a.address != b.address) return a.address < b.address;
                  return a.confidence > b.confidence;
              });
    auto eqAddress = [](const ScanMatch& a, const ScanMatch& b) { return a.address == b.address; };
    auto last = std::unique(result.matches.begin(), result.matches.end(), eqAddress);
    result.matches.erase(last, result.matches.end());
    result.matchesFound = static_cast<size_t>(result.matches.size());

    result.success = true;
    KE_LOG_INFO() << "Multi-type scan completed: variants=" << variants.size()
                  << " matches=" << result.matchesFound
                  << " bytes=" << result.bytesScanned
                  << " mode=" << performanceModeToString(performance.resolvedMode)
                  << " workers=" << performance.workerThreads
                  << " chunk=" << chunkSize;
    return result;
}

} // namespace killcore
