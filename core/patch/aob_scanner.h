#pragma once

#include "memory/memory_region.h"
#include "process/process_handle.h"

#include <QByteArray>
#include <QList>
#include <QString>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace killcore {

struct AobPattern {
    std::vector<std::optional<uint8_t>> bytes;
    QString error;

    bool isValid() const { return error.isEmpty() && !bytes.empty(); }
    qsizetype size() const { return static_cast<qsizetype>(bytes.size()); }
};

struct AobScanOptions {
    bool executableOnly{true};
    bool writableOnly{false};
    bool imageOnly{false};
    uint64_t startAddress{0};
    uint64_t stopAddress{0};
    int maxResults{200};
    size_t chunkSize{1024 * 1024};
};

struct AobMatch {
    uint64_t address{0};
    uint64_t regionBase{0};
    uint64_t regionSize{0};
    uint32_t protection{0};
    MemoryType memoryType{MemoryType::Unknown};
};

struct AobScanResult {
    bool success{false};
    bool partial{false};
    QString error;
    uint64_t bytesScanned{0};
    int regionsScanned{0};
    int matchesFound{0};
    QList<AobMatch> matches;
};

struct AobPatternQuality {
    int score{0};
    QString level;
    QString warning;
    int patternBytes{0};
    int fixedBytes{0};
    int wildcardBytes{0};
    int uniqueFixedBytes{0};
    double fixedRatio{0.0};
    bool trainerSafe{false};
};

AobPattern parseAobPattern(const QString& patternText);
QString bytesToAobPattern(const QByteArray& bytes);
AobPatternQuality evaluateAobPatternQuality(const AobPattern& pattern);
QList<uint64_t> searchAobBuffer(const QByteArray& haystack, const AobPattern& pattern, uint64_t baseAddress = 0);
AobScanResult scanAobPattern(const ProcessHandle& process, const AobPattern& pattern, const AobScanOptions& options = {});

} // namespace killcore
